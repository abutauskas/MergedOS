/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * XNU (macOS) x86-64 personality.
 *
 *   number : rax = (class << 24) | number
 *              class 1  Mach traps           (return kern_return_t, no carry)
 *              class 2  BSD/Unix syscalls    (carry flag + positive errno)
 *              class 3  machine-dependent    (thread_fast_set_cthread_self)
 *   args   : rdi, rsi, rdx, r10, r8, r9
 *
 * Covers what a dynamic linker and a small libSystem need to start and run
 * command-line programs; see docs/DARWIN_USERSPACE.md. Mach IPC proper is not
 * implemented: mach_msg answers every request with KERN_NOT_SUPPORTED.
 */
#include "abi_dispatch/abi.h"

#define XNU_CLASS_MACH  1
#define XNU_CLASS_UNIX  2
#define XNU_CLASS_MDEP  3

#define KERN_SUCCESS            0
#define KERN_INVALID_ARGUMENT   4
#define KERN_NOT_SUPPORTED      46
#define MACH_SEND_INVALID_DEST  0x10000003u

/* Fixed Mach port names handed out by the *_self traps. */
#define PORT_TASK   0x103
#define PORT_THREAD 0x203
#define PORT_HOST   0x303
#define PORT_REPLY  0x403

#define VM_FLAGS_ANYWHERE 1

/* ---- BSD class ------------------------------------------------------------ */

static long xn_ok(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return 0;
}

static long xn_notsup(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return -MERR_NOTSUP;
}

static long xn_read(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_read(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2]);
}

/* Darwin open flags -> VFS (Linux) values. */
static u32 xn_oflags(u64 f)
{
	static const struct { u32 from, to; } map[] = {
		{ 0x0004, 0x800 }, { 0x0008, 0x400 }, { 0x0200, 0x40 }, { 0x0400, 0x200 },
		{ 0x0800, 0x80 }, { 0x100000, 0x10000 }, { 0x1000000, 0x80000 },
	};
	u32 r = (u32)f & 3;

	for (unsigned i = 0; i < ARRAY_SIZE(map); i++)
		if (f & map[i].from)
			r |= map[i].to;
	return r;
}

static long xn_open(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_open(ctx->task, c->arg[0], xn_oflags(c->arg[1]));
}

static long xn_openat(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_open(ctx->task, c->arg[1], xn_oflags(c->arg[2]));  /* AT_FDCWD or absolute */
}

static long xn_close(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_close(ctx->task, (int)c->arg[0]);
}

static long xn_pread(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_pread(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2], c->arg[3]);
}

static long xn_lseek(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_lseek(ctx->task, (int)c->arg[0], (s64)c->arg[1], (int)c->arg[2]);
}

static long xn_unlink(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_unlink(ctx->task, c->arg[0], false);
}

static long xn_rmdir(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_unlink(ctx->task, c->arg[0], true);
}

static long xn_mkdir(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_mkdir(ctx->task, c->arg[0]);
}

static long xn_rename(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_rename(ctx->task, c->arg[0], c->arg[1]);
}

static long xn_dup(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_dup(ctx->task, (int)c->arg[0], 0, false);
}

static long xn_dup2(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_dup2(ctx->task, (int)c->arg[0], (int)c->arg[1], false);
}

static long xn_fcntl(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_fcntl(ctx->task, (int)c->arg[0], (int)c->arg[1], c->arg[2]);
}

static long xn_ioctl(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return -MERR_NOTTY;
}

static long xn_getppid(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_getppid(ctx->task);
}

static long xn_gettid(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_gettid(ctx->task);
}

