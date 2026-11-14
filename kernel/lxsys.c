/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Linux system calls that need the kernel's internals (VFS, memory manager,
 * readiness): consulted before the portable table in
 * abi_dispatch/personality_linux.c. See docs/WINE_STACK.md.
 */
#include <mergedos/kernel.h>
#include <mergedos/cpu.h>
#include <mergedos/mm.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>

#define A(i) (c->arg[i])
#define TASK (ctx->task)
#define LXFN(name) static long name(struct abi_ctx *ctx, const struct abi_call *c)

LXFN(x_zero) { (void)ctx; (void)c; return 0; }
LXFN(x_nosys) { (void)ctx; (void)c; return -MERR_NOSYS; }

/* ---- files ------------------------------------------------------------------- */

LXFN(x_open)     { return vfs_open_at(TASK, VFS_AT_FDCWD, A(0), (u32)A(1), (u32)A(2)); }
LXFN(x_openat)   { return vfs_open_at(TASK, (int)A(0), A(1), (u32)A(2), (u32)A(3)); }
LXFN(x_creat)    { return vfs_open_at(TASK, VFS_AT_FDCWD, A(0), VFS_O_CREAT | VFS_O_TRUNC | VFS_O_WRONLY, (u32)A(1)); }
LXFN(x_getcwd)   { return vfs_getcwd(TASK, A(0), A(1)); }
LXFN(x_chdir)    { return vfs_chdir(TASK, A(0)); }
LXFN(x_fchdir)   { return vfs_fchdir(TASK, (int)A(0)); }
LXFN(x_mkdir)    { return vfs_mkdir_at(TASK, VFS_AT_FDCWD, A(0), (u32)A(1)); }
LXFN(x_mkdirat)  { return vfs_mkdir_at(TASK, (int)A(0), A(1), (u32)A(2)); }
LXFN(x_rmdir)    { return vfs_unlink_at(TASK, VFS_AT_FDCWD, A(0), true); }
LXFN(x_unlink)   { return vfs_unlink_at(TASK, VFS_AT_FDCWD, A(0), false); }
LXFN(x_unlinkat) { return vfs_unlink_at(TASK, (int)A(0), A(1), A(2) & 0x200); }
LXFN(x_rename)   { return vfs_rename_at(TASK, VFS_AT_FDCWD, A(0), VFS_AT_FDCWD, A(1)); }
LXFN(x_renameat) { return vfs_rename_at(TASK, (int)A(0), A(1), (int)A(2), A(3)); }
LXFN(x_symlink)  { return vfs_symlink_at(TASK, A(0), VFS_AT_FDCWD, A(1)); }
LXFN(x_symlinkat) { return vfs_symlink_at(TASK, A(0), (int)A(1), A(2)); }
LXFN(x_readlink) { return vfs_readlink_at(TASK, VFS_AT_FDCWD, A(0), A(1), A(2)); }
LXFN(x_readlinkat) { return vfs_readlink_at(TASK, (int)A(0), A(1), A(2), A(3)); }
LXFN(x_access)   { return vfs_access_at(TASK, VFS_AT_FDCWD, A(0), (u32)A(1)); }
LXFN(x_faccessat) { return vfs_access_at(TASK, (int)A(0), A(1), (u32)A(2)); }
LXFN(x_chmod)    { return vfs_chmod_at(TASK, VFS_AT_FDCWD, A(0), (u32)A(1)); }
LXFN(x_fchmodat) { return vfs_chmod_at(TASK, (int)A(0), A(1), (u32)A(2)); }
LXFN(x_fchmod)   { return vfs_fchmod(TASK, (int)A(0), (u32)A(1)); }
LXFN(x_getdents) { return vfs_getdents(TASK, (int)A(0), A(1), A(2)); }

LXFN(x_umask)
{
	u32 old = TASK->umask;

	TASK->umask = (u32)A(0) & 0777;
	return old;
}

LXFN(x_pwrite)
{
	struct file *f = fd_file(TASK, (int)A(0));

	return f ? vfs_pwrite(TASK, f, A(1), A(2), A(3)) : -MERR_BADF;
}

LXFN(x_ftruncate)
{
	struct file *f = fd_file(TASK, (int)A(0));

	return f ? vfs_ftruncate(TASK, f, A(1)) : -MERR_BADF;
}

