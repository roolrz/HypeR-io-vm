// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0

/* Linux-only acceptance: vhost-scsi must read/write a disposable LIO block
 * device using page-backed memory supplied by the external Hyper module. */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/vhost.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_scsi.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MEMORY_SIZE (16 * 4096)
#define QUEUES 6
#define GUEST_BASE UINT64_C(0x10000000)
#define QUEUE_SIZE 8
#define REQUEST_OFFSET (6 * 4096)
#define RESPONSE_OFFSET (7 * 4096)
#define DATA_OFFSET (8 * 4096)

struct backend {
	int fd;
	int memory_fd;
	int kick[QUEUES];
	int call[QUEUES];
	unsigned char *memory;
	struct vring ring[QUEUES];
	uint16_t next[QUEUES];
	unsigned int selected;
};

static void cleanup(struct backend *backend)
{
	/* close(vhost) synchronously releases the backend and flushes in-flight
	 * commands before the mappings or notification descriptors disappear. */
	if (backend->fd >= 0)
		close(backend->fd);
	for (unsigned int i = 0; i < QUEUES; ++i) {
		if (backend->kick[i] >= 0)
			close(backend->kick[i]);
		if (backend->call[i] >= 0)
			close(backend->call[i]);
	}
	if (backend->memory != MAP_FAILED)
		munmap(backend->memory, MEMORY_SIZE);
	if (backend->memory_fd >= 0)
		close(backend->memory_fd);
}

static int checked_ioctl(int fd, unsigned long request, void *argument)
{
	if (ioctl(fd, request, argument) == 0)
		return 0;
	fprintf(stderr, "ioctl %#lx: %s\n", request, strerror(errno));
	return -1;
}

