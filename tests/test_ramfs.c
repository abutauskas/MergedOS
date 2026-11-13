/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ramfs (the writable /tmp and /home/user file system) and the routing of the
 * process system calls through the Linux and FreeBSD personalities.
 */
#include "tests/test.h"
#include "abi_dispatch/abi.h"
#include <mergedos/ramfs.h>

static void test_tree(void)
{
	struct rnode *root = ramfs_new_root(), *a, *b, *n;
	char buf[32];

	CHECK(root && root->dir);
	CHECK_EQ(ramfs_create(root, "dir", true, true, &a), 0);
	CHECK_EQ(ramfs_create(root, "dir", true, true, &a), -MERR_EXIST);
	CHECK_EQ(ramfs_create(root, "dir/f.txt", false, false, &b), 0);
	CHECK_EQ(ramfs_create(root, "dir/f.txt", false, true, &n), -MERR_EXIST);
	CHECK_EQ(ramfs_create(root, "dir/f.txt", false, false, &n), 0);
	CHECK(n == b);
	CHECK_EQ(ramfs_create(root, "nope/f", false, false, &n), -MERR_NOENT);
	CHECK_EQ(ramfs_create(root, "dir/f.txt/x", false, false, &n), -MERR_NOTDIR);
	CHECK_EQ(ramfs_create(root, "", false, false, &n), -MERR_INVAL);
	CHECK_EQ(ramfs_create(root, "dir/..", true, false, &n), -MERR_INVAL);

	/* "." and ".." resolve, and never climb above the root. */
	CHECK_EQ(ramfs_lookup(root, "/dir/./f.txt", &n), 0);
	CHECK(n == b);
	CHECK_EQ(ramfs_lookup(root, "../../dir//f.txt", &n), 0);
	CHECK(n == b);
	CHECK_EQ(ramfs_lookup(root, "dir/../dir", &n), 0);
	CHECK(n == a);

	/* Data: sparse writes zero-fill, reads stop at the end. */
	CHECK_EQ(ramfs_write(b, 0, "hello", 5), 5);
	CHECK_EQ(ramfs_write(b, 1000, "!", 1), 1);
	CHECK_EQ(b->size, 1001);
	CHECK_EQ(ramfs_read(b, 0, buf, sizeof(buf)), (long)sizeof(buf));
	CHECK(!memcmp(buf, "hello\0\0", 7));
	CHECK_EQ(ramfs_read(b, 1000, buf, sizeof(buf)), 1);
	CHECK_EQ(ramfs_read(b, 5000, buf, sizeof(buf)), 0);
	CHECK_EQ(ramfs_truncate(b, 2), 0);
	CHECK_EQ(ramfs_read(b, 0, buf, sizeof(buf)), 2);
	CHECK_EQ(ramfs_read(a, 0, buf, 1), -MERR_ISDIR);
	CHECK_EQ(ramfs_write(b, ~0ULL - 1, "xy", 2), -MERR_RANGE);

	/* Directory listing. */
	CHECK_EQ(ramfs_create(root, "dir/g", false, false, &n), 0);
	CHECK(ramfs_entry(a, 0) && ramfs_entry(a, 1) && !ramfs_entry(a, 2));
	CHECK(!ramfs_entry(b, 0));

	/* Rename: within and across directories, replacing a file, never into itself. */
	CHECK_EQ(ramfs_rename(root, "dir/g", "g2"), 0);
	CHECK_EQ(ramfs_lookup(root, "g2", &n), 0);
	CHECK_EQ(ramfs_lookup(root, "dir/g", &n), -MERR_NOENT);
	CHECK_EQ(ramfs_rename(root, "g2", "dir/f.txt"), 0);       /* replaces f.txt */
	CHECK_EQ(ramfs_lookup(root, "dir/f.txt", &n), 0);
	CHECK_EQ(n->size, 0);
	CHECK_EQ(ramfs_rename(root, "dir", "dir/sub"), -MERR_INVAL);
	CHECK_EQ(ramfs_rename(root, "missing", "x"), -MERR_NOENT);

	/* Unlink: a file still open lives on until its last close. */
	CHECK_EQ(ramfs_lookup(root, "dir/f.txt", &n), 0);
	ramfs_open(n);
	CHECK_EQ(ramfs_unlink(root, "dir", true), -MERR_NOTEMPTY);
	CHECK_EQ(ramfs_unlink(root, "dir", false), -MERR_ISDIR);
	CHECK_EQ(ramfs_unlink(root, "dir/f.txt", true), -MERR_NOTDIR);
	CHECK_EQ(ramfs_unlink(root, "dir/f.txt", false), 0);
	CHECK(n->unlinked);
	CHECK_EQ(ramfs_write(n, 0, "still", 5), 5);
	ramfs_close(n);                                 /* freed here (ASan checks) */
	CHECK_EQ(ramfs_unlink(root, "dir", true), 0);
	CHECK_EQ(ramfs_unlink(root, "", true), -MERR_PERM);
	CHECK(!root->child);
	ramfs_destroy(root);
}