LXFN(x_truncate)
{
	long fd = vfs_open_at(TASK, VFS_AT_FDCWD, A(0), VFS_O_WRONLY, 0), rc;

	if (fd < 0)
		return fd;
	rc = vfs_ftruncate(TASK, fd_file(TASK, (int)fd), A(1));
	msys_close(TASK, (int)fd);
	return rc;
}

LXFN(x_fsync)
{
	return fd_file(TASK, (int)A(0)) ? 0 : -MERR_BADF;
}

/* utimensat(dirfd, path, times[2], flags) / futimens via path == NULL */
LXFN(x_utimensat)
{
	u64 t[2][2] = { { 0 } };
	bool have = A(2) != 0;
	struct file *f;
	long fd, rc;

	if (have && uaccess_copy_in(TASK, t, A(2), sizeof(t)))
		return -MERR_FAULT;
	if (!A(1)) {
		f = fd_file(TASK, (int)A(0));
		return f ? vfs_utimes(TASK, f, (const u64 (*)[2])t, have) : -MERR_BADF;
	}
	fd = vfs_open_at(TASK, (int)A(0), A(1), VFS_O_PATH, 0);
	if (fd < 0)
		return fd;
	rc = vfs_utimes(TASK, fd_file(TASK, (int)fd), (const u64 (*)[2])t, have);
	msys_close(TASK, (int)fd);
	return rc;
}

LXFN(x_fcntl)
{
	switch ((int)A(1)) {
	case 5: case 6: case 7:                 /* F_GETLK / F_SETLK / F_SETLKW: no contention here */
		if (A(1) == 5) {
			u8 fl[32];

			if (uaccess_copy_in(TASK, fl, A(2), sizeof(fl)))
				return -MERR_FAULT;
			fl[0] = 2;                      /* F_UNLCK: nobody holds it */
			fl[1] = 0;
			uaccess_copy_out(TASK, A(2), fl, sizeof(fl));
		}
		return fd_file(TASK, (int)A(0)) ? 0 : -MERR_BADF;
	case 8: case 9: case 1032: case 1033: case 1034: case 1031:   /* SETOWN, GETOWN, pipe size, seals */
		return fd_file(TASK, (int)A(0)) ? (A(1) == 1032 ? 65536 : 0) : -MERR_BADF;
	default:
		return msys_fcntl(TASK, (int)A(0), (int)A(1), A(2));
	}
}

/* ---- stat family ---------------------------------------------------------------- */

static void ts_split(u64 ns, u64 *s, u64 *n)
{
	*s = ns / 1000000000ULL;
	*n = ns % 1000000000ULL;
}

static long put_stat(struct task *t, u64 ubuf, const struct kstat *k)
{
	u8 st[144] = { 0 };
	u64 blocks = (k->size + 511) / 512, blksize = 4096, nlink = k->nlink, s, n;
	u32 mode = k->mode;

	memcpy(st + 0, &k->dev, 8);
	memcpy(st + 8, &k->ino, 8);
	memcpy(st + 16, &nlink, 8);
	memcpy(st + 24, &mode, 4);
	memcpy(st + 40, &k->rdev, 8);
	memcpy(st + 48, &k->size, 8);
	memcpy(st + 56, &blksize, 8);
	memcpy(st + 64, &blocks, 8);
	ts_split(k->atime, &s, &n);
	memcpy(st + 72, &s, 8);
	memcpy(st + 80, &n, 8);
	ts_split(k->mtime, &s, &n);
	memcpy(st + 88, &s, 8);
	memcpy(st + 96, &n, 8);
	ts_split(k->ctime, &s, &n);
	memcpy(st + 104, &s, 8);
	memcpy(st + 112, &n, 8);
	return uaccess_copy_out(t, ubuf, st, sizeof(st)) ? -MERR_FAULT : 0;
}

LXFN(x_stat)  { struct kstat k; long rc = vfs_stat_at(TASK, VFS_AT_FDCWD, A(0), true, &k); return rc ? rc : put_stat(TASK, A(1), &k); }
LXFN(x_lstat) { struct kstat k; long rc = vfs_stat_at(TASK, VFS_AT_FDCWD, A(0), false, &k); return rc ? rc : put_stat(TASK, A(1), &k); }