/* struct stat (stat64 layout, 144 bytes). */
static long xn_put_stat(struct abi_ctx *ctx, u64 ubuf, long kind, u64 size)
{
	static const u16 modes[] = {
		[MSYS_ST_FILE] = 0100555, [MSYS_ST_DIR] = 040755, [MSYS_ST_TTY] = 020620,
		[MSYS_ST_FIFO] = 010600, [MSYS_ST_FILE_RW] = 0100644,
	};
	u8 st[144] = { 0 };
	u64 blocks = (size + 511) / 512;
	u32 dev = 1, blksize = 4096;
	u16 mode, nlink = 1;

	if (kind < 0)
		return kind;
	mode = modes[kind < (long)ARRAY_SIZE(modes) ? kind : 0];
	memcpy(st + 0, &dev, 4);
	memcpy(st + 4, &mode, 2);
	memcpy(st + 6, &nlink, 2);
	memcpy(st + 8, &vfs_last_ino, 8);
	memcpy(st + 96, &size, 8);
	memcpy(st + 104, &blocks, 8);
	memcpy(st + 112, &blksize, 4);
	return uaccess_copy_out(ctx->task, ubuf, st, sizeof(st));
}

static long xn_stat(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 size = 0;
	bool dir = false;
	long kind = msys_stat(ctx->task, c->arg[0], &size, &dir);     /* size is set by the call */

	return xn_put_stat(ctx, c->arg[1], kind, size);
}

static long xn_fstat(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 size = 0;
	bool tty = false;
	long kind = msys_fstat(ctx->task, (int)c->arg[0], &size, &tty);

	return xn_put_stat(ctx, c->arg[1], kind, size);
}

static long xn_access(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 size;
	bool dir;
	long rc = msys_stat(ctx->task, c->arg[0], &size, &dir);

	return rc < 0 ? rc : 0;
}

/* mmap(addr, len, prot, flags, fd, off): Darwin MAP_ANON is 0x1000. */
static long xn_mmap(struct abi_ctx *ctx, const struct abi_call *c)
{
	u32 f = (u32)c->arg[3];
	u32 flags = ((f & 0x10) ? MSYS_MAP_FIXED : 0) | ((f & 0x1000) ? MSYS_MAP_ANON : 0);

	return msys_mmap(ctx->task, c->arg[0], c->arg[1], (u32)c->arg[2] & 7, flags,
			 (int)c->arg[4], c->arg[5]);
}

static long xn_munmap(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_munmap(ctx->task, c->arg[0], c->arg[1]);
}

static long xn_mprotect(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_protect(ctx->task, c->arg[0], c->arg[1],
			    ((c->arg[2] & 2) ? MSYS_PROT_W : 0) | ((c->arg[2] & 4) ? MSYS_PROT_X : 0));
}

static long xn_gettimeofday(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 ns = msys_clock_ns(), tv[2] = { ns / 1000000000ULL, (ns % 1000000000ULL) / 1000 };

	if (!c->arg[0])
		return 0;
	return uaccess_copy_out(ctx->task, c->arg[0], tv, sizeof(tv));
}

static long xn_getentropy(struct abi_ctx *ctx, const struct abi_call *c)
{
	long rc;

	if (c->arg[1] > 256)
		return -MERR_INVAL;
	rc = msys_getrandom(ctx->task, c->arg[0], c->arg[1]);
	return rc < 0 ? rc : 0;
}

/* shared_region_check_np: ENOMEM tells dyld to map every library itself. */
static long xn_shared_region(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return -MERR_NOMEM;
}

/* ulock_wait / ulock_wake: one CPU and no contended locks yet; a wait returns
 * at once (spurious wakeups are legal), a wake reports nobody was waiting. */
static long xn_ulock(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return 0;
}

/* ---- sysctl ---------------------------------------------------------------- */

enum { SC_INT, SC_U64, SC_STR };
enum { ID_OSTYPE, ID_OSRELEASE, ID_VERSION, ID_NODENAME, ID_MACHINE, ID_MODEL, ID_OSVERS };

