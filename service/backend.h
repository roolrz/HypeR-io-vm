// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#ifndef HYPER_BACKEND_H
#define HYPER_BACKEND_H
#include "hyper_io.h"
#include <linux/vhost.h>
#include <linux/virtio_net.h>
#include <stdint.h>
#define VERSION_1 (UINT64_C(1) << 32)
#define NETWORK_MAC (UINT64_C(1) << 5)
#define NETWORK_CSUM (UINT64_C(1) << VIRTIO_NET_F_CSUM)
#define NETWORK_TSO \
	((UINT64_C(1) << VIRTIO_NET_F_HOST_TSO4) | \
	 (UINT64_C(1) << VIRTIO_NET_F_HOST_TSO6))
#define NETWORK_FEATURES (VERSION_1 | NETWORK_MAC | NETWORK_CSUM | NETWORK_TSO)
struct endpoint {
	int notification, fd;
	int kick[HYPER_IO_QUEUES], call[HYPER_IO_QUEUES];
	int bound, attached;
};
/* One client owns memory; devices may only borrow it until their drain. */
struct backend {
	struct endpoint disk, net;
	int memory_fd, tap;
	void *memory;
	struct hyper_memory_info region;
	struct vhost_scsi_target target;
	int managed, network;
	const char *memory_path;
	char bridge[16];
	unsigned char mac[6];
	uint64_t token;
	unsigned diagnostics;
};
static inline struct endpoint *memory_endpoint(struct backend *b)
{
	return b->target.vhost_wwpn[0] ? &b->disk : &b->net;
}
void diagnostic(struct backend *, const char *, ...)
    __attribute__((format(printf, 2, 3)));
void backend_error(struct backend *, const char *, const char *, int);
void endpoint_init(struct endpoint *);
int endpoint_close(struct endpoint *);
int quiesce_disk(struct backend *);
int quiesce_network(struct backend *);
int quiesce_all(struct backend *);
int release_memory(struct backend *);
int prepare_memory(struct backend *, const struct hyper_io_prepare_memory *);
int configure_vhost(struct backend *, struct endpoint *, const char *, uint32_t,
		    uint64_t, const struct hyper_io_queue *, unsigned,
		    unsigned);
int activate_disk(struct backend *, const struct hyper_io_activate *);
int activate_network(struct backend *,
		     const struct hyper_io_network_activate *);
int probe_network(struct backend *);
int network_bridge(const char *, const char *, char[16]);
#endif
