// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "hyper_network.h"
#include <linux/sockios.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
static int interface_up(int socket_fd, const char *name)
{
	struct ifreq request = {0};
	memcpy(request.ifr_name, name, strlen(name) + 1);
	if (ioctl(socket_fd, SIOCGIFFLAGS, &request))
		return -1;
	request.ifr_flags |= IFF_UP;
	return ioctl(socket_fd, SIOCSIFFLAGS, &request);
}
int main(int argc, char **argv)
{
	struct hyper_network networks[HYPER_MAX_NETWORKS];
	unsigned count;
	if (argc != 2 || hyper_networks_read(argv[1], networks, &count))
		return 1;
	int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (socket_fd < 0)
		return 1;
	/* Validate the entire inventory before changing links. Existing bridges
	 * are never adopted; failures abort appliance startup. No IP is set. */
	for (unsigned i = 0; i < count; ++i) {
		struct ifreq uplink = {0};
		memcpy(uplink.ifr_name, networks[i].uplink,
		       strlen(networks[i].uplink) + 1);
		if (!if_nametoindex(networks[i].uplink) ||
		    if_nametoindex(networks[i].bridge) ||
		    ioctl(socket_fd, SIOCGIFMTU, &uplink) ||
		    uplink.ifr_mtu != 1500) {
			close(socket_fd);
			return 1;
		}
	}
	for (unsigned i = 0; i < count; ++i) {
		struct hyper_network *n = &networks[i];
		struct ifreq request = {.ifr_ifindex =
					    (int)if_nametoindex(n->uplink)};
		memcpy(request.ifr_name, n->bridge, strlen(n->bridge) + 1);
		if (ioctl(socket_fd, SIOCBRADDBR, n->bridge) ||
		    ioctl(socket_fd, SIOCBRADDIF, &request) ||
		    interface_up(socket_fd, n->uplink) ||
		    interface_up(socket_fd, n->bridge)) {
			perror("network bridge");
			close(socket_fd);
			return 1;
		}
		printf("HypeR I/O: network %s ready on %s\n", n->name,
		       n->uplink);
	}
	close(socket_fd);
	return 0;
}
