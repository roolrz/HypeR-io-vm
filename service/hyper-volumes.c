// SPDX-FileCopyrightText: 2026 roolrz
// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#ifndef HYPER_VOLUMES_PARSE_TEST
#include <endian.h>
#endif
#include <fcntl.h>
#include <inttypes.h>
#ifndef HYPER_VOLUMES_PARSE_TEST
#include <linux/dm-ioctl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifndef HYPER_VOLUMES_PARSE_TEST
#include <sys/sysmacros.h>
#endif
#include <unistd.h>

#define MAX_VOLUMES 128
struct volume {
	char name[32];
	char uuid[37];
	char owner[32];
	char mapper[40];
	uint64_t sectors;
	unsigned major;
	unsigned minor;
};
static struct volume volumes[MAX_VOLUMES];
static unsigned count;
static int token(const char *s)
{
	if (*s < 'a' || *s > 'z')
		return 0;
	for (; *s; ++s)
		if (!(*s >= 'a' && *s <= 'z') && !isdigit((unsigned char)*s) &&
		    *s != '-')
			return 0;
	return 1;
}
static int manifest(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f) || strcmp(line, "hyper.volumes.v1\n"))
		goto bad;
	while (fgets(line, sizeof(line), f)) {
		if (count == MAX_VOLUMES || !strchr(line, '\n'))
			goto bad;
		struct volume *v = &volumes[count];
		char size[32];
		char extra;
		char expected[40];
		if (sscanf(line, "%31s %36s %31s %31s %39s %c", v->name,
			   v->uuid, size, v->owner, v->mapper, &extra) != 5)
			goto bad;
		if (!token(v->name) || !token(v->owner) ||
		    strlen(v->uuid) != 36 || !*size)
			goto bad;
		for (unsigned i = 0; i < 36; ++i) {
			if (i == 8 || i == 13 || i == 18 || i == 23) {
				if (v->uuid[i] != '-')
					goto bad;
			} else if (!isdigit((unsigned char)v->uuid[i]) &&
				   !(v->uuid[i] >= 'a' && v->uuid[i] <= 'f'))
				goto bad;
		}
		for (char *p = size; *p; ++p)
			if (!isdigit((unsigned char)*p))
				goto bad;
		errno = 0;
		v->sectors = strtoull(size, NULL, 10);
		if (errno || !v->sectors || v->sectors > UINT64_MAX / 512)
			goto bad;
		snprintf(expected, sizeof(expected), "hyper-%s", v->name);
		if (strcmp(expected, v->mapper) ||
		    (!count &&
		     (strcmp(v->name, "config") || strcmp(v->owner, "hyper"))))
			goto bad;
		if (count &&
		    (!strcmp(v->owner, "hyper") || strcmp(v->owner, v->name)))
			goto bad;
		for (unsigned i = 0; i < count; ++i)
			if (!strcmp(volumes[i].uuid, v->uuid) ||
			    !strcmp(volumes[i].name, v->name))
				goto bad;
		++count;
	}
	if (ferror(f) || !count)
		goto bad;
	fclose(f);
	return 0;
bad:
	fclose(f);
	errno = EINVAL;
	return -1;
}
/* Bootstrap authorization, never a client-supplied target or device path. */
static int clients(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[128];
	unsigned ids[128];
	unsigned indices[128];
	unsigned n = 0;
	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f) || strcmp(line, "hyper.clients.v1\n"))
		goto bad;
	while (fgets(line, sizeof(line), f)) {
		char id[8];
		char volume[32];
		char extra;
		unsigned index;
		if (n == 128 || !strchr(line, '\n') ||
		    sscanf(line, "%7s %31s %c", id, volume, &extra) != 2)
			goto bad;
		for (char *p = id; *p; ++p)
			if (!isdigit((unsigned char)*p))
				goto bad;
		unsigned client = strtoul(id, NULL, 10);
		if (client >= 128)
			goto bad;
		for (index = 0; index < count; ++index)
			if (!strcmp(volume, volumes[index].name))
				break;
		if (index == count || (index == 0 && client != 0) ||
		    (client == 0 && index != 0))
			goto bad;
		for (unsigned i = 0; i < n; ++i)
			if (ids[i] == client || indices[i] == index)
				goto bad;
		ids[n] = client;
		indices[n++] = index;
	}
	if (ferror(f) || !n)
		goto bad;
	int config = 0;
	for (unsigned i = 0; i < n; ++i)
		if (!ids[i])
			config = 1;
	if (!config)
		goto bad;
	fclose(f);
	/* No output before every authorization and duplicate check has passed.
	 */
	for (unsigned i = 0; i < n; ++i)
		printf("%u naa.5001405%09x\n", ids[i], indices[i] + 1);
	return ferror(stdout) ? -1 : 0;
