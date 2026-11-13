/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Linux x86-64 personality.
 *
 *   number : rax
 *   args   : rdi, rsi, rdx, r10, r8, r9
 *   result : rax; errors are returned in-band as -errno (range -4095..-1)
 *
 * Syscall numbers are the stable ones from arch/x86/entry/syscalls/
 * syscall_64.tbl. Coverage is what a statically linked C library needs to
 * start, print, read and write files, allocate memory, run processes and
 * threads and handle signals: enough for unmodified musl programs.
 * Not yet: sockets, poll/select, file mappings, job control.
 */
#include "abi_dispatch/abi.h"

#define LX_PATH_AT_CWD   (-100)
#define LX_O_ACCMODE     3
#define LX_O_CREAT       0x40
#define LX_MAP_FIXED     0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_TIOCGWINSZ    0x5413
#define LX_ARCH_SET_FS   0x1002
#define LX_ARCH_GET_FS   0x1003
#define LX_IOV_MAX       16
#define LX_SIGCHLD       17
#define LX_AT_REMOVEDIR  0x200
#define LXV_R 0x1               /* VMM_R */
#define LXV_W 0x2               /* VMM_W */
#define LXV_X 0x4               /* VMM_X */

static long lx_ok(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return 0;                       /* accepted and ignored (signal masks, mprotect, munmap) */
}

static long lx_enosys(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return -MERR_NOSYS;
}

static long lx_read(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_read(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2]);
}

static long lx_open(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_open(ctx->task, c->arg[0], (u32)c->arg[1]);
}

/* *at() calls: the directory descriptor matters only for relative paths. */
static long at_check(struct abi_ctx *ctx, u64 dirfd, u64 upath)
{
	char first = 0;

	if ((int)dirfd == LX_PATH_AT_CWD)
		return 0;
	if (uaccess_copy_in(ctx->task, &first, upath, 1))
		return -MERR_FAULT;
	return first == '/' ? 0 : -MERR_NOTSUP;
}

static long lx_openat(struct abi_ctx *ctx, const struct abi_call *c)
{
	long rc = at_check(ctx, c->arg[0], c->arg[1]);

	return rc ? rc : msys_open(ctx->task, c->arg[1], (u32)c->arg[2]);
}

static long lx_close(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_close(ctx->task, (int)c->arg[0]);
}

static long lx_lseek(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_lseek(ctx->task, (int)c->arg[0], (s64)c->arg[1], (int)c->arg[2]);
}

/* struct stat as the x86-64 kernel lays it out (144 bytes). */
static long put_stat(struct abi_ctx *ctx, u64 ubuf, long kind, u64 size)
{
	static const u32 modes[] = {
		[MSYS_ST_FILE] = 0100555, [MSYS_ST_DIR] = 040755, [MSYS_ST_TTY] = 020620,
		[MSYS_ST_FIFO] = 010600, [MSYS_ST_FILE_RW] = 0100644,
	};
	u8 st[144] = { 0 };
	u64 nlink = 1, blksize = 4096, blocks = (size + 511) / 512;
	u32 mode;

	if (kind < 0)
		return kind;
	mode = modes[kind < (long)ARRAY_SIZE(modes) ? kind : 0];
	memcpy(st + 8, &vfs_last_ino, 8);
	memcpy(st + 16, &nlink, 8);
	memcpy(st + 24, &mode, 4);
	memcpy(st + 48, &size, 8);
	memcpy(st + 56, &blksize, 8);
	memcpy(st + 64, &blocks, 8);
	return uaccess_copy_out(ctx->task, ubuf, st, sizeof(st));
}

static long lx_fstat(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 size = 0;
	bool tty = false;

	return put_stat(ctx, c->arg[1], msys_fstat(ctx->task, (int)c->arg[0], &size, &tty), size);
}

static long stat_path(struct abi_ctx *ctx, u64 upath, u64 ubuf)
{
	u64 size = 0;
	bool dir = false;

	return put_stat(ctx, ubuf, msys_stat(ctx->task, upath, &size, &dir), size);
}

static long lx_stat(struct abi_ctx *ctx, const struct abi_call *c)
{
	return stat_path(ctx, c->arg[0], c->arg[1]);
}

static long lx_newfstatat(struct abi_ctx *ctx, const struct abi_call *c)
{
	long rc = at_check(ctx, c->arg[0], c->arg[1]);

	return rc ? rc : stat_path(ctx, c->arg[1], c->arg[2]);
}