LXFN(x_fstat)
{
	struct file *f = fd_file(TASK, (int)A(0));
	struct kstat k;

	if (!f)
		return -MERR_BADF;
	vfs_fstat(f, &k);
	return put_stat(TASK, A(1), &k);
}

/* Shared by newfstatat and statx: AT_EMPTY_PATH (0x1000), AT_SYMLINK_NOFOLLOW (0x100). */
static long stat_at(struct abi_ctx *ctx, int dirfd, u64 upath, u32 flags, struct kstat *k)
{
	char first = 1;

	if ((flags & 0x1000) && (uaccess_copy_in(TASK, &first, upath, 1) || !first)) {
		struct file *f = fd_file(TASK, dirfd);

		if (!f)
			return -MERR_BADF;
		vfs_fstat(f, k);
		return 0;
	}
	return vfs_stat_at(TASK, dirfd, upath, !(flags & 0x100), k);
}

LXFN(x_newfstatat)
{
	struct kstat k;
	long rc = stat_at(ctx, (int)A(0), A(1), (u32)A(3), &k);

	return rc ? rc : put_stat(TASK, A(2), &k);
}

LXFN(x_statx)
{
	struct kstat k;
	u8 sx[256] = { 0 };
	u32 mask = 0x7ff, bs = 4096, nl, md, i;
	u64 blocks, s, n;
	long rc = stat_at(ctx, (int)A(0), A(1), (u32)A(2), &k);

	if (rc)
		return rc;
	nl = k.nlink;
	md = k.mode;
	blocks = (k.size + 511) / 512;
	memcpy(sx + 0, &mask, 4);
	memcpy(sx + 4, &bs, 4);
	memcpy(sx + 16, &nl, 4);
	memcpy(sx + 28, &md, 2);
	memcpy(sx + 32, &k.ino, 8);
	memcpy(sx + 40, &k.size, 8);
	memcpy(sx + 48, &blocks, 8);
	{
		const u64 times[4] = { k.atime, k.mtime, k.ctime, k.mtime };   /* atime, btime slot, ctime, mtime */
		const unsigned off[4] = { 64, 80, 96, 112 };

		for (i = 0; i < 4; i++) {
			ts_split(times[i], &s, &n);
			memcpy(sx + off[i], &s, 8);
			memcpy(sx + off[i] + 8, &n, 4);
		}
	}
	{ u32 dmaj = 0, dmin = (u32)k.dev; memcpy(sx + 136, &dmaj, 4); memcpy(sx + 140, &dmin, 4); }
	return uaccess_copy_out(TASK, A(4), sx, sizeof(sx)) ? -MERR_FAULT : 0;
}

/* statfs/fstatfs: a tmpfs-like answer. */
LXFN(x_statfs)
{
	u64 sf[15] = { 0x01021994, 4096, 1 << 16, 1 << 15, 1 << 15, 1 << 16, 1 << 15, 0, 255, 4096 };

	return uaccess_copy_out(TASK, A(1), sf, sizeof(sf)) ? -MERR_FAULT : 0;
}

/* ---- ioctl, misc --------------------------------------------------------------- */

LXFN(x_ioctl)
{
	struct file *f = fd_file(TASK, (int)A(0));

	if (!f)
		return -MERR_BADF;
	switch ((u32)A(1)) {
	case 0x5451: return 0;                  /* FIOCLEX */
	case 0x5450: return 0;                  /* FIONCLEX */
	case 0x5421:                            /* FIONBIO */
		{ int on = 0; uaccess_copy_in(TASK, &on, A(2), 4); f->flags = on ? f->flags | VFS_O_NONBLOCK : f->flags & ~VFS_O_NONBLOCK; }
		return 0;
	case 0x541B:                            /* FIONREAD */
		if (f->type == F_OPS && f->ops->ioctl)
			return f->ops->ioctl(TASK, f, (u32)A(1), A(2));
		{ int n = f->type == F_PIPE ? (int)f->pipe->count : 0; uaccess_copy_out(TASK, A(2), &n, 4); }
		return 0;
	default:
		if (f->type == F_OPS && f->ops->ioctl)
			return f->ops->ioctl(TASK, f, (u32)A(1), A(2));
		{
			struct abi_call p = *c;
			static const u16 winsize[4] = { 25, 80, 0, 0 };
			u64 size;
			bool tty = false;

			if (p.arg[1] == 0x5413 && msys_fstat(TASK, (int)p.arg[0], &size, &tty) >= 0 && tty)
				return uaccess_copy_out(TASK, p.arg[2], winsize, sizeof(winsize)) ? -MERR_FAULT : 0;
		}
		return -MERR_NOTTY;
	}
}

