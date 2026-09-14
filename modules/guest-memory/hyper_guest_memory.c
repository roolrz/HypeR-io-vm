// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Expose HypeR-granted RAM as page-backed Linux mappings suitable for vhost.
 *
 * The DT range must be ordinary memory with a reserved-memory reservation,
 * never no-map or reusable. Linux creates struct page metadata but does not
 * allocate these pages. HypeR owns the backing storage for the whole I/O VM
 * lifetime; unbinding this driver does not revoke the foreign mapping.
 *
 * This external module is distributed separately from the Apache-2.0 HypeR
 * implementation and is built against an unmodified upstream kernel.
 */

#include <linux/fs.h>
#include <linux/compat.h>
#include <linux/uaccess.h>
#include "hyper_io.h"
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct hyper_guest_memory {
	struct miscdevice misc;
	struct kref references;
	struct mutex lock;
	unsigned long first_pfn;
	unsigned long pages;
	bool live;
	u64 guest_base;
};

static void hyper_memory_destroy(struct kref *reference)
{
	struct hyper_guest_memory *memory =
		container_of(reference, struct hyper_guest_memory, references);
	unsigned long index;

	for (index = 0; index < memory->pages; ++index)
		put_page(pfn_to_page(memory->first_pfn + index));
	kfree(memory->misc.name);
	kfree(memory);
}

static int hyper_memory_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct hyper_guest_memory *memory =
		container_of(misc, struct hyper_guest_memory, misc);
	int result = 0;

	/* misc_open and misc_deregister serialize admission. The initial driver
	 * reference remains held until deregistration has finished. */
	mutex_lock(&memory->lock);
	if (!memory->live)
		result = -ENODEV;
	else {
		kref_get(&memory->references);
		file->private_data = memory;
	}
	mutex_unlock(&memory->lock);
	return result;
}

static int hyper_memory_release(struct inode *inode, struct file *file)
{
	struct hyper_guest_memory *memory = file->private_data;

	/* A VMA keeps the file and its module owner alive even after close(fd). */
	kref_put(&memory->references, hyper_memory_destroy);
	return 0;
}

static int hyper_memory_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct hyper_guest_memory *memory = file->private_data;
	unsigned long count = vma_pages(vma);
	unsigned long index;
	int result = 0;

	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	if (vma->vm_pgoff >= memory->pages || count > memory->pages - vma->vm_pgoff)
		return -EINVAL;
	mutex_lock(&memory->lock);
	if (!memory->live) {
		result = -ENODEV;
		goto out;
	}
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_MIXEDMAP);
	vm_flags_clear(vma, VM_MAYEXEC);
	/* Do not use remap_pfn_range: VM_PFNMAP cannot supply the normal page
	 * references consumed by vhost-scsi's scatterlist construction. */
	for (index = 0; index < count; ++index) {
		struct page *page = pfn_to_page(memory->first_pfn + vma->vm_pgoff + index);

		result = vm_insert_page(vma, vma->vm_start + (index << PAGE_SHIFT), page);
		if (result)
			break;
	}
out:
	mutex_unlock(&memory->lock);
	return result;
}

static long hyper_memory_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct hyper_guest_memory *memory = file->private_data;
	struct hyper_memory_info info;
	int result = 0;
	if (command != HYPER_MEMORY_INFO) return -ENOTTY;
	mutex_lock(&memory->lock);
	if (!memory->live) result = -ENODEV;
	else {
		info.guest_base = memory->guest_base;
		info.length = (u64)memory->pages << PAGE_SHIFT;
		if (copy_to_user((void __user *)argument, &info, sizeof(info))) result = -EFAULT;
	}
	mutex_unlock(&memory->lock);
	return result;
}