static long lx_access(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 size;
	bool dir;
	long rc = msys_stat(ctx->task, c->arg[0], &size, &dir);

	return rc < 0 ? rc : 0;
}

static long lx_faccessat(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 size;
	bool dir;
	long rc = at_check(ctx, c->arg[0], c->arg[1]);

	if (!rc)
		rc = msys_stat(ctx->task, c->arg[1], &size, &dir);
	return rc < 0 ? rc : 0;
}

/* mmap(addr, len, prot, flags, fd, offset). Linux prot/flags match MSYS_*. */
static long lx_mmap(struct abi_ctx *ctx, const struct abi_call *c)
{
	u32 prot = ((c->arg[2] & 1) ? LXV_R : 0) | ((c->arg[2] & 2) ? LXV_W : 0) |
		   ((c->arg[2] & 4) ? LXV_X : 0);
	u32 flags = (u32)c->arg[3] & (MSYS_MAP_FIXED | MSYS_MAP_ANON | MSYS_MAP_FIXED_NOREPLACE);

	return msys_mmap(ctx->task, c->arg[0], c->arg[1], prot, flags, (int)c->arg[4], c->arg[5]);
}

static long lx_mprotect(struct abi_ctx *ctx, const struct abi_call *c)
{
	u32 prot = ((c->arg[2] & 2) ? MSYS_PROT_W : 0) | ((c->arg[2] & 4) ? MSYS_PROT_X : 0);

	return msys_protect(ctx->task, c->arg[0], c->arg[1], prot);
}

static long lx_pread(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_pread(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2], c->arg[3]);
}

static long lx_readlink(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_readlink(ctx->task, c->arg[0], c->arg[1], c->arg[2]);
}

static long lx_readlinkat(struct abi_ctx *ctx, const struct abi_call *c)
{
	long rc = at_check(ctx, c->arg[0], c->arg[1]);

	return rc ? rc : msys_readlink(ctx->task, c->arg[1], c->arg[2], c->arg[3]);
}

static long lx_getrandom(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_getrandom(ctx->task, c->arg[0], c->arg[1]);
}

/* sched_getaffinity(pid, cpusetsize, mask): one CPU, so bit 0 of the mask. */
static long lx_sched_getaffinity(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 mask = 1;
	u64 n = c->arg[1] < sizeof(mask) ? c->arg[1] : sizeof(mask);

	if (n < 1)
		return -MERR_INVAL;
	if (uaccess_copy_out(ctx->task, c->arg[2], &mask, (size_t)n))
		return -MERR_FAULT;
	return (long)n;                         /* bytes written */
}

/* prlimit64(pid, resource, new, old): report generous, fixed limits. */
static long lx_prlimit64(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 lim[2];                             /* rlim_cur, rlim_max */

	if (!c->arg[3])
		return 0;                       /* only setting: accepted and ignored */
	switch (c->arg[1]) {
	case 3:  lim[0] = 8ULL << 20; lim[1] = 8ULL << 20; break;  /* RLIMIT_STACK */
	case 7:  lim[0] = 32;         lim[1] = 32;         break;  /* RLIMIT_NOFILE */
	default: lim[0] = ~0ULL;          lim[1] = ~0ULL;          break;  /* RLIM_INFINITY */
	}
	return uaccess_copy_out(ctx->task, c->arg[3], lim, sizeof(lim)) ? -MERR_FAULT : 0;
}

static long lx_getrlimit(struct abi_ctx *ctx, const struct abi_call *c)
{
	struct abi_call p = *c;

	p.arg[1] = c->arg[0];                   /* resource */
	p.arg[3] = c->arg[1];                   /* old limit buffer */
	return lx_prlimit64(ctx, &p);
}

