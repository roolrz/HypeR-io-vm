// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Expose HypeR-granted RAM as page-backed Linux mappings suitable for vhost.
 *
 * Static config pools are ordinary memory with a reserved-memory reservation.
 * Dynamic slots describe only an address aperture; page metadata is created
 * after the Native owner maps an admitted identity grant and removed before
 * acknowledging release. This driver never allocates the foreign backing.
 *
 * This external module is distributed separately from the Apache-2.0 HypeR
 * implementation and is built against an unmodified upstream kernel.
 */

#include <linux/fs.h>
#include <linux/compat.h>
#include <linux/uaccess.h>
#include "hyper_io.h"
#include "hyper_io_layout.h"
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/memremap.h>
#include <linux/memory_hotplug.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/xarray.h>
#include <linux/sort.h>

struct hyper_granule { unsigned int users; struct dev_pagemap pgmap; };
static DEFINE_MUTEX(granules_lock);
static DEFINE_XARRAY(granules);

/* Metadata ownership is per 2 MiB granule. Authorization stays per 4 KiB page. */
static struct hyper_granule *granule_get(unsigned long index)
{
	struct hyper_granule *granule;
	void *mapping;
	int result;
	mutex_lock(&granules_lock);
	granule = xa_load(&granules, index);
	if (granule) { ++granule->users; goto out; }
	granule = kzalloc(sizeof(*granule), GFP_KERNEL);
	if (!granule) { granule = ERR_PTR(-ENOMEM); goto out; }
	granule->pgmap.type = MEMORY_DEVICE_GENERIC;
	granule->pgmap.owner = &granules;
	granule->pgmap.nr_range = 1;
	granule->pgmap.range.start = (u64)index * SZ_2M;
	granule->pgmap.range.end = granule->pgmap.range.start + SZ_2M - 1;
	if (!request_mem_region(granule->pgmap.range.start, SZ_2M, "hyper-guest-granule")) {
		kfree(granule); granule = ERR_PTR(-EBUSY); goto out;
	}
	mapping = memremap_pages(&granule->pgmap, NUMA_NO_NODE);
	if (IS_ERR(mapping)) {
		result = PTR_ERR(mapping); goto release;
	}
	result = xa_err(xa_store(&granules, index, granule, GFP_KERNEL));
	if (result) { memunmap_pages(&granule->pgmap); goto release; }
	granule->users = 1;
	goto out;
release:
	release_mem_region(granule->pgmap.range.start, SZ_2M);
	kfree(granule); granule = ERR_PTR(result);
out:
	mutex_unlock(&granules_lock);
	return granule;
}
static void granule_put(struct hyper_granule *granule)
{
	mutex_lock(&granules_lock);
	if (--granule->users) { mutex_unlock(&granules_lock); return; }
	xa_erase(&granules, granule->pgmap.range.start / SZ_2M);
	mutex_unlock(&granules_lock);
	/* Keep the resource claimed while waiting for the last page references;
	 * another admission of this granule fails busy instead of reusing it. */
	memunmap_pages(&granule->pgmap);
	release_mem_region(granule->pgmap.range.start, SZ_2M);
	kfree(granule);
}
static int compare_indices(const void *left, const void *right)
{
	unsigned long a = *(const unsigned long *)left, b = *(const unsigned long *)right;
	return (a > b) - (a < b);
}
struct hyper_guest_memory {
	struct miscdevice misc;
	struct kref references;
	struct mutex lock;
	unsigned long first_pfn;
	unsigned long pages;
	bool live;
	bool dynamic, opened;
	unsigned int vmas;
	struct resource aperture;
	unsigned long *pfns;
	struct hyper_granule **granules;
	unsigned int granule_count;
	u64 guest_base;
};

/* Caller owns lock; no mmap can be created while this teardown is running.
 * memunmap_pages kills admission and waits for every GUP/DMA page reference. */
