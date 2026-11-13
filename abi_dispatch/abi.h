/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_ABI_H
#define MERGEDOS_ABI_H

/*
 * ABI dispatch: one kernel, several system-call dialects.
 *
 * Every task carries a *personality* chosen at exec time from the binary's
 * own headers (see binfmt.h). All personalities enter the kernel through the
 * same SYSCALL instruction; abi_dispatch() then
 *
 *   1. decodes the trap frame using that personality's register convention,
 *   2. looks the call up in that personality's table (O(1), dense array),
 *   3. runs a thunk that marshals arguments into a native msys_*() service,
 *   4. encodes the result in that personality's error convention.
 *
 * No instruction is ever interpreted or rewritten: foreign code runs
 * directly on the CPU and only the kernel boundary is translated.
 */
#include <mergedos/compat.h>

enum abi_personality {
	ABI_NONE = 0,
	ABI_LINUX,      /* Linux x86-64: nr in rax, -errno in rax                    */
	ABI_FREEBSD,    /* FreeBSD amd64: nr in rax, errno in rax + carry flag       */
	ABI_XNU,        /* macOS x86-64: class<<24 | nr in rax, errno + carry flag   */
	ABI_NT,         /* Windows x64: service index in eax, args r10/rdx/r8/r9 +
			   stack, NTSTATUS in eax                                    */
	ABI_COUNT
};

/* Register frame saved by syscall_entry (entry.S); lowest address first. */
struct syscall_frame {
	u64 r15, r14, r13, r12, r11, r10, r9, r8;
	u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
	u64 rip, rflags, rsp;
};

#define RFLAGS_CF 0x1ULL

/* A system call normalised out of whichever convention it arrived in. */
struct abi_call {
	u64 nr;         /* call number within its class/table            */
	u32 klass;      /* XNU syscall class / NT service table index    */
	u64 arg[6];     /* register arguments, in the personality's order */
	u64 usp;        /* user stack pointer, for stack-passed arguments */
};

struct task;
struct abi_ctx;

/* Thunk: returns >= 0 on success or -MERR_* (native error) on failure. */
typedef long (*abi_handler_fn)(struct abi_ctx *ctx, const struct abi_call *c);

struct abi_sysent {
	abi_handler_fn fn;
	const char *name;
};

struct abi_stats {
	u64 calls;              /* system calls dispatched                    */
	u64 unknown;            /* calls with no table entry (-> ENOSYS etc.) */
	u64 errors;             /* calls that returned an error               */
	u64 last_unknown_nr;
};

struct abi_ctx {
	enum abi_personality personality;
	struct task *task;
	struct abi_stats stats;
};

struct abi_personality_ops {
	const char *name;
	void (*decode)(const struct syscall_frame *f, struct abi_call *c);
	const struct abi_sysent *(*lookup)(const struct abi_call *c);
	void (*encode)(struct syscall_frame *f, const struct abi_call *c, long ret);
};

extern const struct abi_personality_ops abi_linux_ops;
extern const struct abi_personality_ops abi_freebsd_ops;
extern const struct abi_personality_ops abi_xnu_ops;
extern const struct abi_personality_ops abi_nt_ops;

void abi_ctx_init(struct abi_ctx *ctx, enum abi_personality p, struct task *task);
void abi_dispatch(struct abi_ctx *ctx, struct syscall_frame *f);
const char *abi_personality_name(enum abi_personality p);
const struct abi_personality_ops *abi_personality_ops(enum abi_personality p);

/* Thunks shared by the POSIX-shaped personalities (Linux, FreeBSD, XNU). */
long abi_posix_write(struct abi_ctx *ctx, const struct abi_call *c);
long abi_posix_exit(struct abi_ctx *ctx, const struct abi_call *c);
long abi_posix_getpid(struct abi_ctx *ctx, const struct abi_call *c);

