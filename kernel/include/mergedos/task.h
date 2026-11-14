/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_TASK_H
#define MERGEDOS_TASK_H

#include <mergedos/types.h>
#include <mergedos/cpu.h>
#include "abi_dispatch/abi.h"
#include "abi_dispatch/binfmt.h"

#define TASK_NAME_MAX    48
#define TASK_CAPTURE_MAX 256
#define TASK_FD_MAX      1024
#define TASK_CWD_MAX     192
#define TASK_VM_LIMIT    (1ULL << 47)  /* address space a process may reserve (lazily backed) */
#define TASK_PAGE_LIMIT  (1u << 18)    /* pages a process may actually hold: 1 GiB */
#define TASK_MMAP_BASE   0x0000100000000000ULL
#define USER_STACK_TOP   0x00007FFFFFFFF000ULL
#define USER_STACK_PAGES 16             /* mapped at exec; the rest on demand */
#define USER_STACK_MAX   (8ULL << 20)
#define TASK_NSIG        64

struct file;
struct thread;

/*
 * A reserved range of the address space, backed on first touch. Private
 * ranges get a zeroed page, or for an immutable file image a private copy of
 * its bytes (`src`, `srclen`, zeros after that). Shared ranges (VMA_SHARED
 * with `file`) map the frames of the ramfs file at `foff` itself. The array
 * is sorted by address and never overlaps.
 */
#define VMA_SHARED 0x1
#define VMA_HEAP   0x2
struct vma {
	u64 start, end;
	u32 prot;                       /* VMM_*, 0 = PROT_NONE */
	u32 flags;
	const u8 *src;                  /* immutable file contents, or NULL */
	u64 srclen;
	struct file *file;              /* shared file mapping (holds a reference) */
	u64 foff;
};

/* Linux kernel layout of struct sigaction (x86-64). */
struct sigact {
	u64 handler, flags, restorer, mask;
};

/*
 * A process. Its threads (struct thread, sched.h) share everything here:
 * address space, descriptors, signal actions. A process without threads is
 * a zombie until its parent reaps it.
 */
struct task {
	int pid, ppid, pgid;
	char name[TASK_NAME_MAX];
	u64 pml4;                       /* physical address of the top-level table */
	bool mm_borrowed;               /* vfork child running in its parent's space */
	struct abi_ctx abi;             /* personality + syscall statistics        */
	enum bin_format format;

	bool exited;                    /* exit_group requested / process finished */
	bool zombie;                    /* no threads left, waiting to be reaped   */
	bool autoreap;                  /* nobody will wait: free it at once       */
	bool kernel_wait;               /* task_exec() is waiting for it           */
	bool vfork_done;
	int exit_code;
	int termsig;                    /* killed by this signal (wait status)     */
	bool faulted;
	u32 fault_vector;
	u64 fault_rip, fault_addr;
	int nthreads;

	u32 pages;                      /* user pages mapped                       */
	u64 vm_reserved;                /* bytes of VMAs                           */
	u32 cpu_ticks;                  /* timer ticks spent in user mode          */

	struct file *fd[TASK_FD_MAX];
	u32 cloexec[TASK_FD_MAX / 32];  /* bit per descriptor                      */
	char cwd[TASK_CWD_MAX];         /* canonical, no leading '/'               */
	u32 umask;
	u64 brk_start, brk;             /* heap grown by brk()                     */
	u64 gs_base;                    /* NT: the TEB, read through gs:[0x30]      */
	char exe[128];                  /* path of the program (/proc/self/exe)     */
	struct vma *vma;                /* sorted, see umm.c */
	u32 nvma, vma_cap;

	struct sigact sig[TASK_NSIG + 1];
	u64 sigpending;                 /* bit (sig - 1)                           */

	char out[TASK_CAPTURE_MAX];     /* head of stdout, kept for self-tests     */
	u32 out_len;
	struct task *next;              /* all processes                           */
};

/* Result of one run, kept for `ps` / `tasklist` after the task is gone. */
struct exec_report {
	int pid;
	char name[TASK_NAME_MAX];
	enum bin_format format;
	enum abi_personality personality;
	int exit_code;
	bool faulted;
	u32 fault_vector;
	u32 pages;
	u64 syscalls, unknown_syscalls;
	char out[TASK_CAPTURE_MAX];
	u32 out_len;
};

/*
 * Load `image` (any supported executable format), run it in ring 3 until it
 * exits or faults, then tear its address space down.
 * Returns 0 if the image ran (see rep->exit_code), or -MERR_* if it could
 * not be loaded.
 */
int task_exec(const char *name, const void *image, size_t len, struct exec_report *rep);
int task_exec_args(const char *name, const void *image, size_t len, const char *const *argv,
		   struct exec_report *rep);
