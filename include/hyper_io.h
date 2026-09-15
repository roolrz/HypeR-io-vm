// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#ifndef HYPER_IO_H
#define HYPER_IO_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define HYPER_IO_MAGIC 0x314f4948U
#define HYPER_IO_VERSION 2
#define HYPER_IO_QUEUES 6
#define HYPER_IO_RECORD 256
#define HYPER_IO_MAX_GRANT_PAGES (1U << 20)
#define HYPER_IO_REPLY 1
#define HYPER_IO_HELLO 1
#define HYPER_IO_ACTIVATE 2
#define HYPER_IO_RESET 3
#define HYPER_IO_STOP_QUEUE 4
#define HYPER_IO_PREPARE_MEMORY 5
#define HYPER_IO_RELEASE_MEMORY 6
#define HYPER_IO_OK 0
#define HYPER_IO_UNSUPPORTED 1
#define HYPER_IO_INVALID 2
#define HYPER_IO_BUSY 3
#define HYPER_IO_BACKEND_FAILURE 4
#define HYPER_IO_QUIESCENCE_FAILED 5
struct hyper_io_header {
	__le32 magic;
	__le16 version, operation;
	__le32 length, flags;
	__le64 binding, epoch, transaction;
};
struct hyper_io_queue {
	__le32 size, reserved;
	__le64 descriptor, available, used;
};
struct hyper_io_activate {
	struct hyper_io_header header;
	__le64 features;
	struct hyper_io_queue queues[HYPER_IO_QUEUES];
};
struct hyper_io_prepare_memory {
	struct hyper_io_header header;
	__le64 alias, guest_base, length, token;
};
struct hyper_io_reply {
	struct hyper_io_header header;
	__le32 status, reserved;
	__le64 features;
	__le32 queues, queue_max;
};
/* Linux-local control ABI. File descriptor numbers never cross the VM boundary. */
struct hyper_io_eventfds {
	__u32 epoch, reserved;
	__s32 kick[HYPER_IO_QUEUES], call[HYPER_IO_QUEUES];
};
struct hyper_memory_info {
	__u64 guest_base, length;
};
struct hyper_io_admission { __u64 token, guest_base, length, extent_count; };
struct hyper_io_extent { __u64 index, alias, offset, length; };
struct hyper_memory_prepare {
	__u64 alias, guest_base, length, token, pages;
	__u32 count, reserved;
};
/* Shared by the Linux wait loop and its deterministic delayed-IRQ test.
 * A one-shot IRQ can mask a newer arm without making RX_READY true. */
#define HYPER_IO_MAILBOX_CLOSED 4U
static inline int hyper_io_wait_ready(__u32 status, __u32 wanted,
                                     __u64 armed_generation, __u64 irq_generation)
{
	return (status & (wanted | HYPER_IO_MAILBOX_CLOSED)) || armed_generation != irq_generation;
}
#define HYPER_IO_BIND _IOW('H', 0x40, struct hyper_io_eventfds)
#define HYPER_IO_UNBIND _IO('H', 0x41)
#define HYPER_MEMORY_PREPARE _IOW('H', 0x43, struct hyper_memory_prepare)
#define HYPER_IO_ADMIT _IOWR('H', 0x46, struct hyper_io_admission)
#define HYPER_IO_EXTENT _IOWR('H', 0x47, struct hyper_io_extent)
#define HYPER_IO_QUIESCENT _IOW('H', 0x45, __u64)
#define HYPER_MEMORY_RELEASE _IO('H', 0x44)
#define HYPER_MEMORY_INFO _IOR('H', 0x42, struct hyper_memory_info)
#endif