static const struct xn_sysctl {
	const char *name;
	u8 mib0, mib1, kind;
	u64 val;                        /* integer value, or ID_* for strings */
} xn_sysctl[] = {
	{ "kern.ostype",        1, 1,  SC_STR, ID_OSTYPE },
	{ "kern.osrelease",     1, 2,  SC_STR, ID_OSRELEASE },
	{ "kern.version",       1, 4,  SC_STR, ID_VERSION },
	{ "kern.hostname",      1, 10, SC_STR, ID_NODENAME },
	{ "kern.osversion",     1, 65, SC_STR, ID_OSVERS },
	{ "kern.argmax",        1, 8,  SC_INT, 262144 },
	{ "kern.maxproc",       1, 6,  SC_INT, 64 },
	{ "kern.posix1version", 1, 17, SC_INT, 200112 },
	{ "hw.machine",         6, 1,  SC_STR, ID_MACHINE },
	{ "hw.model",           6, 2,  SC_STR, ID_MODEL },
	{ "hw.ncpu",            6, 3,  SC_INT, 1 },
	{ "hw.physmem",         6, 5,  SC_INT, 1 << 30 },
	{ "hw.pagesize",        6, 7,  SC_INT, 4096 },
	{ "hw.cachelinesize",   6, 16, SC_INT, 64 },
	{ "hw.activecpu",       6, 25, SC_INT, 1 },
	{ "hw.memsize",         6, 24, SC_U64, 1ULL << 30 },
	{ "hw.logicalcpu",      6, 103, SC_INT, 1 },
	{ "hw.physicalcpu",     6, 104, SC_INT, 1 },
	{ "hw.optional.x86_64", 6, 107, SC_INT, 1 },
};

static long xn_sysctl_reply(struct abi_ctx *ctx, const struct xn_sysctl *e, u64 uold, u64 uoldlen,
			    u64 unew)
{
	const struct abi_identity *id = abi_identity(ABI_XNU);
	const char *strs[] = { id->sysname, id->release, id->version, id->nodename, id->machine,
			       "MergedOS", "23A344" };
	u32 iv = (u32)e->val;
	const void *src = e->kind == SC_INT ? (const void *)&iv : (const void *)&e->val;
	u64 have = 0, n = e->kind == SC_INT ? 4 : 8;

	if (unew)
		return -MERR_PERM;
	if (e->kind == SC_STR) {
		src = strs[e->val];
		n = strlen(src) + 1;
	}
	if (!uoldlen)
		return 0;
	if (uaccess_copy_in(ctx->task, &have, uoldlen, 8))
		return -MERR_FAULT;
	if (uold && have < n) {
		if (uaccess_copy_out(ctx->task, uoldlen, &n, 8))
			return -MERR_FAULT;
		return -MERR_NOMEM;             /* ENOMEM: buffer too small */
	}
	if (uold && uaccess_copy_out(ctx->task, uold, src, n))
		return -MERR_FAULT;
	return uaccess_copy_out(ctx->task, uoldlen, &n, 8);
}

/* sysctl(int *mib, u_int namelen, old, *oldlenp, new, newlen) */
static long xn_sysctl_mib(struct abi_ctx *ctx, const struct abi_call *c)
{
	int mib[2] = { 0, 0 };

	if (c->arg[1] < 2 || c->arg[1] > 4 ||
	    uaccess_copy_in(ctx->task, mib, c->arg[0], 8))
		return -MERR_INVAL;
	for (unsigned i = 0; i < ARRAY_SIZE(xn_sysctl); i++)
		if (xn_sysctl[i].mib0 == mib[0] && xn_sysctl[i].mib1 == mib[1])
			return xn_sysctl_reply(ctx, &xn_sysctl[i], c->arg[2], c->arg[3], c->arg[4]);
	return -MERR_NOENT;
}

/* sysctlbyname(name, namelen, old, *oldlenp, new, newlen) */
static long xn_sysctl_name(struct abi_ctx *ctx, const struct abi_call *c)
{
	char name[40] = { 0 };

	if (c->arg[1] == 0 || c->arg[1] >= sizeof(name) ||
	    uaccess_copy_in(ctx->task, name, c->arg[0], c->arg[1]))
		return -MERR_INVAL;
	for (unsigned i = 0; i < ARRAY_SIZE(xn_sysctl); i++)
		if (!strcmp(xn_sysctl[i].name, name))
			return xn_sysctl_reply(ctx, &xn_sysctl[i], c->arg[2], c->arg[3], c->arg[4]);
	return -MERR_NOENT;
}

