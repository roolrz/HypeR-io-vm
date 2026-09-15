// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: GPL-2.0-only
/* Linux-local eventfd and control bridge. No block requests enter userspace. */
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/compat.h>
#include <linux/kref.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "hyper_io.h"
#define RX_READY 1
#define TX_SPACE 2
#define CLOSED HYPER_IO_MAILBOX_CLOSED
#define ERROR 8
#define MB_STATUS 0x08
#define MB_MASK 0x0c
#define MB_RX_LEN 0x10
#define MB_RX_SEQ 0x18
#define MB_RX_CONSUME 0x20
#define MB_TX_LEN 0x28
#define MB_TX_COMMIT 0x2c
#define MB_RX_DATA 0x100
#define MB_TX_DATA 0x200
struct bridge;
struct call_binding {
	struct bridge *bridge;
	struct file *file;
	struct eventfd_ctx *event;
	wait_queue_entry_t wait;
	wait_queue_head_t *queue;
	u32 epoch;
};
struct call_poll { poll_table table; struct call_binding *call; };
struct bridge {
	struct kref references;
	bool dead;
	void __iomem *base;
	struct miscdevice misc;
	/* One owner executes read-request/write-response transactions serially. */
	struct mutex control_lock;
	struct mutex bind_lock;
	wait_queue_head_t changed;
	atomic_t opened;
	atomic64_t irq_generation;
	bool mailbox;
	int irq;
	struct eventfd_ctx *kick[HYPER_IO_QUEUES];
	struct call_binding call[HYPER_IO_QUEUES];
	bool bound;
	bool irq_enabled;
};
static void notify_call(struct call_binding *call)
{
	u64 count;
	/* eventfd invokes wake callbacks with its waitqueue lock held. The
	 * no-lock drain is exported for exactly this use; eventfd_ctx_read would
	 * recursively acquire the same lock and deadlock. */
	eventfd_ctx_do_read(call->event, &count);
	if (count)
		writeq(((u64)call->epoch << 32) | 1, call->bridge->base + 0x18);
}
static int call_wakeup(wait_queue_entry_t *wait, unsigned int mode, int sync, void *key)
{
	struct call_binding *call = container_of(wait, struct call_binding, wait);
	if (key_to_poll(key) & EPOLLIN)
		notify_call(call);
	return 0;
}
static void call_poll_queue(struct file *file, wait_queue_head_t *queue, poll_table *table)
{
	struct call_binding *call = container_of(table, struct call_poll, table)->call;
	/* vfs_poll on a validated eventfd supplies exactly its one waitqueue. */
	call->queue = queue;
	add_wait_queue(queue, &call->wait);
}
static void unbind(struct bridge *bridge)
{
	unsigned int i;
	/* Caller disables its backend before this ioctl. IRQ synchronization
	 * excludes all readers of kick[] before eventfd references are released. */
	if (bridge->irq_enabled) {
		disable_irq(bridge->irq);
		bridge->irq_enabled = false;
	}
	for (i = 0; i < HYPER_IO_QUEUES; ++i) {
		struct call_binding *call = &bridge->call[i];
		if (call->queue) {
			u64 count;
			eventfd_ctx_remove_wait_queue(call->event, &call->wait, &count);
			call->queue = NULL;
		}
		if (call->event) { eventfd_ctx_put(call->event); call->event = NULL; }
		if (call->file) { fput(call->file); call->file = NULL; }
		if (bridge->kick[i]) { eventfd_ctx_put(bridge->kick[i]); bridge->kick[i] = NULL; }
	}
	bridge->bound = false;
}
static long notification_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct bridge *bridge = file->private_data;
	struct hyper_io_eventfds fds;
	unsigned int i;
	int result = 0;
	if (bridge->mailbox) return -ENOTTY;
	if (command != HYPER_IO_BIND && command != HYPER_IO_UNBIND && command != HYPER_IO_QUIESCENT && command != HYPER_IO_ADMIT && command != HYPER_IO_EXTENT) return -ENOTTY;
	mutex_lock(&bridge->bind_lock);
	if (bridge->dead) { result = -ENODEV; goto out; }
	if (command == HYPER_IO_ADMIT) {
		struct hyper_io_admission grant;
		if (bridge->bound) { result = -EBUSY; goto out; }
		if (copy_from_user(&grant, (void __user *)argument, sizeof(grant))) { result = -EFAULT; goto out; }
		if (!grant.token) { result = -EINVAL; goto out; }
		/* Admission atomically binds the one-use token to this route. Query
		 * immutable kernel facts instead of trusting a claimed alias/length. */
		writeq(grant.token, bridge->base + 0x28);
		if (readq(bridge->base + 0x40)) { result = -EACCES; goto out; }
		if (readq(bridge->base + 0x30) != grant.guest_base || readq(bridge->base + 0x38) != grant.length) {
			/* Admitted but never touched: retire this token before rejection. */
			writeq(grant.token, bridge->base + 0x20);
			result = -EINVAL; goto out;
		}
		grant.extent_count = readq(bridge->base + 0x48);
		if (!grant.extent_count || grant.extent_count > grant.length / 4096 ||
		    copy_to_user((void __user *)argument, &grant, sizeof(grant))) {
			writeq(grant.token, bridge->base + 0x20);
			result = -EINVAL;
		}
		goto out;
	}
	if (command == HYPER_IO_EXTENT) {
		struct hyper_io_extent extent;
		if (bridge->bound) { result = -EBUSY; goto out; }
		if (copy_from_user(&extent, (void __user *)argument, sizeof(extent))) { result = -EFAULT; goto out; }
		writeq(extent.index, bridge->base + 0x50);
		if (readq(bridge->base + 0x70)) { result = -EINVAL; goto out; }
		extent.alias = readq(bridge->base + 0x58);
		extent.offset = readq(bridge->base + 0x60);
		extent.length = readq(bridge->base + 0x68);
		if (copy_to_user((void __user *)argument, &extent, sizeof(extent))) result = -EFAULT;
		goto out;
	}
	if (command == HYPER_IO_QUIESCENT) {
		u64 token;
		if (bridge->bound) { result = -EBUSY; goto out; }
		if (copy_from_user(&token, (void __user *)argument, sizeof(token))) { result = -EFAULT; goto out; }
		if (!token) { result = -EINVAL; goto out; }
		/* Hyper derives the sender VM from the trapped write, never from a
		 * caller-controlled Native capability or a claimed peer identifier. */
		writeq(token, bridge->base + 0x20);
		goto out;
	}
	if (command == HYPER_IO_UNBIND) { unbind(bridge); goto out; }
	if (bridge->bound) { result = -EBUSY; goto out; }
	if (copy_from_user(&fds, (void __user *)argument, sizeof(fds))) { result = -EFAULT; goto out; }
	if (!fds.epoch || fds.reserved || fds.epoch != readl(bridge->base + 0x08) ||
	    readl(bridge->base + 0x0c)) { result = -EINVAL; goto out; }
	/* Unbound notification IRQs remain masked, including before a route exists. */
	for (i = 0; i < HYPER_IO_QUEUES; ++i) {
		struct call_binding *call = &bridge->call[i];
		struct call_poll poll = {.call = call};
		__poll_t ready;
		if (i >= 3 && fds.kick[i] == -1 && fds.call[i] == -1) continue;
		bridge->kick[i] = eventfd_ctx_fdget(fds.kick[i]);
		if (IS_ERR(bridge->kick[i])) { result = PTR_ERR(bridge->kick[i]); bridge->kick[i] = NULL; break; }
		call->file = eventfd_fget(fds.call[i]);
		if (IS_ERR(call->file)) { result = PTR_ERR(call->file); call->file = NULL; break; }
		call->event = eventfd_ctx_fileget(call->file);
		if (IS_ERR(call->event)) { result = PTR_ERR(call->event); call->event = NULL; break; }
		call->bridge = bridge; call->epoch = fds.epoch;
		init_waitqueue_func_entry(&call->wait, call_wakeup);
		init_poll_funcptr(&poll.table, call_poll_queue);
		ready = vfs_poll(call->file, &poll.table);
		if (!call->queue) { result = -EINVAL; break; }
		if (ready & EPOLLIN) {
			unsigned long flags;
			spin_lock_irqsave(&call->queue->lock, flags);
			notify_call(call);
			spin_unlock_irqrestore(&call->queue->lock, flags);
		}
	}
	if (!result) {
		bridge->bound = true;
		bridge->irq_enabled = true;
		enable_irq(bridge->irq);
	} else unbind(bridge);
