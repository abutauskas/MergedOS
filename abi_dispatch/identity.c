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
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define NOSYS { return -MERR_NOSYS; }
WEAK long msys_read(struct task *t, int fd, u64 b, u64 n) NOSYS
WEAK long msys_open(struct task *t, u64 p, u32 f) NOSYS
WEAK long msys_close(struct task *t, int fd) NOSYS
WEAK long msys_lseek(struct task *t, int fd, s64 o, int w) NOSYS
WEAK u64 vfs_last_ino;
WEAK long msys_fstat(struct task *t, int fd, u64 *s, bool *tty) NOSYS
WEAK long msys_stat(struct task *t, u64 p, u64 *s, bool *d) NOSYS
WEAK long msys_pipe(struct task *t, int fds[2], u32 f) NOSYS
WEAK long msys_dup(struct task *t, int fd, int min, bool c) NOSYS
WEAK long msys_dup2(struct task *t, int a, int b, bool c) NOSYS
WEAK long msys_fcntl(struct task *t, int fd, int cmd, u64 a) NOSYS
WEAK long msys_mkdir(struct task *t, u64 p) NOSYS
WEAK long msys_unlink(struct task *t, u64 p, bool d) NOSYS
WEAK long msys_rename(struct task *t, u64 a, u64 b) NOSYS
WEAK long msys_readdir(struct task *t, int fd, char *n, size_t c, bool *d, u64 *i) NOSYS
WEAK long msys_brk(struct task *t, u64 a) NOSYS
WEAK long msys_mmap_anon(struct task *t, u64 n) NOSYS
WEAK long msys_mmap(struct task *t, u64 a, u64 n, u32 p, u32 f, int fd, u64 o) NOSYS
WEAK long msys_munmap(struct task *t, u64 a, u64 n) NOSYS
WEAK long msys_pread(struct task *t, int fd, u64 b, u64 n, u64 o) NOSYS
WEAK long msys_readlink(struct task *t, u64 p, u64 b, u64 n) NOSYS
WEAK long msys_getrandom(struct task *t, u64 b, u64 n) NOSYS
WEAK long msys_set_fs_base(struct task *t, u64 b) NOSYS
WEAK long msys_get_fs_base(struct task *t, u64 a) NOSYS
WEAK long msys_set_gs_base(struct task *t, u64 b) NOSYS
WEAK u64 msys_clock_ns(void) { return 0; }
WEAK long msys_nanosleep(struct task *t, u64 ns) NOSYS
WEAK long msys_protect(struct task *t, u64 a, u64 n, u32 p) NOSYS
WEAK long msys_clone(struct task *t, u64 f, u64 sp, u64 p, u64 c, u64 tls) NOSYS
WEAK long msys_execve(struct task *t, u64 p, u64 a, u64 e) NOSYS
WEAK long msys_wait(struct task *t, int pid, u64 s, int o) NOSYS
WEAK long msys_getppid(struct task *t) NOSYS
WEAK long msys_gettid(struct task *t) NOSYS
WEAK long msys_setpgid(struct task *t, int pid, int pg) NOSYS
WEAK long msys_getpgid(struct task *t, int pid) NOSYS
WEAK long msys_thread_exit(struct task *t, int s) NOSYS
WEAK long msys_set_tid_address(struct task *t, u64 a) NOSYS
WEAK long msys_futex(struct task *t, u64 a, int op, u32 v, u64 to) NOSYS
WEAK long msys_thread_spawn(struct task *t, u64 e, u64 a, u64 s, u64 x) NOSYS
WEAK long msys_thread_join(struct task *t, int tid, u64 to) NOSYS
WEAK long msys_kill(struct task *t, int pid, int sig) NOSYS
WEAK long msys_sigaction(struct task *t, int sig, u64 a, u64 o) NOSYS
WEAK long msys_sigprocmask(struct task *t, int how, u64 s, u64 o) NOSYS
WEAK long msys_sigreturn(struct task *t) NOSYS

/* Kernel-only Linux calls (kernel/lxsys.c) take precedence over the portable table. */
WEAK const struct abi_sysent *lx_ext_lookup(u64 nr) { return NULL; }
WEAK void lx_trace(struct abi_ctx *ctx, const struct abi_call *c, long ret) {}