/* The BSD table: number -> thunk, as two parallel arrays built from one list. */
#define BSD_CALLS(X) \
	X(1, abi_posix_exit)  X(3, xn_read)       X(4, abi_posix_write)  X(5, xn_open) \
	X(6, xn_close)        X(10, xn_unlink)    X(20, abi_posix_getpid) X(24, xn_ok) \
	X(25, xn_ok)          X(33, xn_access)    X(39, xn_getppid)      X(41, xn_dup) \
	X(43, xn_ok)          X(46, xn_ok)        X(47, xn_ok)           X(48, xn_ok) \
	X(53, xn_ok)          X(54, xn_ioctl)     X(73, xn_munmap)       X(74, xn_mprotect) \
	X(75, xn_ok)          X(90, xn_dup2)      X(92, xn_fcntl)        X(95, xn_ok) \
	X(116, xn_gettimeofday) X(128, xn_rename) X(136, xn_mkdir)       X(137, xn_rmdir) \
	X(153, xn_pread)      X(169, xn_ok)       X(197, xn_mmap)        X(199, xn_lseek) \
	X(202, xn_sysctl_mib) X(220, xn_notsup)   X(274, xn_sysctl_name) X(294, xn_shared_region) \
	X(327, xn_ok)         X(336, xn_ok)       X(338, xn_stat)        X(339, xn_fstat) \
	X(340, xn_stat)       X(366, xn_ok)       X(372, xn_gettid)      X(396, xn_read) \
	X(397, abi_posix_write) X(398, xn_open)   X(399, xn_close)       X(463, xn_openat) \
	X(500, xn_getentropy) X(515, xn_ulock)    X(516, xn_ulock)

#define BSD_NR(n, f)  n,
#define BSD_ENT(n, f) { f, NULL },
static const u16 xnu_bsd_nr[] = { BSD_CALLS(BSD_NR) };
static const struct abi_sysent xnu_bsd_sysent[] = { BSD_CALLS(BSD_ENT) };

/* ---- Mach class -------------------------------------------------------------- */

static long mach_port(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	switch (c->nr) {
	case 27: return PORT_THREAD;
	case 28: return PORT_TASK;
	case 29: return PORT_HOST;
	default: return PORT_REPLY;     /* mach_reply_port */
	}
}

/* mach_vm_allocate(task, *addr, size, flags) */
static long mach_vm_allocate(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 addr = 0;
	long rc;

	if (uaccess_copy_in(ctx->task, &addr, c->arg[1], 8))
		return KERN_INVALID_ARGUMENT;
	rc = msys_mmap(ctx->task, addr, c->arg[2], 3,
		       MSYS_MAP_ANON | ((c->arg[3] & VM_FLAGS_ANYWHERE) ? 0 : MSYS_MAP_FIXED), -1, 0);
	if (rc < 0)
		return rc == -MERR_NOMEM ? 3 /* KERN_NO_SPACE */ : KERN_INVALID_ARGUMENT;
	addr = (u64)rc;
	return uaccess_copy_out(ctx->task, c->arg[1], &addr, 8) ? KERN_INVALID_ARGUMENT : KERN_SUCCESS;
}

/* mach_vm_map(task, *addr, size, mask, flags, cur_prot): anonymous memory only. */
static long mach_vm_map(struct abi_ctx *ctx, const struct abi_call *c)
{
	struct abi_call a = *c;

	a.arg[3] = c->arg[4];
	if (mach_vm_allocate(ctx, &a) != KERN_SUCCESS)
		return KERN_INVALID_ARGUMENT;
	return KERN_SUCCESS;
}

static long mach_vm_deallocate(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_munmap(ctx->task, c->arg[1], c->arg[2]) ? KERN_INVALID_ARGUMENT : KERN_SUCCESS;
}

/* mach_vm_protect(task, addr, size, set_max, prot) */
static long mach_vm_protect(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_protect(ctx->task, c->arg[1], c->arg[2],
			    ((c->arg[4] & 2) ? MSYS_PROT_W : 0) | ((c->arg[4] & 4) ? MSYS_PROT_X : 0))
		       ? KERN_INVALID_ARGUMENT : KERN_SUCCESS;
}

