/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Open files, descriptor tables, pipes, the terminal and path lookup, plus
 * the file system calls every personality is translated onto.
 *
 * Name space (canonical paths have no leading '/'):
 *   ramfs mounts   /tmp /home/user /run /var  (writable, in memory)
 *   virtual files  /dev/{null,zero,urandom,...}, /proc/self/{maps,exe,fd,...}
 *   everything else: the read-only boot ramdisk and boot modules
 * Paths resolve against the process's cwd or a directory descriptor, and
 * follow symlinks (ramfs and the virtual /proc/self links).
 */
#include <mergedos/kernel.h>
#include <mergedos/console.h>
#include <mergedos/initrd.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/mm.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>

#define PATH_MAX_LEN 256
#define S_ISDIR_MODE(m) (((m) & S_IFMT) == S_IFDIR)
#define MSYS_IO_MAX  (1u << 20)

static struct {
	const char *prefix;
	struct rnode *root;
} mounts[] = { { "tmp", NULL }, { "home/user", NULL }, { "run", NULL }, { "var", NULL } };

static struct rnode *anon_root;          /* unlinked files (memfd): not reachable by path */
int fg_pgid;
bool vfs_debug;
u64 vfs_last_ino;

void vfs_init(void)
{
	for (unsigned i = 0; i < ARRAY_SIZE(mounts); i++)
		mounts[i].root = ramfs_new_root();
	anon_root = ramfs_new_root();
	socket_init();
}

void socket_init(void);                 /* kernel/socket.c */

static char *kstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = kzalloc(n);

	if (d)
		memcpy(d, s, n);
	return d;
}

/* ---- open files and descriptors ------------------------------------------- */

static struct file *file_new(enum file_type type, u32 flags)
{
	struct file *f = kzalloc(sizeof(*f));

	if (f) {
		f->refs = 1;
		f->type = type;
		f->flags = flags;
	}
	return f;
}

struct file *file_new_ops(const struct fops *ops, void *priv, u32 flags)
{
	struct file *f = file_new(F_OPS, flags);

	if (f) {
		f->ops = ops;
		f->priv = priv;
	}
	return f;
}

struct file *file_get(struct file *f)
{
	f->refs++;
	return f;
}

void file_put(struct file *f)
{
	if (--f->refs > 0)
		return;
	if (f->type == F_OPS && f->ops->release)
		f->ops->release(f);
	if (f->type == F_RAMFS)
		ramfs_close(f->node);
	if (f->type == F_PIPE) {
		struct pipe *p = f->pipe;

		if (f->pipe_write)
			p->writers--;
		else
			p->readers--;
		thread_wake(p, 0, SCHED_MAX_THREADS);
		poll_wake();
		if (!p->readers && !p->writers)
			kfree(p);
	}
	kfree(f->path);
	kfree(f);
}

struct file *console_file(void)
{
	return file_new(F_CONSOLE, VFS_O_RDWR);
}

struct file *fd_file(struct task *t, int fd)
{
	return fd >= 0 && fd < TASK_FD_MAX ? t->fd[fd] : NULL;
}

#define CLOEXEC_SET(t, fd, on) ((t)->cloexec[(fd) / 32] = (on) ? (t)->cloexec[(fd) / 32] | (1u << ((fd) % 32)) : \
				(t)->cloexec[(fd) / 32] & ~(1u << ((fd) % 32)))
#define CLOEXEC_GET(t, fd) (((t)->cloexec[(fd) / 32] >> ((fd) % 32)) & 1)

int fd_install(struct task *t, struct file *f, int min, bool cloexec)
{
	for (int fd = min < 0 ? 0 : min; fd < TASK_FD_MAX; fd++) {
		if (!t->fd[fd]) {
			t->fd[fd] = f;
			CLOEXEC_SET(t, fd, cloexec);
			return fd;
		}
	}
	file_put(f);
	return -MERR_MFILE;
}

static void fd_drop(struct task *t, int fd)
{
	struct file *f = t->fd[fd];

	t->fd[fd] = NULL;
	CLOEXEC_SET(t, fd, false);
	if (f)
		file_put(f);
}

void fd_close_all(struct task *t)
{
	for (int fd = 0; fd < TASK_FD_MAX; fd++)
		if (t->fd[fd])
			fd_drop(t, fd);
}

void fd_close_on_exec(struct task *t)
{
	for (int fd = 0; fd < TASK_FD_MAX; fd++)
		if (t->fd[fd] && CLOEXEC_GET(t, fd))
			fd_drop(t, fd);
}

void fd_fork(struct task *child, const struct task *parent)
{
	for (int fd = 0; fd < TASK_FD_MAX; fd++)
		child->fd[fd] = parent->fd[fd] ? file_get(parent->fd[fd]) : NULL;
	memcpy(child->cloexec, parent->cloexec, sizeof(child->cloexec));
}

/* ---- readiness ----------------------------------------------------------------- */

static const char poll_chan;
static bool stdin_ready(void);

void poll_wake(void)
{
	thread_wake(&poll_chan, 0, SCHED_MAX_THREADS);
}

bool poll_wait(u64 timeout_ticks)
{
	return thread_block(&poll_chan, 0, timeout_ticks);
}

u32 file_poll(struct file *f, u32 events)
{
	u32 r = 0;

	if (f->type == F_OPS)
		return f->ops->poll ? f->ops->poll(f, events) : events & (POLLIN | POLLOUT);
	switch (f->type) {
	case F_CONSOLE:
		if (stdin_ready())
			r |= POLLIN;
		return (r | POLLOUT) & (events | POLLERR | POLLHUP);
	case F_PIPE:
		if (!f->pipe_write) {
			if (f->pipe->count)
				r |= POLLIN;
			if (!f->pipe->writers)
				r |= POLLHUP;
		} else {
			if (f->pipe->count < PIPE_SIZE)
				r |= POLLOUT;
			if (!f->pipe->readers)
				r |= POLLERR;
		}
		return r & (events | POLLERR | POLLHUP);
	default:
		return events & (POLLIN | POLLOUT);
	}
}

/*
 * Bytes of an open file for mmap. A read-only ramdisk/sysroot file returns a
 * pointer straight into its immutable image (for demand-paged, file-backed
 * VMAs); a writable ramfs file reports its data pointer, which the caller must
 * copy eagerly (or pin) because the buffer can move. Returns the readable
 * length from `offset`, or -MERR_* (ENODEV for the console / a pipe).
 */
long vfs_mmap_source(struct task *t, int fd, u64 offset, const u8 **data, bool *immutable)
{
	struct file *f = fd_file(t, fd);

	if (!f)
		return -MERR_BADF;
	if (f->type == F_ROFILE) {
		if (offset > f->size)
			return -MERR_INVAL;
		*data = f->data + offset;
		*immutable = true;
		return (long)(f->size - offset);
	}
	if (f->type == F_RAMFS && !f->node->dir) {
		if (offset > f->node->size)
			return -MERR_INVAL;
		*data = f->node->data + offset;
		*immutable = false;
		return (long)(f->node->size - offset);
	}
	return -MERR_NODEV;
}

/* ---- virtual files ---------------------------------------------------------------- */

enum {
	V_NONE, V_DEVDIR, V_MWM, V_NULL, V_ZERO, V_URANDOM, V_TTY, V_PROCDIR, V_SELFDIR, V_FDDIR,
	V_MAPS, V_MEMINFO, V_CPUINFO, V_EXE, V_CWD, V_FDLINK, V_SYNTH,
};