LXFN(x_memfd_create)
{
	char name[64];
	int i;

	for (i = 0; i < 63; i++) {
		if (uaccess_copy_in(TASK, &name[i], A(0) + (u64)i, 1))
			return -MERR_FAULT;
		if (!name[i])
			break;
	}
	name[i] = '\0';
	return vfs_memfd_create(TASK, name, (u32)A(1));
}

/* ---- memory --------------------------------------------------------------------- */

LXFN(x_mmap)
{
	u32 prot = ((A(2) & 1) ? VMM_R : 0) | ((A(2) & 2) ? VMM_W : 0) | ((A(2) & 4) ? VMM_X : 0);
	u32 flags = (u32)A(3) & (MSYS_MAP_SHARED | MSYS_MAP_PRIVATE | MSYS_MAP_FIXED | MSYS_MAP_ANON |
				 MSYS_MAP_FIXED_NOREPLACE);

	return msys_mmap(TASK, A(0), A(1), prot, flags, (int)A(4), A(5));
}

LXFN(x_mprotect)
{
	return umm_protect(TASK, A(0), A(1), ((A(2) & 1) ? VMM_R : 0) | ((A(2) & 2) ? VMM_W : 0) |
					     ((A(2) & 4) ? VMM_X : 0));
}

LXFN(x_mremap)  { return umm_mremap(TASK, A(0), A(1), A(2), (u32)A(3), A(4)); }
LXFN(x_madvise) { return umm_madvise(TASK, A(0), A(1), (int)A(2)); }

/* arch_prctl: Wine keeps its TEB in %gs. */
LXFN(x_arch_prctl)
{
	switch (A(0)) {
	case 0x1001: return msys_set_gs_base(TASK, A(1));
	case 0x1004: { u64 b = thread_current()->gs_base; return uaccess_copy_out(TASK, A(1), &b, 8) ? -MERR_FAULT : 0; }
	case 0x1002: return msys_set_fs_base(TASK, A(1));
	case 0x1003: return msys_get_fs_base(TASK, A(1));
	default:     return -MERR_INVAL;
	}
}

/* prctl: names and flags are remembered nowhere; the calls just succeed. */
LXFN(x_prctl) { (void)c; (void)ctx; return 0; }

LXFN(x_prlimit)
{
	u64 lim[2];
	int res = (int)(c->nr == 97 ? A(0) : A(1));
	u64 out = c->nr == 97 ? A(1) : A(3);

	switch (res) {
	case 3:  lim[0] = 8ULL << 20; lim[1] = ~0ULL; break;           /* STACK */
	case 7:  lim[0] = lim[1] = TASK_FD_MAX; break;                 /* NOFILE */
	default: lim[0] = lim[1] = ~0ULL; break;
	}
	return out && uaccess_copy_out(TASK, out, lim, sizeof(lim)) ? -MERR_FAULT : 0;
}

LXFN(x_sysinfo)
{
	struct pmm_stats st;
	u64 si[14] = { 0 };

	pmm_get_stats(&st);
	si[0] = msys_clock_ns() / 1000000000ULL;
	si[4] = st.usable_frames * 4096;
	si[5] = st.free_frames * 4096;
	((u32 *)&si[10])[0] = 1;
	((u32 *)&si[11])[0] = 4096;
	return uaccess_copy_out(TASK, A(0), si, 112) ? -MERR_FAULT : 0;
}

LXFN(x_sigaltstack) { return task_sigaltstack(TASK, A(0), A(1)); }
LXFN(x_tgkill)      { return task_tgkill(TASK, (int)A(0), (int)A(1), (int)A(2)); }
LXFN(x_tkill)       { return task_tgkill(TASK, 0, (int)A(0), (int)A(1)); }
LXFN(x_sigtimedwait) { return task_sigtimedwait(TASK, A(0), A(1), A(2)); }
LXFN(x_sigpending)  { return task_sigpending(TASK, A(0)); }
LXFN(x_sigsuspend)  { return task_sigsuspend(TASK, A(0)); }

