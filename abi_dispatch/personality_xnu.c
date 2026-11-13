/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * XNU (macOS) x86-64 personality.
 *
 *   number : rax = (class << 24) | number
 *              class 1  Mach traps           (return kern_return_t, no carry)
 *              class 2  BSD/Unix syscalls    (carry flag + positive errno)
 *              class 3  machine-dependent
 *   args   : rdi, rsi, rdx, r10, r8, r9
 *
 * Only the BSD class is populated in this PoC. Real macOS binaries reach the
 * kernel through libSystem and dyld, which also need Mach traps (class 1)
 * and Mach IPC; see docs/ABI_TRANSLATION.md for the plan.
 */
#include "abi_dispatch/abi.h"

#define XNU_CLASS_MACH 1
#define XNU_CLASS_UNIX 2

#define KERN_INVALID_ARGUMENT 4

static const struct abi_sysent xnu_bsd_sysent[] = {
	[1]  = { abi_posix_exit,   "exit" },
	[4]  = { abi_posix_write,  "write" },
	[20] = { abi_posix_getpid, "getpid" },
};

static void xnu_decode(const struct syscall_frame *f, struct abi_call *c)
{
	c->nr = f->rax & 0x00FFFFFFULL;
	c->klass = (u32)((f->rax >> 24) & 0xFF);
	c->arg[0] = f->rdi;
	c->arg[1] = f->rsi;
	c->arg[2] = f->rdx;
	c->arg[3] = f->r10;
	c->arg[4] = f->r8;
	c->arg[5] = f->r9;
	c->usp = f->rsp;
}

static const struct abi_sysent *xnu_lookup(const struct abi_call *c)
{
	if (c->klass == XNU_CLASS_UNIX && c->nr < ARRAY_SIZE(xnu_bsd_sysent) &&
	    xnu_bsd_sysent[c->nr].fn)
		return &xnu_bsd_sysent[c->nr];
	return NULL;
}

static void xnu_encode(struct syscall_frame *f, const struct abi_call *c, long ret)
{
	if (c->klass == XNU_CLASS_MACH) {
		/* Mach traps return a kern_return_t and never touch the carry flag. */
		f->rax = ret < 0 ? KERN_INVALID_ARGUMENT : (u64)ret;
		return;
	}
	if (ret < 0) {
		f->rax = (u64)abi_errno_bsd((int)-ret);
		f->rflags |= RFLAGS_CF;
	} else {
		f->rax = (u64)ret;
		f->rflags &= ~RFLAGS_CF;
	}
}

const struct abi_personality_ops abi_xnu_ops = {
	.name   = "xnu",
	.decode = xnu_decode,
	.lookup = xnu_lookup,
	.encode = xnu_encode,
};