static int virt_lookup(const char *p)
{
	static const struct { const char *path; int id; } tab[] = {
		{ "dev", V_DEVDIR }, { "dev/mwm", V_MWM }, { "dev/null", V_NULL }, { "dev/zero", V_ZERO },
		{ "dev/urandom", V_URANDOM }, { "dev/random", V_URANDOM }, { "dev/tty", V_TTY },
		{ "dev/console", V_TTY }, { "proc", V_PROCDIR }, { "proc/self", V_SELFDIR },
		{ "proc/self/fd", V_FDDIR }, { "proc/self/maps", V_MAPS },
		{ "proc/self/exe", V_EXE }, { "proc/self/cwd", V_CWD },
		{ "proc/meminfo", V_MEMINFO }, { "proc/cpuinfo", V_CPUINFO },
	};

	for (unsigned i = 0; i < ARRAY_SIZE(tab); i++)
		if (!strcmp(p, tab[i].path))
			return tab[i].id;
	if (!strncmp(p, "proc/self/fd/", 13) && p[13])
		return V_FDLINK;
	/* A directory that only exists because a mount point lies below it ("home"). */
	for (unsigned i = 0; i < ARRAY_SIZE(mounts); i++) {
		size_t n = strlen(p);

		if (n && !strncmp(mounts[i].prefix, p, n) && mounts[i].prefix[n] == '/')
			return V_SYNTH;
	}
	return V_NONE;
}

static bool virt_is_link(int id)
{
	return id == V_EXE || id == V_CWD || id == V_FDLINK;
}

static bool virt_is_dir(int id)
{
	return id == V_DEVDIR || id == V_PROCDIR || id == V_SELFDIR || id == V_FDDIR || id == V_SYNTH;
}

/* A growing text buffer for generated /proc files. */
struct vbuf {
	char *s;
	size_t len, cap;
};

static void vb_printf(struct vbuf *b, const char *fmt, ...) MOS_PRINTF(2, 3);
static void vb_printf(struct vbuf *b, const char *fmt, ...)
{
	va_list ap;
	char line[160];
	int n;

	va_start(ap, fmt);
	n = kvsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0 || !b->s)
		return;
	if ((size_t)n >= sizeof(line))
		n = sizeof(line) - 1;
	if (b->len + (size_t)n + 1 > b->cap) {
		size_t cap = b->cap * 2 + 4096;
		char *ns = kzalloc(cap);

		if (!ns) {
			kfree(b->s);
			b->s = NULL;
			return;
		}
		memcpy(ns, b->s, b->len);
		kfree(b->s);
		b->s = ns;
		b->cap = cap;
	}
	memcpy(b->s + b->len, line, (size_t)n);
	b->len += (size_t)n;
	b->s[b->len] = '\0';
}

static void gen_maps(struct task *t, struct vbuf *b)
{
	for (u32 i = 0; i < t->nvma; i++) {
		const struct vma *v = &t->vma[i];
		const char *name = v->end == USER_STACK_TOP ? "[stack]" : (v->flags & VMA_HEAP) ? "[heap]" : "";

		vb_printf(b, "%012llx-%012llx %c%c%c%c %08llx 00:00 0 %s\n", (unsigned long long)v->start,
			  (unsigned long long)v->end, (v->prot & VMM_R) ? 'r' : '-',
			  (v->prot & VMM_W) ? 'w' : '-', (v->prot & VMM_X) ? 'x' : '-',
			  (v->flags & VMA_SHARED) ? 's' : 'p', (unsigned long long)v->foff, name);
	}
}

static void gen_meminfo(struct vbuf *b)
{
	struct pmm_stats st;

	pmm_get_stats(&st);
	vb_printf(b, "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\n"
		  "Buffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\n"
		  "SwapFree:              0 kB\n", (unsigned long long)st.usable_frames * 4,
		  (unsigned long long)st.free_frames * 4, (unsigned long long)st.free_frames * 4);
}

static void gen_cpuinfo(struct vbuf *b)
{
	vb_printf(b, "processor\t: 0\nvendor_id\t: MergedOS\ncpu family\t: 6\nmodel\t\t: 1\n"
		  "model name\t: x86-64\ncpu MHz\t\t: 1000.000\nflags\t\t: fpu sse sse2\n\n");
}

/* A read-only text file whose content was generated at open. */
static long vtext_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	struct vbuf *b = f->priv;
	u64 n = f->pos < b->len ? MOS_MIN(len, b->len - f->pos) : 0;

	if (n && uaccess_copy_out(t, ubuf, b->s + f->pos, (size_t)n))
		return -MERR_FAULT;
	f->pos += n;
	return (long)n;
}

static void vtext_release(struct file *f)
{
	struct vbuf *b = f->priv;

	kfree(b->s);
	kfree(b);
}

static const struct fops vtext_ops = { .read = vtext_read, .release = vtext_release, .mode = S_IFREG | 0444 };

static long null_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	(void)t; (void)f; (void)ubuf; (void)len;
	return 0;
}

static long null_write(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	(void)f;
	if (vfs_debug) {                        /* development: show what daemons send to /dev/null */
		char chunk[128];

		for (u64 done = 0; done < len;) {
			size_t n = (size_t)MOS_MIN(len - done, sizeof(chunk));

			if (uaccess_copy_in(t, chunk, ubuf + done, n))
				break;
			console_write(chunk, n);
			done += n;
		}
	}
	return (long)len;
}

static long zero_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	static const u8 zeros[256];
	u64 done = 0;

	(void)f;
	while (done < len) {
		u64 n = MOS_MIN(len - done, (u64)sizeof(zeros));

		if (uaccess_copy_out(t, ubuf + done, zeros, (size_t)n))
			return done ? (long)done : -MERR_FAULT;
		done += n;
	}
	return (long)done;
}

static long random_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	(void)f;
	return msys_getrandom(t, ubuf, len);
}

static const struct fops null_ops = { .read = null_read, .write = null_write, .mode = S_IFCHR | 0666 };
static const struct fops zero_ops = { .read = zero_read, .write = null_write, .mode = S_IFCHR | 0666 };
static const struct fops random_ops = { .read = random_read, .write = null_write, .mode = S_IFCHR | 0666 };
static const struct fops vdir_ops = { .mode = S_IFDIR | 0555 };

static long vfs_open_virtual(struct task *t, int id, const char *canon, u32 flags, struct file **out)
{
	struct file *f = NULL;
	struct vbuf *b;

	switch (id) {
	case V_NULL:    f = file_new_ops(&null_ops, NULL, flags); break;
	case V_ZERO:    f = file_new_ops(&zero_ops, NULL, flags); break;
	case V_MWM: {
		long rc = mwm_open();

		if (rc)
			return rc;
		f = file_new_ops(&mwm_ops, NULL, flags | VFS_O_RDWR);
		if (!f)
			mwm_close();
		break;
	}
	case V_URANDOM: f = file_new_ops(&random_ops, NULL, flags); break;
	case V_TTY:     f = console_file(); break;
	case V_MAPS: case V_MEMINFO: case V_CPUINFO:
		b = kzalloc(sizeof(*b));
		if (!b)
			return -MERR_NOMEM;
		b->cap = 4096;
		b->s = kzalloc(b->cap);
		if (id == V_MAPS)
			gen_maps(t, b);
		else if (id == V_MEMINFO)
			gen_meminfo(b);
		else
			gen_cpuinfo(b);
		if (!b->s) {
			kfree(b);
			return -MERR_NOMEM;
		}
		f = file_new_ops(&vtext_ops, b, flags);
		if (!f) {
			kfree(b->s);
			kfree(b);
		}
		break;
	default:
		if (virt_is_dir(id))
			f = file_new_ops(&vdir_ops, NULL, flags);
		else
			return -MERR_LOOP;      /* a /proc link opened with O_NOFOLLOW */
	}
	if (!f)
		return -MERR_NOMEM;
	f->path = kstrdup(canon);
	*out = f;
	return 0;
}

