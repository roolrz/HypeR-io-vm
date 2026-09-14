// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define main service_entry
#include "../../service/hyper-io-service.c"
#undef main
#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>
static struct hyper_io_reply exchange(int fd, unsigned int op, uint64_t transaction)
{
    struct hyper_io_header request = {.magic=htole32(HYPER_IO_MAGIC), .version=htole16(1),
        .operation=htole16(op), .length=htole32(40), .binding=htole64(7),
        .epoch=htole64(1), .transaction=htole64(transaction)};
    struct hyper_io_reply reply = {0};
    assert(write(fd, &request, sizeof(request)) == (ssize_t)sizeof(request));
    ssize_t size = read(fd, &reply, sizeof(reply));
    assert(size == (op == HYPER_IO_HELLO ? 64 : 48));
    assert(le64toh(reply.header.transaction) == transaction);
    assert(le32toh(reply.header.flags) == HYPER_IO_REPLY);
    assert(le32toh(reply.header.length) == (uint32_t)size);
    return reply;
}
static void managed_session(void)
{
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(sockets[0]);
        struct backend b = {.managed=1, .memory=MAP_FAILED, .memory_fd=-1, .fd=-1,
            .notification=-1, .kick={-1,-1,-1}, .call={-1,-1,-1}};
        assert(serve(&b, sockets[1], VERSION_1) == -1);
        close(sockets[1]); _exit(0);
    }
    close(sockets[1]);
    const struct { unsigned operation, bytes, status; uint64_t binding, epoch, transaction; } cases[] = {
        {HYPER_IO_HELLO,40,HYPER_IO_OK,7,1,1},
        {HYPER_IO_PREPARE_MEMORY,64,HYPER_IO_INVALID,7,1,2}, /* invalid empty grant */
        {HYPER_IO_RELEASE_MEMORY,40,HYPER_IO_OK,7,1,3},
        {HYPER_IO_RELEASE_MEMORY,40,HYPER_IO_OK,7,1,3}, /* replay successful release */
        {HYPER_IO_PREPARE_MEMORY,64,HYPER_IO_INVALID,7,1,4}, /* retired binding */
        {HYPER_IO_HELLO,40,HYPER_IO_INVALID,8,1,1}, /* epoch reuse */
        {HYPER_IO_HELLO,40,HYPER_IO_OK,8,2,1},
        {HYPER_IO_RELEASE_MEMORY,40,HYPER_IO_INVALID,7,1,5}, /* stale owner */
        {HYPER_IO_RESET,40,HYPER_IO_OK,8,2,2},
    };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        struct hyper_io_prepare_memory request = {0};
        request.header = (struct hyper_io_header){.magic=htole32(HYPER_IO_MAGIC), .version=htole16(1),
            .operation=htole16(cases[i].operation), .length=htole32(cases[i].bytes),
            .binding=htole64(cases[i].binding), .epoch=htole64(cases[i].epoch),
            .transaction=htole64(cases[i].transaction)};
        assert(write(sockets[0], &request, cases[i].bytes) == cases[i].bytes);
        struct hyper_io_reply reply;
        ssize_t bytes = read(sockets[0], &reply, sizeof(reply));
        assert(bytes >= 48 && bytes == le32toh(reply.header.length));
        assert(le32toh(reply.status) == cases[i].status);
    }
    close(sockets[0]); int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && !WEXITSTATUS(status));
}
int main(void)
{
    /* A delayed IRQ masks a newer RX arm but finds no new record yet.
     * The actual driver predicate must return to its outer rearm loop. */
    assert(hyper_io_wait_ready(2, 1, 7, 8));
    assert(!hyper_io_wait_ready(2, 1, 8, 8)); /* rearmed, no event */
    assert(hyper_io_wait_ready(3, 1, 8, 8)); /* record arrives */
    assert(hyper_io_wait_ready(4, 1, 8, 8)); /* peer closes */
    assert(hyper_io_wait_ready(0, 1, UINT64_MAX, 0)); /* generation wraps */
    assert(hyper_io_wait_ready(2, 2, 8, 8)); /* TX waiter */
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets));
    pid_t child=fork(); assert(child >= 0);
    if (!child) {
        close(sockets[0]);
        struct backend b={.fd=-1,.kick={-1,-1,-1},.call={-1,-1,-1}};
        assert(serve(&b,sockets[1],VERSION_1)==-1);
        close(sockets[1]); _exit(0);
    }
    close(sockets[1]);
    struct hyper_io_reply reply=exchange(sockets[0],HYPER_IO_HELLO,1);
    assert(!reply.status && le64toh(reply.features)==VERSION_1);
    reply=exchange(sockets[0],HYPER_IO_HELLO,1); assert(!reply.status);
    reply=exchange(sockets[0],HYPER_IO_RESET,2); assert(!reply.status);
    reply=exchange(sockets[0],HYPER_IO_RESET,1); assert(le32toh(reply.status)==HYPER_IO_INVALID);
    reply=exchange(sockets[0],HYPER_IO_RESET,3); assert(!reply.status);
    close(sockets[0]); int status; assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    struct backend b={.memory=(void *)0x100000,.region={.guest_base=0x40000000,.length=4096}};
    uint64_t address;
    assert(!queue_address(&b,0x40000000,4096,4096,&address) && address==0x100000);
    assert(queue_address(&b,0x40000001,4096,1,&address));
    assert(queue_address(&b,UINT64_MAX-7,16,8,&address));
    assert(queue_address(&b,0x3ffff000,16,16,&address));
    struct backend idle = {.fd=-1, .notification=-1, .memory=MAP_FAILED};
    struct hyper_io_activate activation = {0};
    assert(activate(&idle, &activation, VERSION_1) == HYPER_IO_INVALID);
    assert(idle.fd == -1 && !idle.bound && !idle.attached);
    managed_session();
    puts("HypeR control protocol tests: PASS");
    return 0;
}
