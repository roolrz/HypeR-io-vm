// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "../../service/backend.h"
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>

#include <linux/if_tun.h>
#include <linux/sockios.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Exercise the real activation/rollback code with deterministic syscall
 * failures. Cross-VM acceptance separately checks actual packet contents. */
enum { TAP_FD = 40, CONTROL_FD, VHOST_FD };
static int tap_open, control_open, vhost_open, drains, attaches;
static unsigned long fail_ioctl;
static int fail_attach = -1, configure_status = HYPER_IO_OK;

static int network_open(const char *path, int flags, ...)
{
	assert(!strcmp(path, "/dev/net/tun"));
	assert(flags == (O_RDWR | O_CLOEXEC) && !tap_open);
	tap_open = 1;
	return TAP_FD;
}
static int network_close(int fd)
{
	assert(fd == TAP_FD || fd == CONTROL_FD);
	int *state = fd == TAP_FD ? &tap_open : &control_open;
	assert(*state);
	*state = 0;
	return 0;
}
static int network_socket(int domain, int type, int protocol)
{
	assert(domain == AF_INET && type == (SOCK_DGRAM | SOCK_CLOEXEC));
	assert(!protocol && !control_open);
	control_open = 1;
	return CONTROL_FD;
}
static unsigned network_index(const char *name)
{
	assert(!strcmp(name, "htap-test"));
	return 7;
}
static int network_ioctl(int fd, unsigned long command, ...)
{
	va_list args;
	va_start(args, command);
	if (command == TUNSETOFFLOAD) {
		/* These are guest RX capabilities, not guest TX capabilities.
		 */
		assert(fd == TAP_FD && va_arg(args, unsigned long) == 0);
	} else {
		void *argument = va_arg(args, void *);
		switch (command) {
		case TUNSETIFF: {
			struct ifreq *tap = argument;
			assert(fd == TAP_FD);
			assert(tap->ifr_flags ==
			       (IFF_TAP | IFF_NO_PI | IFF_VNET_HDR));
			strcpy(tap->ifr_name, "htap-test");
			break;
		}
		case TUNSETVNETHDRSZ:
			assert(fd == TAP_FD && *(int *)argument == 12);
			break;
		case TUNSETVNETLE:
			assert(fd == TAP_FD && *(int *)argument == 1);
			break;
		case SIOCBRADDIF: {
			struct ifreq *bridge = argument;
			assert(fd == CONTROL_FD && bridge->ifr_ifindex == 7);
			assert(!strcmp(bridge->ifr_name, "hbr0"));
			break;
		}
		case SIOCGIFFLAGS:
			assert(fd == CONTROL_FD);
			((struct ifreq *)argument)->ifr_flags = 0;
			break;
		case SIOCSIFFLAGS:
			assert(fd == CONTROL_FD);
			assert(((struct ifreq *)argument)->ifr_flags & IFF_UP);
			break;
		case VHOST_NET_SET_BACKEND: {
			struct vhost_vring_file *queue = argument;
			assert(fd == VHOST_FD && queue->fd == TAP_FD);
			assert(queue->index < 2 && tap_open && !control_open);
			++attaches;
			if ((int)queue->index == fail_attach) {
				va_end(args);
				errno = EIO;
				return -1;
			}
			break;
		}
		case VHOST_RESET_OWNER:
			assert(fd == VHOST_FD && vhost_open && tap_open);
			++drains;
			break;
		default:
			assert(!"unexpected network ioctl");
		}
	}
	va_end(args);
	if (command == fail_ioctl) {
		errno = EIO;
		return -1;
	}
	return 0;
}
#define open network_open
#define close network_close
#define socket network_socket
#define ioctl network_ioctl
#define if_nametoindex network_index
#include "../../service/network.c"
#undef open
#undef close
#undef socket
#undef ioctl
#undef if_nametoindex

