// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "backend.h"
#include <endian.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <linux/sockios.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
int quiesce_network(struct backend *b)
{
	/* RESET_OWNER stops both RX/TX producers and flushes outstanding work,
	 * including zerocopy callbacks should that ever be enabled. It also
	 * handles activation which attached only one of the two queues. */
	if (b->net.attached) {
		if (ioctl(b->net.fd, VHOST_RESET_OWNER, NULL)) {
			backend_error(b, "/dev/vhost-net", "RESET_OWNER", -1);
			return -1;
		}
		b->net.attached = 0;
	}
	if (endpoint_close(&b->net))
		return -1;
	if (b->tap >= 0) {
		/* Nonpersistent TAP is removed from its bridge. */
		close(b->tap);
		b->tap = -1;
	}
	return 0;
}
/* TAP owns the modern, little-endian virtio header in both directions. The
 * guest TX checksum/GSO metadata reaches Linux intact, even when the physical
 * uplink needs software segmentation. TAP offloads describe the opposite
 * direction (Linux -> guest): leave them disabled because guest RX offloads
 * are not offered. MTU and receive buffer requirements therefore stay fixed. */
static int open_tap(struct backend *b, struct ifreq *tap)
{
	b->tap = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
	if (b->tap < 0) {
		backend_error(b, "/dev/net/tun", "open", -1);
		return -1;
	}
	*tap = (struct ifreq){.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR};
	memcpy(tap->ifr_name, "htap%d", 7);
	if (ioctl(b->tap, TUNSETIFF, tap)) {
		backend_error(b, "/dev/net/tun", "TUNSETIFF", -1);
		return -1;
	}
	int header_size = sizeof(struct virtio_net_hdr_v1);
	int little_endian = 1;
	if (ioctl(b->tap, TUNSETVNETHDRSZ, &header_size) ||
	    ioctl(b->tap, TUNSETVNETLE, &little_endian) ||
	    ioctl(b->tap, TUNSETOFFLOAD, 0UL)) {
		backend_error(b, tap->ifr_name, "virtio header configuration", -1);
		return -1;
	}
	return 0;
}

int probe_network(struct backend *b)
{
	/* Verify the complete TAP contract before HELLO advertises offloads. The
	 * probe is never attached to a bridge and has no packet producers. */
	struct ifreq tap;
	int result = open_tap(b, &tap);
	if (b->tap >= 0) {
		close(b->tap);
		b->tap = -1;
	}
	return result;
}

static int create_tap(struct backend *b)
{
	struct ifreq tap;
	if (open_tap(b, &tap))
		return -1;
	unsigned index = if_nametoindex(tap.ifr_name);
	if (!index) {
		backend_error(b, tap.ifr_name, "if_nametoindex", -1);
		return -1;
	}
	int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (socket_fd < 0) {
		backend_error(b, tap.ifr_name, "control socket", -1);
		return -1;
	}
	struct ifreq bridge = {.ifr_ifindex = (int)index};
	memcpy(bridge.ifr_name, b->bridge, sizeof(bridge.ifr_name));
	int result = ioctl(socket_fd, SIOCBRADDIF, &bridge);
	if (result) {
		backend_error(b, b->bridge, "SIOCBRADDIF", -1);
		goto out;
	}
	result = ioctl(socket_fd, SIOCGIFFLAGS, &tap);
	if (result) {
		backend_error(b, tap.ifr_name, "SIOCGIFFLAGS", -1);
		goto out;
	}
	tap.ifr_flags |= IFF_UP;
	result = ioctl(socket_fd, SIOCSIFFLAGS, &tap);
	if (result)
		backend_error(b, tap.ifr_name, "SIOCSIFFLAGS", -1);
out:
	close(socket_fd);
	return result;
}
int activate_network(struct backend *b,
		     const struct hyper_io_network_activate *request)
{
	uint64_t features = le64toh(request->features);
	if (!b->network)
		return HYPER_IO_UNSUPPORTED;
	if (!(features & VERSION_1) || (features & ~NETWORK_FEATURES) ||
	    ((features & NETWORK_TSO) && !(features & NETWORK_CSUM)))
		return HYPER_IO_INVALID;
	if (b->net.fd >= 0 || b->net.bound || b->tap >= 0)
		return HYPER_IO_BUSY;
	/* CSUM/TSO are TAP capabilities, not VHOST_SET_FEATURES bits. In
	 * particular, VHOST_NET_F_VIRTIO_NET_HDR must stay clear: asking vhost
	 * to consume the header would discard the guest's offload metadata. */
	uint64_t backend_features = VERSION_1;
	int status = configure_vhost(
	    b, &b->net, "/dev/vhost-net",
	    (uint32_t)le64toh(request->header.epoch), backend_features,
	    request->queues, HYPER_IO_NETWORK_QUEUES, HYPER_IO_NETWORK_QUEUES);
	if (status == HYPER_IO_BUSY || status == HYPER_IO_INVALID)
		return status;
	if (status != HYPER_IO_OK || create_tap(b))
		goto failed;
	for (unsigned i = 0; i < HYPER_IO_NETWORK_QUEUES; ++i) {
		struct vhost_vring_file backend = {.index = i, .fd = b->tap};
		if (ioctl(b->net.fd, VHOST_NET_SET_BACKEND, &backend)) {
			backend_error(b, "/dev/vhost-net", "NET_SET_BACKEND",
				      i);
			goto failed;
		}
		b->net.attached = 1;
	}
	return HYPER_IO_OK;
failed:
	return quiesce_network(b) ? HYPER_IO_QUIESCENCE_FAILED
				  : HYPER_IO_BACKEND_FAILURE;
}