/* futex: REQUEUE / CMP_REQUEUE move waiters to a second word; the rest is msys_futex. */
LXFN(x_futex)
{
	int cmd = (int)A(1) & 0x7F;

	if (cmd == 3 || cmd == 4) {
		u32 cur;
		long woken;
		int moved = 0;

		if (cmd == 4 && (uaccess_copy_in(TASK, &cur, A(0), 4) || cur != (u32)A(5)))
			return -MERR_AGAIN;
		woken = thread_wake((const void *)A(0), TASK->pml4, (int)MOS_MIN((u32)A(2), 0x7FFFFFFFu));
		for (int i = 0; i < SCHED_MAX_THREADS && moved < (int)A(3); i++) {
			struct thread *th = thread_slot(i);

			if (th && th->state == T_BLOCKED && th->chan == (const void *)A(0) && th->chan_aux == TASK->pml4) {
				th->chan = (const void *)A(4);
				moved++;
			}
		}
		return woken + moved;
	}
	return msys_futex(TASK, A(0), (int)A(1), (u32)A(2), A(3));
}

LXFN(x_setsid) { return TASK->pid; (void)c; }

LXFN(x_time)
{
	u64 s = msys_clock_ns() / 1000000000ULL;

	return A(0) && uaccess_copy_out(TASK, A(0), &s, 8) ? -MERR_FAULT : (long)s;
}

LXFN(x_gettimeofday)
{
	u64 ns = msys_clock_ns(), tv[2] = { ns / 1000000000ULL, ns % 1000000000ULL / 1000 };

	return A(0) && uaccess_copy_out(TASK, A(0), tv, sizeof(tv)) ? -MERR_FAULT : 0;
}

LXFN(x_clock_getres)
{
	u64 ts[2] = { 0, 1000000 };

	return A(1) && uaccess_copy_out(TASK, A(1), ts, sizeof(ts)) ? -MERR_FAULT : 0;
}

LXFN(x_getrusage)
{
	u8 ru[144] = { 0 };

	return uaccess_copy_out(TASK, A(1), ru, sizeof(ru)) ? -MERR_FAULT : 0;
}

long lx_socket(struct abi_ctx *ctx, const struct abi_call *c);
long lx_socketpair(struct abi_ctx *ctx, const struct abi_call *c);
long lx_bind(struct abi_ctx *ctx, const struct abi_call *c);
long lx_listen(struct abi_ctx *ctx, const struct abi_call *c);
long lx_accept(struct abi_ctx *ctx, const struct abi_call *c);
long lx_accept4(struct abi_ctx *ctx, const struct abi_call *c);
long lx_connect(struct abi_ctx *ctx, const struct abi_call *c);
long lx_shutdown(struct abi_ctx *ctx, const struct abi_call *c);
long lx_getsockname(struct abi_ctx *ctx, const struct abi_call *c);
long lx_getpeername(struct abi_ctx *ctx, const struct abi_call *c);
long lx_setsockopt(struct abi_ctx *ctx, const struct abi_call *c);
long lx_getsockopt(struct abi_ctx *ctx, const struct abi_call *c);
long lx_sendmsg(struct abi_ctx *ctx, const struct abi_call *c);
long lx_sendto(struct abi_ctx *ctx, const struct abi_call *c);
long lx_recvfrom(struct abi_ctx *ctx, const struct abi_call *c);
long lx_recvmsg(struct abi_ctx *ctx, const struct abi_call *c);
long lx_sendmmsg(struct abi_ctx *ctx, const struct abi_call *c);
long lx_poll(struct abi_ctx *ctx, const struct abi_call *c);
long lx_ppoll(struct abi_ctx *ctx, const struct abi_call *c);
long lx_select(struct abi_ctx *ctx, const struct abi_call *c);
long lx_pselect6(struct abi_ctx *ctx, const struct abi_call *c);
long lx_epoll_create(struct abi_ctx *ctx, const struct abi_call *c);
long lx_epoll_ctl(struct abi_ctx *ctx, const struct abi_call *c);
long lx_epoll_wait(struct abi_ctx *ctx, const struct abi_call *c);
long lx_eventfd(struct abi_ctx *ctx, const struct abi_call *c);