out:
	mutex_unlock(&bridge->bind_lock);
	return result;
}
static irqreturn_t bridge_irq(int irq, void *opaque)
{
	struct bridge *bridge = opaque;
	if (bridge->mailbox) {
		/* One-shot source arming. Readers/writers recheck durable status
		 * before sleeping and restore their desired mask. */
		writel(0, bridge->base + MB_MASK);
		atomic64_inc(&bridge->irq_generation);
		wake_up_interruptible(&bridge->changed);
	} else {
		u32 pending = readl(bridge->base + 0x10);
		unsigned int i;
		for (i = 0; i < HYPER_IO_QUEUES; ++i)
			if ((pending & BIT(i)) && bridge->kick[i])
				eventfd_signal(bridge->kick[i]);
	}
	return IRQ_HANDLED;
}
static void bridge_destroy(struct kref *ref)
{
	struct bridge *bridge = container_of(ref, struct bridge, references);
	kfree(bridge->misc.name);
	kfree(bridge);
}
static int bridge_open(struct inode *inode, struct file *file)
{
	struct bridge *bridge = container_of(file->private_data, struct bridge, misc);
	if (atomic_cmpxchg(&bridge->opened, 0, 1)) return -EBUSY;
	kref_get(&bridge->references);
	file->private_data = bridge;
	return 0;
}
static int bridge_release(struct inode *inode, struct file *file)
{
	struct bridge *bridge = file->private_data;
	if (!bridge->mailbox) {
		mutex_lock(&bridge->bind_lock);
		if (!bridge->dead) unbind(bridge);
		mutex_unlock(&bridge->bind_lock);
	} else {
		mutex_lock(&bridge->control_lock);
		if (!bridge->dead) writel(0, bridge->base + MB_MASK);
		mutex_unlock(&bridge->control_lock);
	}
	atomic_set(&bridge->opened, 0);
	kref_put(&bridge->references, bridge_destroy);
	return 0;
}
static int wait_status(struct bridge *bridge, u32 wanted)
{
	int result;
	for (;;) {
		u32 status;
		s64 generation;
		if (READ_ONCE(bridge->dead)) return -ENODEV;
		status = readl(bridge->base + MB_STATUS);
		if (status & CLOSED) return -EPIPE;
		if (status & wanted) return 0;
		/* A delayed IRQ from a previous transaction may mask this new arm.
		 * Waking on its generation change forces the outer loop to rearm,
		 * even if the new transaction has no RX data yet. */
		generation = atomic64_read(&bridge->irq_generation);
		writel(wanted | CLOSED, bridge->base + MB_MASK);
		result = wait_event_interruptible(bridge->changed,
			READ_ONCE(bridge->dead) ||
			hyper_io_wait_ready(readl(bridge->base + MB_STATUS), wanted,
				generation, atomic64_read(&bridge->irq_generation)));
		writel(0, bridge->base + MB_MASK);
		if (result) return result;
	}
}
static ssize_t control_read(struct file *file, char __user *buffer, size_t capacity, loff_t *offset)
{
	struct bridge *bridge = file->private_data;
	u32 bytes[64], length;
	u64 sequence;
	ssize_t result;
	unsigned int i;
	if (!bridge->mailbox) return -EINVAL;
	if (mutex_lock_interruptible(&bridge->control_lock)) return -ERESTARTSYS;
	result = wait_status(bridge, RX_READY);
	if (result) goto out;
	length = readl(bridge->base + MB_RX_LEN);
	sequence = readq(bridge->base + MB_RX_SEQ);
	if (!length || length > sizeof(bytes)) { result = -EPROTO; goto out; }
	if (capacity < length) { result = -EMSGSIZE; goto out; }
	for (i = 0; i < DIV_ROUND_UP(length, 4); ++i)
		bytes[i] = cpu_to_le32(readl(bridge->base + MB_RX_DATA + i * 4));
	if (copy_to_user(buffer, bytes, length)) { result = -EFAULT; goto out; }
	/* Retain the record on user-copy failure; consume only its exact sequence. */
	writeq(sequence, bridge->base + MB_RX_CONSUME);
	result = length;
out:
	mutex_unlock(&bridge->control_lock);
	return result;
}
static ssize_t control_write(struct file *file, const char __user *buffer, size_t length, loff_t *offset)
{
	struct bridge *bridge = file->private_data;
	u32 bytes[64] = {0};
	ssize_t result;
	unsigned int i;
	if (!bridge->mailbox || !length || length > sizeof(bytes)) return -EINVAL;
	if (copy_from_user(bytes, buffer, length)) return -EFAULT;
	if (mutex_lock_interruptible(&bridge->control_lock)) return -ERESTARTSYS;
	result = wait_status(bridge, TX_SPACE);
	if (result) goto out;
	writel(ERROR, bridge->base + MB_STATUS);
	for (i = 0; i < DIV_ROUND_UP(length, 4); ++i)
		writel(le32_to_cpu(bytes[i]), bridge->base + MB_TX_DATA + i * 4);
	writel(length, bridge->base + MB_TX_LEN);
	writel(1, bridge->base + MB_TX_COMMIT);
	result = readl(bridge->base + MB_STATUS) & (ERROR | CLOSED) ? -EIO : length;
out:
	mutex_unlock(&bridge->control_lock);
	return result;
}
static const struct file_operations bridge_operations = {
	.owner = THIS_MODULE, .open = bridge_open, .release = bridge_release,
	.read = control_read, .write = control_write, .unlocked_ioctl = notification_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};
