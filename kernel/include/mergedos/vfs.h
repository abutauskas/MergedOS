/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_VFS_H
#define MERGEDOS_VFS_H

/*
 * Open files and descriptor tables (kernel/vfs.c).
 *
 * A descriptor points at a refcounted struct file; fork() and dup() share
 * the file (and so its offset), as POSIX requires. Paths resolve (cwd,
 * symlinks) to the writable ramfs mounts, the virtual /dev and /proc files,
 * or the read-only boot ramdisk and modules. Files that are not any of
 * these (sockets, eventfds, epoll sets, devices) carry a `struct fops`.
 */
#include <mergedos/types.h>
#include <mergedos/ramfs.h>

struct task;
struct file;

enum file_type { F_CONSOLE = 1, F_ROFILE, F_RODIR, F_RAMFS, F_PIPE, F_OPS };

/* Open flags: the native set uses the Linux values. */
#define VFS_O_ACCMODE   0x3
#define VFS_O_WRONLY    0x1
#define VFS_O_RDWR      0x2
#define VFS_O_CREAT     0x40
#define VFS_O_EXCL      0x80
#define VFS_O_NOCTTY    0x100
#define VFS_O_TRUNC     0x200
#define VFS_O_APPEND    0x400
#define VFS_O_NONBLOCK  0x800
#define VFS_O_DIRECTORY 0x10000
#define VFS_O_NOFOLLOW  0x20000
#define VFS_O_CLOEXEC   0x80000
#define VFS_O_PATH      0x200000

/* poll(2) event bits (Linux values). */
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
#define POLLRDHUP 0x2000

#define PIPE_SIZE 4000

struct pipe {
	u32 rpos, count;
	int readers, writers;
	u8 buf[PIPE_SIZE];
};

/* What a file that is not a plain ramfs/ramdisk file can do. NULL = not supported. */
struct fops {
	long (*read)(struct task *t, struct file *f, u64 ubuf, u64 len);
	long (*write)(struct task *t, struct file *f, u64 ubuf, u64 len);
	u32  (*poll)(struct file *f, u32 events);       /* ready events (may add POLLERR/HUP) */
	void (*release)(struct file *f);
	long (*ioctl)(struct task *t, struct file *f, u32 cmd, u64 arg);
	u32  mode;                                      /* S_IF* type bits for fstat */
};

struct file {
	int refs;
	enum file_type type;
	u32 flags;                      /* VFS_O_* given at open */
	u64 pos;
	const u8 *data;                 /* F_ROFILE */
	u64 size;
	char *path;                     /* canonical absolute path (no leading '/'), or NULL */
	struct rnode *node;             /* F_RAMFS */
	struct pipe *pipe;              /* F_PIPE */
	bool pipe_write;
	const struct fops *ops;         /* F_OPS */
	void *priv;                     /* owned by the ops */
};

/* stat results, in the shape the Linux layer needs. */
struct kstat {
	u64 dev, ino, size, rdev;
	u64 atime, mtime, ctime;        /* nanoseconds */
	u32 mode, nlink;
};
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000
#define S_IFIFO  0010000

void vfs_init(void);
struct file *file_get(struct file *f);          /* +1 reference */
void file_put(struct file *f);                  /* -1, releases on the last */
struct file *console_file(void);                /* a new reference */
struct file *file_new_ops(const struct fops *ops, void *priv, u32 flags);
struct file *fd_file(struct task *t, int fd);   /* NULL if not open */
int  fd_install(struct task *t, struct file *f, int min, bool cloexec);  /* takes the ref */
void fd_close_all(struct task *t);
void fd_close_on_exec(struct task *t);
void fd_fork(struct task *child, const struct task *parent);

/* Load a whole executable: ramdisk data or a ramfs file's buffer. */
int  vfs_image(const char *path, const u8 **data, size_t *size);

/* Bytes of an open file for mmap (see kernel/vfs.c). */
long vfs_mmap_source(struct task *t, int fd, u64 offset, const u8 **data, bool *immutable);

/* Terminal input for the foreground process group. */
void stdin_feed(int c);                         /* from task_keyboard_pump() */
void stdin_drain_to_console(void);              /* unread input back to the shell */
extern int fg_pgid;

/* ---- path-based services (the Linux layer's building blocks) ---------------------- */
#define VFS_AT_FDCWD (-100)
long vfs_path(struct task *t, int dirfd, u64 upath, bool follow, char *canon);   /* canon: PATH_MAX_LEN */
long vfs_open_canon(struct task *t, const char *canon, u32 flags, u32 mode, struct file **out);
long vfs_open_at(struct task *t, int dirfd, u64 upath, u32 flags, u32 mode);     /* installs fd */
long vfs_stat_at(struct task *t, int dirfd, u64 upath, bool follow, struct kstat *st);
void vfs_fstat(const struct file *f, struct kstat *st);
long vfs_mkdir_at(struct task *t, int dirfd, u64 upath, u32 mode);
long vfs_unlink_at(struct task *t, int dirfd, u64 upath, bool dir);
long vfs_rename_at(struct task *t, int fromfd, u64 ufrom, int tofd, u64 uto);
long vfs_symlink_at(struct task *t, u64 utarget, int dirfd, u64 ulink);
long vfs_readlink_at(struct task *t, int dirfd, u64 upath, u64 ubuf, u64 size);
long vfs_chdir(struct task *t, u64 upath);
long vfs_fchdir(struct task *t, int fd);
long vfs_getcwd(struct task *t, u64 ubuf, u64 size);
long vfs_chmod_at(struct task *t, int dirfd, u64 upath, u32 mode);
long vfs_fchmod(struct task *t, int fd, u32 mode);
long vfs_utimes(struct task *t, struct file *f, const u64 times[2][2], bool have_times);
long vfs_access_at(struct task *t, int dirfd, u64 upath, u32 mode);
long vfs_pwrite(struct task *t, struct file *f, u64 ubuf, u64 len, u64 off);
long vfs_ftruncate(struct task *t, struct file *f, u64 size);
long vfs_getdents(struct task *t, int fd, u64 ubuf, u64 count);
long vfs_memfd_create(struct task *t, const char *name, u32 flags);

/* poll support: wake every blocked poll/epoll/select when any pollable object changes. */
u32  file_poll(struct file *f, u32 events);
void poll_wake(void);
bool poll_wait(u64 timeout_ticks);              /* false on timeout */

#endif /* MERGEDOS_VFS_H */