/* statx: fill the subset glibc/musl read (mode, size, blocks). */
static long lx_statx(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 size = 0;
	bool dir = false;
	long kind, rc = at_check(ctx, c->arg[0], c->arg[1]);
	u8 stx[256] = { 0 };
	static const u16 modes[] = {
		[MSYS_ST_FILE] = 0100555, [MSYS_ST_DIR] = 040755, [MSYS_ST_TTY] = 020620,
		[MSYS_ST_FIFO] = 010600, [MSYS_ST_FILE_RW] = 0100644,
	};
	u16 mode;
	u32 mask = 0x7ff;                       /* STATX_BASIC_STATS */

	if (rc)
		return rc;
	/* AT_EMPTY_PATH (0x1000) with an empty path means "the dirfd itself". */
	kind = (c->arg[2] & 0x1000) ? msys_fstat(ctx->task, (int)c->arg[0], &size, &dir)
				    : msys_stat(ctx->task, c->arg[1], &size, &dir);
	if (kind < 0)
		return kind;
	mode = modes[kind < (long)ARRAY_SIZE(modes) ? kind : 0];
	memcpy(stx + 0, &mask, 4);
	memcpy(stx + 32, &vfs_last_ino, 8);     /* stx_ino */              /* stx_mask */
	{ u32 bs = 4096; memcpy(stx + 4, &bs, 4); }     /* stx_blksize */
	{ u32 nl = 1; memcpy(stx + 16, &nl, 4); }       /* stx_nlink */
	memcpy(stx + 28, &mode, 2);             /* stx_mode */
	memcpy(stx + 40, &size, 8);             /* stx_size */
	{ u64 blk = (size + 511) / 512; memcpy(stx + 48, &blk, 8); }  /* stx_blocks */
	return uaccess_copy_out(ctx->task, c->arg[4], stx, sizeof(stx));
}

static long lx_brk(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_brk(ctx->task, c->arg[0]);
}

static long lx_ioctl(struct abi_ctx *ctx, const struct abi_call *c)
{
	static const u16 winsize[4] = { 25, 80, 0, 0 };
	u64 size;
	bool tty = false;

	/* "Is this a terminal?" is how a C library decides to line-buffer stdout. */
	if (c->arg[1] == LX_TIOCGWINSZ && msys_fstat(ctx->task, (int)c->arg[0], &size, &tty) >= 0 && tty)
		return uaccess_copy_out(ctx->task, c->arg[2], winsize, sizeof(winsize));
	return -MERR_NOTTY;
}

/* readv/writev: walk the iovec array, stop at the first short transfer. */
static long lx_iov(struct abi_ctx *ctx, const struct abi_call *c, bool write)
{
	long total = 0;

	if (c->arg[2] > LX_IOV_MAX)
		return -MERR_INVAL;
	for (u64 i = 0; i < c->arg[2]; i++) {
		u64 iov[2];
		long n;

		if (uaccess_copy_in(ctx->task, iov, c->arg[1] + i * 16, sizeof(iov)))
			return total ? total : -MERR_FAULT;
		if (!iov[1])
			continue;
		n = write ? msys_write(ctx->task, (int)c->arg[0], iov[0], iov[1])
			  : msys_read(ctx->task, (int)c->arg[0], iov[0], iov[1]);
		if (n < 0)
			return total ? total : n;
		total += n;
		if ((u64)n < iov[1])
			break;
	}
	return total;
}

static long lx_readv(struct abi_ctx *ctx, const struct abi_call *c)
{
	return lx_iov(ctx, c, false);
}

static long lx_writev(struct abi_ctx *ctx, const struct abi_call *c)
{
	return lx_iov(ctx, c, true);
}

static long lx_uname(struct abi_ctx *ctx, const struct abi_call *c)
{
	const struct abi_identity *id = abi_identity(ABI_LINUX);
	char u[6][65];                  /* struct utsname: six NUL-padded fields */

	memset(u, 0, sizeof(u));
	mos_strlcpy(u[0], id->sysname, 65);
	mos_strlcpy(u[1], id->nodename, 65);
	mos_strlcpy(u[2], id->release, 65);
	mos_strlcpy(u[3], id->version, 65);
	mos_strlcpy(u[4], id->machine, 65);
	return uaccess_copy_out(ctx->task, c->arg[0], u, sizeof(u));
}

static long lx_arch_prctl(struct abi_ctx *ctx, const struct abi_call *c)
{
	/* The C library points %fs at its thread control block during start-up. */
	if (c->arg[0] == LX_ARCH_SET_FS)
		return msys_set_fs_base(ctx->task, c->arg[1]);
	if (c->arg[0] == LX_ARCH_GET_FS)
		return msys_get_fs_base(ctx->task, c->arg[1]);
	return -MERR_INVAL;
}

static long lx_clock_gettime(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 ns = msys_clock_ns(), ts[2] = { ns / 1000000000ULL, ns % 1000000000ULL };

	return uaccess_copy_out(ctx->task, c->arg[1], ts, sizeof(ts));
}