/* mach_port_allocate(task, right, *name): hand out a fresh-looking name. */
static long mach_port_allocate(struct abi_ctx *ctx, const struct abi_call *c)
{
	u32 name = PORT_REPLY;

	return uaccess_copy_out(ctx->task, c->arg[2], &name, 4) ? KERN_INVALID_ARGUMENT : KERN_SUCCESS;
}

/* mach_msg(msg, option, send_size, rcv_size, rcv_name, timeout, notify): no
 * Mach IPC servers exist, so a request/reply call gets a MIG error reply. */
static long mach_msg_trap(struct abi_ctx *ctx, const struct abi_call *c)
{
	u32 h[9] = { 0 };       /* mach_msg_header_t (24), NDR record (8), RetCode */

	if ((c->arg[1] & 3) != 3 || uaccess_copy_in(ctx->task, h, c->arg[0], 24))
		return MACH_SEND_INVALID_DEST;
	h[2] = h[3];                    /* remote := the reply port the client named */
	h[3] = 0;
	h[1] = sizeof(h);
	h[5] += 100;                    /* MIG: reply id = request id + 100 */
	h[6] = 0;
	h[7] = 0;
	h[8] = KERN_NOT_SUPPORTED;
	return uaccess_copy_out(ctx->task, c->arg[0], h, sizeof(h)) ? MACH_SEND_INVALID_DEST : 0;
}

static long mach_timebase(struct abi_ctx *ctx, const struct abi_call *c)
{
	u32 tb[2] = { 1, 1 };           /* mach_absolute_time counts nanoseconds */

	return uaccess_copy_out(ctx->task, c->arg[0], tb, sizeof(tb)) ? KERN_INVALID_ARGUMENT : 0;
}

static long mach_abs_time(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	(void)c;
	return (long)(msys_clock_ns() & 0x7FFFFFFFFFFFFFFFULL);
}

#define MACH_CALLS(X) \
	X(10, mach_vm_allocate) X(12, mach_vm_deallocate) X(14, mach_vm_protect) \
	X(15, mach_vm_map)      X(16, mach_port_allocate) X(18, xn_ok) X(19, xn_ok) \
	X(21, xn_ok)            X(26, mach_port)  X(27, mach_port)  X(28, mach_port) \
	X(29, mach_port)        X(31, mach_msg_trap) X(32, mach_msg_trap) \
	X(59, xn_ok) X(60, xn_ok) X(61, xn_ok) X(89, mach_timebase) X(91, mach_abs_time)

static const u16 xnu_mach_nr[] = { MACH_CALLS(BSD_NR) };
static const struct abi_sysent xnu_mach_sysent[] = { MACH_CALLS(BSD_ENT) };

/* Class 3: thread_fast_set_cthread_self(base) points %gs at the thread's TSD. */
static long mdep_set_cthread(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_set_gs_base(ctx->task, c->arg[0]);
}

static const struct abi_sysent xnu_mdep = { mdep_set_cthread, NULL };

/* ---- dispatch --------------------------------------------------------------- */

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

static const struct abi_sysent *xnu_find(const u16 *nr, const struct abi_sysent *ent, size_t n,
					 u64 want)
{
	for (size_t i = 0; i < n; i++)
		if (nr[i] == want)
			return &ent[i];
	return NULL;
}

static const struct abi_sysent *xnu_lookup(const struct abi_call *c)
{
	if (c->klass == XNU_CLASS_UNIX)
		return xnu_find(xnu_bsd_nr, xnu_bsd_sysent, ARRAY_SIZE(xnu_bsd_nr), c->nr);
	if (c->klass == XNU_CLASS_MACH)
		return xnu_find(xnu_mach_nr, xnu_mach_sysent, ARRAY_SIZE(xnu_mach_nr), c->nr);
	if (c->klass == XNU_CLASS_MDEP && c->nr == 3)
		return &xnu_mdep;
	return NULL;
}

static void xnu_encode(struct syscall_frame *f, const struct abi_call *c, long ret)
{
	if (c->klass == XNU_CLASS_MACH || c->klass == XNU_CLASS_MDEP) {
		/* Mach traps return a kern_return_t (or a value) and never touch the carry flag. */
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
