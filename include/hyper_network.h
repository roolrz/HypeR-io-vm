// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#ifndef HYPER_NETWORK_H
#define HYPER_NETWORK_H
#include <stddef.h>
#include <string.h>
#define HYPER_MAX_NETWORKS 8
struct hyper_network {
	char name[32], bridge[16], uplink[16];
};
int hyper_networks_read(const char *, struct hyper_network[HYPER_MAX_NETWORKS],
			unsigned *);
static inline int hyper_mac_parse(const char *text, unsigned char mac[6])
{
	if (strlen(text) != 17)
		return -1;
	for (unsigned i = 0; i < 6; ++i) {
		unsigned value = 0;
		for (unsigned j = 0; j < 2; ++j) {
			char c = text[3 * i + j];
			if (c >= '0' && c <= '9')
				value = value * 16 + (unsigned)(c - '0');
			else if (c >= 'a' && c <= 'f')
				value = value * 16 + (unsigned)(c - 'a' + 10);
			else
				return -1;
		}
		mac[i] = value;
		if (i != 5 && text[3 * i + 2] != ':')
			return -1;
	}
	return (mac[0] & 3) == 2 ? 0 : -1;
}
#endif