/* ---- files ------------------------------------------------------------------ */

static long put_fds(struct abi_ctx *ctx, u64 ufds, u32 flags)
{
	int fds[2];
	long rc = msys_pipe(ctx->task, fds, flags);

	if (rc)
		return rc;
	if (uaccess_copy_out(ctx->task, ufds, fds, sizeof(fds))) {
		msys_close(ctx->task, fds[0]);
		msys_close(ctx->task, fds[1]);
		return -MERR_FAULT;
	}
	return 0;
}

static long lx_pipe(struct abi_ctx *ctx, const struct abi_call *c)
{
	return put_fds(ctx, c->arg[0], 0);
}

static long lx_pipe2(struct abi_ctx *ctx, const struct abi_call *c)
{
	return put_fds(ctx, c->arg[0], (u32)c->arg[1]);
}

static long lx_dup(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_dup(ctx->task, (int)c->arg[0], 0, false);
}

static long lx_dup2(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_dup2(ctx->task, (int)c->arg[0], (int)c->arg[1], false);
}

static long lx_dup3(struct abi_ctx *ctx, const struct abi_call *c)
{
	if (c->arg[0] == c->arg[1])
		return -MERR_INVAL;
	return msys_dup2(ctx->task, (int)c->arg[0], (int)c->arg[1], c->arg[2] & 0x80000);
}

static long lx_fcntl(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_fcntl(ctx->task, (int)c->arg[0], (int)c->arg[1], c->arg[2]);
}

static long lx_mkdir(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_mkdir(ctx->task, c->arg[0]);
}

static long lx_mkdirat(struct abi_ctx *ctx, const struct abi_call *c)
{
	long rc = at_check(ctx, c->arg[0], c->arg[1]);

	return rc ? rc : msys_mkdir(ctx->task, c->arg[1]);
}

static long lx_unlink(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_unlink(ctx->task, c->arg[0], false);
}

static long lx_rmdir(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_unlink(ctx->task, c->arg[0], true);
}

static long lx_unlinkat(struct abi_ctx *ctx, const struct abi_call *c)
{
	long rc = at_check(ctx, c->arg[0], c->arg[1]);

	return rc ? rc : msys_unlink(ctx->task, c->arg[1], c->arg[2] & LX_AT_REMOVEDIR);
}

static long lx_rename(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_rename(ctx->task, c->arg[0], c->arg[1]);
}

static long lx_renameat(struct abi_ctx *ctx, const struct abi_call *c)
{
	long rc = at_check(ctx, c->arg[0], c->arg[1]);

	if (!rc)
		rc = at_check(ctx, c->arg[2], c->arg[3]);
	return rc ? rc : msys_rename(ctx->task, c->arg[1], c->arg[3]);
}

/* getdents64(fd, struct linux_dirent64 *buf, count) */
static long lx_getdents64(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 done = 0;

	for (;;) {
		u8 ent[8 + 8 + 2 + 1 + 69] = { 0 };
		char *name = (char *)ent + 19;
		bool dir = false;
		u64 ino = 0, off;
		u16 reclen;
		long rc;

		if (c->arg[2] - done < sizeof(ent))
			break;                          /* the largest entry might not fit */
		rc = msys_readdir(ctx->task, (int)c->arg[0], name, 64, &dir, &ino);
		if (rc < 0)
			return done ? (long)done : rc;
		if (rc == 0)
			break;
		reclen = (u16)ALIGN_UP(19 + strlen(name) + 1, 8);
		off = done + reclen;
		memcpy(ent, &ino, 8);
		memcpy(ent + 8, &off, 8);
		memcpy(ent + 16, &reclen, 2);
		ent[18] = dir ? 4 : 8;                  /* DT_DIR / DT_REG */
		if (uaccess_copy_out(ctx->task, c->arg[1] + done, ent, reclen))
			return -MERR_FAULT;
		done += reclen;
	}
	return (long)done;
}

static long lx_getcwd(struct abi_ctx *ctx, const struct abi_call *c)
{
	if (c->arg[1] < 2)
		return -MERR_RANGE;
	return uaccess_copy_out(ctx->task, c->arg[0], "/", 2) ? -MERR_FAULT : 2;
}

/* ---- memory and time ---------------------------------------------------------- */

static long lx_munmap(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_munmap(ctx->task, c->arg[0], c->arg[1]);
}