static int bridge_probe(struct platform_device *device)
{
	struct bridge *bridge;
	struct resource *resource;
	int result;
	bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge) return -ENOMEM;
	kref_init(&bridge->references);
	bridge->mailbox = of_device_is_compatible(device->dev.of_node, "hyper,guest-mailbox-v1");
	resource = platform_get_resource(device, IORESOURCE_MEM, 0);
	if (!resource || resource_size(resource) < 4096) { result = -EINVAL; goto fail; }
	bridge->base = devm_ioremap_resource(&device->dev, resource);
	if (IS_ERR(bridge->base)) { result = PTR_ERR(bridge->base); goto fail; }
	/* Managed notification routes are installed only when a client binds. */
	if ((bridge->mailbox || !of_find_property(device->dev.of_node, "hyper,client-id", NULL)) &&
	    (readl(bridge->base) != (bridge->mailbox ? 0x48594d42 : 0x48594e42) ||
	     readl(bridge->base + 4) != 1)) { result = -ENODEV; goto fail; }
	mutex_init(&bridge->control_lock); mutex_init(&bridge->bind_lock);
	init_waitqueue_head(&bridge->changed); atomic_set(&bridge->opened, 0);
	atomic64_set(&bridge->irq_generation, 0);
	bridge->irq = platform_get_irq(device, 0);
	if (bridge->irq < 0) { result = bridge->irq; goto fail; }
	/* Threaded IRQ context avoids nested eventfd wake recursion and lets the
	 * supported vhost eventfd consumer schedule its normal kernel worker. */
	result = devm_request_threaded_irq(&device->dev, bridge->irq, NULL, bridge_irq,
		IRQF_ONESHOT | (bridge->mailbox ? 0 : IRQF_NO_AUTOEN), dev_name(&device->dev), bridge);
	if (result) goto fail;
	bridge->misc.minor = MISC_DYNAMIC_MINOR;
	{
		u32 client;
		const char *base = bridge->mailbox ? "hyper-io-control" : "hyper-io-notification";
		if (of_find_property(device->dev.of_node, "hyper,client-id", NULL)) {
			if (of_property_read_u32(device->dev.of_node, "hyper,client-id", &client) || client >= 128) {
				result = -EINVAL; goto free_irq;
			}
			bridge->misc.name = kasprintf(GFP_KERNEL, "%s-%u", base, client);
		} else bridge->misc.name = kstrdup(base, GFP_KERNEL);
		if (!bridge->misc.name) { result = -ENOMEM; goto free_irq; }
	}
	bridge->misc.mode = 0600; bridge->misc.fops = &bridge_operations;
	bridge->misc.parent = &device->dev;
	result = misc_register(&bridge->misc);
	if (result) goto free_irq;
	platform_set_drvdata(device, bridge);
	return 0;