bad:
	fclose(f);
	errno = EINVAL;
	return -1;
}
#ifndef HYPER_VOLUMES_PARSE_TEST
static int number(const char *path, uint64_t *value)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int ok = fscanf(f, "%" SCNu64, value) == 1;
	fclose(f);
	return ok ? 0 : -1;
}
static uint32_t crc32_bytes(const unsigned char *p, size_t size)
{
	uint32_t crc = ~0U;
	while (size--) {
		crc ^= *p++;
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
	}
	return ~crc;
}
static uint32_t get32(const unsigned char *p)
{
	uint32_t n;
	memcpy(&n, p, 4);
	return le32toh(n);
}
static uint64_t get64(const unsigned char *p)
{
	uint64_t n;
	memcpy(&n, p, 8);
	return le64toh(n);
}
/* Linux deliberately does not expose PARTUUID in partition uevents. Read only
 * the outer GPT, never probe the contents of an opaque guest partition. */
static int partition_uuid(const char *name, char uuid[37], uint64_t *start,
			  uint64_t *length)
{
	char path[512];
	uint64_t part;
	uint64_t logical;
	snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name);
	if (number(path, &part) || !part)
		return -1;
	snprintf(path, sizeof(path),
		 "/sys/class/block/%s/../queue/logical_block_size", name);
	if (number(path, &logical) || logical != 512)
		return -1;
	snprintf(path, sizeof(path), "/sys/class/block/%s/../dev", name);
	FILE *f = fopen(path, "r");
	unsigned maj;
	unsigned min;
	if (!f)
		return -1;
	int ok = fscanf(f, "%u:%u", &maj, &min) == 2;
	fclose(f);
	if (!ok)
		return -1;
	snprintf(path, sizeof(path), "/dev/block/%u:%u", maj, min);
	/* devtmpfs supplies disk names, not /dev/block symlinks without udev.
	 */
	const char *temporary = "/run/hyper-gpt-device";
	if (mknod(temporary, S_IFBLK | 0600, makedev(maj, min)))
		return -1;
	int fd = open(temporary, O_RDONLY | O_CLOEXEC);
	unlink(temporary);
	if (fd < 0)
		return -1;
	unsigned char header[512];
	unsigned char *table = NULL;
	int result = -1;
	if (pread(fd, header, 512, 512) != 512 || memcmp(header, "EFI PART", 8))
		goto done;
	uint32_t bytes = get32(header + 12);
	uint32_t crc = get32(header + 16);
	uint32_t entries = get32(header + 80);
	uint32_t stride = get32(header + 84);
	if (bytes < 92 || bytes > 512 || get64(header + 24) != 1 || !entries ||
	    entries > 128 || stride != 128 || part > entries)
		goto done;
	memset(header + 16, 0, 4);
	if (crc32_bytes(header, bytes) != crc)
		goto done;
	uint64_t table_lba = get64(header + 72);
	size_t table_size = entries * stride;
	if (table_lba < 2 || table_lba > INT64_MAX / 512)
		goto done;
	table = malloc(table_size);
	if (!table)
		goto done;
	if (pread(fd, table, table_size, table_lba * 512) !=
		(ssize_t)table_size ||
	    crc32_bytes(table, table_size) != get32(header + 88))
		goto done;
	unsigned char *entry = table + (part - 1) * stride;
	unsigned char *id = entry + 16;
	*start = get64(entry + 32);
	uint64_t end = get64(entry + 40);
	if (*start < get64(header + 40) || end > get64(header + 48) ||
	    end < *start)
		goto done;
	*length = end - *start + 1;
	snprintf(uuid, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%"
		 "02x%02x",
		 id[3], id[2], id[1], id[0], id[5], id[4], id[7], id[6], id[8],
		 id[9], id[10], id[11], id[12], id[13], id[14], id[15]);
	result = 0;
