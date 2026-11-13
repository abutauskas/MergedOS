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
 * start, print, read files and allocate memory: enough for unmodified musl
 * programs. Not yet: fork/exec, threads, signals, sockets, writable files.
 */
#include "abi_dispatch/abi.h"

#define LX_PATH_AT_CWD   (-100)
#define LX_O_ACCMODE     3
#define LX_O_CREAT       0x40
#define LX_MAP_FIXED     0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_TIOCGWINSZ    0x5413
#define LX_ARCH_SET_FS   0x1002
#define LX_IOV_MAX       16

static long lx_ok(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return 0;                       /* accepted and ignored (signal masks, mprotect, munmap) */
}

static long lx_read(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_read(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2]);
}

static long lx_open(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_open(ctx->task, c->arg[0],
			 (c->arg[1] & LX_O_ACCMODE) != 0 || (c->arg[1] & LX_O_CREAT));
}

/* openat(dirfd, path, flags): only paths that do not depend on dirfd. */
static long lx_openat(struct abi_ctx *ctx, const struct abi_call *c)
{
	if ((int)c->arg[0] != LX_PATH_AT_CWD)
		return -MERR_NOTSUP;
	return msys_open(ctx->task, c->arg[1],
			 (c->arg[2] & LX_O_ACCMODE) != 0 || (c->arg[2] & LX_O_CREAT));
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
static long put_stat(struct abi_ctx *ctx, u64 ubuf, u32 mode, u64 size)
{
	u8 st[144] = { 0 };
	u64 nlink = 1, blksize = 4096, blocks = (size + 511) / 512;

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
	long rc = msys_fstat(ctx->task, (int)c->arg[0], &size, &tty);

	return rc ? rc : put_stat(ctx, c->arg[1], tty ? 020620 : 0100444, size);
}

static long stat_path(struct abi_ctx *ctx, u64 upath, u64 ubuf)
{
	u64 size = 0;
	bool dir = false;
	long rc = msys_stat(ctx->task, upath, &size, &dir);

	return rc ? rc : put_stat(ctx, ubuf, dir ? 040555 : 0100555, size);
}

static long lx_stat(struct abi_ctx *ctx, const struct abi_call *c)
{
	return stat_path(ctx, c->arg[0], c->arg[1]);
}

static long lx_newfstatat(struct abi_ctx *ctx, const struct abi_call *c)
{
	return (int)c->arg[0] == LX_PATH_AT_CWD ? stat_path(ctx, c->arg[1], c->arg[2]) : -MERR_NOTSUP;
}

static long lx_mmap(struct abi_ctx *ctx, const struct abi_call *c)
{
	/* Anonymous memory only, placed by the kernel. File mappings need the page cache. */
	if (!(c->arg[3] & LX_MAP_ANONYMOUS) || (c->arg[3] & LX_MAP_FIXED))
		return -MERR_NOMEM;
	return msys_mmap_anon(ctx->task, c->arg[1]);
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
	if (c->arg[1] == LX_TIOCGWINSZ && !msys_fstat(ctx->task, (int)c->arg[0], &size, &tty) && tty)
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
	return c->arg[0] == LX_ARCH_SET_FS ? msys_set_fs_base(ctx->task, c->arg[1]) : -MERR_INVAL;
}

static long lx_clock_gettime(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 ns = msys_clock_ns(), ts[2] = { ns / 1000000000ULL, ns % 1000000000ULL };

	return uaccess_copy_out(ctx->task, c->arg[1], ts, sizeof(ts));
}

static const struct abi_sysent linux_sysent[] = {
	[0]   = { lx_read,          "read" },
	[1]   = { abi_posix_write,  "write" },
	[2]   = { lx_open,          "open" },
	[3]   = { lx_close,         "close" },
	[4]   = { lx_stat,          "stat" },
	[5]   = { lx_fstat,         "fstat" },
	[8]   = { lx_lseek,         "lseek" },
	[9]   = { lx_mmap,          "mmap" },
	[10]  = { lx_ok,            "mprotect" },
	[11]  = { lx_ok,            "munmap" },
	[12]  = { lx_brk,           "brk" },
	[13]  = { lx_ok,            "rt_sigaction" },
	[14]  = { lx_ok,            "rt_sigprocmask" },
	[16]  = { lx_ioctl,         "ioctl" },
	[19]  = { lx_readv,         "readv" },
	[20]  = { lx_writev,        "writev" },
	[39]  = { abi_posix_getpid, "getpid" },
	[60]  = { abi_posix_exit,   "exit" },
	[63]  = { lx_uname,         "uname" },
	[102] = { lx_ok,            "getuid" },
	[104] = { lx_ok,            "getgid" },
	[107] = { lx_ok,            "geteuid" },
	[108] = { lx_ok,            "getegid" },
	[158] = { lx_arch_prctl,    "arch_prctl" },
	[186] = { abi_posix_getpid, "gettid" },
	[218] = { abi_posix_getpid, "set_tid_address" },
	[228] = { lx_clock_gettime, "clock_gettime" },
	[231] = { abi_posix_exit,   "exit_group" },
	[257] = { lx_openat,        "openat" },
	[262] = { lx_newfstatat,    "newfstatat" },
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