void backend_error(struct backend *b, const char *path, const char *op,
		   int queue)
{
	(void)b;
	(void)path;
	(void)op;
	(void)queue;
}
int configure_vhost(struct backend *b, struct endpoint *endpoint,
		    const char *path, uint32_t epoch, uint64_t features,
		    const struct hyper_io_queue *queues, unsigned count,
		    unsigned required)
{
	(void)queues;
	assert(endpoint == &b->net && !strcmp(path, "/dev/vhost-net"));
	assert(features == VERSION_1 && epoch == 1);
	assert(count == 2 && required == 2 && !vhost_open);
	if (configure_status == HYPER_IO_INVALID ||
	    configure_status == HYPER_IO_BUSY)
		return configure_status;
	vhost_open = 1;
	endpoint->fd = VHOST_FD;
	endpoint->bound = 1;
	return configure_status;
}
int endpoint_close(struct endpoint *endpoint)
{
	assert(!endpoint->attached);
	vhost_open = 0;
	endpoint->fd = -1;
	endpoint->bound = 0;
	return 0;
}
static struct backend idle(void)
{
	assert(!tap_open && !control_open && !vhost_open);
	drains = attaches = 0;
	struct backend b = {
	    .network = 1, .tap = -1, .net = {.fd = -1}, .disk = {.fd = 99}};
	strcpy(b.bridge, "hbr0");
	return b;
}
static void retired(const struct backend *b)
{
	assert(!tap_open && !control_open && !vhost_open);
	assert(b->tap == -1 && b->net.fd == -1 && !b->net.bound &&
	       !b->net.attached);
	assert(b->disk.fd == 99);
}
int main(void)
{
	struct hyper_io_network_activate request = {
	    .header = {.epoch = htole64(1)},
	    .features = htole64(NETWORK_FEATURES)};
	const unsigned long stages[] = {
	    TUNSETIFF,	 TUNSETVNETHDRSZ, TUNSETVNETLE, TUNSETOFFLOAD,
	    SIOCBRADDIF, SIOCGIFFLAGS,	  SIOCSIFFLAGS};
	for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
		struct backend b = idle();
		fail_ioctl = stages[i];
		assert(activate_network(&b, &request) ==
		       HYPER_IO_BACKEND_FAILURE);
		assert(!attaches && !drains);
		retired(&b);
		if (i < 4) {
			assert(probe_network(&b) == -1);
			retired(&b);
		}
	}
	fail_ioctl = 0;
	struct backend b = idle();
	assert(!probe_network(&b));
	retired(&b);
	for (fail_attach = 0; fail_attach < 2; ++fail_attach) {
		b = idle();
		assert(activate_network(&b, &request) ==
		       HYPER_IO_BACKEND_FAILURE);
		assert(attaches == fail_attach + 1 && drains == fail_attach);
		retired(&b);
	}
	fail_attach = -1;
	b = idle();
	fail_attach = 1;
	fail_ioctl = VHOST_RESET_OWNER;
	assert(activate_network(&b, &request) == HYPER_IO_QUIESCENCE_FAILED);
	assert(b.net.attached && tap_open && vhost_open && !control_open);
	fail_ioctl = 0;
	fail_attach = -1;
	assert(!quiesce_network(&b));
	retired(&b);
	const uint64_t valid[] = {VERSION_1, VERSION_1 | NETWORK_MAC,
				  VERSION_1 | NETWORK_CSUM, NETWORK_FEATURES};
	for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
		b = idle();
		request.features = htole64(valid[i]);
		assert(activate_network(&b, &request) == HYPER_IO_OK);
		assert(attaches == 2 && b.net.attached && tap_open &&
		       vhost_open);
		assert(activate_network(&b, &request) == HYPER_IO_BUSY);
		assert(!quiesce_network(&b) && drains == 1);
		retired(&b);
	}
	const uint64_t invalid[] = {
	    0, NETWORK_CSUM, VERSION_1 | NETWORK_TSO,
	    NETWORK_FEATURES | (UINT64_C(1) << VIRTIO_NET_F_GUEST_CSUM)};
	for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
		b = idle();
		request.features = htole64(invalid[i]);
		assert(activate_network(&b, &request) == HYPER_IO_INVALID);
		retired(&b);
	}
	request.features = htole64(NETWORK_FEATURES);
	const int failures[] = {HYPER_IO_INVALID, HYPER_IO_BUSY,
				HYPER_IO_BACKEND_FAILURE};
	for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
		b = idle();
		configure_status = failures[i];
		assert(activate_network(&b, &request) == configure_status);
		retired(&b);
	}
	puts("HypeR network activation and rollback tests: PASS");
	return 0;
}