/* ---- personality routing: strong definitions replace the weak NOSYS stubs ---- */

static u64 seen[6];
static int seen_calls;

long msys_clone(struct task *t, u64 flags, u64 newsp, u64 ptid, u64 ctid, u64 tls)
{
	(void)t;
	seen[0] = flags, seen[1] = newsp, seen[2] = ptid, seen[3] = ctid, seen[4] = tls;
	seen_calls++;
	return 77;
}

long msys_wait(struct task *t, int pid, u64 ustatus, int options)
{
	(void)t;
	seen[0] = (u64)pid, seen[1] = ustatus, seen[2] = (u64)options;
	seen_calls++;
	return -MERR_CHILD;
}

static long call(enum abi_personality p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
		 u64 *rflags)
{
	static struct task task;
	struct abi_ctx ctx;
	struct syscall_frame f = { 0 };

	abi_ctx_init(&ctx, p, &task);
	f.rax = nr;
	f.rdi = a0, f.rsi = a1, f.rdx = a2, f.r10 = a3, f.r8 = a4;
	abi_dispatch(&ctx, &f);
	if (rflags)
		*rflags = f.rflags;
	return (long)f.rax;
}

static void test_routing(void)
{
	u64 fl;

	/* Linux clone: flags, newsp, parent_tid, child_tid, tls (x86-64 order). */
	CHECK_EQ(call(ABI_LINUX, 56, 0x3d0f00, 0x1000, 0x2000, 0x3000, 0x4000, NULL), 77);
	CHECK(seen[0] == 0x3d0f00 && seen[1] == 0x1000 && seen[2] == 0x2000 &&
	      seen[3] == 0x3000 && seen[4] == 0x4000);
	/* fork and vfork are clone with fixed flags. */
	CHECK_EQ(call(ABI_LINUX, 57, 0, 0, 0, 0, 0, NULL), 77);
	CHECK_EQ(seen[0], 17u);
	CHECK_EQ(call(ABI_LINUX, 58, 0, 0, 0, 0, 0, NULL), 77);
	CHECK_EQ(seen[0], (u64)(MSYS_CLONE_VM | MSYS_CLONE_VFORK | 17));
	/* wait4 errors: -ECHILD on Linux, ECHILD + carry on FreeBSD. */
	CHECK_EQ(call(ABI_LINUX, 61, (u64)-1, 0x5000, 1, 0, 0, NULL), -10);
	CHECK(seen[0] == (u64)-1 && seen[1] == 0x5000 && seen[2] == 1);
	CHECK_EQ(call(ABI_FREEBSD, 7, 3, 0, 0, 0, 0, &fl), 10);
	CHECK(fl & RFLAGS_CF);
	CHECK_EQ(call(ABI_FREEBSD, 2, 0, 0, 0, 0, 0, &fl), 77);           /* fork */
	CHECK(!(fl & RFLAGS_CF) && seen[0] == 20);
	/* Without a kernel behind them the remaining services report ENOSYS. */
	CHECK_EQ(call(ABI_LINUX, 22, 0x6000, 0, 0, 0, 0, NULL), -38);   /* pipe  */
	CHECK_EQ(call(ABI_LINUX, 202, 0x6000, 0, 0, 0, 0, NULL), -38);  /* futex */
}

void test_ramfs(void)
{
	seen_calls = 0;
	test_tree();
	test_routing();
	CHECK_EQ(seen_calls, 6);
}