/* ---- paths ------------------------------------------------------------------ */

static long copy_path(struct task *t, u64 upath, char *buf, size_t cap)
{
	for (size_t i = 0; i < cap; i++) {
		if (uaccess_copy_in(t, &buf[i], upath + i, 1))
			return -MERR_FAULT;
		if (!buf[i])
			return i ? 0 : -MERR_NOENT;
	}
	return -MERR_NAMETOOLONG;
}

/* The ramfs mount holding `path` (canonical), and the path inside it. */
static struct rnode *mount_of(const char *path, const char **rest, unsigned *index)
{
	for (unsigned i = 0; i < ARRAY_SIZE(mounts); i++) {
		size_t n = strlen(mounts[i].prefix);

		if (!strncmp(path, mounts[i].prefix, n) && (path[n] == '/' || !path[n])) {
			*rest = path + n;
			if (index)
				*index = i;
			return mounts[i].root;
		}
	}
	return NULL;
}

static int parse_dec(const char *s)
{
	int v = 0;

	for (; *s >= '0' && *s <= '9'; s++)
		v = v * 10 + (*s - '0');
	return v;
}

/* Target of the symlink at canonical path `p`, or false. */
static bool link_target(const char *p, char *buf, size_t cap)
{
	const char *rest;
	struct rnode *root, *n;

	if (!strncmp(p, "proc/self/", 10)) {
		struct task *t = task_current();
		int id = virt_lookup(p);

		if (!t || !virt_is_link(id))
			return false;
		if (id == V_EXE) {
			mos_strlcpy(buf, t->exe[0] == '/' ? t->exe : "/", cap);
			if (t->exe[0] != '/')
				mos_strlcpy(buf + 1, t->exe, cap - 1);
		} else if (id == V_CWD) {
			buf[0] = '/';
			mos_strlcpy(buf + 1, t->cwd, cap - 1);
		} else {
			struct file *f = fd_file(t, parse_dec(p + 13));

			if (!f || !f->path)
				return false;
			buf[0] = '/';
			mos_strlcpy(buf + 1, f->path, cap - 1);
		}
		return true;
	}
	root = mount_of(p, &rest, NULL);
	if (!root || ramfs_lookup(root, rest, &n) || !n->symlink)
		return false;
	if (n->size >= cap)
		return false;
	memcpy(buf, n->data, (size_t)n->size);
	buf[n->size] = '\0';
	return true;
}

/*
 * Canonicalise `in` (absolute, or a base joined with a relative path) into
 * `out`: "." and ".." folded, symlinks followed (the last component only
 * with follow_last).
 */
static long resolve(const char *in, char *out, bool follow_last)
{
	char work[PATH_MAX_LEN], target[PATH_MAX_LEN], nw[PATH_MAX_LEN];
	const char *p = work;
	size_t o = 0, inlen = strlen(in);
	int loops = 0;

	if (inlen >= sizeof(work))
		return -MERR_NAMETOOLONG;
	memcpy(work, in, inlen + 1);
	out[0] = '\0';
	for (;;) {
		const char *s, *q;
		size_t len, parent, tl, ql;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		s = p;
		while (*p && *p != '/')
			p++;
		len = (size_t)(p - s);
		if (len == 1 && s[0] == '.')
			continue;
		if (len == 2 && s[0] == '.' && s[1] == '.') {
			while (o && out[o - 1] != '/')
				o--;
			if (o)
				o--;
			out[o] = '\0';
			continue;
		}
		parent = o;
		if (o + len + 2 > PATH_MAX_LEN)
			return -MERR_NAMETOOLONG;
		if (o)
			out[o++] = '/';
		memcpy(out + o, s, len);
		o += len;
		out[o] = '\0';
		q = p;
		while (*q == '/')
			q++;
		if (!*q && !follow_last)
			break;
		if (!link_target(out, target, sizeof(target)))
			continue;
		if (++loops > 16)
			return -MERR_LOOP;
		tl = strlen(target);
		ql = strlen(q);
		if (tl + ql + 2 > sizeof(nw))
			return -MERR_NAMETOOLONG;
		memcpy(nw, target, tl);
		nw[tl] = '/';
		memcpy(nw + tl + 1, q, ql + 1);
		memcpy(work, nw, tl + ql + 2);
		p = work;
		o = target[0] == '/' ? 0 : parent;      /* relative links start at their directory */
		out[o] = '\0';
	}
	return 0;
}

/* Base directory (canonical) for a relative path: the cwd or an open directory. */
static long base_of(struct task *t, int dirfd, const char **base)
{
	struct file *f;

	if (dirfd == VFS_AT_FDCWD) {
		*base = t->cwd;
		return 0;
	}
	f = fd_file(t, dirfd);
	if (!f)
		return -MERR_BADF;
	if (!f->path || !(f->type == F_RODIR || (f->type == F_RAMFS && f->node->dir) ||
			  (f->type == F_OPS && f->ops == &vdir_ops)))
		return -MERR_NOTDIR;
	*base = f->path;
	return 0;
}

long vfs_path(struct task *t, int dirfd, u64 upath, bool follow, char *canon)
{
	char raw[PATH_MAX_LEN], joined[2 * PATH_MAX_LEN];
	const char *base = "";
	long rc = copy_path(t, upath, raw, sizeof(raw));

	if (rc)
		return rc;
	if (raw[0] != '/') {
		rc = base_of(t, dirfd, &base);
		if (rc)
			return rc;
	}
	ksnprintf(joined, sizeof(joined), "%s/%s", base, raw);
	return resolve(joined, canon, follow);
}

/* Resolve a kernel-side path (exec, interpreters): always from the root. */
static long kernel_path(const char *path, char *canon)
{
	char joined[2 * PATH_MAX_LEN];

	ksnprintf(joined, sizeof(joined), "/%s", path);
	return resolve(joined, canon, true);
}

enum { L_NONE, L_RAMFS, L_TAR_FILE, L_TAR_DIR, L_VIRT };

struct loc {
	int kind, vid;
	unsigned mnt;
	struct rnode *root, *node;
	const char *rest;
	const u8 *data;
	size_t size;
};

static void locate(const char *canon, struct loc *l)
{
	memset(l, 0, sizeof(*l));
	l->vid = virt_lookup(canon);
	if (l->vid) {
		l->kind = L_VIRT;
		return;
	}
	l->root = mount_of(canon, &l->rest, &l->mnt);
	if (l->root) {
		l->kind = ramfs_lookup(l->root, l->rest, &l->node) ? L_NONE : L_RAMFS;
		return;
	}
	if (!initrd_find(canon, &l->data, &l->size))
		l->kind = L_TAR_FILE;
	else if (!*canon || initrd_is_dir(canon))
		l->kind = L_TAR_DIR;
}

