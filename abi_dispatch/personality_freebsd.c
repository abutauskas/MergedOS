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

static const struct abi_sysent freebsd_sysent[] = {
	[1]  = { abi_posix_exit,   "exit" },
	[4]  = { abi_posix_write,  "write" },
	[20] = { abi_posix_getpid, "getpid" },
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