/* errno_map.c: native -MERR_* -> foreign error numbering. `merr` is positive. */
int abi_errno_linux(int merr);
int abi_errno_bsd(int merr);            /* FreeBSD and XNU share these values */
u32 abi_ntstatus(int merr);

#define NT_STATUS_SUCCESS                0x00000000u
#define NT_STATUS_UNSUCCESSFUL           0xC0000001u
#define NT_STATUS_NOT_IMPLEMENTED        0xC0000002u
#define NT_STATUS_ACCESS_VIOLATION       0xC0000005u
#define NT_STATUS_INVALID_HANDLE         0xC0000008u
#define NT_STATUS_INVALID_PARAMETER      0xC000000Du
#define NT_STATUS_NO_MEMORY              0xC0000017u
#define NT_STATUS_INVALID_SYSTEM_SERVICE 0xC000001Cu
#define NT_STATUS_ACCESS_DENIED          0xC0000022u
#define NT_STATUS_OBJECT_NAME_NOT_FOUND  0xC0000034u
#define NT_STATUS_INVALID_IMAGE_FORMAT   0xC000007Bu
#define NT_STATUS_NOT_SUPPORTED          0xC00000BBu
#define NT_STATUS_IO_DEVICE_ERROR        0xC0000185u
#define NT_STATUS_RETRY                  0xC000022Du
#define NT_STATUS_BUFFER_OVERFLOW        0x80000005u

/*
 * Native kernel services ("msys") and user-memory accessors.
 *
 * This is the link seam between the dispatcher and the rest of the system:
 * the kernel implements these in kernel/syscall.c, the host unit tests in
 * tests/mock_native.c. Direct calls, no function-pointer indirection.
 */
long msys_write(struct task *t, int fd, u64 ubuf, u64 len);
long msys_getpid(struct task *t);
void msys_exit(struct task *t, int status);
int  uaccess_copy_in(struct task *t, void *dst, u64 uaddr, size_t len);
int  uaccess_copy_out(struct task *t, u64 uaddr, const void *src, size_t len);

/*
 * Files. Paths are user pointers; open flags use the Linux values (VFS_O_*
 * in kernel/include/mergedos/vfs.h). fstat/stat return the kind of object.
 */
#define MSYS_ST_FILE    0               /* read-only file (ramdisk) */
#define MSYS_ST_DIR     1
#define MSYS_ST_TTY     2
#define MSYS_ST_FIFO    3
#define MSYS_ST_FILE_RW 4               /* writable file (ramfs)    */
long msys_read(struct task *t, int fd, u64 ubuf, u64 len);
long msys_open(struct task *t, u64 upath, u32 flags);
long msys_close(struct task *t, int fd);
long msys_lseek(struct task *t, int fd, s64 offset, int whence);
long msys_fstat(struct task *t, int fd, u64 *size, bool *is_tty);
extern u64 vfs_last_ino;                    /* fake st_ino of the last stat */
long msys_stat(struct task *t, u64 upath, u64 *size, bool *is_dir);
long msys_pipe(struct task *t, int fds[2], u32 flags);
long msys_dup(struct task *t, int fd, int min, bool cloexec);
long msys_dup2(struct task *t, int oldfd, int newfd, bool cloexec);
#define MSYS_F_DUPFD 0
#define MSYS_F_GETFD 1
#define MSYS_F_SETFD 2
#define MSYS_F_GETFL 3
#define MSYS_F_SETFL 4
#define MSYS_F_DUPFD_CLOEXEC 1030
long msys_fcntl(struct task *t, int fd, int cmd, u64 arg);
long msys_mkdir(struct task *t, u64 upath);
long msys_unlink(struct task *t, u64 upath, bool dir);
long msys_rename(struct task *t, u64 ufrom, u64 uto);
long msys_readdir(struct task *t, int fd, char *name, size_t cap, bool *is_dir, u64 *ino);

