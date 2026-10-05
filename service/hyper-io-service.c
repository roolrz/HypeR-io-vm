// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "backend.h"
#include "hyper_io_session.h"
#include "hyper_network.h"
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
_Static_assert(sizeof(struct hyper_io_header) == 40, "header layout");
_Static_assert(sizeof(struct hyper_io_activate) == 240,
	       "storage activation layout");
_Static_assert(sizeof(struct hyper_io_network_activate) == 112,
	       "network activation layout");
_Static_assert(sizeof(struct hyper_io_network_reply) == 80,
	       "network reply layout");
_Static_assert(sizeof(struct hyper_io_prepare_memory) == 72, "prepare layout");
static volatile sig_atomic_t stopping;
static void stop_signal(int signal)
{
	(void)signal;
	stopping = 1;
}
static const char *operation_name(uint16_t operation)
{
	switch (operation) {
	case HYPER_IO_HELLO:
		return "HELLO";
	case HYPER_IO_ACTIVATE:
		return "ACTIVATE";
	case HYPER_IO_RESET:
		return "RESET";
	case HYPER_IO_STOP_QUEUE:
		return "STOP_QUEUE";
	case HYPER_IO_PREPARE_MEMORY:
		return "PREPARE_MEMORY";
	case HYPER_IO_RELEASE_MEMORY:
		return "RELEASE_MEMORY";
	case HYPER_IO_NETWORK_HELLO:
		return "NETWORK_HELLO";
	case HYPER_IO_NETWORK_ACTIVATE:
		return "NETWORK_ACTIVATE";
	case HYPER_IO_NETWORK_RESET:
		return "NETWORK_RESET";
	case HYPER_IO_NETWORK_STOP_QUEUE:
		return "NETWORK_STOP_QUEUE";
	default:
		return "UNKNOWN";
	}
}
static const char *status_name(uint32_t status)
{
	switch (status) {
	case HYPER_IO_OK:
		return "ok";
	case HYPER_IO_UNSUPPORTED:
		return "unsupported";
	case HYPER_IO_INVALID:
		return "invalid request";
	case HYPER_IO_BUSY:
		return "busy";
	case HYPER_IO_BACKEND_FAILURE:
		return "backend failure";
	case HYPER_IO_QUIESCENCE_FAILED:
		return "quiescence failed";
	default:
		return "unknown status";
	}
}
void diagnostic(struct backend *b, const char *format, ...)
{
	/* Bounded control-plane breadcrumbs; never log the virtqueue data path.
	 */
	if (!b->managed || b->diagnostics >= 128)
		return;
	b->diagnostics++;
	char message[256];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	fprintf(stderr, "HypeR I/O [%s]: %s\n",
		b->target.vhost_wwpn[0] ? b->target.vhost_wwpn : b->bridge,
		message);
}
static int idle(const struct backend *b)
{
	return b->memory == MAP_FAILED && b->memory_fd < 0 && !b->token &&
	       b->disk.fd < 0 && !b->disk.bound && b->net.fd < 0 &&
	       !b->net.bound && b->tap < 0;
}
static uint32_t dispatch(struct backend *b, struct hyper_io_session *session,
			 const unsigned char *message, size_t size,
			 struct hyper_io_network_reply *response,
			 size_t *reply_size)
{
	uint16_t operation = le16toh(response->common.header.operation);
	if (operation == HYPER_IO_RELEASE_MEMORY && size == 40 && b->managed) {
		int status = release_memory(b);
		if (status == HYPER_IO_OK)
			hyper_io_session_retire(session);
		return status;
	}
	if (!hyper_io_session_preparable(session))
		return HYPER_IO_INVALID;
	switch (operation) {
	case HYPER_IO_HELLO:
	case HYPER_IO_NETWORK_HELLO:
		if (size != 40)
			break;
		if ((operation == HYPER_IO_NETWORK_HELLO && !b->network) ||
		    (operation == HYPER_IO_HELLO && !b->target.vhost_wwpn[0]))
			return HYPER_IO_UNSUPPORTED;
		response->common.features = htole64(
		    operation == HYPER_IO_HELLO ? VERSION_1 : NETWORK_FEATURES);
		response->common.queues = htole32(
		    operation == HYPER_IO_HELLO ? HYPER_IO_QUEUES
						: HYPER_IO_NETWORK_QUEUES);
		response->common.queue_max = htole32(128);
		*reply_size = operation == HYPER_IO_HELLO ? 64 : 80;
		if (operation == HYPER_IO_NETWORK_HELLO) {
			memcpy(response->mac, b->mac, 6);
			response->mtu = htole16(1500);
		}
		return HYPER_IO_OK;
	case HYPER_IO_PREPARE_MEMORY:
		if (size == 72 && b->managed) {
			struct hyper_io_prepare_memory request;
			memcpy(&request, message, sizeof(request));
			return prepare_memory(b, &request);
		}
		break;
	case HYPER_IO_ACTIVATE:
		if (size == 240) {
			struct hyper_io_activate request;
			memcpy(&request, message, sizeof(request));
			return activate_disk(b, &request);
		}
		break;
	case HYPER_IO_NETWORK_ACTIVATE:
		if (size == 112) {
			struct hyper_io_network_activate request;
			memcpy(&request, message, sizeof(request));
			return activate_network(b, &request);
		}
		break;
	case HYPER_IO_RESET:
	case HYPER_IO_STOP_QUEUE:
	case HYPER_IO_NETWORK_RESET:
	case HYPER_IO_NETWORK_STOP_QUEUE: {
		int network = operation >= HYPER_IO_NETWORK_RESET;
		int queue_stop = operation == HYPER_IO_STOP_QUEUE ||
				 operation == HYPER_IO_NETWORK_STOP_QUEUE;
		if (size != (queue_stop ? 48U : 40U))
			break;
		if ((network && !b->network) ||
		    (!network && !b->target.vhost_wwpn[0]))
			return HYPER_IO_UNSUPPORTED;
		if (queue_stop) {
			uint32_t queue, reserved;
			memcpy(&queue, message + 40, 4);
			memcpy(&reserved, message + 44, 4);
			if (reserved ||
			    le32toh(queue) >= (network ? HYPER_IO_NETWORK_QUEUES
						       : HYPER_IO_QUEUES))
				break;
		}
		return (network ? quiesce_network(b) : quiesce_disk(b))
			   ? HYPER_IO_QUIESCENCE_FAILED
			   : HYPER_IO_OK;
	}
	default:
		return HYPER_IO_UNSUPPORTED;
	}
	return HYPER_IO_INVALID;
}
static int serve(struct backend *b, int control)
{
	unsigned char message[HYPER_IO_RECORD], previous[HYPER_IO_RECORD];
	struct hyper_io_network_reply response = {0}, previous_reply = {0};
	struct hyper_io_session session = {0};
	uint64_t last_transaction = 0;
	size_t previous_size = 0;
	while (!stopping) {
		ssize_t size;
		do {
			size = read(control, message, sizeof(message));
		} while (size < 0 && errno == EINTR && !stopping);
		if (stopping || (size < 0 && errno == EPIPE))
			return 0;
		if (size < (ssize_t)sizeof(struct hyper_io_header))
			return -1;
		struct hyper_io_header header;
		memcpy(&header, message, sizeof(header));
		uint64_t binding = le64toh(header.binding),
			 epoch = le64toh(header.epoch);
		uint64_t transaction = le64toh(header.transaction);
		uint16_t operation = le16toh(header.operation);
		unsigned primary = b->target.vhost_wwpn[0] ? 0 : 1;
		unsigned device = operation >= HYPER_IO_NETWORK_HELLO &&
				  operation <= HYPER_IO_NETWORK_STOP_QUEUE;
		if (operation == HYPER_IO_PREPARE_MEMORY ||
		    operation == HYPER_IO_RELEASE_MEMORY)
			device = primary;
		if (le32toh(header.magic) != HYPER_IO_MAGIC ||
		    le16toh(header.version) != HYPER_IO_VERSION ||
		    le32toh(header.length) != (uint32_t)size || header.flags ||
		    !binding || !transaction || !epoch || epoch > UINT32_MAX)
			return -1;
		diagnostic(b, "request %s", operation_name(operation));
		int admission = hyper_io_session_admit(
		    &session, binding, epoch,
		    operation == (primary ? HYPER_IO_NETWORK_HELLO
					  : HYPER_IO_HELLO) &&
			size == 40,
		    b->managed ? idle(b) : !session.binding, device);
		if (admission > 0) {
			last_transaction = 0;
			previous_size = 0;
		}
		if (admission >= 0 && transaction == last_transaction &&
		    (size_t)size == previous_size &&
		    !memcmp(previous, message, previous_size)) {
			response = previous_reply;
		} else {
			memset(&response, 0, sizeof(response));
			response.common.header = header;
			size_t reply_size = 48;
			uint32_t status = HYPER_IO_INVALID;
			if (admission >= 0 && transaction > last_transaction)
				status =
				    dispatch(b, &session, message, (size_t)size,
					     &response, &reply_size);
			response.common.header.length = htole32(reply_size);
			response.common.header.flags = htole32(HYPER_IO_REPLY);
			response.common.status = htole32(status);
			if (admission >= 0 && transaction > last_transaction) {
				session.epoch[device] = epoch;
				last_transaction = transaction;
				previous_size = size;
				memcpy(previous, message, size);
				previous_reply = response;
			}
		}
		diagnostic(b, "reply %s: %s", operation_name(operation),
			   status_name(le32toh(response.common.status)));
		ssize_t written;
		do {
			written = write(control, &response,
					le32toh(response.common.header.length));
		} while (written < 0 && errno == EINTR && !stopping);
		if (stopping || (written < 0 && errno == EPIPE))
			return 0;
		if (written != (ssize_t)le32toh(response.common.header.length))
			return -1;
	}
	return 0;
}
static int probe_backend(const char *path, uint64_t required)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	uint64_t features = 0;
	if (fd < 0)
		return -1;
	int result = ioctl(fd, VHOST_GET_FEATURES, &features);
	close(fd);
	return result || (features & required) != required ? -1 : 0;
}
int main(int argc, char **argv)
{
	struct backend b = {.memory_fd = -1, .tap = -1, .memory = MAP_FAILED};
	endpoint_init(&b.disk);
	endpoint_init(&b.net);
	/* Managed: memory target control notification network mac
	 * net-notification. The three-argument mode remains for standalone
	 * qualification fixtures. */
	if ((argc != 3 && argc != 8) ||
	    strlen(argv[2]) >= sizeof(b.target.vhost_wwpn))
		return 2;
	b.managed = argc == 8;
	b.memory_path = argv[1];
	struct sigaction action = {.sa_handler = stop_signal};
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) ||
	    sigaction(SIGINT, &action, NULL))
		return 1;
	if (strcmp(argv[2], "-"))
		memcpy(b.target.vhost_wwpn, argv[2], strlen(argv[2]) + 1);
	if (b.managed && strcmp(argv[5], "-")) {
		if (hyper_mac_parse(argv[6], b.mac) ||
		    network_bridge("/etc/hyper-networks.conf", argv[5],
				   b.bridge))
			return 1;
		b.net.notification = open(argv[7], O_RDWR | O_CLOEXEC);
		if (b.net.notification < 0 ||
		    probe_backend("/dev/vhost-net",
				  VERSION_1 | (UINT64_C(1)
					       << VHOST_NET_F_VIRTIO_NET_HDR)))
			return 1;
		b.network = 1;
	}
	int standby = !strcmp(argv[1], "--standby");
	if (!standby && !b.managed) {
		b.memory_fd = open(argv[1], O_RDWR | O_CLOEXEC);
		if (b.memory_fd < 0 ||
		    ioctl(b.memory_fd, HYPER_MEMORY_INFO, &b.region) ||
		    !b.region.length ||
		    (uint64_t)(size_t)b.region.length != b.region.length)
			return 1;
		b.memory = mmap(NULL, b.region.length, PROT_READ | PROT_WRITE,
				MAP_SHARED, b.memory_fd, 0);
		if (b.memory == MAP_FAILED)
			return 1;
	}
	if ((!standby || b.managed) && b.target.vhost_wwpn[0]) {
		b.disk.notification =
		    open(b.managed ? argv[4] : "/dev/hyper-io-notification",
			 O_RDWR | O_CLOEXEC);
		if (b.disk.notification < 0)
			return 1;
	}
	int control = open(b.managed ? argv[3] : "/dev/hyper-io-control",
			   O_RDWR | O_CLOEXEC);
	if (control < 0 || (b.target.vhost_wwpn[0] &&
			    probe_backend("/dev/vhost-scsi", VERSION_1)))
		return 1;
	printf("HypeR I/O: %s service ready [%s]\n",
	       standby ? "standby" : "backend",
	       b.target.vhost_wwpn[0] ? b.target.vhost_wwpn : b.bridge);
	fflush(stdout);
	int result = serve(&b, control);
	if (release_memory(&b) != HYPER_IO_OK) {
		fputs("HypeR I/O: quiescence failed; retaining backend\n",
		      stderr);
		for (;;)
			pause();
	}
	close(control);
	if (b.disk.notification >= 0)
		close(b.disk.notification);
	if (b.net.notification >= 0)
		close(b.net.notification);
	return result ? 1 : 0;
}