free_irq:
	devm_free_irq(&device->dev, bridge->irq, bridge);
fail:
	kref_put(&bridge->references, bridge_destroy);
	return result;
}
static void bridge_remove(struct platform_device *device)
{
	struct bridge *bridge = platform_get_drvdata(device);
	/* Stop admission, wake blocking readers, then drain all file operations
	 * before devm releases IRQ/MMIO. Open files retain only the dead object. */
	misc_deregister(&bridge->misc);
	WRITE_ONCE(bridge->dead, true);
	wake_up_interruptible(&bridge->changed);
	mutex_lock(&bridge->control_lock);
	mutex_lock(&bridge->bind_lock);
	if (bridge->mailbox) writel(0, bridge->base + MB_MASK);
	else unbind(bridge);
	devm_free_irq(&device->dev, bridge->irq, bridge);
	mutex_unlock(&bridge->bind_lock);
	mutex_unlock(&bridge->control_lock);
	kref_put(&bridge->references, bridge_destroy);
}
static const struct of_device_id bridge_match[] = {
	{.compatible = "hyper,guest-mailbox-v1"}, {.compatible = "hyper,guest-notification-v1"}, {}
};
MODULE_DEVICE_TABLE(of, bridge_match);
static struct platform_driver bridge_driver = {
	.probe = bridge_probe, .remove = bridge_remove,
	.driver = {.name = "hyper-io-bridge", .of_match_table = bridge_match, .suppress_bind_attrs = true},
};
module_platform_driver(bridge_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HypeR guest notification and control bridge");