#define X(n, f) [n] = { f, #f }
static const struct abi_sysent ext[512] = {
	X(2, x_open), X(257, x_openat), X(85, x_creat), X(79, x_getcwd), X(80, x_chdir), X(81, x_fchdir),
	X(83, x_mkdir), X(258, x_mkdirat), X(84, x_rmdir), X(87, x_unlink), X(263, x_unlinkat),
	X(82, x_rename), X(264, x_renameat), X(316, x_renameat), X(88, x_symlink), X(266, x_symlinkat),
	X(89, x_readlink), X(267, x_readlinkat), X(21, x_access), X(269, x_faccessat), X(439, x_faccessat),
	X(90, x_chmod), X(268, x_fchmodat), X(91, x_fchmod), X(217, x_getdents), X(95, x_umask),
	X(18, x_pwrite), X(77, x_ftruncate), X(76, x_truncate), X(74, x_fsync), X(75, x_fsync),
	X(280, x_utimensat), X(72, x_fcntl), X(73, x_zero), X(4, x_stat), X(6, x_lstat), X(5, x_fstat),
	X(262, x_newfstatat), X(332, x_statx), X(137, x_statfs), X(138, x_statfs), X(16, x_ioctl),
	X(319, x_memfd_create), X(9, x_mmap), X(10, x_mprotect), X(25, x_mremap), X(28, x_madvise),
	X(158, x_arch_prctl), X(157, x_prctl), X(97, x_prlimit), X(302, x_prlimit), X(160, x_zero),
	X(112, x_setsid), X(202, x_futex), X(188, x_nosys), X(189, x_nosys), X(190, x_nosys), X(191, x_nosys), X(192, x_nosys), X(193, x_nosys), X(194, x_nosys), X(195, x_nosys), X(196, x_nosys), X(131, x_sigaltstack), X(234, x_tgkill), X(200, x_tkill), X(128, x_sigtimedwait), X(127, x_sigpending), X(130, x_sigsuspend), X(201, x_time), X(99, x_sysinfo), X(96, x_gettimeofday), X(229, x_clock_getres), X(98, x_getrusage),
	X(92, x_zero), X(93, x_zero), X(94, x_zero), X(260, x_zero), X(285, x_nosys), X(221, x_zero),
	X(203, x_zero), X(142, x_zero), X(144, x_zero), X(140, x_zero), X(141, x_zero), X(435, x_nosys),
	X(274, x_zero),
	X(41, lx_socket), X(42, lx_connect), X(43, lx_accept), X(44, lx_sendto), X(45, lx_recvfrom),
	X(46, lx_sendmsg), X(47, lx_recvmsg), X(48, lx_shutdown), X(49, lx_bind), X(50, lx_listen),
	X(51, lx_getsockname), X(52, lx_getpeername), X(53, lx_socketpair), X(54, lx_setsockopt),
	X(55, lx_getsockopt), X(288, lx_accept4), X(307, lx_sendmmsg), X(7, lx_poll), X(23, lx_select),
	X(213, lx_epoll_create), X(291, lx_epoll_create), X(232, lx_epoll_wait), X(281, lx_epoll_wait),
	X(233, lx_epoll_ctl), X(270, lx_pselect6), X(271, lx_ppoll), X(284, lx_eventfd), X(290, lx_eventfd),
};

const struct abi_sysent *lx_ext_lookup(u64 nr)
{
	return nr < ARRAY_SIZE(ext) && ext[nr].fn ? &ext[nr] : NULL;
}

/* Development aid: log every call (and result) of the process named by lx_trace_name. */
const char *lx_trace_name;
int lx_trace_pid;

void lx_trace(struct abi_ctx *ctx, const struct abi_call *c, long ret)
{
	if ((lx_trace_name && !strcmp(ctx->task->name, lx_trace_name)) ||
	    (lx_trace_pid && ctx->task->pid == lx_trace_pid))
		kprintf("[trace] %d: nr %llu (%llx %llx %llx) = %ld\n", ctx->task->pid,
			(unsigned long long)c->nr, (unsigned long long)c->arg[0],
			(unsigned long long)c->arg[1], (unsigned long long)c->arg[2], ret);
}