static u64 hash_path(const char *p)
{
	u64 h = 1;

	for (; *p; p++)
		h = h * 131 + (u8)*p;
	return h;
}

static void kstat_node(struct kstat *st, const struct rnode *n, u32 dev)
{
	st->dev = dev;
	st->ino = n->ino;
	st->mode = (n->dir ? S_IFDIR : n->symlink ? S_IFLNK : n->sock ? S_IFSOCK : S_IFREG) | n->mode;
	st->nlink = n->dir ? 2 : 1;
	st->size = n->size;
	st->atime = n->atime;
	st->mtime = n->mtime;
	st->ctime = n->ctime;
}

static void kstat_loc(const char *canon, const struct loc *l, struct kstat *st)
{
	memset(st, 0, sizeof(*st));
	st->nlink = 1;
	switch (l->kind) {
	case L_RAMFS:
		kstat_node(st, l->node, 0x100 + l->mnt);
		break;
	case L_TAR_FILE:
		st->dev = 1;
		st->ino = (u64)(uintptr_t)l->data;
		st->mode = S_IFREG | 0755;
		st->size = l->size;
		break;
	case L_TAR_DIR:
		st->dev = 1;
		st->ino = hash_path(canon);
		st->mode = S_IFDIR | 0755;
		st->nlink = 2;
		break;
	case L_VIRT:
		st->dev = 2;
		st->ino = 0x10000 + (u64)l->vid;
		st->mode = virt_is_dir(l->vid) ? S_IFDIR | 0555 :
			   virt_is_link(l->vid) ? S_IFLNK | 0777 :
			   (l->vid == V_NULL || l->vid == V_ZERO || l->vid == V_URANDOM || l->vid == V_TTY) ?
			   S_IFCHR | 0666 : S_IFREG | 0444;
		break;
	}
}

static long stat_canon(const char *canon, struct kstat *st)
{
	struct loc l;

	locate(canon, &l);
	if (l.kind == L_NONE)
		return -MERR_NOENT;
	kstat_loc(canon, &l, st);
	vfs_last_ino = st->ino;
	return 0;
}

long vfs_stat_at(struct task *t, int dirfd, u64 upath, bool follow, struct kstat *st)
{
	char canon[PATH_MAX_LEN];
	long rc = vfs_path(t, dirfd, upath, follow, canon);

	return rc ? rc : stat_canon(canon, st);
}

void vfs_fstat(const struct file *f, struct kstat *st)
{
	memset(st, 0, sizeof(*st));
	st->nlink = 1;
	st->ino = (u64)(uintptr_t)f;
	switch (f->type) {
	case F_CONSOLE:
		st->mode = S_IFCHR | 0620;
		st->dev = 2;
		break;
	case F_PIPE:
		st->mode = S_IFIFO | 0600;
		st->dev = 3;
		st->ino = (u64)(uintptr_t)f->pipe;
		break;
	case F_ROFILE:
		st->mode = S_IFREG | 0755;
		st->dev = 1;
		st->ino = (u64)(uintptr_t)f->data;
		st->size = f->size;
		break;
	case F_RODIR:
		st->mode = S_IFDIR | 0755;
		st->dev = 1;
		st->ino = f->path ? hash_path(f->path) : 1;
		st->nlink = 2;
		break;
	case F_RAMFS: {
		const char *rest;
		unsigned mnt = 0xff;

		if (f->path)
			mount_of(f->path, &rest, &mnt);
		kstat_node(st, f->node, 0x100 + mnt);
		break;
	}
	default:
		st->mode = f->ops->mode;
		st->dev = 4;
		if (S_ISDIR_MODE(st->mode))
			st->nlink = 2;
		break;
	}
	vfs_last_ino = st->ino;
}

/* ---- open ---------------------------------------------------------------------- */

long vfs_open_canon(struct task *t, const char *canon, u32 flags, u32 mode, struct file **out)
{
	bool writing = (flags & VFS_O_ACCMODE) != 0 && !(flags & VFS_O_PATH);
	struct file *f = NULL;
	struct loc l;
	long rc;

	locate(canon, &l);
	if (l.kind == L_NONE && (flags & VFS_O_CREAT)) {
		struct rnode *n;

		if (!l.root)
			return -MERR_ACCES;     /* the ramdisk cannot be written */
		rc = ramfs_create(l.root, l.rest, false, true, &n);
		if (rc)
			return rc;
		n->mode = mode & ~t->umask & 07777;
		n->atime = n->mtime = n->ctime = msys_clock_ns();
		l.kind = L_RAMFS;
		l.node = n;
	} else if (l.kind != L_NONE && (flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) {
		return -MERR_EXIST;
	}
	switch (l.kind) {
	case L_NONE:
		return -MERR_NOENT;
	case L_VIRT:
		rc = vfs_open_virtual(t, l.vid, canon, flags, &f);
		if (rc)
			return rc;
		if (f->type == F_OPS && f->ops != &vdir_ops && (flags & VFS_O_DIRECTORY)) {
			file_put(f);
			return -MERR_NOTDIR;
		}
		*out = f;
		return 0;
	case L_TAR_DIR:
		if (writing)
			return -MERR_ISDIR;
		f = file_new(F_RODIR, flags);
		break;
	case L_TAR_FILE:
		if (writing)
			return -MERR_ACCES;
		if (flags & VFS_O_DIRECTORY)
			return -MERR_NOTDIR;
		f = file_new(F_ROFILE, flags);
		if (f) {
			f->data = l.data;
			f->size = l.size;
		}
		break;
	case L_RAMFS:
		if (l.node->symlink && !(flags & VFS_O_PATH))
			return -MERR_LOOP;
		if (l.node->dir && writing)
			return -MERR_ISDIR;
		if (!l.node->dir && (flags & VFS_O_DIRECTORY))
			return -MERR_NOTDIR;
		if ((flags & VFS_O_TRUNC) && writing && !l.node->dir) {
			ramfs_truncate(l.node, 0);
			l.node->mtime = l.node->ctime = msys_clock_ns();
		}
		f = file_new(F_RAMFS, flags);
		if (f) {
			f->node = l.node;
			ramfs_open(l.node);
		}
		break;
	}
	if (!f)
		return -MERR_NOMEM;
	f->path = kstrdup(canon);
	*out = f;
	return 0;
}

long vfs_open_at(struct task *t, int dirfd, u64 upath, u32 flags, u32 mode)
{
	char canon[PATH_MAX_LEN];
	struct file *f;
	bool nofollow = (flags & VFS_O_NOFOLLOW) || ((flags & VFS_O_CREAT) && (flags & VFS_O_EXCL));
	long rc = vfs_path(t, dirfd, upath, !nofollow, canon);

	if (rc)
		return rc;
	rc = vfs_open_canon(t, canon, flags, mode, &f);
	if (vfs_debug && rc)
		kprintf("[vfs] open /%s flags %x: error %ld\n", canon, flags, rc);
	if (rc)
		return rc;
	rc = fd_install(t, f, 0, flags & VFS_O_CLOEXEC);
	if (vfs_debug)
		kprintf("[vfs] pid %d open /%s = %ld\n", t->pid, canon, rc);
	return rc;
}

long msys_open(struct task *t, u64 upath, u32 flags)
{
	return vfs_open_at(t, VFS_AT_FDCWD, upath, flags, 0666);
}

int vfs_image(const char *path, const u8 **data, size_t *size)
{
	char canon[PATH_MAX_LEN];
	struct loc l;
	long rc = kernel_path(path, canon);

	if (rc)
		return (int)rc;
	locate(canon, &l);
	switch (l.kind) {
	case L_TAR_FILE:
		*data = l.data;
		*size = l.size;
		return 0;
	case L_RAMFS:
		if (l.node->dir)
			return -MERR_ISDIR;
		*data = l.node->data;
		*size = (size_t)l.node->size;
		return 0;
	case L_NONE:
		return -MERR_NOENT;
	default:
		return -MERR_ISDIR;
	}
}

/* ---- terminal input ----------------------------------------------------------- */

static char edit[256], ready[512];
static u32 edit_len, ready_len;
static bool stdin_eof;

static bool stdin_ready(void)
{
	return ready_len || stdin_eof;
}

static void commit_line(void)
{
	u32 n = MOS_MIN(edit_len, (u32)sizeof(ready) - ready_len);

	memcpy(ready + ready_len, edit, n);
	ready_len += n;
	edit_len = 0;
	thread_wake(ready, 0, SCHED_MAX_THREADS);
	poll_wake();
}

/* Cooked mode: echo, backspace, line at a time, Ctrl-D for end of file. */
void stdin_feed(int c)
{
	if (c == 4) {
		if (!edit_len)
			stdin_eof = true;
		commit_line();
	} else if (c == '\b' || c == 0x7F) {
		if (edit_len) {
			edit_len--;
			console_write("\b \b", 3);
		}
	} else if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) {
		char ch = (char)c;

		if (edit_len < sizeof(edit))
			edit[edit_len++] = ch;
		console_write(&ch, 1);
		if (c == '\n' || edit_len == sizeof(edit))
			commit_line();
	}
}

