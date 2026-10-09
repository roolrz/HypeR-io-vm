// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>

#include <linux/if_tun.h>
#include <linux/sockios.h>
#include <linux/virtio_net.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Run on the pinned appliance kernel. Two TAPs on a private bridge force
 * software segmentation, independently of the physical uplink's features.
 * The destination deliberately cannot accept checksum or GSO metadata. */
enum { HEADER = 12, ETHERNET = 14, TCP = 20, PAYLOAD = 8192 + 137 };
static const unsigned char source_mac[6] = {2, 0x48, 0x59, 0, 0, 1};
static const unsigned char dest_mac[6] = {2, 0x48, 0x59, 0, 0, 2};

static void put16(unsigned char *p, uint16_t n)
{
	p[0] = n >> 8;
	p[1] = n;
}
static unsigned get16(const unsigned char *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}
static void put32(unsigned char *p, uint32_t n)
{
	p[0] = n >> 24;
	p[1] = n >> 16;
	p[2] = n >> 8;
	p[3] = n;
}
static uint32_t get32(const unsigned char *p)
{
	return (uint32_t)get16(p) << 16 | get16(p + 2);
}
static uint16_t sum(const unsigned char *p, size_t size, uint32_t initial)
{
	while (size >= 2) {
		initial += get16(p);
		p += 2;
		size -= 2;
	}
	if (size)
		initial += (unsigned)*p << 8;
	while (initial >> 16)
		initial = (initial & 0xffff) + (initial >> 16);
	return initial;
}
static uint16_t pseudo(const unsigned char *ip, int ipv6, unsigned length)
{
	return sum(ip + (ipv6 ? 8 : 12), ipv6 ? 32 : 8, 6 + length);
}
static unsigned char payload_byte(unsigned offset)
{
	return (unsigned char)(offset * 37 + offset / 7);
}
static int tap(int control, const char *name)
{
	int fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
	assert(fd >= 0);
	struct ifreq device = {.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR};
	strcpy(device.ifr_name, name);
	assert(!ioctl(fd, TUNSETIFF, &device));
	int header = HEADER, little_endian = 1;
	assert(sizeof(struct virtio_net_hdr_v1) == HEADER);
	assert(!ioctl(fd, TUNSETVNETHDRSZ, &header));
	assert(!ioctl(fd, TUNSETVNETLE, &little_endian));
	assert(!ioctl(fd, TUNSETOFFLOAD, 0UL));
	struct ifreq bridge = {.ifr_ifindex = (int)if_nametoindex(name)};
	assert(bridge.ifr_ifindex);
	strcpy(bridge.ifr_name, "offload-test");
	assert(!ioctl(control, SIOCBRADDIF, &bridge));
	device.ifr_flags = IFF_UP;
	assert(!ioctl(control, SIOCSIFFLAGS, &device));
	return fd;
}
static void transfer(int tx, int rx, int ipv6, int segmented)
{
	unsigned ip_size = ipv6 ? 40 : 20;
	unsigned data_size = segmented ? PAYLOAD : 137;
	unsigned tcp_size = TCP + data_size;
	unsigned packet_size = HEADER + ETHERNET + ip_size + tcp_size;
	unsigned char packet[HEADER + ETHERNET + 40 + TCP + PAYLOAD] = {0};
	struct virtio_net_hdr_v1 header = {
	    .flags = VIRTIO_NET_HDR_F_NEEDS_CSUM,
	    .gso_type = segmented ? (ipv6 ? VIRTIO_NET_HDR_GSO_TCPV6
					  : VIRTIO_NET_HDR_GSO_TCPV4)
				  : VIRTIO_NET_HDR_GSO_NONE,
	    .hdr_len = htole16(ETHERNET + ip_size + TCP),
	    .gso_size = htole16(segmented ? 1500 - ip_size - TCP : 0),
	    .csum_start = htole16(ETHERNET + ip_size),
	    .csum_offset = htole16(16)};
	memcpy(packet, &header, HEADER);
	unsigned char *eth = packet + HEADER, *ip = eth + ETHERNET,
		      *tcp = ip + ip_size;
	memcpy(eth, dest_mac, 6);
	memcpy(eth + 6, source_mac, 6);
	put16(eth + 12, ipv6 ? 0x86dd : 0x0800);
	if (ipv6) {
		ip[0] = 0x60;
		put16(ip + 4, tcp_size);
		ip[6] = 6;
		ip[7] = 64;
		assert(inet_pton(AF_INET6, "fd42::1", ip + 8) == 1);
		assert(inet_pton(AF_INET6, "fd42::2", ip + 24) == 1);
	} else {
		ip[0] = 0x45;
		put16(ip + 2, ip_size + tcp_size);
		put16(ip + 6, 0x4000);
		ip[8] = 64;
		ip[9] = 6;
		assert(inet_pton(AF_INET, "192.0.2.1", ip + 12) == 1);
		assert(inet_pton(AF_INET, "192.0.2.2", ip + 16) == 1);
		put16(ip + 10, (uint16_t)~sum(ip, ip_size, 0));
	}
	put16(tcp, 1234);
	put16(tcp + 2, 4321);
	put32(tcp + 4, 1000);
	put32(tcp + 8, 1);
	tcp[12] = 5 << 4;
	tcp[13] = 0x18; /* ACK | PSH */
	put16(tcp + 14, 65535);
	put16(tcp + 16, pseudo(ip, ipv6, tcp_size));
	for (unsigned i = 0; i < data_size; ++i)
		tcp[TCP + i] = payload_byte(i);
	assert(write(tx, packet, packet_size) == (ssize_t)packet_size);

	unsigned received = 0, packets = 0;
	while (received < data_size) {
		struct pollfd ready = {.fd = rx, .events = POLLIN};
		assert(poll(&ready, 1, 5000) == 1 && (ready.revents & POLLIN));
		unsigned char frame[2048];
		ssize_t bytes = read(rx, frame, sizeof(frame));
		assert(bytes >= HEADER + ETHERNET + ip_size + TCP);
		assert(bytes <= HEADER + ETHERNET + 1500);
		struct virtio_net_hdr_v1 output;
		memcpy(&output, frame, HEADER);
		assert(!(output.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM));
		assert(output.gso_type == VIRTIO_NET_HDR_GSO_NONE);
		const unsigned char *out_ip = frame + HEADER + ETHERNET;
		const unsigned char *out_tcp = out_ip + ip_size;
		unsigned length =
		    ipv6 ? get16(out_ip + 4) : get16(out_ip + 2) - ip_size;
		assert(length > TCP &&
		       bytes == HEADER + ETHERNET + ip_size + length);
		assert(get32(out_tcp + 4) == 1000 + received);
		assert(sum(out_tcp, length, pseudo(out_ip, ipv6, length)) ==
		       0xffff);
		if (!ipv6)
			assert(sum(out_ip, ip_size, 0) == 0xffff);
		assert(length - TCP <= data_size - received);
		for (unsigned i = 0; i < length - TCP; ++i)
			assert(out_tcp[TCP + i] == payload_byte(received + i));
		received += length - TCP;
		++packets;
	}
	assert(segmented ? packets > 1 : packets == 1);
	printf("TAP TCPv%d %s: %u packets, payload and checksums valid\n",
	       ipv6 ? 6 : 4, segmented ? "segmentation" : "checksum", packets);
}
int main(void)
{
	int control = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	assert(control >= 0 && !ioctl(control, SIOCBRADDBR, "offload-test"));
	struct ifreq bridge = {.ifr_flags = IFF_UP};
	strcpy(bridge.ifr_name, "offload-test");
	assert(!ioctl(control, SIOCSIFFLAGS, &bridge));
	int tx = tap(control, "offload-tx"), rx = tap(control, "offload-rx");
	for (int ipv6 = 0; ipv6 <= 1; ++ipv6)
		for (int segmented = 0; segmented <= 1; ++segmented)
			transfer(tx, rx, ipv6, segmented);
	/* Reject malformed GSO metadata without breaking subsequent traffic. */
	unsigned char invalid[HEADER + 64] = {0};
	invalid[1] = 0xff;
	errno = 0;
	assert(write(tx, invalid, sizeof(invalid)) < 0 && errno == EINVAL);
	transfer(tx, rx, 0, 1);
	close(tx);
	close(rx);
	bridge.ifr_flags = 0;
	assert(!ioctl(control, SIOCSIFFLAGS, &bridge));
	assert(!ioctl(control, SIOCBRDELBR, "offload-test"));
	close(control);
	puts("TAP-OFFLOAD-PASS");
	return 0;
}