static long sleep_ts(struct abi_ctx *ctx, u64 uts, bool absolute)
{
	u64 ts[2], ns, now = msys_clock_ns();

	if (uaccess_copy_in(ctx->task, ts, uts, sizeof(ts)))
		return -MERR_FAULT;
	if (ts[1] >= 1000000000ULL)
		return -MERR_INVAL;
	ns = ts[0] * 1000000000ULL + ts[1];
	if (absolute)
		ns = ns > now ? ns - now : 0;
	return msys_nanosleep(ctx->task, ns);
}

static long lx_nanosleep(struct abi_ctx *ctx, const struct abi_call *c)
{
	return sleep_ts(ctx, c->arg[0], false);
}

static long lx_clock_nanosleep(struct abi_ctx *ctx, const struct abi_call *c)
{
	return sleep_ts(ctx, c->arg[2], c->arg[1] & 1);
}

static long lx_sched_yield(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_nanosleep(ctx->task, 0);
}

/* ---- processes, threads, signals ----------------------------------------------- */

static long lx_clone(struct abi_ctx *ctx, const struct abi_call *c)
{
	/* x86-64 order: flags, newsp, parent_tid, child_tid, tls */
	return msys_clone(ctx->task, c->arg[0], c->arg[1], c->arg[2], c->arg[3], c->arg[4]);
}

static long lx_fork(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_clone(ctx->task, LX_SIGCHLD, 0, 0, 0, 0);
}

static long lx_vfork(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_clone(ctx->task, MSYS_CLONE_VM | MSYS_CLONE_VFORK | LX_SIGCHLD, 0, 0, 0, 0);
}

static long lx_execve(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_execve(ctx->task, c->arg[0], c->arg[1], c->arg[2]);
}

static long lx_wait4(struct abi_ctx *ctx, const struct abi_call *c)
{
	static const u8 zero_rusage[144];
	long rc = msys_wait(ctx->task, (int)c->arg[0], c->arg[1], (int)c->arg[2]);

	if (rc > 0 && c->arg[3] &&
	    uaccess_copy_out(ctx->task, c->arg[3], zero_rusage, sizeof(zero_rusage)))
		return -MERR_FAULT;
	return rc;
}

static long lx_exit(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_thread_exit(ctx->task, (int)(c->arg[0] & 0xFF));
}

static long lx_getppid(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_getppid(ctx->task);
}

static long lx_gettid(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_gettid(ctx->task);
}

static long lx_setpgid(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_setpgid(ctx->task, (int)c->arg[0], (int)c->arg[1]);
}

static long lx_getpgid(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_getpgid(ctx->task, (int)c->arg[0]);
}

static long lx_getpgrp(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_getpgid(ctx->task, 0);
}

static long lx_set_tid_address(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_set_tid_address(ctx->task, c->arg[0]);
}

static long lx_futex(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_futex(ctx->task, c->arg[0], (int)c->arg[1], (u32)c->arg[2], c->arg[3]);
}

static long lx_kill(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_kill(ctx->task, (int)c->arg[0], (int)c->arg[1]);
}

static long lx_tgkill(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_kill(ctx->task, (int)c->arg[0], (int)c->arg[2]);
}

static long lx_rt_sigaction(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_sigaction(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2]);
}

static long lx_rt_sigprocmask(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_sigprocmask(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2]);
}

static long lx_rt_sigreturn(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_sigreturn(ctx->task);
}