void stdin_drain_to_console(void)
{
	char buf[sizeof(ready) + sizeof(edit) + 1];
	u32 n = 0;

	memcpy(buf, ready, ready_len);
	n = ready_len;
	memcpy(buf + n, edit, edit_len);
	n += edit_len;
	buf[n] = '\0';
	ready_len = edit_len = 0;
	stdin_eof = false;
	if (n)
		console_inject(buf);
}

static long stdin_read(struct task *t, u64 ubuf, u64 len)
{
	if (t->pgid != fg_pgid)
		return 0;                       /* background: end of file */
	for (;;) {
		if (ready_len) {
			u32 n = (u32)MOS_MIN(len, (u64)ready_len);

			if (uaccess_copy_out(t, ubuf, ready, n))
				return -MERR_FAULT;
			memmove(ready, ready + n, ready_len - n);
			ready_len -= n;
			return n;
		}
		if (stdin_eof) {
			stdin_eof = false;
			return 0;
		}
		if (task_signal_pending())
			return -MERR_INTR;
		thread_block(ready, 0, 0);
	}
}

/* ---- pipes ------------------------------------------------------------------- */

long msys_pipe(struct task *t, int fds[2], u32 flags)
{
	struct pipe *p = kzalloc(sizeof(*p));
	struct file *r, *w;
	bool cloexec = flags & VFS_O_CLOEXEC;

	if (!p)
		return -MERR_NOMEM;
	r = file_new(F_PIPE, flags & VFS_O_NONBLOCK);
	w = file_new(F_PIPE, VFS_O_WRONLY | (flags & VFS_O_NONBLOCK));
	if (!r || !w) {
		kfree(r);
		kfree(w);
		kfree(p);
		return -MERR_NOMEM;
	}
	r->pipe = w->pipe = p;
	w->pipe_write = true;
	p->readers = p->writers = 1;
	fds[0] = fd_install(t, r, 0, cloexec);
	if (fds[0] < 0) {
		file_put(w);
		return fds[0];
	}
	fds[1] = fd_install(t, w, 0, cloexec);
	if (fds[1] < 0) {
		fd_drop(t, fds[0]);
		return fds[1];
	}
	return 0;
}

static long pipe_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	struct pipe *p = f->pipe;

	if (f->pipe_write)
		return -MERR_BADF;
	while (!p->count) {
		if (!p->writers)
			return 0;
		if (f->flags & VFS_O_NONBLOCK)
			return -MERR_AGAIN;
		if (task_signal_pending())
			return -MERR_INTR;
		thread_block(p, 0, 0);
	}
	{
		u32 n = (u32)MOS_MIN(len, (u64)p->count), first = MOS_MIN(n, PIPE_SIZE - p->rpos);

		if (uaccess_copy_out(t, ubuf, p->buf + p->rpos, first) ||
		    uaccess_copy_out(t, ubuf + first, p->buf, n - first))
			return -MERR_FAULT;
		p->rpos = (p->rpos + n) % PIPE_SIZE;
		p->count -= n;
		thread_wake(p, 0, SCHED_MAX_THREADS);
		poll_wake();
		return n;
	}
}

static long pipe_write(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	struct pipe *p = f->pipe;
	u64 done = 0;

	if (!f->pipe_write)
		return -MERR_BADF;
	while (done < len) {
		u32 space, wpos, n, first;

		if (!p->readers) {
			task_signal(t, 13);     /* SIGPIPE */
			return done ? (long)done : -MERR_PIPE;
		}
		space = PIPE_SIZE - p->count;
		if (!space) {
			if (f->flags & VFS_O_NONBLOCK)
				return done ? (long)done : -MERR_AGAIN;
			if (task_signal_pending())
				return done ? (long)done : -MERR_INTR;
			thread_block(p, 0, 0);
			continue;
		}
		wpos = (p->rpos + p->count) % PIPE_SIZE;
		n = (u32)MOS_MIN(len - done, (u64)space);
		first = MOS_MIN(n, PIPE_SIZE - wpos);
		if (uaccess_copy_in(t, p->buf + wpos, ubuf + done, first) ||
		    uaccess_copy_in(t, p->buf, ubuf + done + first, n - first))
			return done ? (long)done : -MERR_FAULT;
		p->count += n;
		done += n;
		thread_wake(p, 0, SCHED_MAX_THREADS);
		poll_wake();
	}
	return (long)done;
}

/* ---- reading and writing ---------------------------------------------------------- */

/* Everything any process writes to the console while a test listens (child processes included). */
static char *tee_buf;
static size_t tee_cap, tee_len;

void vfs_console_tee(char *buf, size_t cap)
{
	tee_buf = buf;
	tee_cap = cap;
	tee_len = 0;
	if (buf && cap)
		buf[0] = '\0';
}

static long console_out(struct task *t, u64 ubuf, u64 len)
{
	char chunk[128];
	u64 done = 0;

	while (done < len) {
		size_t n = (size_t)MOS_MIN(len - done, sizeof(chunk));

		if (uaccess_copy_in(t, chunk, ubuf + done, n))
			return done ? (long)done : -MERR_FAULT;
		console_write(chunk, n);
		if (tee_buf && tee_len + n + 1 <= tee_cap) {
			memcpy(tee_buf + tee_len, chunk, n);
			tee_len += n;
			tee_buf[tee_len] = '\0';
		}
		if (t->out_len < sizeof(t->out)) {
			size_t keep = MOS_MIN(n, sizeof(t->out) - t->out_len);

			memcpy(t->out + t->out_len, chunk, keep);
			t->out_len += (u32)keep;
		}
		done += n;
	}
	return (long)done;
}

