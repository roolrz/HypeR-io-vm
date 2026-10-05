// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "backend.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

void backend_error(struct backend *b, const char *device, const char *operation,
		   int queue)
{
	int error = errno;
	if (queue >= 0)
		diagnostic(b, "%s %s queue %d failed: errno %d (%s)", device,
			   operation, queue, error, strerror(error));
	else
		diagnostic(b, "%s %s failed: errno %d (%s)", device, operation,
			   error, strerror(error));
	errno = error;
}
static int vhost_ioctl(struct backend *b, struct endpoint *endpoint,
		       const char *device, unsigned long command,
		       const char *name, void *argument, int queue)
{
	if (!ioctl(endpoint->fd, command, argument))
		return 0;
	backend_error(b, device, name, queue);
	return -1;
}
void endpoint_init(struct endpoint *endpoint)
{
	endpoint->notification = endpoint->fd = -1;
	for (unsigned i = 0; i < HYPER_IO_QUEUES; ++i)
		endpoint->kick[i] = endpoint->call[i] = -1;
}
int endpoint_close(struct endpoint *endpoint)
{
	/* Caller has drained the device. Remove producers before callbacks. */
	if (endpoint->fd >= 0) {
		close(endpoint->fd);
		endpoint->fd = -1;
	}
	if (endpoint->bound) {
		if (ioctl(endpoint->notification, HYPER_IO_UNBIND))
			return -1;
		endpoint->bound = 0;
	}
	for (unsigned i = 0; i < HYPER_IO_QUEUES; ++i) {
		if (endpoint->kick[i] >= 0)
			close(endpoint->kick[i]);
		if (endpoint->call[i] >= 0)
			close(endpoint->call[i]);
		endpoint->kick[i] = endpoint->call[i] = -1;
	}
	return 0;
}
int quiesce_disk(struct backend *b)
{
	/* CLEAR_ENDPOINT synchronously drains SCSI commands, unlike stopping
	 * KICK or reading a queue index. Retain everything if drain fails. */
	if (b->disk.attached) {
		if (ioctl(b->disk.fd, VHOST_SCSI_CLEAR_ENDPOINT, &b->target))
			return -1;
		b->disk.attached = 0;
	}
	return endpoint_close(&b->disk);
}
int quiesce_all(struct backend *b)
{
	/* Try both drains even if one fails, but never release shared RAM until
	 * both succeeded. A device reset calls only its own drain. */
	int disk = quiesce_disk(b);
	int network = quiesce_network(b);
	return disk || network ? -1 : 0;
}
static int queue_address(struct backend *b, uint64_t gpa, uint64_t bytes,
			 uint64_t alignment, uint64_t *result)
{
	if (gpa < b->region.guest_base || gpa % alignment ||
	    bytes > b->region.length ||
	    gpa - b->region.guest_base > b->region.length - bytes)
		return -1;
	*result = (uintptr_t)b->memory + (gpa - b->region.guest_base);
	return 0;
}
int configure_vhost(struct backend *b, struct endpoint *endpoint,
		    const char *device, uint32_t epoch, uint64_t features,
		    const struct hyper_io_queue *queues, unsigned count,
		    unsigned required)
{
	struct {
		struct vhost_memory header;
		struct vhost_memory_region region;
	} table = {
	    .header = {.nregions = 1},
	    .region = {.guest_phys_addr = b->region.guest_base,
		       .memory_size = b->region.length,
		       .userspace_addr = (uintptr_t)b->memory},
	};
	struct hyper_io_eventfds bridge = {
	    .epoch = epoch,
	    .kick = {-1, -1, -1, -1, -1, -1},
	    .call = {-1, -1, -1, -1, -1, -1},
	};
	if (endpoint->fd >= 0 || endpoint->bound || endpoint->attached)
		return HYPER_IO_BUSY;
	if (b->memory == MAP_FAILED || !b->region.length ||
	    endpoint->notification < 0 || !epoch || count > HYPER_IO_QUEUES)
		return HYPER_IO_INVALID;
	uint64_t starts[HYPER_IO_QUEUES * 3] = {0};
	uint64_t ends[HYPER_IO_QUEUES * 3] = {0};
	for (unsigned i = 0; i < count; ++i) {
		uint32_t size = le32toh(queues[i].size);
		uint64_t addresses[3] = {le64toh(queues[i].descriptor),
					 le64toh(queues[i].available),
					 le64toh(queues[i].used)};
		uint64_t lengths[3] = {16ULL * size, 6 + 2ULL * size,
				       6 + 8ULL * size};
		const unsigned alignments[3] = {16, 2, 4};
		if (!size && i >= required && !queues[i].reserved &&
		    !addresses[0] && !addresses[1] && !addresses[2])
			continue;
		if (!size || size > 128 || (size & (size - 1)) ||
		    queues[i].reserved)
			return HYPER_IO_INVALID;
		for (unsigned part = 0; part < 3; ++part) {
			unsigned index = i * 3 + part;
			if (queue_address(b, addresses[part], lengths[part],
					  alignments[part], &starts[index]))
				return HYPER_IO_INVALID;
			ends[index] = starts[index] + lengths[part];
			for (unsigned old = 0; old < index; ++old)
				if (starts[index] < ends[old] &&
				    starts[old] < ends[index])
					return HYPER_IO_INVALID;
		}
	}
	endpoint->fd = open(device, O_RDWR | O_CLOEXEC);
	if (endpoint->fd < 0) {
		backend_error(b, device, "open", -1);
		return HYPER_IO_BACKEND_FAILURE;
	}
	if (vhost_ioctl(b, endpoint, device, VHOST_SET_OWNER, "SET_OWNER", NULL,
			-1) ||
	    vhost_ioctl(b, endpoint, device, VHOST_SET_FEATURES, "SET_FEATURES",
			&features, -1) ||
	    vhost_ioctl(b, endpoint, device, VHOST_SET_MEM_TABLE,
			"SET_MEM_TABLE", &table, -1))
		return HYPER_IO_BACKEND_FAILURE;
	for (unsigned i = 0; i < count; ++i) {
		if (!queues[i].size)
			continue;
		struct vhost_vring_state state = {
		    .index = i, .num = le32toh(queues[i].size)};
		struct vhost_vring_addr address = {
		    .index = i,
		    .desc_user_addr = starts[i * 3],
		    .avail_user_addr = starts[i * 3 + 1],
		    .used_user_addr = starts[i * 3 + 2]};
		struct vhost_vring_file event = {.index = i};
		if (vhost_ioctl(b, endpoint, device, VHOST_SET_VRING_NUM,
				"SET_VRING_NUM", &state, i) ||
		    vhost_ioctl(b, endpoint, device, VHOST_SET_VRING_ADDR,
				"SET_VRING_ADDR", &address, i))
			return HYPER_IO_BACKEND_FAILURE;
		state.num = 0;
		if (vhost_ioctl(b, endpoint, device, VHOST_SET_VRING_BASE,
				"SET_VRING_BASE", &state, i))
			return HYPER_IO_BACKEND_FAILURE;
		endpoint->kick[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (endpoint->kick[i] < 0) {
			backend_error(b, device, "kick eventfd", i);
			return HYPER_IO_BACKEND_FAILURE;
		}
		endpoint->call[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (endpoint->call[i] < 0) {
			backend_error(b, device, "call eventfd", i);
			return HYPER_IO_BACKEND_FAILURE;
		}
		event.fd = endpoint->kick[i];
		if (vhost_ioctl(b, endpoint, device, VHOST_SET_VRING_KICK,
				"SET_VRING_KICK", &event, i))
			return HYPER_IO_BACKEND_FAILURE;
		event.fd = endpoint->call[i];
		if (vhost_ioctl(b, endpoint, device, VHOST_SET_VRING_CALL,
				"SET_VRING_CALL", &event, i))
			return HYPER_IO_BACKEND_FAILURE;
		bridge.kick[i] = endpoint->kick[i];
		bridge.call[i] = endpoint->call[i];
	}
	if (ioctl(endpoint->notification, HYPER_IO_BIND, &bridge)) {
		backend_error(b, device, "HYPER_IO_BIND", -1);
		return HYPER_IO_BACKEND_FAILURE;
	}
	endpoint->bound = 1;
	return HYPER_IO_OK;
}
int activate_disk(struct backend *b, const struct hyper_io_activate *message)
{
	if (!b->target.vhost_wwpn[0])
		return HYPER_IO_UNSUPPORTED;
	if (le64toh(message->features) != VERSION_1)
		return HYPER_IO_INVALID;
	int status =
	    configure_vhost(b, &b->disk, "/dev/vhost-scsi",
			    (uint32_t)le64toh(message->header.epoch), VERSION_1,
			    message->queues, HYPER_IO_QUEUES, 3);
	if (status == HYPER_IO_BUSY || status == HYPER_IO_INVALID)
		return status;
	if (status == HYPER_IO_OK &&
	    !ioctl(b->disk.fd, VHOST_SCSI_SET_ENDPOINT, &b->target)) {
		b->disk.attached = 1;
		return HYPER_IO_OK;
	}
	return quiesce_disk(b) ? HYPER_IO_QUIESCENCE_FAILED
			       : HYPER_IO_BACKEND_FAILURE;
}
