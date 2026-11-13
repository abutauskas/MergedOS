/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ramfs: files and directories in memory. A file's bytes are one
 * contiguous buffer that doubles as it grows, so an executable written here
 * can be handed to the loader without copying.
 */
#include <mergedos/ramfs.h>

static u64 next_ino = 1;

struct rnode *ramfs_new_root(void)
{
	struct rnode *r = mos_zalloc(sizeof(*r));

	if (r) {
		r->dir = true;
		r->ino = next_ino++;
	}
	return r;
}

static void free_node(struct rnode *n)
{
	mos_free(n->data);
	mos_free(n);
}

void ramfs_destroy(struct rnode *root)
{
	struct rnode *c = root->child;

	while (c) {
		struct rnode *nx = c->next;

		ramfs_destroy(c);
		c = nx;
	}
	free_node(root);
}

static struct rnode *find_child(const struct rnode *dir, const char *name, size_t len)
{
	for (struct rnode *c = dir->child; c; c = c->next)
		if (strlen(c->name) == len && !memcmp(c->name, name, len))
			return c;
	return NULL;
}

/*
 * Walk `path`. With `leaf` set, stop before the last component and return
 * it in *leaf / *leaf_len (the parent directory is the result).
 */
static int walk(struct rnode *root, const char *path, struct rnode **out, const char **leaf,
		size_t *leaf_len)
{
	struct rnode *n = root;
	const char *p = path;

	if (leaf)
		*leaf = NULL;
	for (;;) {
		const char *s;
		size_t len;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		s = p;
		while (*p && *p != '/')
			p++;
		len = (size_t)(p - s);
		if (leaf) {
			const char *q = p;

			while (*q == '/')
				q++;
			if (!*q) {                      /* last component */
				if ((len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.'))
					return -MERR_INVAL;
				if (len >= RAMFS_NAME_MAX)
					return -MERR_RANGE;
				*leaf = s;
				*leaf_len = len;
				break;
			}
		}
		if (!n->dir)
			return -MERR_NOTDIR;
		if (len == 1 && s[0] == '.')
			continue;
		if (len == 2 && s[0] == '.' && s[1] == '.') {
			if (n->parent)
				n = n->parent;
			continue;
		}
		n = find_child(n, s, len);
		if (!n)
			return -MERR_NOENT;
	}
	if (leaf && !*leaf)
		return -MERR_INVAL;                     /* the root itself */
	if (leaf && !n->dir)
		return -MERR_NOTDIR;
	*out = n;
	return 0;
}

int ramfs_lookup(struct rnode *root, const char *path, struct rnode **out)
{
	return walk(root, path, out, NULL, NULL);
}

int ramfs_create(struct rnode *root, const char *path, bool dir, bool excl, struct rnode **out)
{
	struct rnode *parent, *n;
	const char *leaf;
	size_t len;
	int rc = walk(root, path, &parent, &leaf, &len);

	if (rc)
		return rc;
	n = find_child(parent, leaf, len);
	if (n) {
		if (excl || dir)
			return -MERR_EXIST;
		if (n->dir)
			return -MERR_ISDIR;
		*out = n;
		return 0;
	}
	n = mos_zalloc(sizeof(*n));
	if (!n)
		return -MERR_NOMEM;
	memcpy(n->name, leaf, len);
	n->dir = dir;
	n->ino = next_ino++;
	n->parent = parent;
	n->next = parent->child;
	parent->child = n;
	*out = n;
	return 0;
}

static void detach(struct rnode *n)
{
	struct rnode **pp = &n->parent->child;

	while (*pp != n)
		pp = &(*pp)->next;
	*pp = n->next;
	n->next = NULL;
}

int ramfs_unlink(struct rnode *root, const char *path, bool dir)
{
	struct rnode *n;
	int rc = ramfs_lookup(root, path, &n);

	if (rc)
		return rc;
	if (n == root)
		return -MERR_PERM;
	if (n->dir != dir)
		return dir ? -MERR_NOTDIR : -MERR_ISDIR;
	if (dir && n->child)
		return -MERR_NOTEMPTY;
	detach(n);
	n->parent = NULL;
	if (n->opens)
		n->unlinked = true;
	else
		free_node(n);
	return 0;
}

int ramfs_rename(struct rnode *root, const char *from, const char *to)
{
	struct rnode *n, *parent, *old, *a;
	const char *leaf;
	size_t len;
	int rc = ramfs_lookup(root, from, &n);

	if (rc)
		return rc;
	if (n == root)
		return -MERR_PERM;
	rc = walk(root, to, &parent, &leaf, &len);
	if (rc)
		return rc;
	for (a = parent; a; a = a->parent)
		if (a == n)
			return -MERR_INVAL;             /* into its own subtree */
	old = find_child(parent, leaf, len);
	if (old == n)
		return 0;
	if (old) {
		if (old->dir != n->dir)
			return old->dir ? -MERR_ISDIR : -MERR_NOTDIR;
		if (old->dir && old->child)
			return -MERR_NOTEMPTY;
		detach(old);
		old->parent = NULL;
		if (old->opens)
			old->unlinked = true;
		else
			free_node(old);
	}
	detach(n);
	memset(n->name, 0, sizeof(n->name));
	memcpy(n->name, leaf, len);
	n->parent = parent;
	n->next = parent->child;
	parent->child = n;
	return 0;
}

long ramfs_read(const struct rnode *n, u64 pos, void *buf, u64 len)
{
	if (n->dir)
		return -MERR_ISDIR;
	if (pos >= n->size)
		return 0;
	if (len > n->size - pos)
		len = n->size - pos;
	memcpy(buf, n->data + pos, (size_t)len);
	return (long)len;
}

static int reserve(struct rnode *n, u64 size)
{
	u64 cap = n->cap ? n->cap : 256;
	u8 *d;

	if (size <= n->cap)
		return 0;
	if (size > RAMFS_FILE_MAX)
		return -MERR_RANGE;
	while (cap < size)
		cap *= 2;
	d = mos_zalloc((size_t)cap);
	if (!d)
		return -MERR_NOMEM;
	if (n->size)
		memcpy(d, n->data, (size_t)n->size);
	mos_free(n->data);
	n->data = d;
	n->cap = cap;
	return 0;
}

long ramfs_write(struct rnode *n, u64 pos, const void *buf, u64 len)
{
	int rc;

	if (n->dir)
		return -MERR_ISDIR;
	if (pos + len < pos)
		return -MERR_RANGE;
	rc = reserve(n, pos + len);
	if (rc)
		return rc;
	if (pos > n->size)
		memset(n->data + n->size, 0, (size_t)(pos - n->size));
	memcpy(n->data + pos, buf, (size_t)len);
	if (pos + len > n->size)
		n->size = pos + len;
	return (long)len;
}

int ramfs_truncate(struct rnode *n, u64 size)
{
	int rc;

	if (n->dir)
		return -MERR_ISDIR;
	if (size > n->size) {
		rc = reserve(n, size);
		if (rc)
			return rc;
		memset(n->data + n->size, 0, (size_t)(size - n->size));
	}
	n->size = size;
	return 0;
}

struct rnode *ramfs_entry(const struct rnode *dir, u64 index)
{
	struct rnode *c = dir->dir ? dir->child : NULL;

	while (c && index--)
		c = c->next;
	return c;
}

void ramfs_open(struct rnode *n)
{
	n->opens++;
}

void ramfs_close(struct rnode *n)
{
	if (--n->opens == 0 && n->unlinked)
		free_node(n);
}