/* Bytes of a seekable file at `pos` into user memory. */
static long file_pread(struct task *t, struct file *f, u64 ubuf, u64 len, u64 pos)
{
	const u8 *data;
	u64 size;
	long n;

	if (f->type == F_ROFILE) {
		data = f->data;
		size = f->size;
	} else if (f->type == F_RAMFS && !f->node->dir) {
		data = f->node->data;
		size = f->node->size;
	} else {
		return f->type == F_RAMFS || f->type == F_RODIR ? -MERR_ISDIR : -MERR_SPIPE;
	}
	n = (long)MOS_MIN(len, size - MOS_MIN(pos, size));
	if (n && uaccess_copy_out(t, ubuf, data + pos, (size_t)n))
		return -MERR_FAULT;
	return n;
}

long msys_read(struct task *t, int fd, u64 ubuf, u64 len)
{
	struct file *f = fd_file(t, fd);
	long n;

	if (!f || (f->flags & VFS_O_ACCMODE) == VFS_O_WRONLY || (f->flags & VFS_O_PATH))
		return -MERR_BADF;
	if (len > MSYS_IO_MAX)
		len = MSYS_IO_MAX;
	switch (f->type) {
	case F_CONSOLE:
		return stdin_read(t, ubuf, len);
	case F_PIPE:
		return pipe_read(t, f, ubuf, len);
	case F_OPS:
		return f->ops->read ? f->ops->read(t, f, ubuf, len) : -MERR_INVAL;
	default:
		n = file_pread(t, f, ubuf, len, f->pos);
		if (n > 0)
			f->pos += (u64)n;
		return n;
	}
}

/* pread64: read at an explicit offset, leaving the file position untouched. */
long msys_pread(struct task *t, int fd, u64 ubuf, u64 len, u64 offset)
{
	struct file *f = fd_file(t, fd);

	if (!f || (f->flags & VFS_O_ACCMODE) == VFS_O_WRONLY)
		return -MERR_BADF;
	if (len > MSYS_IO_MAX)
		len = MSYS_IO_MAX;
	return file_pread(t, f, ubuf, len, offset);
}

static long ramfs_put(struct task *t, struct rnode *n, u64 pos, u64 ubuf, u64 len)
{
	/* Grow the buffer, then copy straight from user memory into it. */
	u64 old = n->size;
	long rc = ramfs_truncate(n, MOS_MAX(n->size, pos + len));

	if (rc)
		return rc;
	if (pos > old)
		memset(n->data + old, 0, (size_t)(pos - old));
	if (uaccess_copy_in(t, n->data + pos, ubuf, (size_t)len))
		return -MERR_FAULT;
	n->mtime = n->ctime = msys_clock_ns();
	return (long)len;
}

long msys_write(struct task *t, int fd, u64 ubuf, u64 len)
{
	struct file *f = fd_file(t, fd);
	long rc;

	if (!f || (f->flags & VFS_O_ACCMODE) == 0 || (f->flags & VFS_O_PATH))
		return -MERR_BADF;
	if (len > MSYS_IO_MAX)
		len = MSYS_IO_MAX;
	switch (f->type) {
	case F_CONSOLE:
		return console_out(t, ubuf, len);
	case F_PIPE:
		return pipe_write(t, f, ubuf, len);
	case F_OPS:
		return f->ops->write ? f->ops->write(t, f, ubuf, len) : -MERR_BADF;
	case F_RAMFS: {
		u64 pos = (f->flags & VFS_O_APPEND) ? f->node->size : f->pos;

		if (f->node->dir)
			return -MERR_ISDIR;
		rc = ramfs_put(t, f->node, pos, ubuf, len);
		if (rc > 0)
			f->pos = pos + (u64)rc;
		return rc;
	}
	default:
		return -MERR_BADF;
	}
}

long vfs_pwrite(struct task *t, struct file *f, u64 ubuf, u64 len, u64 off)
{
	if (f->type != F_RAMFS || f->node->dir || (f->flags & VFS_O_ACCMODE) == 0)
		return f->type == F_RAMFS ? -MERR_BADF : -MERR_SPIPE;
	if (len > MSYS_IO_MAX)
		len = MSYS_IO_MAX;
	return ramfs_put(t, f->node, off, ubuf, len);
}

long vfs_ftruncate(struct task *t, struct file *f, u64 size)
{
	(void)t;
	if (f->type != F_RAMFS || f->node->dir || (f->flags & VFS_O_ACCMODE) == 0)
		return -MERR_INVAL;
	f->node->mtime = f->node->ctime = msys_clock_ns();
	return ramfs_truncate(f->node, size);
}

long msys_close(struct task *t, int fd)
{
	if (!fd_file(t, fd))
		return -MERR_BADF;
	fd_drop(t, fd);
	return 0;
}

long msys_lseek(struct task *t, int fd, s64 offset, int whence)
{
	struct file *f = fd_file(t, fd);
	s64 base, size;

	if (!f)
		return -MERR_BADF;
	if (f->type == F_CONSOLE || f->type == F_PIPE || f->type == F_OPS)
		return -MERR_SPIPE;
	size = f->type == F_ROFILE ? (s64)f->size : f->type == F_RAMFS ? (s64)f->node->size : 0;
	base = whence == 0 ? 0 : whence == 1 ? (s64)f->pos : whence == 2 ? size : -1;
	if (base < 0 || base + offset < 0 || (f->type == F_ROFILE && base + offset > size))
		return -MERR_INVAL;
	f->pos = (u64)(base + offset);
	return (long)f->pos;
}

/* Success returns the kind of object (MSYS_ST_*). */
long msys_fstat(struct task *t, int fd, u64 *size, bool *is_tty)
{
	struct file *f = fd_file(t, fd);
	struct kstat st;

	if (!f)
		return -MERR_BADF;
	vfs_fstat(f, &st);
	*is_tty = f->type == F_CONSOLE;
	*size = st.size;
	switch (st.mode & S_IFMT) {
	case S_IFCHR:  return MSYS_ST_TTY;
	case S_IFIFO:  return MSYS_ST_FIFO;
	case S_IFDIR:  return MSYS_ST_DIR;
	default:       return f->type == F_RAMFS ? MSYS_ST_FILE_RW : MSYS_ST_FILE;
	}
}

long msys_stat(struct task *t, u64 upath, u64 *size, bool *is_dir)
{
	struct kstat st;
	long rc = vfs_stat_at(t, VFS_AT_FDCWD, upath, true, &st);

	if (rc)
		return rc;
	*size = st.size;
	*is_dir = (st.mode & S_IFMT) == S_IFDIR;
	if (*is_dir)
		return MSYS_ST_DIR;
	return st.dev >= 0x100 ? MSYS_ST_FILE_RW : MSYS_ST_FILE;
}

long msys_dup(struct task *t, int fd, int min, bool cloexec)
{
	struct file *f = fd_file(t, fd);

	if (!f)
		return -MERR_BADF;
	return fd_install(t, file_get(f), min, cloexec);
}

long msys_dup2(struct task *t, int oldfd, int newfd, bool cloexec)
{
	struct file *f = fd_file(t, oldfd);

	if (!f || newfd < 0 || newfd >= TASK_FD_MAX)
		return -MERR_BADF;
	if (oldfd == newfd)
		return newfd;
	file_get(f);
	fd_drop(t, newfd);
	return fd_install(t, f, newfd, cloexec);
}

