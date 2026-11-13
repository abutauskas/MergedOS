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

/* Files (read-only ramdisk for now), memory and time. Paths are user pointers. */
long msys_read(struct task *t, int fd, u64 ubuf, u64 len);
long msys_open(struct task *t, u64 upath, bool for_writing);
long msys_close(struct task *t, int fd);
long msys_lseek(struct task *t, int fd, s64 offset, int whence);
long msys_fstat(struct task *t, int fd, u64 *size, bool *is_tty);
long msys_stat(struct task *t, u64 upath, u64 *size, bool *is_dir);
long msys_brk(struct task *t, u64 addr);                /* returns the (new) break */
long msys_mmap_anon(struct task *t, u64 len);           /* returns the address     */
long msys_set_fs_base(struct task *t, u64 base);
u64  msys_clock_ns(void);
/* Add rights (MSYS_PROT_*) to pages that are already mapped; never removes any. */
#define MSYS_PROT_W 0x2
#define MSYS_PROT_X 0x4
long msys_protect(struct task *t, u64 addr, u64 len, u32 prot);

/* identity.c: the operating system each personality reports to its programs. */
struct abi_identity {
	const char *sysname, *nodename, *release, *version, *machine;
	u32 major, minor, build;
};
const struct abi_identity *abi_identity(enum abi_personality p);

#endif /* MERGEDOS_ABI_H */
