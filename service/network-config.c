// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#include "hyper_network.h"
#include <stdio.h>
#include <string.h>
static int name_valid(const char *name, int interface)
{
	if (!*name || (!interface && (*name < 'a' || *name > 'z')))
		return 0;
	for (; *name; ++name)
		if (!((*name >= 'a' && *name <= 'z') ||
		      (*name >= '0' && *name <= '9') || *name == '-' ||
		      (interface &&
		       ((*name >= 'A' && *name <= 'Z') || *name == '_'))))
			return 0;
	return 1;
}
int hyper_networks_read(const char *path,
			struct hyper_network networks[HYPER_MAX_NETWORKS],
			unsigned *count)
{
	FILE *file = fopen(path, "r");
	char line[128], extra;
	*count = 0;
	if (!file)
		return -1;
	if (!fgets(line, sizeof(line), file) ||
	    strcmp(line, "hyper.networks.v1\n"))
		goto bad;
	while (fgets(line, sizeof(line), file)) {
		if (*count == HYPER_MAX_NETWORKS || !strchr(line, '\n'))
			goto bad;
		struct hyper_network *n = &networks[*count];
		memset(n, 0, sizeof(*n));
		if (sscanf(line, "%31s %15s %15s %c", n->name, n->bridge,
			   n->uplink, &extra) != 3 ||
		    !name_valid(n->name, 0) || !name_valid(n->bridge, 1) ||
		    !name_valid(n->uplink, 1) || !strcmp(n->bridge, n->uplink))
			goto bad;
		for (unsigned i = 0; i < *count; ++i)
			if (!strcmp(n->name, networks[i].name) ||
			    !strcmp(n->bridge, networks[i].bridge) ||
			    !strcmp(n->uplink, networks[i].uplink) ||
			    !strcmp(n->bridge, networks[i].uplink) ||
			    !strcmp(n->uplink, networks[i].bridge))
				goto bad;
		++*count;
	}
	if (ferror(file))
		goto bad;
	fclose(file);
	return 0;
bad:
	fclose(file);
	return -1;
}
int network_bridge(const char *path, const char *name, char bridge[16])
{
	struct hyper_network networks[HYPER_MAX_NETWORKS];
	unsigned count;
	if (hyper_networks_read(path, networks, &count))
		return -1;
	for (unsigned i = 0; i < count; ++i)
		if (!strcmp(name, networks[i].name)) {
			memcpy(bridge, networks[i].bridge, 16);
			return 0;
		}
	return -1;
}