/* pread: read at an explicit offset without disturbing the file position. */
long msys_pread(struct task *t, int fd, u64 ubuf, u64 len, u64 offset);
/* readlink: only "/proc/self/exe" is resolved (to the running program's path). */
long msys_readlink(struct task *t, u64 upath, u64 ubuf, u64 bufsize);
long msys_getrandom(struct task *t, u64 ubuf, u64 len);

/* Memory and time. */
long msys_brk(struct task *t, u64 addr);                /* returns the (new) break */
long msys_mmap_anon(struct task *t, u64 len);           /* returns the address     */
/* Full mmap: prot in VMM_* bits, flags in MSYS_MAP_*; fd/offset for file maps. */
#define MSYS_MAP_SHARED         0x01
#define MSYS_MAP_PRIVATE        0x02
#define MSYS_MAP_FIXED          0x10
#define MSYS_MAP_ANON           0x20
#define MSYS_MAP_FIXED_NOREPLACE 0x100000
long msys_mmap(struct task *t, u64 addr, u64 len, u32 prot, u32 flags, int fd, u64 offset);
long msys_munmap(struct task *t, u64 addr, u64 len);
long msys_set_fs_base(struct task *t, u64 base);
long msys_get_fs_base(struct task *t, u64 uaddr);
long msys_set_gs_base(struct task *t, u64 base);        /* XNU thread-local storage */
u64  msys_clock_ns(void);
long msys_nanosleep(struct task *t, u64 ns);
/* Add rights (MSYS_PROT_*) to pages that are already mapped; never removes any. */
#define MSYS_PROT_W 0x2
#define MSYS_PROT_X 0x4
long msys_protect(struct task *t, u64 addr, u64 len, u32 prot);

/*
 * Processes and threads. clone flags use the Linux values (MSYS_CLONE_*);
 * wait statuses use the traditional UNIX encoding shared by Linux and BSD.
 */
#define MSYS_CLONE_VM             0x00000100
#define MSYS_CLONE_VFORK          0x00004000
#define MSYS_CLONE_THREAD         0x00010000
#define MSYS_CLONE_SETTLS         0x00080000
#define MSYS_CLONE_PARENT_SETTID  0x00100000
#define MSYS_CLONE_CHILD_CLEARTID 0x00200000
#define MSYS_CLONE_CHILD_SETTID   0x01000000
#define MSYS_WNOHANG              1
long msys_clone(struct task *t, u64 flags, u64 newsp, u64 ptid, u64 ctid, u64 tls);
long msys_execve(struct task *t, u64 upath, u64 uargv, u64 uenvp);
long msys_wait(struct task *t, int pid, u64 ustatus, int options);
long msys_getppid(struct task *t);
long msys_gettid(struct task *t);
long msys_setpgid(struct task *t, int pid, int pgid);
long msys_getpgid(struct task *t, int pid);
long msys_thread_exit(struct task *t, int status);      /* only the calling thread */
long msys_set_tid_address(struct task *t, u64 uaddr);
long msys_futex(struct task *t, u64 uaddr, int op, u32 val, u64 utimeout);
/* NT threads: start `entry(arg)` on a fresh stack; returns the thread id. */
long msys_thread_spawn(struct task *t, u64 entry, u64 arg, u64 stack_size, u64 exit_thunk);
long msys_thread_join(struct task *t, int tid, u64 timeout_ns);   /* 0 or -MERR_TIMEDOUT */

/* Signals: Linux numbers and struct layouts (struct sigaction, rt_sigframe). */
long msys_kill(struct task *t, int pid, int sig);
long msys_sigaction(struct task *t, int sig, u64 uact, u64 uold);
long msys_sigprocmask(struct task *t, int how, u64 uset, u64 uold);
long msys_sigreturn(struct task *t);

/* identity.c: the operating system each personality reports to its programs. */
struct abi_identity {
	const char *sysname, *nodename, *release, *version, *machine;
	u32 major, minor, build;
};
const struct abi_identity *abi_identity(enum abi_personality p);

#endif /* MERGEDOS_ABI_H */