static const struct file_operations hyper_memory_operations = {
	.owner = THIS_MODULE,
	.open = hyper_memory_open,
	.release = hyper_memory_release,
	.mmap = hyper_memory_mmap,
	.unlocked_ioctl = hyper_memory_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int hyper_memory_probe(struct platform_device *device)
{
	struct hyper_guest_memory *memory;
	struct device_node *region;
	struct resource resource;
	resource_size_t bytes;
	unsigned long total;
	int result;

	region = of_parse_phandle(device->dev.of_node, "memory-region", 0);
	if (!region)
		return -EINVAL;
	if (of_property_read_bool(region, "no-map") ||
	    of_property_read_bool(region, "reusable")) {
		of_node_put(region);
		return -EINVAL;
	}
	result = of_address_to_resource(region, 0, &resource);
	of_node_put(region);
	if (result)
		return result;
	bytes = resource_size(&resource);
	if (!bytes || !PAGE_ALIGNED(resource.start) || !PAGE_ALIGNED(bytes) ||
	    (resource.start >> PAGE_SHIFT) > ULONG_MAX ||
	    (bytes >> PAGE_SHIFT) > ULONG_MAX)
		return -EINVAL;
	total = bytes >> PAGE_SHIFT;
	if (total > ULONG_MAX - (resource.start >> PAGE_SHIFT))
		return -EOVERFLOW;
	memory = kzalloc(sizeof(*memory), GFP_KERNEL);
	if (!memory)
		return -ENOMEM;
	kref_init(&memory->references);
	mutex_init(&memory->lock);
	memory->guest_base = resource.start;
	if (of_find_property(device->dev.of_node, "hyper,guest-base", NULL) &&
	    of_property_read_u64(device->dev.of_node, "hyper,guest-base", &memory->guest_base)) {
		result = -EINVAL; goto fail;
	}
	if (!PAGE_ALIGNED(memory->guest_base) || memory->guest_base > U64_MAX - bytes) {
		result = -EINVAL; goto fail;
	}
	memory->first_pfn = resource.start >> PAGE_SHIFT;
	/* Retain every page before publishing the device. Neither a partial
	 * probe nor a failed registration leaves an unowned page reference. */
	for (; memory->pages < total; ++memory->pages) {
		unsigned long pfn = memory->first_pfn + memory->pages;
		struct page *page;

		if (!pfn_valid(pfn)) {
			result = -EINVAL;
			goto fail;
		}
		page = pfn_to_page(pfn);
		if (!PageReserved(page) || PageCompound(page) || !get_page_unless_zero(page)) {
			result = -EINVAL;
			goto fail;
		}
	}
	if (of_find_property(device->dev.of_node, "hyper,client-id", NULL)) {
		u32 client;
		if (of_property_read_u32(device->dev.of_node, "hyper,client-id", &client) || client >= 128) {
			result = -EINVAL; goto fail;
		}
		memory->misc.name = kasprintf(GFP_KERNEL, "hyper-memory-%u", client);
	} else memory->misc.name = kasprintf(GFP_KERNEL, "hyper-memory-%s", dev_name(&device->dev));
	if (!memory->misc.name) {
		result = -ENOMEM;
		goto fail;
	}
	memory->misc.minor = MISC_DYNAMIC_MINOR;
	memory->misc.fops = &hyper_memory_operations;
	memory->misc.parent = &device->dev;
	memory->misc.mode = 0600;
	memory->live = true;
	result = misc_register(&memory->misc);
	if (result)
		goto fail;
	platform_set_drvdata(device, memory);
	dev_info(&device->dev, "registered %lu reserved shared pages\n", total);
	return 0;
fail:
	kref_put(&memory->references, hyper_memory_destroy);
	return result;
}

static void hyper_memory_remove(struct platform_device *device)
{
	struct hyper_guest_memory *memory = platform_get_drvdata(device);

	mutex_lock(&memory->lock);
	memory->live = false;
	mutex_unlock(&memory->lock);
	misc_deregister(&memory->misc);
	kref_put(&memory->references, hyper_memory_destroy);
}

static const struct of_device_id hyper_memory_match[] = {
	{ .compatible = "hyper,guest-memory-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, hyper_memory_match);

static struct platform_driver hyper_memory_driver = {
	.probe = hyper_memory_probe,
	.remove = hyper_memory_remove,
	.driver = {
		.name = "hyper-guest-memory",
		.of_match_table = hyper_memory_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(hyper_memory_driver);

MODULE_DESCRIPTION("HypeR reserved guest RAM mappings for Linux I/O backends");
MODULE_AUTHOR("roolrz");
MODULE_LICENSE("GPL");
