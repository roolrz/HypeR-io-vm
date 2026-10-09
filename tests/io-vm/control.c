// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define main service_entry
#include "../../service/hyper-io-service.c"
#undef main
#include "../../service/memory.c"
#include "../../service/network-config.c"
#include "../../service/network.c"
#include "../../service/vhost.c"
#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>
static struct hyper_io_reply exchange(int fd, unsigned int op,
				      uint64_t transaction)
{
	struct hyper_io_header request = {.magic = htole32(HYPER_IO_MAGIC),
					  .version = htole16(HYPER_IO_VERSION),
					  .operation = htole16(op),
					  .length = htole32(40),
					  .binding = htole64(7),
					  .epoch = htole64(1),
					  .transaction = htole64(transaction)};
	struct hyper_io_reply reply = {0};
	assert(write(fd, &request, sizeof(request)) ==
	       (ssize_t)sizeof(request));
	ssize_t size = read(fd, &reply, sizeof(reply));
	assert(size == (op == HYPER_IO_HELLO ? 64 : 48));
	assert(le64toh(reply.header.transaction) == transaction);
	assert(le32toh(reply.header.flags) == HYPER_IO_REPLY);
	assert(le32toh(reply.header.length) == (uint32_t)size);
	return reply;
}
static void managed_session(void)
{
	int sockets[2];
	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		close(sockets[0]);
		struct backend b = {.managed = 1,
				    .memory = MAP_FAILED,
				    .memory_fd = -1,
				    .tap = -1,
				    .network = 1};
		endpoint_init(&b.disk);
		endpoint_init(&b.net);
		strcpy(b.target.vhost_wwpn, "naa.5001405000000001");
		assert(serve(&b, sockets[1]) == -1);
		close(sockets[1]);
		_exit(0);
	}
	close(sockets[1]);
	const struct {
		unsigned operation, bytes, status;
		uint64_t binding, epoch, transaction;
	} cases[] = {
	    {HYPER_IO_HELLO, 40, HYPER_IO_OK, 7, 1, 1},
	    {HYPER_IO_NETWORK_HELLO, 40, HYPER_IO_OK, 7, 8, 2},
	    {HYPER_IO_NETWORK_HELLO, 40, HYPER_IO_OK, 7, 8,
	     2}, /* replay 80-byte reply */
	    {HYPER_IO_RESET, 40, HYPER_IO_OK, 7, 1,
	     3}, /* disk epoch unaffected */
	    {HYPER_IO_NETWORK_RESET, 40, HYPER_IO_INVALID, 7, 7,
	     4}, /* stale net */
	    {HYPER_IO_NETWORK_RESET, 40, HYPER_IO_OK, 7, 9, 4},
	    {HYPER_IO_PREPARE_MEMORY, 72, HYPER_IO_INVALID, 7, 1,
	     5}, /* invalid empty grant */
	    {HYPER_IO_RELEASE_MEMORY, 40, HYPER_IO_OK, 7, 1, 6},
	    {HYPER_IO_RELEASE_MEMORY, 40, HYPER_IO_OK, 7, 1,
	     6}, /* replay successful release */
	    {HYPER_IO_PREPARE_MEMORY, 72, HYPER_IO_INVALID, 7, 1,
	     7}, /* retired binding */
	    {HYPER_IO_HELLO, 40, HYPER_IO_OK, 8, 1,
	     1}, /* fresh notification epoch */
	    {HYPER_IO_HELLO, 40, HYPER_IO_INVALID, 6, 2,
	     1}, /* old binding generation */
	    {HYPER_IO_RELEASE_MEMORY, 40, HYPER_IO_INVALID, 7, 1,
	     5}, /* stale owner */
	    {HYPER_IO_RESET, 40, HYPER_IO_OK, 8, 1, 2},
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		struct hyper_io_prepare_memory request = {0};
		request.header = (struct hyper_io_header){
		    .magic = htole32(HYPER_IO_MAGIC),
		    .version = htole16(HYPER_IO_VERSION),
		    .operation = htole16(cases[i].operation),
		    .length = htole32(cases[i].bytes),
		    .binding = htole64(cases[i].binding),
		    .epoch = htole64(cases[i].epoch),
		    .transaction = htole64(cases[i].transaction)};
		assert(write(sockets[0], &request, cases[i].bytes) ==
		       cases[i].bytes);
		struct hyper_io_network_reply response;
		struct hyper_io_reply *reply = &response.common;
		ssize_t bytes = read(sockets[0], &response, sizeof(response));
		assert(bytes >= 48 && bytes == le32toh(reply->header.length));
		assert(le32toh(reply->status) == cases[i].status);
	}
	close(sockets[0]);
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
static void network_only_session(void)
{
	int sockets[2];
	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		close(sockets[0]);
		struct backend b = {.managed = 1,
				    .network = 1,
				    .memory = MAP_FAILED,
				    .memory_fd = -1,
				    .tap = -1};
		endpoint_init(&b.disk);
		endpoint_init(&b.net);
		assert(serve(&b, sockets[1]) == -1);
		close(sockets[1]);
		_exit(0);
	}
	close(sockets[1]);
	unsigned ops[] = {HYPER_IO_HELLO, HYPER_IO_NETWORK_HELLO,
			  HYPER_IO_RELEASE_MEMORY};
	unsigned statuses[] = {HYPER_IO_INVALID, HYPER_IO_OK, HYPER_IO_OK};
	for (unsigned i = 0; i < 3; ++i) {
		struct hyper_io_header request = {
		    .magic = htole32(HYPER_IO_MAGIC),
		    .version = htole16(HYPER_IO_VERSION),
		    .operation = htole16(ops[i]),
		    .length = htole32(40),
		    .binding = htole64(1),
		    .epoch = htole64(9),
		    .transaction = htole64(i + 1)};
		assert(write(sockets[0], &request, sizeof(request)) ==
		       sizeof(request));
		struct hyper_io_network_reply reply;
		ssize_t size = read(sockets[0], &reply, sizeof(reply));
		assert(size == (i == 1 ? 80 : 48));
		assert(le32toh(reply.common.status) == statuses[i]);
		if (i == 1)
			assert(le64toh(reply.common.features) == NETWORK_FEATURES);
	}
	close(sockets[0]);
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
static void cancelled_before_prepare(int network_only)
{
	int sockets[2];
	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		close(sockets[0]);
		struct backend b = {.managed = 1,
				    .network = 1,
				    .memory = MAP_FAILED,
				    .memory_fd = -1,
				    .tap = -1,
				    .memory_path =
					"/dev/null/not-a-memory-device"};
		endpoint_init(&b.disk);
		endpoint_init(&b.net);
		if (!network_only)
			strcpy(b.target.vhost_wwpn, "naa.5001405000000001");
		assert(idle(&b));
		assert(serve(&b, sockets[1]) == -1);
		assert(idle(&b));
		close(sockets[1]);
		_exit(0);
	}
	close(sockets[1]);
	unsigned hello = network_only ? HYPER_IO_NETWORK_HELLO : HYPER_IO_HELLO;
	const struct {
		unsigned operation, status;
		uint64_t binding, epoch, transaction;
	} cases[] = {
	    {hello, HYPER_IO_OK, 7, 8, 1},
	    /* The host released its never-admitted grant and disconnected its
	     * routes. No RESET or RELEASE_MEMORY was sent to this Linux
	     * service. */
	    {hello, HYPER_IO_OK, 8, 1, 2},
	    {HYPER_IO_PREPARE_MEMORY, HYPER_IO_INVALID, 7, 8, 3},
	    /* BACKEND_FAILURE proves this fresh binding reached prepare_memory
	     * and attempted the deliberately unavailable device, rather than
	     * being denied by stale session state. No physical hardware is
	     * needed for this test. */
	    {HYPER_IO_PREPARE_MEMORY, HYPER_IO_BACKEND_FAILURE, 8, 1, 3},
	    {hello, HYPER_IO_OK, 9, 1, 4},
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		unsigned bytes =
		    cases[i].operation == HYPER_IO_PREPARE_MEMORY ? 72 : 40;
		struct hyper_io_prepare_memory request = {
		    .guest_base = htole64(0x40000000), .length = htole64(4096)};
		request.header = (struct hyper_io_header){
		    .magic = htole32(HYPER_IO_MAGIC),
		    .version = htole16(HYPER_IO_VERSION),
		    .operation = htole16(cases[i].operation),
		    .length = htole32(bytes),
		    .binding = htole64(cases[i].binding),
		    .epoch = htole64(cases[i].epoch),
		    .transaction = htole64(cases[i].transaction)};
		assert(write(sockets[0], &request, bytes) == bytes);
		struct hyper_io_network_reply reply;
		ssize_t length = read(sockets[0], &reply, sizeof(reply));
		assert(length >= 48 &&
		       length == le32toh(reply.common.header.length));
		assert(le32toh(reply.common.status) == cases[i].status);
	}
	close(sockets[0]);
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
static void notification_queues(void)
{
	struct hyper_io_eventfds fds = {
	    .kick = {10, 11, -1, -1, -1, -1},
	    .call = {20, 21, -1, -1, -1, -1},
	};
	assert(hyper_io_eventfds_valid(&fds, 1));
	assert(!hyper_io_eventfds_valid(&fds, 0));
	fds.kick[2] = 12;
	assert(!hyper_io_eventfds_valid(&fds, 1));
	assert(!hyper_io_eventfds_valid(&fds, 0));
	fds.call[2] = 22;
	assert(!hyper_io_eventfds_valid(&fds, 1));
	assert(hyper_io_eventfds_valid(&fds, 0));
	fds.call[5] = 25;
	assert(!hyper_io_eventfds_valid(&fds, 0));
	fds.kick[5] = 15;
	assert(hyper_io_eventfds_valid(&fds, 0));
	fds.kick[0] = -1;
	fds.call[0] = -1;
	assert(!hyper_io_eventfds_valid(&fds, 0));
	assert(!hyper_io_eventfds_valid(&fds, 1));
}
int main(void)
{
	/* A delayed IRQ masks a newer RX arm but finds no new record yet.
	 * The actual driver predicate must return to its outer rearm loop. */
	assert(hyper_io_wait_ready(2, 1, 7, 8));
	assert(!hyper_io_wait_ready(2, 1, 8, 8)); /* rearmed, no event */
	assert(hyper_io_wait_ready(3, 1, 8, 8));  /* record arrives */
	assert(hyper_io_wait_ready(4, 1, 8, 8));  /* peer closes */
	assert(hyper_io_wait_ready(0, 1, UINT64_MAX, 0)); /* generation wraps */
	assert(hyper_io_wait_ready(2, 2, 8, 8));	  /* TX waiter */
	int sockets[2];
	assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		close(sockets[0]);
		struct backend b = {
		    .memory = MAP_FAILED, .memory_fd = -1, .tap = -1};
		endpoint_init(&b.disk);
		endpoint_init(&b.net);
		strcpy(b.target.vhost_wwpn, "naa.5001405000000001");
		assert(serve(&b, sockets[1]) == -1);
		close(sockets[1]);
		_exit(0);
	}
	close(sockets[1]);
	struct hyper_io_reply reply = exchange(sockets[0], HYPER_IO_HELLO, 1);
	assert(!reply.status && le64toh(reply.features) == VERSION_1);
	reply = exchange(sockets[0], HYPER_IO_HELLO, 1);
	assert(!reply.status);
	reply = exchange(sockets[0], HYPER_IO_RESET, 2);
	assert(!reply.status);
	reply = exchange(sockets[0], HYPER_IO_RESET, 1);
	assert(le32toh(reply.status) == HYPER_IO_INVALID);
	reply = exchange(sockets[0], HYPER_IO_RESET, 3);
	assert(!reply.status);
	close(sockets[0]);
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	struct backend b = {
	    .memory = (void *)0x100000,
	    .region = {.guest_base = 0x40000000, .length = 4096}};
	uint64_t address;
	assert(!queue_address(&b, 0x40000000, 4096, 4096, &address) &&
	       address == 0x100000);
	assert(queue_address(&b, 0x40000001, 4096, 1, &address));
	assert(queue_address(&b, UINT64_MAX - 7, 16, 8, &address));
	assert(queue_address(&b, 0x3ffff000, 16, 16, &address));
	struct backend dormant = {.memory = MAP_FAILED, .tap = -1};
	endpoint_init(&dormant.disk);
	endpoint_init(&dormant.net);
	strcpy(dormant.target.vhost_wwpn, "naa.5001405000000001");
	struct hyper_io_activate activation = {0};
	assert(activate_disk(&dormant, &activation) == HYPER_IO_INVALID);
	assert(dormant.disk.fd == -1 && !dormant.disk.bound &&
	       !dormant.disk.attached);
	managed_session();
	notification_queues();
	network_only_session();
	cancelled_before_prepare(0);
	cancelled_before_prepare(1);
	puts("HypeR control protocol tests: PASS");
	return 0;
}