done:
	free(table);
	close(fd);
	return result;
}
static int discover(struct volume *v)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *entry;
	unsigned matches = 0;
	if (!d)
		return -1;
	while ((entry = readdir(d))) {
		if (entry->d_name[0] == '.')
			continue;
		char path[512];
		char uuid[37];
		uint64_t gpt_start;
		uint64_t gpt_length;
		uint64_t sectors;
		uint64_t start;
		if (partition_uuid(entry->d_name, uuid, &gpt_start,
				   &gpt_length) ||
		    strcmp(uuid, v->uuid))
			continue;
		++matches;
		snprintf(path, sizeof(path), "/sys/class/block/%s/size",
			 entry->d_name);
		if (number(path, &sectors) || sectors != v->sectors ||
		    sectors != gpt_length)
			goto bad;
		snprintf(path, sizeof(path), "/sys/class/block/%s/start",
			 entry->d_name);
		if (number(path, &start) || start != gpt_start)
			goto bad;
		snprintf(path, sizeof(path), "/sys/class/block/%s/dev",
			 entry->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			goto bad;
		int ok = fscanf(f, "%u:%u", &v->major, &v->minor) == 2;
		fclose(f);
		if (!ok)
			goto bad;
	}
	closedir(d);
	return matches == 1 ? 0 : -1;
bad:
	closedir(d);
	return -1;
}
union dm_buffer {
	struct dm_ioctl alignment;
	unsigned char bytes[4096];
};
static struct dm_ioctl *request(union dm_buffer *buffer, const char *name)
{
	memset(buffer, 0, sizeof(*buffer));
	struct dm_ioctl *d = &buffer->alignment;
	d->version[0] = DM_VERSION_MAJOR;
	d->version[1] = DM_VERSION_MINOR;
	d->version[2] = DM_VERSION_PATCHLEVEL;
	d->data_size = sizeof(*buffer);
	d->data_start = sizeof(*d);
	snprintf(d->name, sizeof(d->name), "%s", name);
	return d;
}
static int remove_volume(int control, const struct volume *v)
{
	union dm_buffer buffer;
	char path[80];
	if (ioctl(control, DM_DEV_REMOVE, request(&buffer, v->mapper)))
		return -1;
	snprintf(path, sizeof(path), "/dev/mapper/%s", v->mapper);
	return unlink(path) && errno != ENOENT ? -1 : 0;
}
static int create_volume(int control, const struct volume *v)
{
	union dm_buffer buffer;
	struct dm_ioctl *d = request(&buffer, v->mapper);
	if (ioctl(control, DM_DEV_CREATE, d))
		return -1; /* Never adopt somebody else's mapping. */
	dev_t dev = d->dev;
	d = request(&buffer, v->mapper);
	d->target_count = 1;
	struct dm_target_spec *target = (void *)(buffer.bytes + d->data_start);
	target->length = v->sectors;
	strcpy(target->target_type, "linear");
	char *parameters = (char *)(target + 1);
	int length = snprintf(parameters, 128, "%u:%u 0", v->major, v->minor);
	target->next = (sizeof(*target) + length + 1 + 7) & ~7U;
	if (ioctl(control, DM_TABLE_LOAD, d))
		goto bad;
	if (ioctl(control, DM_DEV_SUSPEND, request(&buffer, v->mapper)))
		goto bad;
	char path[80];
	snprintf(path, sizeof(path), "/dev/mapper/%s", v->mapper);
	if (mknod(path, S_IFBLK | 0600, dev))
		goto bad;
	return 0;
bad: {
	/* mknod may have failed because an unrelated node already exists. Only
	 * the mapping created by this call belongs to rollback; never unlink
	 * that node. */
	int saved = errno;
	if (ioctl(control, DM_DEV_REMOVE, request(&buffer, v->mapper)))
		perror("rollback mapping");
	errno = saved;
	return -1;
}
}
int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "clients"))
		return manifest(argv[2]) || clients(argv[3]) ? 1 : 0;
	if (argc != 3 ||
	    (strcmp(argv[1], "check") && strcmp(argv[1], "create") &&
	     strcmp(argv[1], "remove")))
		return 2;
	if (manifest(argv[2])) {
		perror("volume manifest");
		return 1;
	}
	if (!strcmp(argv[1], "check"))
		return 0;
	if (mkdir("/dev/mapper", 0755) && errno != EEXIST)
		return 1;
	int control = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);
	if (control < 0) {
		perror("device mapper");
		return 1;
	}
	unsigned created = 0;
	int result = 0;
	if (!strcmp(argv[1], "remove")) {
		for (unsigned i = count; i; --i)
			if (remove_volume(control, &volumes[i - 1]))
				result = 1;
	} else {
		/* Validate the complete physical set before creating any
		 * mapping. */
		for (unsigned attempt = 0;; ++attempt) {
			int ready = 1;
			for (unsigned i = 0; i < count; ++i)
				if (discover(&volumes[i])) {
					ready = 0;
					break;
				}
			if (ready)
				break;
			if (attempt == 29) {
				fputs("volume discovery failed\n", stderr);
				result = 1;
				goto done;
			}
			sleep(1);
		}
		for (; created < count; ++created)
			if (create_volume(control, &volumes[created])) {
				perror("create volume");
				result = 1;
				break;
			}
		if (result)
			while (created)
				if (remove_volume(control, &volumes[--created]))
					perror("rollback volume");
	}
done:
	close(control);
	return result;
}

#else
int main(int argc, char **argv)
{
	if (argc == 3)
		return manifest(argv[1]) || clients(argv[2]) ? 1 : 0;
	return argc == 2 && !manifest(argv[1]) ? 0 : 1;
}
#endif
