/* SPDX-License-Identifier: BSD-2-Clause */
#include <mergedos/kernel.h>
#include <mergedos/initrd.h>

extern const u8 initrd_start[], initrd_end[];

static size_t initrd_size(void)
{
	return (size_t)(initrd_end - initrd_start);
}

static const char *strip(const char *path)
{
	while (path[0] == '.' && path[1] == '/')
		path += 2;
	while (*path == '/')
		path++;
	return path;
}

void initrd_init(void)
{
	struct tar_entry e;
	size_t off = 0, files = 0;

	if (!tar_probe(initrd_start, initrd_size()))
		panic("initrd is not a ustar archive");
	while (tar_next(initrd_start, initrd_size(), &off, &e) > 0)
		if (e.type == '0')
			files++;
	kprintf("[initrd] %u files, %u KiB (ustar, mapped in place)\n", (unsigned)files,
		(unsigned)(initrd_size() / 1024));
}

int initrd_find(const char *path, const u8 **data, size_t *size)
{
	struct tar_entry e;
	int rc = tar_find(initrd_start, initrd_size(), strip(path), &e);

	if (rc)
		return rc;
	*data = e.data;
	*size = (size_t)e.size;
	return 0;
}

int initrd_next(size_t *cursor, struct tar_entry *e)
{
	return tar_next(initrd_start, initrd_size(), cursor, e);
}

bool initrd_is_dir(const char *path)
{
	struct tar_entry e;
	size_t off = 0, n;

	path = strip(path);
	n = strlen(path);
	while (n && path[n - 1] == '/')
		n--;
	if (n == 0)
		return true;                    /* the root */
	while (tar_next(initrd_start, initrd_size(), &off, &e) > 0)
		if (!strncmp(e.name, path, n) && (e.name[n] == '/' || e.name[n] == '\0') &&
		    (e.type == '5' || e.name[n] == '/'))
			return true;
	return false;
}
