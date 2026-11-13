/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Open files, descriptor tables, pipes, the terminal and path lookup, plus
 * the file system calls every personality is translated onto.
 *
 * Name space: /tmp and /home/user are writable ramfs mounts; every other
 * path is looked up in the read-only boot ramdisk.
 */
#include <mergedos/kernel.h>
#include <mergedos/console.h>
#include <mergedos/initrd.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>

#define PATH_MAX_LEN 128
#define MSYS_IO_MAX  (1u << 20)

static struct {
	const char *prefix;
	struct rnode *root;
} mounts[] = { { "tmp", NULL }, { "home/user", NULL } };

int fg_pgid;

void vfs_init(void)
{
	for (unsigned i = 0; i < ARRAY_SIZE(mounts); i++)
		mounts[i].root = ramfs_new_root();
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

struct file *file_get(struct file *f)
{
	f->refs++;
	return f;
}

void file_put(struct file *f)
{
	if (--f->refs > 0)
		return;
	if (f->type == F_RAMFS)
		ramfs_close(f->node);
	if (f->type == F_PIPE) {
		struct pipe *p = f->pipe;

		if (f->pipe_write)
			p->writers--;
		else
			p->readers--;
		thread_wake(p, 0, SCHED_MAX_THREADS);
		if (!p->readers && !p->writers)
			kfree(p);
	}
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

int fd_install(struct task *t, struct file *f, int min, bool cloexec)
{
	for (int fd = min < 0 ? 0 : min; fd < TASK_FD_MAX; fd++) {
		if (!t->fd[fd]) {
			t->fd[fd] = f;
			t->cloexec = cloexec ? t->cloexec | (1u << fd) : t->cloexec & ~(1u << fd);
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
	t->cloexec &= ~(1u << fd);
	if (f)
		file_put(f);
}

void fd_close_all(struct task *t)
{
	for (int fd = 0; fd < TASK_FD_MAX; fd++)
		fd_drop(t, fd);
}

void fd_close_on_exec(struct task *t)
{
	for (int fd = 0; fd < TASK_FD_MAX; fd++)
		if (t->cloexec & (1u << fd))
			fd_drop(t, fd);
}

void fd_fork(struct task *child, const struct task *parent)
{
	for (int fd = 0; fd < TASK_FD_MAX; fd++)
		child->fd[fd] = parent->fd[fd] ? file_get(parent->fd[fd]) : NULL;
	child->cloexec = parent->cloexec;
}

/* ---- paths ------------------------------------------------------------------ */

static long copy_path(struct task *t, u64 upath, char *buf, size_t cap)
{
	for (size_t i = 0; i < cap; i++) {
		if (uaccess_copy_in(t, &buf[i], upath + i, 1))
			return -MERR_FAULT;
		if (!buf[i])
			return 0;
	}
	return -MERR_RANGE;
}

/* Canonical form: no leading '/', no "." or "..", single separators. */
static void normalize(const char *in, char *out, size_t cap)
{
	size_t o = 0;

	while (*in) {
		const char *s;
		size_t len;

		while (*in == '/')
			in++;
		s = in;
		while (*in && *in != '/')
			in++;
		len = (size_t)(in - s);
		if (!len || (len == 1 && s[0] == '.'))
			continue;
		if (len == 2 && s[0] == '.' && s[1] == '.') {
			while (o && out[o - 1] != '/')
				o--;
			if (o)
				o--;
			continue;
		}
		if (o + len + 2 > cap)
			break;
		if (o)
			out[o++] = '/';
		memcpy(out + o, s, len);
		o += len;
	}
	out[o] = '\0';
}

/* The ramfs mount holding `path` (canonical), and the path inside it. */
static struct rnode *mount_of(const char *path, const char **rest)
{
	for (unsigned i = 0; i < ARRAY_SIZE(mounts); i++) {
		size_t n = strlen(mounts[i].prefix);

		if (!strncmp(path, mounts[i].prefix, n) && (path[n] == '/' || !path[n])) {
			*rest = path + n;
			return mounts[i].root;
		}
	}
	return NULL;
}

static long user_path(struct task *t, u64 upath, char *canon)
{
	char raw[PATH_MAX_LEN];
	long rc = copy_path(t, upath, raw, sizeof(raw));

	if (rc)
		return rc;
	normalize(raw, canon, PATH_MAX_LEN);
	return 0;
}

int vfs_image(const char *path, const u8 **data, size_t *size)
{
	char canon[PATH_MAX_LEN];
	const char *rest;
	struct rnode *root, *n;
	int rc;

	normalize(path, canon, sizeof(canon));
	root = mount_of(canon, &rest);
	if (!root)
		return initrd_find(canon, data, size) ? -MERR_NOENT : 0;
	rc = ramfs_lookup(root, rest, &n);
	if (rc)
		return rc;
	if (n->dir)
		return -MERR_ISDIR;
	*data = n->data;
	*size = (size_t)n->size;
	return 0;
}

/* ---- terminal input ----------------------------------------------------------- */

static char edit[256], ready[512];
static u32 edit_len, ready_len;
static bool stdin_eof;

static void commit_line(void)
{
	u32 n = MOS_MIN(edit_len, (u32)sizeof(ready) - ready_len);

	memcpy(ready + ready_len, edit, n);
	ready_len += n;
	edit_len = 0;
	thread_wake(ready, 0, SCHED_MAX_THREADS);
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
	}
	return (long)done;
}

/* ---- file system calls ---------------------------------------------------------- */

long msys_open(struct task *t, u64 upath, u32 flags)
{
	char path[PATH_MAX_LEN];
	const char *rest;
	struct rnode *root, *n = NULL;
	struct file *f;
	long rc = user_path(t, upath, path);
	bool writing = (flags & VFS_O_ACCMODE) != 0;

	if (rc)
		return rc;
	root = mount_of(path, &rest);
	if (!root) {
		const u8 *data;
		size_t size;

		if (initrd_find(path, &data, &size)) {
			if (!initrd_is_dir(path) && *path)
				return (flags & VFS_O_CREAT) ? -MERR_ACCES : -MERR_NOENT;
			if (writing)
				return -MERR_ISDIR;
			f = file_new(F_RODIR, flags);
			if (f)
				mos_strlcpy(f->path, path, sizeof(f->path));
		} else {
			if (writing || (flags & VFS_O_CREAT))
				return -MERR_ACCES;     /* the ramdisk cannot be written */
			if (flags & VFS_O_DIRECTORY)
				return -MERR_NOTDIR;
			f = file_new(F_ROFILE, flags);
			if (f) {
				f->data = data;
				f->size = size;
			}
		}
	} else {
		if (flags & VFS_O_CREAT)
			rc = ramfs_create(root, rest, false, flags & VFS_O_EXCL, &n);
		else
			rc = ramfs_lookup(root, rest, &n);
		if (rc)
			return rc;
		if (n->dir && writing)
			return -MERR_ISDIR;
		if (!n->dir && (flags & VFS_O_DIRECTORY))
			return -MERR_NOTDIR;
		if ((flags & VFS_O_TRUNC) && writing)
			ramfs_truncate(n, 0);
		f = file_new(F_RAMFS, flags);
		if (f) {
			f->node = n;
			ramfs_open(n);
		}
	}
	if (!f)
		return -MERR_NOMEM;
	return fd_install(t, f, 0, flags & VFS_O_CLOEXEC);
}

long msys_read(struct task *t, int fd, u64 ubuf, u64 len)
{
	struct file *f = fd_file(t, fd);
	long n;

	if (!f || (f->flags & VFS_O_ACCMODE) == VFS_O_WRONLY)
		return -MERR_BADF;
	if (len > MSYS_IO_MAX)
		len = MSYS_IO_MAX;
	switch (f->type) {
	case F_CONSOLE:
		return stdin_read(t, ubuf, len);
	case F_PIPE:
		return pipe_read(t, f, ubuf, len);
	case F_ROFILE:
		n = (long)MOS_MIN(len, f->size - MOS_MIN(f->pos, f->size));
		if (n && uaccess_copy_out(t, ubuf, f->data + f->pos, (size_t)n))
			return -MERR_FAULT;
		f->pos += (u64)n;
		return n;
	case F_RAMFS:
		if (f->node->dir)
			return -MERR_ISDIR;
		n = (long)MOS_MIN(len, f->node->size - MOS_MIN(f->pos, f->node->size));
		if (n && uaccess_copy_out(t, ubuf, f->node->data + f->pos, (size_t)n))
			return -MERR_FAULT;
		f->pos += (u64)n;
		return n;
	default:
		return -MERR_ISDIR;
	}
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
		if (t->out_len < sizeof(t->out)) {
			size_t keep = MOS_MIN(n, sizeof(t->out) - t->out_len);

			memcpy(t->out + t->out_len, chunk, keep);
			t->out_len += (u32)keep;
		}
		done += n;
	}
	return (long)done;
}

long msys_write(struct task *t, int fd, u64 ubuf, u64 len)
{
	struct file *f = fd_file(t, fd);

	if (!f || (f->flags & VFS_O_ACCMODE) == 0)
		return -MERR_BADF;
	if (len > MSYS_IO_MAX)
		len = MSYS_IO_MAX;
	switch (f->type) {
	case F_CONSOLE:
		return console_out(t, ubuf, len);
	case F_PIPE:
		return pipe_write(t, f, ubuf, len);
	case F_RAMFS: {
		struct rnode *n = f->node;
		u64 pos = (f->flags & VFS_O_APPEND) ? n->size : f->pos;
		/* Grow the buffer, then copy straight from user memory into it. */
		long rc = ramfs_truncate(n, MOS_MAX(n->size, pos + len));

		if (rc)
			return rc;
		if (uaccess_copy_in(t, n->data + pos, ubuf, (size_t)len))
			return -MERR_FAULT;
		f->pos = pos + len;
		return (long)len;
	}
	default:
		return -MERR_BADF;
	}
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
	if (f->type == F_CONSOLE || f->type == F_PIPE)
		return -MERR_INVAL;             /* ESPIPE */
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

	if (!f)
		return -MERR_BADF;
	*is_tty = f->type == F_CONSOLE;
	*size = f->type == F_ROFILE ? f->size : f->type == F_RAMFS ? f->node->size : 0;
	switch (f->type) {
	case F_CONSOLE: return MSYS_ST_TTY;
	case F_PIPE:    return MSYS_ST_FIFO;
	case F_RODIR:   return MSYS_ST_DIR;
	case F_RAMFS:   return f->node->dir ? MSYS_ST_DIR : MSYS_ST_FILE_RW;
	default:        return MSYS_ST_FILE;
	}
}

long msys_stat(struct task *t, u64 upath, u64 *size, bool *is_dir)
{
	char path[PATH_MAX_LEN];
	const char *rest;
	struct rnode *root, *n;
	const u8 *data;
	size_t len;
	long rc = user_path(t, upath, path);

	if (rc)
		return rc;
	*is_dir = false;
	*size = 0;
	root = mount_of(path, &rest);
	if (root) {
		rc = ramfs_lookup(root, rest, &n);
		if (rc)
			return rc;
		*is_dir = n->dir;
		*size = n->size;
		return n->dir ? MSYS_ST_DIR : MSYS_ST_FILE_RW;
	}
	if (!initrd_find(path, &data, &len)) {
		*size = len;
		return MSYS_ST_FILE;
	}
	*is_dir = !*path || initrd_is_dir(path);
	return *is_dir ? MSYS_ST_DIR : -MERR_NOENT;
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
		return (t->cloexec >> fd) & 1;
	case MSYS_F_SETFD:
		t->cloexec = (arg & 1) ? t->cloexec | (1u << fd) : t->cloexec & ~(1u << fd);
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

long msys_mkdir(struct task *t, u64 upath)
{
	char path[PATH_MAX_LEN];
	const char *rest;
	struct rnode *root, *n;
	long rc = user_path(t, upath, path);

	if (rc)
		return rc;
	root = mount_of(path, &rest);
	if (!root)
		return !*path || initrd_is_dir(path) ? -MERR_EXIST : -MERR_ACCES;
	return ramfs_create(root, rest, true, true, &n);
}

long msys_unlink(struct task *t, u64 upath, bool dir)
{
	char path[PATH_MAX_LEN];
	const char *rest;
	struct rnode *root;
	long rc = user_path(t, upath, path);

	if (rc)
		return rc;
	root = mount_of(path, &rest);
	if (!root)
		return -MERR_ACCES;
	return ramfs_unlink(root, rest, dir);
}

long msys_rename(struct task *t, u64 ufrom, u64 uto)
{
	char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
	const char *rf, *rt;
	struct rnode *a, *b;
	long rc = user_path(t, ufrom, from);

	if (!rc)
		rc = user_path(t, uto, to);
	if (rc)
		return rc;
	a = mount_of(from, &rf);
	b = mount_of(to, &rt);
	if (!a || !b)
		return -MERR_ACCES;
	if (a != b)
		return -MERR_NOTSUP;            /* EXDEV: different mounts */
	return ramfs_rename(a, rf, rt);
}

/* Next directory entry of an open directory: 1 and the entry, or 0 at the end. */
long msys_readdir(struct task *t, int fd, char *name, size_t cap, bool *is_dir, u64 *ino)
{
	struct file *f = fd_file(t, fd);

	if (!f)
		return -MERR_BADF;
	if (f->type == F_RAMFS) {
		struct rnode *e = ramfs_entry(f->node, f->pos);

		if (!f->node->dir)
			return -MERR_NOTDIR;
		if (!e)
			return 0;
		mos_strlcpy(name, e->name, cap);
		*is_dir = e->dir;
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
			*is_dir = slash || e.type == '5';
			*ino = 1000 + index;
			f->pos++;
			return 1;
		}
		return 0;
	}
	return -MERR_NOTDIR;
}