static const struct abi_sysent linux_sysent[] = {
	[0]   = { lx_read,          "read" },
	[1]   = { abi_posix_write,  "write" },
	[2]   = { lx_open,          "open" },
	[3]   = { lx_close,         "close" },
	[4]   = { lx_stat,          "stat" },
	[5]   = { lx_fstat,         "fstat" },
	[6]   = { lx_stat,          "lstat" },
	[8]   = { lx_lseek,         "lseek" },
	[9]   = { lx_mmap,          "mmap" },
	[10]  = { lx_mprotect,      "mprotect" },
	[11]  = { lx_munmap,        "munmap" },
	[17]  = { lx_pread,         "pread64" },
	[28]  = { lx_ok,            "madvise" },
	[12]  = { lx_brk,           "brk" },
	[13]  = { lx_rt_sigaction,  "rt_sigaction" },
	[14]  = { lx_rt_sigprocmask, "rt_sigprocmask" },
	[15]  = { lx_rt_sigreturn,  "rt_sigreturn" },
	[16]  = { lx_ioctl,         "ioctl" },
	[19]  = { lx_readv,         "readv" },
	[20]  = { lx_writev,        "writev" },
	[21]  = { lx_access,        "access" },
	[22]  = { lx_pipe,          "pipe" },
	[24]  = { lx_sched_yield,   "sched_yield" },
	[32]  = { lx_dup,           "dup" },
	[33]  = { lx_dup2,          "dup2" },
	[35]  = { lx_nanosleep,     "nanosleep" },
	[39]  = { abi_posix_getpid, "getpid" },
	[56]  = { lx_clone,         "clone" },
	[57]  = { lx_fork,          "fork" },
	[58]  = { lx_vfork,         "vfork" },
	[59]  = { lx_execve,        "execve" },
	[60]  = { lx_exit,          "exit" },
	[61]  = { lx_wait4,         "wait4" },
	[62]  = { lx_kill,          "kill" },
	[63]  = { lx_uname,         "uname" },
	[72]  = { lx_fcntl,         "fcntl" },
	[79]  = { lx_getcwd,        "getcwd" },
	[82]  = { lx_rename,        "rename" },
	[83]  = { lx_mkdir,         "mkdir" },
	[84]  = { lx_rmdir,         "rmdir" },
	[87]  = { lx_unlink,        "unlink" },
	[89]  = { lx_readlink,      "readlink" },
	[97]  = { lx_getrlimit,     "getrlimit" },
	[102] = { lx_ok,            "getuid" },
	[104] = { lx_ok,            "getgid" },
	[107] = { lx_ok,            "geteuid" },
	[108] = { lx_ok,            "getegid" },
	[109] = { lx_setpgid,       "setpgid" },
	[110] = { lx_getppid,       "getppid" },
	[111] = { lx_getpgrp,       "getpgrp" },
	[121] = { lx_getpgid,       "getpgid" },
	[158] = { lx_arch_prctl,    "arch_prctl" },
	[186] = { lx_gettid,        "gettid" },
	[200] = { lx_kill,          "tkill" },
	[202] = { lx_futex,         "futex" },
	[204] = { lx_sched_getaffinity, "sched_getaffinity" },
	[217] = { lx_getdents64,    "getdents64" },
	[218] = { lx_set_tid_address, "set_tid_address" },
	[228] = { lx_clock_gettime, "clock_gettime" },
	[230] = { lx_clock_nanosleep, "clock_nanosleep" },
	[231] = { abi_posix_exit,   "exit_group" },
	[234] = { lx_tgkill,        "tgkill" },
	[257] = { lx_openat,        "openat" },
	[258] = { lx_mkdirat,       "mkdirat" },
	[262] = { lx_newfstatat,    "newfstatat" },
	[263] = { lx_unlinkat,      "unlinkat" },
	[264] = { lx_renameat,      "renameat" },
	[267] = { lx_readlinkat,    "readlinkat" },
	[269] = { lx_faccessat,     "faccessat" },
	[273] = { lx_ok,            "set_robust_list" },
	[292] = { lx_dup3,          "dup3" },
	[293] = { lx_pipe2,         "pipe2" },
	[302] = { lx_prlimit64,     "prlimit64" },
	[318] = { lx_getrandom,     "getrandom" },
	[332] = { lx_statx,         "statx" },
	[334] = { lx_enosys,        "rseq" },
};

static void linux_decode(const struct syscall_frame *f, struct abi_call *c)
{
	c->nr = f->rax;
	c->klass = 0;
	c->arg[0] = f->rdi;
	c->arg[1] = f->rsi;
	c->arg[2] = f->rdx;
	c->arg[3] = f->r10;
	c->arg[4] = f->r8;
	c->arg[5] = f->r9;
	c->usp = f->rsp;
}

static const struct abi_sysent *linux_lookup(const struct abi_call *c)
{
	if (c->nr < ARRAY_SIZE(linux_sysent) && linux_sysent[c->nr].fn)
		return &linux_sysent[c->nr];
	return NULL;
}

static void linux_encode(struct syscall_frame *f, const struct abi_call *c, long ret)
{
	(void)c;
	f->rax = ret < 0 ? (u64)(-(long)abi_errno_linux((int)-ret)) : (u64)ret;
}

const struct abi_personality_ops abi_linux_ops = {
	.name   = "linux",
	.decode = linux_decode,
	.lookup = linux_lookup,
	.encode = linux_encode,
};