static int prepare(struct backend *backend, const char *memory_device, const char *wwpn)
{
	struct {
		struct vhost_memory header;
		struct vhost_memory_region region;
	} table = { .header = { .nregions = 1 } };
	uint64_t features;
	struct vhost_scsi_target target = {0};

	if (sysconf(_SC_PAGESIZE) != 4096 || strlen(wwpn) >= sizeof(target.vhost_wwpn))
		return -1;
	backend->memory_fd = open(memory_device, O_RDWR | O_CLOEXEC);
	if (backend->memory_fd < 0)
		return -1;
	backend->memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
			       backend->memory_fd, 0);
	if (backend->memory == MAP_FAILED)
		return -1;
	/* mmap success alone is insufficient: each request below forces vhost
	 * to acquire normal page references for a DMA-capable block scatterlist. */
	memset(backend->memory, 0, MEMORY_SIZE);
	backend->fd = open("/dev/vhost-scsi", O_RDWR | O_CLOEXEC);
	if (backend->fd < 0 || checked_ioctl(backend->fd, VHOST_SET_OWNER, NULL) ||
	    checked_ioctl(backend->fd, VHOST_GET_FEATURES, &features))
		return -1;
	if (!(features & (UINT64_C(1) << VIRTIO_F_VERSION_1)))
		return -1;
	features = UINT64_C(1) << VIRTIO_F_VERSION_1;
	if (checked_ioctl(backend->fd, VHOST_SET_FEATURES, &features))
		return -1;
	table.region.guest_phys_addr = GUEST_BASE;
	table.region.memory_size = MEMORY_SIZE;
	table.region.userspace_addr = (uintptr_t)backend->memory;
	if (checked_ioctl(backend->fd, VHOST_SET_MEM_TABLE, &table))
		return -1;
	for (unsigned int i = 0; i < QUEUES; ++i) {
		struct vhost_vring_state state = { .index = i, .num = QUEUE_SIZE };
		struct vhost_vring_addr address = { .index = i };
		struct vhost_vring_file event = { .index = i };
		vring_init(&backend->ring[i], QUEUE_SIZE, backend->memory + i * 4096, 512);
		address.desc_user_addr = (uintptr_t)backend->ring[i].desc;
		address.avail_user_addr = (uintptr_t)backend->ring[i].avail;
		address.used_user_addr = (uintptr_t)backend->ring[i].used;
		if (checked_ioctl(backend->fd, VHOST_SET_VRING_NUM, &state) ||
		    checked_ioctl(backend->fd, VHOST_SET_VRING_ADDR, &address))
			return -1;
		state.num = 0;
		if (checked_ioctl(backend->fd, VHOST_SET_VRING_BASE, &state))
			return -1;
		backend->kick[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		backend->call[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (backend->kick[i] < 0 || backend->call[i] < 0)
			return -1;
		event.fd = backend->kick[i];
		if (checked_ioctl(backend->fd, VHOST_SET_VRING_KICK, &event))
			return -1;
		event.fd = backend->call[i];
		if (checked_ioctl(backend->fd, VHOST_SET_VRING_CALL, &event))
			return -1;
	}
	memcpy(target.vhost_wwpn, wwpn, strlen(wwpn) + 1);
	return checked_ioctl(backend->fd, VHOST_SCSI_SET_ENDPOINT, &target);
}

static struct vring_desc descriptor(unsigned int offset, unsigned int length,
				    uint16_t flags, uint16_t next)
{
	return (struct vring_desc) {
		.addr = htole64(GUEST_BASE + offset), .len = htole32(length),
		.flags = htole16(flags), .next = htole16(next),
	};
}

static int command(struct backend *backend, const unsigned char *cdb,
		   size_t cdb_length, size_t data_length, int write_data)
{
	struct virtio_scsi_cmd_req *request = (void *)(backend->memory + REQUEST_OFFSET);
	struct virtio_scsi_cmd_resp *response = (void *)(backend->memory + RESPONSE_OFFSET);
	struct vring *ring = &backend->ring[backend->selected];
	uint64_t kick = 1;
	struct pollfd poller = { .fd = backend->call[backend->selected], .events = POLLIN };
	int ready;

	memset(request, 0, sizeof(*request));
	memset(response, 0xff, sizeof(*response));
	request->lun[0] = 1;
	request->lun[1] = 1; /* target ID is the LIO tpgt_1 index; LUN is zero. */
	request->tag = htole64(backend->next[backend->selected] + 1);
	memcpy(request->cdb, cdb, cdb_length);
	ring->desc[0] = descriptor(REQUEST_OFFSET, sizeof(*request), VRING_DESC_F_NEXT, 1);
	if (write_data && data_length) {
		ring->desc[1] = descriptor(DATA_OFFSET, data_length, VRING_DESC_F_NEXT, 2);
		ring->desc[2] = descriptor(RESPONSE_OFFSET, sizeof(*response), VRING_DESC_F_WRITE, 0);
	} else {
		ring->desc[1] = descriptor(RESPONSE_OFFSET, sizeof(*response),
			VRING_DESC_F_WRITE | (data_length ? VRING_DESC_F_NEXT : 0), 2);
		if (data_length)
			ring->desc[2] = descriptor(DATA_OFFSET, data_length, VRING_DESC_F_WRITE, 0);
	}
	ring->avail->ring[backend->next[backend->selected] % QUEUE_SIZE] = 0;
	++backend->next[backend->selected];
	__atomic_store_n(&ring->avail->idx, htole16(backend->next[backend->selected]), __ATOMIC_RELEASE);
	if (write(backend->kick[backend->selected], &kick, sizeof(kick)) != sizeof(kick))
		return -1;
	do {
		ready = poll(&poller, 1, 5000);
	} while (ready < 0 && errno == EINTR);
	if (ready != 1 || !(poller.revents & POLLIN) ||
	    read(backend->call[backend->selected], &kick, sizeof(kick)) != sizeof(kick)) {
		fprintf(stderr, "vhost completion timeout or notification failure\n");
		return -1;
	}
	if (le16toh(__atomic_load_n(&ring->used->idx, __ATOMIC_ACQUIRE)) != backend->next[backend->selected] ||
	    le32toh(ring->used->ring[(backend->next[backend->selected] - 1) % QUEUE_SIZE].id) != 0 ||
	    response->response != VIRTIO_SCSI_S_OK) {
		fprintf(stderr, "invalid vhost completion: transport=%u\n", response->response);
		return -1;
	}
	if (response->status) {
		fprintf(stderr, "SCSI status=%u sense=%u key=%u\n", response->status,
			le32toh(response->sense_len), response->sense[2] & 15);
	}
	return response->status;
}

static int exercise(struct backend *backend)
{
	backend->selected = 2;
	unsigned char *data = backend->memory + DATA_OFFSET;
	const unsigned char inquiry[6] = {0x12, 0, 0, 0, 96, 0};
	const unsigned char write10[10] = {0x2a, 0, 0, 0, 0, 8, 0, 0, 1, 0};
	const unsigned char read10[10] = {0x28, 0, 0, 0, 0, 8, 0, 0, 1, 0};
	const unsigned char flush[10] = {0x35};
	const unsigned char invalid[6] = {0xff};
	if (command(backend, inquiry, sizeof(inquiry), 96, 0) || data[0] != 0)
		return -1;
	for (unsigned int round = 0; round < 32; ++round) {
		backend->selected = 2 + round % (QUEUES - 2);
		for (unsigned int index = 0; index < 512; ++index)
			data[index] = (unsigned char)(index + round * 17);
		int result = command(backend, write10, sizeof(write10), 512, 1);
		if (result == 2) {
			struct virtio_scsi_cmd_resp *response = (void *)(backend->memory + RESPONSE_OFFSET);
			if ((response->sense[2] & 15) == 6)
				result = command(backend, write10, sizeof(write10), 512, 1);
		}
		if (result || command(backend, flush, sizeof(flush), 0, 0))
			return -1;
		memset(data, 0, 512);
		if (command(backend, read10, sizeof(read10), 512, 0))
			return -1;
		for (unsigned int index = 0; index < 512; ++index) {
			if (data[index] != (unsigned char)(index + round * 17)) {
				fprintf(stderr, "readback mismatch at round=%u offset=%u\n", round, index);
				return -1;
			}
		}
	}
	if (command(backend, invalid, sizeof(invalid), 0, 0) != 2)
		return -1;
	puts("HypeR I/O: vhost-scsi reserved-page read/write/flush passed (32 rounds)");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--load-module")) {
		int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return 1;
		int result = syscall(SYS_finit_module, fd, "", 0);
		int error = errno;
		close(fd);
		if (result) {
			char log[32768];
			long length = syscall(SYS_syslog, 3, log, sizeof(log));
			for (long offset = 0; offset < length;) {
				ssize_t written = write(STDERR_FILENO, log + offset,
						(size_t)(length - offset));
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					break;
				offset += written;
			}
			fprintf(stderr, "finit_module: %s\n", strerror(error));
		}
		return result ? 1 : 0;
	}
	struct backend backend = { .fd = -1, .memory_fd = -1,
		.kick = {-1, -1, -1, -1, -1, -1}, .call = {-1, -1, -1, -1, -1, -1}, .memory = MAP_FAILED };
	if (argc != 3) {
		fprintf(stderr, "usage: vhost-scsi-test SHARED_MEMORY_DEVICE TEST_WWPN\n");
		return 2;
	}
	int result = prepare(&backend, argv[1], argv[2]);
	if (!result)
		result = exercise(&backend);
	if (result)
		fprintf(stderr, "vhost-scsi-test failed: %s\n", strerror(errno));
	cleanup(&backend);
	return result ? 1 : 0;
}