long msys_fcntl(struct task *t, int fd, int cmd, u64 arg)
{
	struct file *f = fd_file(t, fd);

	if (!f)
		return -MERR_BADF;
	switch (cmd) {
	case MSYS_F_DUPFD:
	case MSYS_F_DUPFD_CLOEXEC:
		return msys_dup(t, fd, (int)arg, cmd == MSYS_F_DUPFD_CLOEXEC);
	case MSYS_F_GETFD:
		return CLOEXEC_GET(t, fd);
	case MSYS_F_SETFD:
		CLOEXEC_SET(t, fd, arg & 1);
		return 0;
	case MSYS_F_GETFL:
		return f->flags;
	case MSYS_F_SETFL:
		f->flags = (f->flags & ~(u32)(VFS_O_APPEND | VFS_O_NONBLOCK)) |
			   ((u32)arg & (VFS_O_APPEND | VFS_O_NONBLOCK));
		return 0;
	default:
		return -MERR_INVAL;
	}
}

/* ---- namespace changes ------------------------------------------------------------ */

static void touch_parent(struct rnode *root, const char *rest)
{
	struct rnode *n;
	char dir[PATH_MAX_LEN];
	size_t len = strlen(rest);

	while (len && rest[len - 1] != '/')
		len--;
	memcpy(dir, rest, len);
	dir[len] = '\0';
	if (!ramfs_lookup(root, dir, &n))
		n->mtime = n->ctime = msys_clock_ns();
}

long vfs_mkdir_at(struct task *t, int dirfd, u64 upath, u32 mode)
{
	char canon[PATH_MAX_LEN];
	struct loc l;
	struct rnode *n;
	long rc = vfs_path(t, dirfd, upath, false, canon);

	if (rc)
		return rc;
	locate(canon, &l);
	if (l.kind != L_NONE)
		return -MERR_EXIST;
	if (!l.root)
		return -MERR_ACCES;
	rc = ramfs_create(l.root, l.rest, true, true, &n);
	if (!rc) {
		n->mode = mode & ~t->umask & 07777;
		n->atime = n->mtime = n->ctime = msys_clock_ns();
		touch_parent(l.root, l.rest);
	}
	return rc;
}

long msys_mkdir(struct task *t, u64 upath)
{
	return vfs_mkdir_at(t, VFS_AT_FDCWD, upath, 0777);
}

long vfs_unlink_at(struct task *t, int dirfd, u64 upath, bool dir)
{
	char canon[PATH_MAX_LEN];
	struct loc l;
	long rc = vfs_path(t, dirfd, upath, false, canon);

	if (rc)
		return rc;
	locate(canon, &l);
	if (l.kind == L_NONE)
		return -MERR_NOENT;
	if (!l.root)
		return -MERR_ROFS;
	rc = ramfs_unlink(l.root, l.rest, dir);
	if (!rc)
		touch_parent(l.root, l.rest);
	return rc;
}

long msys_unlink(struct task *t, u64 upath, bool dir)
{
	return vfs_unlink_at(t, VFS_AT_FDCWD, upath, dir);
}

long vfs_rename_at(struct task *t, int fromfd, u64 ufrom, int tofd, u64 uto)
{
	char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
	struct loc a, b;
	long rc = vfs_path(t, fromfd, ufrom, false, from);

	if (!rc)
		rc = vfs_path(t, tofd, uto, false, to);
	if (rc)
		return rc;
	locate(from, &a);
	locate(to, &b);
	if (a.kind == L_NONE)
		return -MERR_NOENT;
	if (!a.root || !b.root)
		return -MERR_ROFS;
	if (a.root != b.root)
		return -MERR_XDEV;
	rc = ramfs_rename(a.root, a.rest, b.rest);
	if (!rc) {
		touch_parent(a.root, a.rest);
		touch_parent(b.root, b.rest);
	}
	return rc;
}

long msys_rename(struct task *t, u64 ufrom, u64 uto)
{
	return vfs_rename_at(t, VFS_AT_FDCWD, ufrom, VFS_AT_FDCWD, uto);
}

long vfs_symlink_at(struct task *t, u64 utarget, int dirfd, u64 ulink)
{
	char canon[PATH_MAX_LEN], target[PATH_MAX_LEN];
	struct loc l;
	struct rnode *n;
	long rc = copy_path(t, utarget, target, sizeof(target));

	if (!rc)
		rc = vfs_path(t, dirfd, ulink, false, canon);
	if (rc)
		return rc;
	locate(canon, &l);
	if (l.kind != L_NONE)
		return -MERR_EXIST;
	if (!l.root)
		return -MERR_ACCES;
	rc = ramfs_create(l.root, l.rest, false, true, &n);
	if (rc)
		return rc;
	rc = ramfs_write(n, 0, target, strlen(target));
	if (rc < 0) {
		ramfs_unlink(l.root, l.rest, false);
		return rc;
	}
	n->symlink = true;
	n->mode = 0777;
	n->atime = n->mtime = n->ctime = msys_clock_ns();
	touch_parent(l.root, l.rest);
	return 0;
}

long vfs_readlink_at(struct task *t, int dirfd, u64 upath, u64 ubuf, u64 size)
{
	char canon[PATH_MAX_LEN], target[PATH_MAX_LEN];
	size_t n;
	long rc = vfs_path(t, dirfd, upath, false, canon);

	if (rc)
		return rc;
	if (!link_target(canon, target, sizeof(target))) {
		struct kstat st;

		return stat_canon(canon, &st) ? -MERR_NOENT : -MERR_INVAL;
	}
	n = MOS_MIN(strlen(target), (size_t)size);
	return uaccess_copy_out(t, ubuf, target, n) ? -MERR_FAULT : (long)n;
}

long msys_readlink(struct task *t, u64 upath, u64 ubuf, u64 bufsize)
{
	return vfs_readlink_at(t, VFS_AT_FDCWD, upath, ubuf, bufsize);
}

static bool dir_mode(const struct kstat *st)
{
	return (st->mode & S_IFMT) == S_IFDIR;
}

long vfs_chdir(struct task *t, u64 upath)
{
	char canon[PATH_MAX_LEN];
	struct kstat st;
	long rc = vfs_path(t, VFS_AT_FDCWD, upath, true, canon);

	if (!rc)
		rc = stat_canon(canon, &st);
	if (rc)
		return rc;
	if (!dir_mode(&st))
		return -MERR_NOTDIR;
	if (strlen(canon) >= sizeof(t->cwd))
		return -MERR_NAMETOOLONG;
	memcpy(t->cwd, canon, strlen(canon) + 1);
	return 0;
}

long vfs_fchdir(struct task *t, int fd)
{
	struct file *f = fd_file(t, fd);

	if (!f)
		return -MERR_BADF;
	if (!f->path || !(f->type == F_RODIR || (f->type == F_RAMFS && f->node->dir) ||
			  (f->type == F_OPS && f->ops == &vdir_ops)))
		return -MERR_NOTDIR;
	if (strlen(f->path) >= sizeof(t->cwd))
		return -MERR_NAMETOOLONG;
	memcpy(t->cwd, f->path, strlen(f->path) + 1);
	return 0;
}

