// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "backend.h"
#include "hyper_io_layout.h"
#include <endian.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int release_memory(struct backend *b)
{
	if (quiesce_all(b))
		return HYPER_IO_QUIESCENCE_FAILED;
	if (b->memory != MAP_FAILED) {
		if (munmap(b->memory, b->region.length))
			return HYPER_IO_QUIESCENCE_FAILED;
		b->memory = MAP_FAILED;
	}
	if (b->memory_fd >= 0) {
		if (b->managed && ioctl(b->memory_fd, HYPER_MEMORY_RELEASE))
			return HYPER_IO_QUIESCENCE_FAILED;
		close(b->memory_fd);
		b->memory_fd = -1;
	}
	/* Also retire an admission that failed before mmap: no grant users
	 * exist, but the host still needs an authenticated proof to revoke its
	 * mapping. */
	if (b->token && ioctl(memory_endpoint(b)->notification,
			      HYPER_IO_QUIESCENT, &b->token))
		return HYPER_IO_QUIESCENCE_FAILED;
	b->token = 0;
	memset(&b->region, 0, sizeof(b->region));
	return HYPER_IO_OK;
}
int prepare_memory(struct backend *b,
		   const struct hyper_io_prepare_memory *request)
{
	uint64_t base = le64toh(request->guest_base),
		 length = le64toh(request->length);
	struct hyper_memory_prepare prepare = {.alias = le64toh(request->alias),
					       .guest_base = base,
					       .length = length,
					       .token =
						   le64toh(request->token)};
	struct hyper_memory_info window;
	if (!b->managed || b->memory != MAP_FAILED || b->memory_fd >= 0 ||
	    b->token)
		return HYPER_IO_BUSY;
	if (!length || length % 4096 || base % 4096 ||
	    base > UINT64_MAX - length || (uint64_t)(size_t)length != length)
		return HYPER_IO_INVALID;
	uint64_t *pages = NULL;
	if (prepare.token) {
		struct hyper_io_admission admission = {.token = prepare.token,
						       .guest_base = base,
						       .length = length};
		uint64_t cursor = 0;
		if (prepare.alias || length / 4096 > HYPER_IO_MAX_GRANT_PAGES)
			return HYPER_IO_INVALID;
		if (ioctl(memory_endpoint(b)->notification, HYPER_IO_ADMIT,
			  &admission))
			return HYPER_IO_INVALID;
		b->token = prepare.token;
		pages = calloc(length / 4096, sizeof(*pages));
		if (!pages)
			goto failed;
		for (uint64_t i = 0; i < admission.extent_count; ++i) {
			struct hyper_io_extent extent = {.index = i};
			if (ioctl(memory_endpoint(b)->notification,
				  HYPER_IO_EXTENT, &extent) ||
			    !hyper_io_extent_valid(cursor, length, extent.alias,
						   extent.offset,
						   extent.length))
				goto failed;
			for (uint64_t at = 0; at < extent.length; at += 4096)
				pages[(cursor + at) / 4096] = extent.alias + at;
			cursor += extent.length;
		}
		if (cursor != length)
			goto failed;
		prepare.pages = (uintptr_t)pages;
		prepare.count = length / 4096;
	}
	diagnostic(b, "opening shared memory: bytes=%llu",
		   (unsigned long long)length);
	int fd = open(b->memory_path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		goto failed;
	b->memory_fd = fd;
	diagnostic(b, "preparing shared memory: mode=%s, token=%llu",
		   prepare.token ? "page grant" : "alias window",
		   (unsigned long long)prepare.token);
	if (ioctl(fd, HYPER_MEMORY_INFO, &window) || length > window.length ||
	    ioctl(fd, HYPER_MEMORY_PREPARE, &prepare))
		goto failed;
	free(pages);
	pages = NULL;
	/* Host has already installed precisely these pages in the alias window.
	 * The unused address-space reservation is never mapped or touched here.
	 */
	diagnostic(b, "mapping shared memory: bytes=%llu",
		   (unsigned long long)length);
	void *memory =
	    mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (memory == MAP_FAILED)
		return release_memory(b) == HYPER_IO_OK
			   ? HYPER_IO_BACKEND_FAILURE
			   : HYPER_IO_QUIESCENCE_FAILED;
	diagnostic(b, "shared memory ready: bytes=%llu",
		   (unsigned long long)length);
	b->memory = memory;
	b->memory_fd = fd;
	b->token = prepare.token;
	b->region.guest_base = base;
	b->region.length = length;
	return HYPER_IO_OK;
failed:
	free(pages);
	return release_memory(b) == HYPER_IO_OK ? HYPER_IO_BACKEND_FAILURE
						: HYPER_IO_QUIESCENCE_FAILED;
}
