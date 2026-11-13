/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_VFS_H
#define MERGEDOS_VFS_H

/*
 * Open files and descriptor tables (kernel/vfs.c).
 *
 * A descriptor points at a refcounted struct file; fork() and dup() share
 * the file (and so its offset), as POSIX requires. Paths resolve to the
 * writable ramfs mounts (/tmp, /home/user) or to the read-only ramdisk.
 */
#include <mergedos/types.h>
#include <mergedos/ramfs.h>

struct task;

enum file_type { F_CONSOLE = 1, F_ROFILE, F_RODIR, F_RAMFS, F_PIPE };

/* Open flags: the native set uses the Linux values. */
#define VFS_O_ACCMODE   0x3
#define VFS_O_WRONLY    0x1
#define VFS_O_RDWR      0x2
#define VFS_O_CREAT     0x40
#define VFS_O_EXCL      0x80
#define VFS_O_TRUNC     0x200
#define VFS_O_APPEND    0x400
#define VFS_O_NONBLOCK  0x800
#define VFS_O_DIRECTORY 0x10000
#define VFS_O_CLOEXEC   0x80000

#define PIPE_SIZE 4000

struct pipe {
	u32 rpos, count;
	int readers, writers;
	u8 buf[PIPE_SIZE];
};

struct file {
	int refs;
	enum file_type type;
	u32 flags;                      /* VFS_O_* given at open */
	u64 pos;
	const u8 *data;                 /* F_ROFILE */
	u64 size;
	char path[64];                  /* F_RODIR: ramdisk directory */
	struct rnode *node;             /* F_RAMFS */
	struct pipe *pipe;              /* F_PIPE */
	bool pipe_write;
};

void vfs_init(void);
int  vfs_write_file(const char *path, const void *data, size_t len);   /* writable mounts */
struct file *file_get(struct file *f);          /* +1 reference */
void file_put(struct file *f);                  /* -1, releases on the last */
struct file *console_file(void);                /* a new reference */
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

#endif /* MERGEDOS_VFS_H */