static void hyper_memory_unprepare(struct hyper_guest_memory *memory)
{
	unsigned int i;
	for (i = 0; i < memory->granule_count; ++i) granule_put(memory->granules[i]);
	kvfree(memory->granules); kvfree(memory->pfns);
	memory->granules = NULL; memory->pfns = NULL; memory->granule_count = 0;
	if (memory->dynamic) { memory->first_pfn = 0; memory->pages = 0; }
}
static void hyper_memory_destroy(struct kref *reference)
{
	struct hyper_guest_memory *memory =
		container_of(reference, struct hyper_guest_memory, references);
	unsigned long index;

	hyper_memory_unprepare(memory);
	for (index = 0; !memory->dynamic && index < memory->pages; ++index)
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
	else if (memory->dynamic && memory->opened)
		result = -EBUSY;
	else {
		kref_get(&memory->references);
		memory->opened = true;
		file->private_data = memory;
	}
	mutex_unlock(&memory->lock);
	return result;
}

static int hyper_memory_release(struct inode *inode, struct file *file)
{
	struct hyper_guest_memory *memory = file->private_data;

	/* A VMA keeps the file and its module owner alive even after close(fd). */
	mutex_lock(&memory->lock);
	if (memory->dynamic) hyper_memory_unprepare(memory);
	memory->opened = false;
	mutex_unlock(&memory->lock);
	kref_put(&memory->references, hyper_memory_destroy);
	return 0;
}

static void hyper_vma_open(struct vm_area_struct *vma)
{
	struct hyper_guest_memory *memory = vma->vm_private_data;
	mutex_lock(&memory->lock); ++memory->vmas; mutex_unlock(&memory->lock);
}
static void hyper_vma_close(struct vm_area_struct *vma)
{
	struct hyper_guest_memory *memory = vma->vm_private_data;
	mutex_lock(&memory->lock); --memory->vmas; mutex_unlock(&memory->lock);
}
static const struct vm_operations_struct hyper_vma_ops = {
	.open = hyper_vma_open, .close = hyper_vma_close,
};
static int hyper_memory_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct hyper_guest_memory *memory = file->private_data;
	unsigned long count = vma_pages(vma);
	unsigned long index;
	int result = 0;

	if (!(vma->vm_flags & VM_SHARED) || (vma->vm_flags & VM_EXEC))
		return -EINVAL;
	mutex_lock(&memory->lock);
	if (vma->vm_pgoff >= memory->pages || count > memory->pages - vma->vm_pgoff) {
		result = -EINVAL; goto out;
	}
	if (!memory->live) {
		result = -ENODEV;
		goto out;
	}
		vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_MIXEDMAP);
	vm_flags_clear(vma, VM_MAYEXEC);
	/* Do not use remap_pfn_range: VM_PFNMAP cannot supply the normal page
	 * references consumed by vhost-scsi's scatterlist construction. */
	for (index = 0; index < count; ++index) {
		unsigned long pfn = memory->dynamic ? memory->pfns[vma->vm_pgoff + index] :
			memory->first_pfn + vma->vm_pgoff + index;
		struct page *page = pfn_to_page(pfn);

		result = vm_insert_page(vma, vma->vm_start + (index << PAGE_SHIFT), page);
		if (result)
			break;
	}
	if (!result) {
		vma->vm_private_data = memory; vma->vm_ops = &hyper_vma_ops;
		++memory->vmas;
	}
out:
	mutex_unlock(&memory->lock);
	return result;
}

