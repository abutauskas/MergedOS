/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_TASK_H
#define MERGEDOS_TASK_H

#include <mergedos/types.h>
#include <mergedos/cpu.h>
#include "abi_dispatch/abi.h"
#include "abi_dispatch/binfmt.h"

#define TASK_NAME_MAX    48
#define TASK_CAPTURE_MAX 192
#define TASK_FD_MAX      16
#define TASK_MAX_PAGES   4096          /* 16 MiB per task for now */
#define TASK_MMAP_BASE   0x0000100000000000ULL
#define USER_STACK_TOP   0x00007FFFFFFFF000ULL
#define USER_STACK_PAGES 16

/*
 * PoC task model: one foreground task at a time, run to completion. The
 * structure already carries what the scheduler milestone needs (address
 * space, personality, accounting); it just is not queued anywhere yet.
 */
struct task {
	int pid;
	char name[TASK_NAME_MAX];
	u64 pml4;                       /* physical address of the top-level table */
	struct abi_ctx abi;             /* personality + syscall statistics        */

	bool exited;
	int exit_code;
	bool faulted;
	u32 fault_vector;
	u64 fault_rip, fault_addr;

	u32 pages;                      /* user pages mapped at exec time          */
	u32 cpu_ticks;                  /* timer ticks spent in user mode          */

	/* Descriptors 3.. are read-only ramdisk files; 0..2 are the console. */
	struct task_file {
		const u8 *data;
		u64 size, pos;
		bool used;
	} file[TASK_FD_MAX];
	u64 brk_start, brk;             /* heap grown by brk()                     */
	u64 mmap_next;                  /* next address for anonymous mmap()       */
	u64 gs_base;                    /* NT: the TEB, read through gs:[0x30]      */
	char out[TASK_CAPTURE_MAX];     /* head of stdout, kept for self-tests     */
	u32 out_len;
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

struct task *task_current(void);

/* Map [va, va + len) into t's address space (VMM_* rights), counting pages. */
int task_map_range(struct task *t, u64 va, u64 len, u32 vmm_prot);

/*
 * kernel/ntload.c: the NT loader for PE images that import DLLs.
 * ntload_place() picks the load address (rebasing the plan if needed) and
 * returns the relocation delta; after the segments are mapped, ntload_link()
 * applies base relocations, maps the built-in Win32 runtime, binds the
 * import table and points `start` at the runtime's process start-up.
 */
int ntload_place(struct exec_plan *plan, u64 *delta);
int ntload_link(struct task *t, const u8 *image, size_t len, const struct exec_plan *plan,
		u64 delta, struct user_start *start);
void task_user_fault(struct trap_frame *tf, u64 fault_addr) MOS_NORETURN;

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

void syscall_handler(struct syscall_frame *f);

#endif /* MERGEDOS_TASK_H */
