// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Ordinary Linux block I/O, deliberately no vhost or Hyper-specific API. The
 * harness supplies a disposable raw disk and independently checks LBA 8. */
int main(int argc, char **argv)
{
	unsigned char *expected = NULL, *actual = NULL;
	if (argc != 2)
		return 2;
	int fd = open(argv[1], O_RDWR | O_CLOEXEC | O_DSYNC | O_DIRECT);
	if (fd < 0) {
		perror("open business disk");
		return 1;
	}
	if (posix_memalign((void **)&expected, 4096, 4096) ||
	    posix_memalign((void **)&actual, 4096, 4096)) {
		free(expected); free(actual); close(fd); return 1;
	}
	for (unsigned int round = 0; round < 32; ++round) {
		for (unsigned int i = 0; i < 512; ++i)
			expected[i] = (unsigned char)(i + round * 17);
		if (pwrite(fd, expected, 512, 8 * 512) != 512 ||
		    fsync(fd) || pread(fd, actual, 512, 8 * 512) != 512 ||
		    memcmp(expected, actual, 512)) {
			fprintf(stderr, "HypeR business disk: FAIL round=%u errno=%d\n", round, errno);
			free(expected); free(actual); close(fd);
			return 1;
		}
	}
	free(expected); free(actual);
	if (close(fd))
		return 1;
	puts("HypeR business disk: PASS read/write/flush (32 rounds)");
	return 0;
}