static int hyper_memory_prepare(struct hyper_guest_memory *memory,
		const struct hyper_memory_prepare *prepare)
{
	u64 *aliases = NULL;
	unsigned long *indices = NULL, *pfns = NULL;
	struct hyper_granule **acquired = NULL;
	unsigned int i, unique = 0, ready = 0;
	int result = -EINVAL;
	if (!prepare->length || !PAGE_ALIGNED(prepare->length) ||
	    !PAGE_ALIGNED(prepare->guest_base) || prepare->reserved ||
	    prepare->guest_base > U64_MAX - prepare->length) return -EINVAL;
	if (!memory->dynamic)
		return !prepare->token && !prepare->count && !prepare->pages &&
			prepare->alias == ((u64)memory->first_pfn << PAGE_SHIFT) &&
			prepare->length <= ((u64)memory->pages << PAGE_SHIFT) ? 0 : -EINVAL;
	if (!prepare->token || !prepare->count || prepare->count > HYPER_IO_MAX_GRANT_PAGES ||
	    prepare->count != prepare->length >> PAGE_SHIFT) return -EINVAL;
	if (memory->pfns) return -EBUSY;
	aliases = kvmalloc_array(prepare->count, sizeof(*aliases), GFP_KERNEL);
	indices = kvmalloc_array(prepare->count, sizeof(*indices), GFP_KERNEL);
	pfns = kvmalloc_array(prepare->count, sizeof(*pfns), GFP_KERNEL);
	if (!aliases || !indices || !pfns) { result = -ENOMEM; goto out; }
	if (copy_from_user(aliases, u64_to_user_ptr(prepare->pages), prepare->count * sizeof(*aliases))) {
		result = -EFAULT; goto out;
	}
	if (prepare->alias) goto out;
	for (i = 0; i < prepare->count; ++i) {
		if (!hyper_io_page_aperture(aliases[i], memory->aperture.start, memory->aperture.end)) goto out;
		pfns[i] = aliases[i] >> PAGE_SHIFT;
		indices[i] = aliases[i] / SZ_2M;
	}
	sort(indices, prepare->count, sizeof(*indices), compare_indices, NULL);
	for (i = 0; i < prepare->count; ++i)
		if (!i || indices[i] != indices[i - 1]) indices[unique++] = indices[i];
	acquired = kvmalloc_array(unique, sizeof(*acquired), GFP_KERNEL);
	if (!acquired) { result = -ENOMEM; goto out; }
	for (; ready < unique; ++ready) {
		acquired[ready] = granule_get(indices[ready]);
		if (IS_ERR(acquired[ready])) { result = PTR_ERR(acquired[ready]); goto out; }
	}
	/* Publish only after every page/granule has been admitted successfully. */
	memory->pfns = pfns; pfns = NULL;
	memory->granules = acquired; acquired = NULL;
	memory->granule_count = ready; ready = 0;
	memory->pages = prepare->count;
	memory->guest_base = prepare->guest_base;
	result = 0;
out:
	while (ready) granule_put(acquired[--ready]);
	kvfree(acquired); kvfree(pfns); kvfree(indices); kvfree(aliases);
	return result;
}
static long hyper_memory_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct hyper_guest_memory *memory = file->private_data;
	struct hyper_memory_info info;
	struct hyper_memory_prepare prepare;
	int result = 0;
	if (command != HYPER_MEMORY_INFO && command != HYPER_MEMORY_PREPARE &&
	    command != HYPER_MEMORY_RELEASE) return -ENOTTY;
	if (command == HYPER_MEMORY_PREPARE && copy_from_user(&prepare, (void __user *)argument, sizeof(prepare))) return -EFAULT;
	mutex_lock(&memory->lock);
	if (!memory->live) result = -ENODEV;
	else if (command == HYPER_MEMORY_PREPARE) result = hyper_memory_prepare(memory, &prepare);
	else if (command == HYPER_MEMORY_RELEASE) {
		if (memory->vmas) result = -EBUSY;
		else hyper_memory_unprepare(memory);
	} else {
		info.guest_base = memory->guest_base;
		info.length = memory->dynamic ? resource_size(&memory->aperture) : (u64)memory->pages << PAGE_SHIFT;
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
	unsigned long total = 0;
	int result;

	if (of_property_read_bool(device->dev.of_node, "hyper,dynamic-memory")) {
		memory = kzalloc(sizeof(*memory), GFP_KERNEL);
		if (!memory) return -ENOMEM;
		kref_init(&memory->references); mutex_init(&memory->lock);
		memory->dynamic = true;
		result = of_address_to_resource(device->dev.of_node, 0, &memory->aperture);
		if (result || !resource_size(&memory->aperture)) { result = -EINVAL; goto fail; }
		goto register_device;
	}
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
register_device:
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
	if (memory->dynamic) dev_info(&device->dev, "registered dynamic grant aperture\n");
	else dev_info(&device->dev, "registered %lu reserved shared pages\n", total);
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
