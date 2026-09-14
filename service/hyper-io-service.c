// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/vhost.h>
#include <stdint.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "hyper_io.h"
#include "hyper_io_session.h"

#define VERSION_1 (UINT64_C(1) << 32)
_Static_assert(sizeof(struct hyper_io_header) == 40, "bridge header layout");
_Static_assert(sizeof(struct hyper_io_activate) == 144, "activation layout");
_Static_assert(sizeof(struct hyper_io_reply) == 64, "reply layout");
_Static_assert(sizeof(struct hyper_io_prepare_memory) == 56, "prepare layout");
static volatile sig_atomic_t stopping;
static void stop_signal(int signal) { (void)signal; stopping = 1; }
struct backend {
	int notification, memory_fd, fd;
	int kick[3], call[3];
	void *memory;
	struct hyper_memory_info region;
	struct vhost_scsi_target target;
	int attached, bound;
	int managed;
	const char *memory_path;
};
static int quiesce(struct backend *b)
{
	/* CLEAR_ENDPOINT clears all queue backends and flushes in-flight commands.
	 * Never replace this with KICK=-1 or GET_VRING_BASE: neither drains SCSI. */
	if (b->attached) {
		if (ioctl(b->fd, VHOST_SCSI_CLEAR_ENDPOINT, &b->target))
			return -1;
		b->attached = 0;
	}
	if (b->fd >= 0) {
		close(b->fd);
		b->fd = -1;
	}
	/* All vhost producers are gone before callbacks and their epoch disappear. */
	if (b->bound) {
		if (ioctl(b->notification, HYPER_IO_UNBIND))
			return -1;
		b->bound = 0;
	}
	for (unsigned int i = 0; i < 3; ++i) {
		if (b->kick[i] >= 0) close(b->kick[i]);
		if (b->call[i] >= 0) close(b->call[i]);
		b->kick[i] = b->call[i] = -1;
	}
	return 0;
}
static int release_memory(struct backend *b)
{
	if (quiesce(b)) return HYPER_IO_QUIESCENCE_FAILED;
	if (b->memory != MAP_FAILED) {
		if (munmap(b->memory, b->region.length)) return HYPER_IO_QUIESCENCE_FAILED;
		b->memory = MAP_FAILED;
	}
	if (b->memory_fd >= 0) { close(b->memory_fd); b->memory_fd = -1; }
	memset(&b->region, 0, sizeof(b->region));
	return HYPER_IO_OK;
}
static int prepare_memory(struct backend *b, const struct hyper_io_prepare_memory *request)
{
	uint64_t base = le64toh(request->guest_base), length = le64toh(request->length);
	struct hyper_memory_info window;
	if (!b->managed || b->memory != MAP_FAILED || b->memory_fd >= 0) return HYPER_IO_BUSY;
	if (!length || length % 4096 || base % 4096 || base > UINT64_MAX - length ||
	    (uint64_t)(size_t)length != length) return HYPER_IO_INVALID;
	int fd = open(b->memory_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) return HYPER_IO_BACKEND_FAILURE;
	if (ioctl(fd, HYPER_MEMORY_INFO, &window) || length > window.length) {
		close(fd); return HYPER_IO_INVALID;
	}
	/* Host has already installed precisely this extent in the alias window.
	 * The unused address-space reservation is never mapped or touched here. */
	void *memory = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (memory == MAP_FAILED) { close(fd); return HYPER_IO_BACKEND_FAILURE; }
	b->memory = memory; b->memory_fd = fd;
	b->region.guest_base = base; b->region.length = length;
	return HYPER_IO_OK;
}
static int queue_address(struct backend *b, uint64_t gpa, uint64_t bytes,
			 uint64_t alignment, uint64_t *result)
{
	if (gpa < b->region.guest_base || gpa % alignment ||
	    bytes > b->region.length || gpa - b->region.guest_base > b->region.length - bytes)
		return -1;
	*result = (uintptr_t)b->memory + (gpa - b->region.guest_base);
	return 0;
}
static int activate(struct backend *b, const struct hyper_io_activate *message, uint64_t offered)
{
	uint64_t features = le64toh(message->features);
	struct { struct vhost_memory header; struct vhost_memory_region region; } table = {
		.header = { .nregions = 1 },
		.region = { .guest_phys_addr = b->region.guest_base,
			.memory_size = b->region.length, .userspace_addr = (uintptr_t)b->memory },
	};
	struct hyper_io_eventfds bridge = { .epoch = le64toh(message->header.epoch) };
	if (b->fd >= 0 || b->bound || b->attached) return HYPER_IO_BUSY;
	/* Standby owns no client RAM or notification endpoint. Never configure
	 * vhost until an explicitly provisioned client exists. */
	if (b->memory == MAP_FAILED || !b->region.length || b->notification < 0)
		return HYPER_IO_INVALID;
	if (features != VERSION_1 || (features & ~offered) || !bridge.epoch)
		return HYPER_IO_INVALID;
	/* Validate every queue extent and all overlap before opening a backend. */
	uint64_t starts[9], ends[9];
	for (unsigned int i = 0; i < 3; ++i) {
		uint32_t size = le32toh(message->queues[i].size);
		uint64_t addresses[3] = {le64toh(message->queues[i].descriptor),
			le64toh(message->queues[i].available), le64toh(message->queues[i].used)};
		uint64_t lengths[3] = {16ULL * size, 6 + 2ULL * size, 6 + 8ULL * size};
		const unsigned int alignments[3] = {16, 2, 4};
		if (!size || size > 128 || (size & (size - 1)) || message->queues[i].reserved)
			return HYPER_IO_INVALID;
		for (unsigned int part = 0; part < 3; ++part) {
			unsigned int index = i * 3 + part;
			if (queue_address(b, addresses[part], lengths[part], alignments[part], &starts[index]))
				return HYPER_IO_INVALID;
			ends[index] = starts[index] + lengths[part];
			for (unsigned int old = 0; old < index; ++old)
				if (starts[index] < ends[old] && starts[old] < ends[index])
					return HYPER_IO_INVALID;
		}
	}
	b->fd = open("/dev/vhost-scsi", O_RDWR | O_CLOEXEC);
	if (b->fd < 0 || ioctl(b->fd, VHOST_SET_OWNER, NULL) ||
	    ioctl(b->fd, VHOST_SET_FEATURES, &features) || ioctl(b->fd, VHOST_SET_MEM_TABLE, &table))
		goto fail;
	for (unsigned int i = 0; i < 3; ++i) {
		struct vhost_vring_state state = {.index = i, .num = le32toh(message->queues[i].size)};
		struct vhost_vring_addr address = {.index = i, .desc_user_addr = starts[i * 3],
			.avail_user_addr = starts[i * 3 + 1], .used_user_addr = starts[i * 3 + 2]};
		struct vhost_vring_file event = {.index = i};
		if (ioctl(b->fd, VHOST_SET_VRING_NUM, &state) ||
		    ioctl(b->fd, VHOST_SET_VRING_ADDR, &address)) goto fail;
		state.num = 0;
		if (ioctl(b->fd, VHOST_SET_VRING_BASE, &state)) goto fail;
		b->kick[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		b->call[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (b->kick[i] < 0 || b->call[i] < 0) goto fail;
		event.fd = b->kick[i];
		if (ioctl(b->fd, VHOST_SET_VRING_KICK, &event)) goto fail;
		event.fd = b->call[i];
		if (ioctl(b->fd, VHOST_SET_VRING_CALL, &event)) goto fail;
		bridge.kick[i] = b->kick[i]; bridge.call[i] = b->call[i];
	}
	if (ioctl(b->notification, HYPER_IO_BIND, &bridge)) goto fail;
	b->bound = 1;
	if (ioctl(b->fd, VHOST_SCSI_SET_ENDPOINT, &b->target)) goto fail;
	b->attached = 1;
	return HYPER_IO_OK;
fail:
	fprintf(stderr, "HypeR I/O: activation failed: %s\n", strerror(errno));
	return quiesce(b) ? HYPER_IO_QUIESCENCE_FAILED : HYPER_IO_BACKEND_FAILURE;
}
static int serve(struct backend *b, int control, uint64_t features)
{
	unsigned char message[256], previous[256];
	struct hyper_io_reply response = {0}, previous_reply = {0};
	struct hyper_io_session session = {0};
	uint64_t last_transaction = 0;
	size_t previous_size = 0, reply_size = 0;
	for (;;) {
		ssize_t size;
		if (stopping) return 0;
		do { size = read(control, message, sizeof(message)); } while (size < 0 && errno == EINTR && !stopping);
		if (stopping) return 0;
		/* Peer close is administrative shutdown; main still drains before exit. */
		if (size < 0 && errno == EPIPE) return 0;
		if (size <= 0) return -1;
		if (size < (ssize_t)sizeof(struct hyper_io_header)) return -1;
		struct hyper_io_header header;
		memcpy(&header, message, sizeof(header));
		uint64_t request_binding = le64toh(header.binding), transaction = le64toh(header.transaction);
		if (!le64toh(header.epoch)) return -1;
		if (le32toh(header.magic) != HYPER_IO_MAGIC || le16toh(header.version) != 1 ||
		    le32toh(header.length) != (uint32_t)size || header.flags || !request_binding || !transaction ||
		    le64toh(header.epoch) > UINT32_MAX) return -1;
		uint16_t operation = le16toh(header.operation);
		int admission = hyper_io_session_admit(&session, request_binding, le64toh(header.epoch),
			operation == HYPER_IO_HELLO && size == 40,
			b->managed ? b->memory == MAP_FAILED : !session.binding);
		if (admission > 0) { last_transaction = 0; previous_size = 0; }
		if (admission >= 0 && transaction == last_transaction && (size_t)size == previous_size &&
		    !memcmp(previous, message, previous_size)) {
			/* Same transaction is replay-only, including failures. Retrying a
			 * failed drain requires a fresh transaction and RESET. */
			response = previous_reply;
		} else {
			uint32_t status = HYPER_IO_INVALID;
			memset(&response, 0, sizeof(response)); response.header = header;
			reply_size = 48;
			if (admission >= 0 && transaction > last_transaction) {
				if (operation == HYPER_IO_HELLO && size == 40 && hyper_io_session_preparable(&session)) {
					status = HYPER_IO_OK; reply_size = 64;
					response.features = htole64(features);
					response.queues = htole32(3); response.queue_max = htole32(128);
				} else if (operation == HYPER_IO_PREPARE_MEMORY && size == 56 && b->managed && hyper_io_session_preparable(&session)) {
					struct hyper_io_prepare_memory request; memcpy(&request, message, sizeof(request));
					status = prepare_memory(b, &request);
				} else if (operation == HYPER_IO_RELEASE_MEMORY && size == 40 && b->managed) {
					status = release_memory(b);
					if (status == HYPER_IO_OK) hyper_io_session_retire(&session);
				} else if (operation == HYPER_IO_ACTIVATE && size == 144) {
					struct hyper_io_activate request; memcpy(&request, message, sizeof(request));
					status = activate(b, &request, features);
				} else if (((operation == HYPER_IO_RESET && size == 40) ||
				           (operation == HYPER_IO_STOP_QUEUE && size == 48))) {
					uint32_t queue = 0, reserved = 0;
					if (size == 48) { memcpy(&queue, message + 40, 4); memcpy(&reserved, message + 44, 4); }
					if (le32toh(queue) < 3 && !reserved)
						status = quiesce(b) ? HYPER_IO_QUIESCENCE_FAILED : HYPER_IO_OK;
				}
			}
			response.header.length = htole32(reply_size); response.header.flags = htole32(HYPER_IO_REPLY);
			response.status = htole32(status);
			if (admission >= 0 && transaction > last_transaction) {
				if (le64toh(header.epoch) > session.epoch) session.epoch = le64toh(header.epoch);
				last_transaction = transaction; previous_size = size;
				memcpy(previous, message, size); previous_reply = response;
			}
		}
		ssize_t written;
		do { written = write(control, &response, le32toh(response.header.length)); }
		while (written < 0 && errno == EINTR && !stopping);
		if (stopping) return 0;
		if (written < 0 && errno == EPIPE) return 0;
		if (written != (ssize_t)le32toh(response.header.length))
			return -1;
	}
}
int main(int argc, char **argv)
{
	struct backend b = {.notification = -1, .memory_fd = -1, .fd = -1,
		.kick = {-1,-1,-1}, .call = {-1,-1,-1}, .memory = MAP_FAILED};
	if ((argc != 3 && argc != 5) || strlen(argv[2]) >= sizeof(b.target.vhost_wwpn)) return 2;
	b.managed = argc == 5; b.memory_path = argv[1];
	struct sigaction action = {.sa_handler = stop_signal};
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL)) return 1;
	memcpy(b.target.vhost_wwpn, argv[2], strlen(argv[2]) + 1);
	int standby = !strcmp(argv[1], "--standby");
	if (!standby && !b.managed) {
		b.memory_fd = open(argv[1], O_RDWR | O_CLOEXEC);
		if (b.memory_fd < 0 || ioctl(b.memory_fd, HYPER_MEMORY_INFO, &b.region) ||
		    !b.region.length || (uint64_t)(size_t)b.region.length != b.region.length) return 1;
		b.memory = mmap(NULL, b.region.length, PROT_READ | PROT_WRITE, MAP_SHARED, b.memory_fd, 0);
		b.notification = open("/dev/hyper-io-notification", O_RDWR | O_CLOEXEC);
		if (b.memory == MAP_FAILED || b.notification < 0) return 1;
	}
	if (b.managed) b.notification = open(argv[4], O_RDWR | O_CLOEXEC);
	if (b.managed && b.notification < 0) return 1;
	int control = open(b.managed ? argv[3] : "/dev/hyper-io-control", O_RDWR | O_CLOEXEC);
	int probe = open("/dev/vhost-scsi", O_RDWR | O_CLOEXEC);
	uint64_t features = 0;
	if (control < 0 || probe < 0 ||
	    ioctl(probe, VHOST_GET_FEATURES, &features) || !(features & VERSION_1)) return 1;
	close(probe); features &= VERSION_1;
	puts(standby ? "HypeR I/O: standby service ready" : "HypeR I/O: backend service ready"); fflush(stdout);
	int result = serve(&b, control, features);
	if (release_memory(&b) != HYPER_IO_OK) {
		/* No successful acknowledgement or unmap is allowed after failed drain.
		 * Keep the process and all resources alive for host-side quarantine. */
		fputs("HypeR I/O: quiescence failed; retaining backend\n", stderr);
		for (;;) pause();
	}
	close(control);
	if (b.notification >= 0) close(b.notification);
	if (b.memory != MAP_FAILED) munmap(b.memory, b.region.length);
	if (b.memory_fd >= 0) close(b.memory_fd);
	return result ? 1 : 0;
}