/* Same, for a program in the VFS with its own argv and environment (NULL-terminated). */
int task_exec_argv(const char *path, const char *const argv[], const char *const envp[],
		   struct exec_report *rep);

struct task *task_current(void);
struct task *task_new(const char *name);        /* on the process list, no memory yet */
void task_free(struct task *t);                 /* descriptors, memory, the struct */
int  task_new_pid(void);                        /* pids and thread ids share one space */
void task_mark_cleanup(void);                   /* zombies to release at the next switch */
int  task_copy_to_as(u64 pml4, u64 va, const void *src, u64 len);

/* Strings for a new program's stack: argc argv strings, then envc env strings. */
#define EXEC_ARGS_MAX 64
#define EXEC_ARGS_BUF 3072
struct exec_args {
	u32 argc, envc, len;
	char path[128];                 /* the executable's path (AT_EXECFN)       */
	char buf[EXEC_ARGS_BUF];
};
void exec_args_init(struct exec_args *a, const char *argv0);
int  task_load_image(struct task *t, const u8 *image, size_t len, const struct exec_args *args,
		     struct user_start *start);
void trap_frame_from_start(struct trap_frame *tf, const struct user_start *s);
u64  task_pml4(const struct task *t);
const char *task_name(const struct task *t);
void task_deferred_cleanup(void);       /* sched.c, after every switch */
struct task *task_find(int pid);
struct task *task_first(void);          /* walk with ->next */

/* Process lifecycle (kernel/proc.c). */
void task_exit_thread(int status) MOS_NORETURN;                 /* the calling thread */
void task_exit_group(struct task *t, int code, int sig) MOS_NORETURN;
void task_signal(struct task *t, int sig);
bool task_signal_pending(void);         /* an unblocked signal waits for the caller */
/* Deliver pending signals into a ring-3 frame; may not return (default action). */
void task_signal_deliver(struct trap_frame *tf);
bool task_fault_signal(struct trap_frame *tf, int sig, int code, u64 addr);   /* Linux CPU faults */
long task_sigaltstack(struct task *t, u64 uss, u64 uold);
long task_tgkill(struct task *t, int tgid, int tid, int sig);
long task_sigtimedwait(struct task *t, u64 uset, u64 uinfo, u64 utimeout);
long task_sigpending(struct task *t, u64 uset);
long task_sigsuspend(struct task *t, u64 umask);
int  task_fault_in(struct task *t, u64 va, bool write);         /* demand paging */
void task_keyboard_pump(struct task *fg);                       /* stdin + Ctrl-C */

/* kernel/umm.c: the process's virtual memory (VMAs, mmap family, faults, fork). */
struct vma *umm_find(struct task *t, u64 va);
int  umm_cover(struct task *t, u64 start, u64 end, u32 prot);  /* VMA for the unmapped parts */
int  umm_fork(struct task *child, const struct task *parent);
void umm_release(struct task *t);                              /* drop every VMA */
long umm_protect(struct task *t, u64 addr, u64 len, u32 vmm_prot);
long umm_mremap(struct task *t, u64 old, u64 oldlen, u64 newlen, u32 flags, u64 newaddr);
long umm_madvise(struct task *t, u64 addr, u64 len, int advice);

/* Map [va, va + len) into t's address space (VMM_* rights), counting pages. */
int task_map_range(struct task *t, u64 va, u64 len, u32 vmm_prot);

/*
 * kernel/ntload.c: the NT loader for PE images that import DLLs.
 * ntload_place() picks the load address (rebasing the plan if needed) and
 * returns the relocation delta; after the segments are mapped, ntload_link()
 * applies base relocations, maps the built-in runtime (MergedOS's ntdll,
 * which binds the imports itself) and points `start` at its start-up.
 */
int ntload_place(struct exec_plan *plan, u64 *delta);
int ntload_link(struct task *t, const u8 *image, size_t len, const struct exec_plan *plan,
		u64 delta, const struct exec_args *args, struct user_start *start);
void task_user_fault(struct trap_frame *tf, u64 fault_addr);

/* History ring for the shell. Returns the number of valid records copied. */
#define TASK_HISTORY_MAX 16
u32 task_history(struct exec_report *out, u32 max);

const char *trap_name(unsigned vector);

/*
 * CPU budget. A task that uses more user-mode time than this is ended with
 * TASK_EXIT_TIMEOUT. A stand-in for signals and job control: without them,
 * nothing else could stop a program that loops forever.
 */
#define TASK_DEFAULT_TICK_LIMIT 1000    /* 10 s */
#define TASK_EXIT_TIMEOUT       124
void task_set_tick_limit(u32 ticks);    /* 0 = unlimited */
void task_user_tick(void);              /* timer interrupt arrived in ring 3 */

u64  syscall_handler(struct syscall_frame *f);

#endif /* MERGEDOS_TASK_H */
