/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * What each personality tells a program about "the operating system".
 *
 * A Linux binary asking uname() must hear "Linux"; a Windows binary asking
 * for the OS version must hear a Windows version new enough for it to start
 * (many installers and browsers refuse anything they consider outdated).
 * This is the same answer-in-kind that Wine and FreeBSD's Linux layer give:
 * the program is told about the ABI it is actually running against.
 *
 * Linux is served today (uname). The NT and XNU values are the single
 * source for the PEB / RtlGetVersion and sysctl paths when those exist.
 */
#include "abi_dispatch/abi.h"

static const struct abi_identity identities[ABI_COUNT] = {
	[ABI_NONE]    = { "MergedOS", "mergedos", MERGEDOS_VERSION, "MergedOS", "x86_64", 0, 1, 0 },
	[ABI_LINUX]   = { "Linux", "mergedos", "6.6.0-mergedos", "#1 SMP MergedOS", "x86_64", 6, 6, 0 },
	[ABI_FREEBSD] = { "FreeBSD", "mergedos", "14.1-RELEASE", "FreeBSD 14.1-RELEASE MergedOS",
			  "amd64", 14, 1, 0 },
	[ABI_XNU]     = { "Darwin", "mergedos", "23.0.0", "Darwin Kernel Version 23.0.0 (macOS 14.0)",
			  "x86_64", 14, 0, 0 },
	[ABI_NT]      = { "Windows_NT", "MERGEDOS", "10.0.22631", "Windows 11 23H2", "AMD64",
			  10, 0, 22631 },
};

const struct abi_identity *abi_identity(enum abi_personality p)
{
	return &identities[(unsigned)p < ABI_COUNT ? p : ABI_NONE];
}

/*
 * Fallbacks for hosts that link the dispatcher without a kernel behind it
 * (the mpkg tool, unit tests that do not care). The kernel's definitions in
 * kernel/syscall.c replace them at link time.
 */
#define WEAK __attribute__((weak))
WEAK long msys_read(struct task *t, int fd, u64 b, u64 n) { (void)t; (void)fd; (void)b; (void)n; return -MERR_NOSYS; }
WEAK long msys_open(struct task *t, u64 p, bool w) { (void)t; (void)p; (void)w; return -MERR_NOSYS; }
WEAK long msys_close(struct task *t, int fd) { (void)t; (void)fd; return -MERR_NOSYS; }
WEAK long msys_lseek(struct task *t, int fd, s64 o, int w) { (void)t; (void)fd; (void)o; (void)w; return -MERR_NOSYS; }
WEAK long msys_fstat(struct task *t, int fd, u64 *s, bool *tty) { (void)t; (void)fd; (void)s; (void)tty; return -MERR_NOSYS; }
WEAK long msys_stat(struct task *t, u64 p, u64 *s, bool *d) { (void)t; (void)p; (void)s; (void)d; return -MERR_NOSYS; }
WEAK long msys_brk(struct task *t, u64 a) { (void)t; (void)a; return -MERR_NOSYS; }
WEAK long msys_mmap_anon(struct task *t, u64 n) { (void)t; (void)n; return -MERR_NOSYS; }
WEAK long msys_set_fs_base(struct task *t, u64 b) { (void)t; (void)b; return -MERR_NOSYS; }
WEAK u64 msys_clock_ns(void) { return 0; }
WEAK long msys_protect(struct task *t, u64 a, u64 n, u32 p) { (void)t; (void)a; (void)n; (void)p; return -MERR_NOSYS; }
