// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "../../service/backend.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int fail_disk, fail_network, disk_drains, net_drains, memory_releases,
    proofs;
static int expected_proof_fd = 201;
static int retirement_ioctl(int fd, unsigned long command, ...)
{
	(void)fd;
	if (command == VHOST_SCSI_CLEAR_ENDPOINT) {
		++disk_drains;
		if (fail_disk) {
			errno = EIO;
			return -1;
		}
	} else if (command == VHOST_RESET_OWNER) {
		++net_drains;
		if (fail_network) {
			errno = EIO;
			return -1;
		}
	} else if (command == HYPER_MEMORY_RELEASE) {
		assert(!fail_disk && !fail_network);
		++memory_releases;
	} else if (command == HYPER_IO_QUIESCENT) {
		assert(!fail_disk && !fail_network && memory_releases);
		assert(fd == expected_proof_fd);
		++proofs;
	} else if (command != HYPER_IO_UNBIND) {
		assert(!"unexpected retirement ioctl");
	}
	return 0;
}
#define ioctl retirement_ioctl
#include "../../service/memory.c"
#include "../../service/network.c"
#include "../../service/vhost.c"
#undef ioctl
void diagnostic(struct backend *backend, const char *format, ...)
{
	(void)backend;
	(void)format;
}
static int dummy_fd(void)
{
	int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	assert(fd >= 0);
	return fd;
}
static struct backend active(void)
{
	struct backend b = {.managed = 1,
			    .network = 1,
			    .tap = -1,
			    .token = 7,
			    .memory_fd = dummy_fd(),
			    .region = {.length = 4096}};
	endpoint_init(&b.disk);
	endpoint_init(&b.net);
	b.memory = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(b.memory != MAP_FAILED);
	b.target.vhost_wwpn[0] = 'x';
	b.disk.fd = dummy_fd();
	b.net.fd = dummy_fd();
	b.disk.attached = b.net.attached = 1;
	b.disk.notification = 201;
	b.net.notification = 202;
	return b;
}
int main(void)
{
	struct backend b = active();
	void *memory = b.memory;
	fail_disk = 1;
	assert(release_memory(&b) == HYPER_IO_QUIESCENCE_FAILED);
	assert(disk_drains == 1 && net_drains == 1);
	assert(b.disk.attached && !b.net.attached && b.net.fd == -1);
	assert(b.memory == memory && b.memory_fd >= 0 && b.token == 7);
	assert(!memory_releases && !proofs);
	fail_disk = 0;
	assert(release_memory(&b) == HYPER_IO_OK);
	assert(b.memory == MAP_FAILED && b.memory_fd == -1 && !b.token);
	assert(memory_releases == 1 && proofs == 1);

	b = active();
	memory = b.memory;
	int disk_fd = b.disk.fd;
	assert(!quiesce_network(&b));
	assert(b.disk.fd == disk_fd && b.disk.attached && b.memory == memory);
	assert(!release_memory(&b));

	b = active();
	fail_network = 1;
	memory = b.memory;
	unsigned before = proofs;
	assert(release_memory(&b) == HYPER_IO_QUIESCENCE_FAILED);
	assert(!b.disk.attached && b.net.attached && b.memory == memory);
	assert((unsigned)proofs == before);
	fail_network = 0;
	assert(!release_memory(&b));
	assert((unsigned)proofs == before + 1);
	/* A net-only client uses its own route for the same shared-memory
	 * proof. */
	b = active();
	close(b.disk.fd);
	b.disk.fd = -1;
	b.disk.attached = 0;
	b.target.vhost_wwpn[0] = 0;
	expected_proof_fd = 202;
	assert(!release_memory(&b));
	return 0;
}
