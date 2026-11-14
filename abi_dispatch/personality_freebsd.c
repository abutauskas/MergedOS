/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * FreeBSD amd64 personality.
 *
 *   number : rax
 *   args   : rdi, rsi, rdx, r10, r8, r9
 *   result : rax; on error the carry flag is set and rax holds a positive
 *            errno. libc's syscall stubs test CF, not the sign of rax.
 *
 * Numbers follow sys/kern/syscalls.master. Note how the same service has a
 * different number than on Linux (write is 4 here, 1 there).
 */
#include "abi_dispatch/abi.h"

#define BSD_SIGCHLD 20

/*
 * Process and descriptor calls whose arguments mean the same as on Linux.
 * fork's return convention (rdx = 1 in the child) is set by the kernel.
 */
static long fb_fork(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_clone(ctx->task, BSD_SIGCHLD, 0, 0, 0, 0);
}

static long fb_vfork(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_clone(ctx->task, MSYS_CLONE_VM | MSYS_CLONE_VFORK | BSD_SIGCHLD, 0, 0, 0, 0);
}

static long fb_read(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_read(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2]);
}

static long fb_close(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_close(ctx->task, (int)c->arg[0]);
}

static long fb_wait4(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_wait(ctx->task, (int)c->arg[0], c->arg[1], (int)c->arg[2]);
}

static long fb_unlink(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_unlink(ctx->task, c->arg[0], false);
}

static long fb_rmdir(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_unlink(ctx->task, c->arg[0], true);
}

static long fb_mkdir(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_mkdir(ctx->task, c->arg[0]);
}

static long fb_rename(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_rename(ctx->task, c->arg[0], c->arg[1]);
}

static long fb_kill(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_kill(ctx->task, (int)c->arg[0], (int)c->arg[1]);    /* 1..15 agree */
}

static long fb_getppid(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_getppid(ctx->task);
}

static long fb_dup(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_dup(ctx->task, (int)c->arg[0], 0, false);
}

static long fb_dup2(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_dup2(ctx->task, (int)c->arg[0], (int)c->arg[1], false);
}

static long fb_execve(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_execve(ctx->task, c->arg[0], c->arg[1], c->arg[2]);
}

static const struct abi_sysent freebsd_sysent[] = {
	[1]   = { abi_posix_exit,   "exit" },
	[2]   = { fb_fork,          "fork" },
	[3]   = { fb_read,          "read" },
	[4]   = { abi_posix_write,  "write" },
	[6]   = { fb_close,         "close" },
	[7]   = { fb_wait4,         "wait4" },
	[10]  = { fb_unlink,        "unlink" },
	[20]  = { abi_posix_getpid, "getpid" },
	[37]  = { fb_kill,          "kill" },
	[39]  = { fb_getppid,       "getppid" },
	[41]  = { fb_dup,           "dup" },
	[59]  = { fb_execve,        "execve" },
	[66]  = { fb_vfork,         "vfork" },
	[90]  = { fb_dup2,          "dup2" },
	[128] = { fb_rename,        "rename" },
	[136] = { fb_mkdir,         "mkdir" },
	[137] = { fb_rmdir,         "rmdir" },
};

static void freebsd_decode(const struct syscall_frame *f, struct abi_call *c)
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

static const struct abi_sysent *freebsd_lookup(const struct abi_call *c)
{
	if (c->nr < ARRAY_SIZE(freebsd_sysent) && freebsd_sysent[c->nr].fn)
		return &freebsd_sysent[c->nr];
	return NULL;
}

static void freebsd_encode(struct syscall_frame *f, const struct abi_call *c, long ret)
{
	(void)c;
	if (ret < 0) {
		f->rax = (u64)abi_errno_bsd((int)-ret);
		f->rflags |= RFLAGS_CF;
	} else {
		f->rax = (u64)ret;
		f->rflags &= ~RFLAGS_CF;
	}
}

const struct abi_personality_ops abi_freebsd_ops = {
	.name   = "freebsd",
	.decode = freebsd_decode,
	.lookup = freebsd_lookup,
	.encode = freebsd_encode,
};