long vfs_getcwd(struct task *t, u64 ubuf, u64 size)
{
	char buf[TASK_CWD_MAX + 2];
	size_t n;

	buf[0] = '/';
	mos_strlcpy(buf + 1, t->cwd, sizeof(buf) - 1);
	n = strlen(buf) + 1;
	if (size < n)
		return -MERR_RANGE;
	return uaccess_copy_out(t, ubuf, buf, n) ? -MERR_FAULT : (long)n;
}

long vfs_access_at(struct task *t, int dirfd, u64 upath, u32 mode)
{
	struct kstat st;
	long rc = vfs_stat_at(t, dirfd, upath, true, &st);

	(void)mode;
	return rc;
}

long vfs_chmod_at(struct task *t, int dirfd, u64 upath, u32 mode)
{
	char canon[PATH_MAX_LEN];
	struct loc l;
	long rc = vfs_path(t, dirfd, upath, true, canon);

	if (rc)
		return rc;
	locate(canon, &l);
	if (l.kind == L_NONE)
		return -MERR_NOENT;
	if (l.kind == L_RAMFS) {
		l.node->mode = mode & 07777;
		l.node->ctime = msys_clock_ns();
	}
	return 0;
}

long vfs_fchmod(struct task *t, int fd, u32 mode)
{
	struct file *f = fd_file(t, fd);

	if (!f)
		return -MERR_BADF;
	if (f->type == F_RAMFS) {
		f->node->mode = mode & 07777;
		f->node->ctime = msys_clock_ns();
	}
	return 0;
}

long vfs_utimes(struct task *t, struct file *f, const u64 times[2][2], bool have_times)
{
	u64 now = msys_clock_ns();

	(void)t;
	if (f->type != F_RAMFS)
		return 0;
	f->node->atime = have_times ? times[0][0] * 1000000000ULL + times[0][1] : now;
	f->node->mtime = have_times ? times[1][0] * 1000000000ULL + times[1][1] : now;
	f->node->ctime = now;
	return 0;
}

/* memfd_create: an anonymous ramfs file that disappears with its last reference. */
long vfs_memfd_create(struct task *t, const char *name, u32 flags)
{
	static u32 serial;
	char leaf[RAMFS_NAME_MAX];
	struct rnode *n;
	struct file *f;
	long rc;

	ksnprintf(leaf, sizeof(leaf), "m%u", ++serial);
	rc = ramfs_create(anon_root, leaf, false, true, &n);
	if (rc)
		return rc;
	n->atime = n->mtime = n->ctime = msys_clock_ns();
	n->mode = 0777;
	f = file_new(F_RAMFS, VFS_O_RDWR);
	if (!f) {
		ramfs_unlink(anon_root, leaf, false);
		return -MERR_NOMEM;
	}
	f->node = n;
	ramfs_open(n);
	ramfs_unlink(anon_root, leaf, false);   /* nameless from now on; freed at the last close */
	f->path = kstrdup(name);
	return fd_install(t, f, 0, flags & 1);
}

/* Next directory entry of an open directory: 1 and the entry, or 0 at the end. */
static long dir_next(struct file *f, char *name, size_t cap, u32 *mode, u64 *ino)
{
	if (f->type == F_RAMFS) {
		struct rnode *e = ramfs_entry(f->node, f->pos);

		if (!f->node->dir)
			return -MERR_NOTDIR;
		if (!e)
			return 0;
		mos_strlcpy(name, e->name, cap);
		*mode = e->dir ? S_IFDIR : e->symlink ? S_IFLNK : S_IFREG;
		*ino = e->ino;
		f->pos++;
		return 1;
	}
	if (f->type == F_RODIR) {
		size_t cursor = 0, plen = strlen(f->path);
		struct tar_entry e;
		u64 index = 0;

		while (initrd_next(&cursor, &e) > 0) {
			const char *rel = e.name, *slash;

			if (plen && (strncmp(rel, f->path, plen) || rel[plen] != '/'))
				continue;
			rel += plen ? plen + 1 : 0;
			slash = strchr(rel, '/');
			if (!*rel || (slash && slash[1]))
				continue;               /* the directory itself, or deeper */
			if (index++ < f->pos)
				continue;
			mos_strlcpy(name, rel, slash ? MOS_MIN(cap, (size_t)(slash - rel) + 1) : cap);
			*mode = (slash || e.type == '5') ? S_IFDIR : S_IFREG;
			*ino = 1000 + index;
			f->pos++;
			return 1;
		}
		return 0;
	}
	if (f->type == F_OPS && f->ops == &vdir_ops)
		return 0;
	return -MERR_NOTDIR;
}

long msys_readdir(struct task *t, int fd, char *name, size_t cap, bool *is_dir, u64 *ino)
{
	struct file *f = fd_file(t, fd);
	u32 mode = 0;
	long rc;

	if (!f)
		return -MERR_BADF;
	rc = dir_next(f, name, cap, &mode, ino);
	*is_dir = (mode & S_IFMT) == S_IFDIR;
	return rc;
}

/* getdents64: linux_dirent64 records until the buffer is full. */
long vfs_getdents(struct task *t, int fd, u64 ubuf, u64 count)
{
	struct file *f = fd_file(t, fd);
	u64 done = 0;

	if (!f)
		return -MERR_BADF;
	for (;;) {
		u8 ent[19 + RAMFS_NAME_MAX + 8] = { 0 };
		char *name = (char *)ent + 19;
		u32 mode = 0;
		u64 ino = 0, off;
		u16 reclen;
		long rc;
		u64 save = f->pos;

		rc = dir_next(f, name, RAMFS_NAME_MAX, &mode, &ino);
		if (rc < 0)
			return done ? (long)done : rc;
		if (rc == 0)
			break;
		reclen = (u16)ALIGN_UP(19 + strlen(name) + 1, 8);
		if (count - done < reclen) {
			f->pos = save;
			if (!done)
				return -MERR_INVAL;
			break;
		}
		off = f->pos;
		memcpy(ent, &ino, 8);
		memcpy(ent + 8, &off, 8);
		memcpy(ent + 16, &reclen, 2);
		ent[18] = (mode & S_IFMT) == S_IFDIR ? 4 : (mode & S_IFMT) == S_IFLNK ? 10 : 8;
		if (uaccess_copy_out(t, ubuf + done, ent, reclen))
			return -MERR_FAULT;
		done += reclen;
	}
	return (long)done;
}

/* A socket's name in the file system (AF_UNIX bind/connect): the ramfs node, created on bind. */
long vfs_sock_path(struct task *t, const char *path, bool create, struct rnode **out)
{
	char canon[PATH_MAX_LEN];
	struct loc l;
	struct rnode *n;
	char joined[2 * PATH_MAX_LEN];
	long rc;

	ksnprintf(joined, sizeof(joined), "%s/%s", path[0] == '/' ? "" : t->cwd, path);
	rc = resolve(joined, canon, true);
	if (rc)
		return rc;
	locate(canon, &l);
	if (!create) {
		if (l.kind != L_RAMFS)
			return -MERR_NOENT;
		*out = l.node;
		return 0;
	}
	if (l.kind != L_NONE)
		return -MERR_ADDRINUSE;
	if (!l.root)
		return -MERR_ACCES;
	rc = ramfs_create(l.root, l.rest, false, true, &n);
	if (rc)
		return rc;
	n->mode = 0777 & ~t->umask;
	n->atime = n->mtime = n->ctime = msys_clock_ns();
	*out = n;
	return 0;
}
