/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_SCHED_H
#define MERGEDOS_SCHED_H

/*
 * Threads, preempted by the timer: kernel threads and user threads alike.
 *
 * One CPU, round-robin, one tick (10 ms) per turn. Every thread has its own
 * 16 KiB kernel stack; a switch saves the callee-saved registers on the old
 * stack and resumes the new one. A user thread also carries its process's
 * address space, FS/GS bases and FPU/SSE state, which the switch loads; its
 * kernel stack is where SYSCALL and ring-3 interrupts land.
 *
 * Code that must not be interleaved (console output, the frame allocator)
 * brackets itself with irq_save()/irq_restore(); on one CPU that is a lock.
 * System calls run with interrupts off, so they are atomic unless they
 * block (thread_block), which is the only way they give up the CPU.
 */
#include <mergedos/types.h>
#include <mergedos/cpu.h>

#define SCHED_HZ          100
#define SCHED_MAX_THREADS 64
#define KSTACK_SIZE       16384

struct task;
struct syscall_frame;

enum thread_state { T_FREE = 0, T_READY, T_RUNNING, T_BLOCKED, T_DEAD };

struct thread {
	u64 *rsp;                       /* saved stack pointer while not running   */
	enum thread_state state;
	const char *name;
	void (*fn)(void *);
	void *arg;
	u64 wake_tick, run_ticks;       /* wake_tick: 0 = no timeout               */
	const void *chan;               /* what a blocked thread waits for         */
	u64 chan_aux;                   /* second half of the wait key             */
	int id;                         /* slot number                             */
	u8 *stack;                      /* KSTACK_SIZE bytes; NULL for thread 0    */

	/* User threads only. */
	struct task *proc;
	int tid;
	u64 fs_base, gs_base;
	u64 clear_tid;                  /* CLONE_CHILD_CLEARTID / set_tid_address  */
	u64 sigmask;
	struct syscall_frame *uframe;   /* registers of the system call in progress */
	bool iret_pending;              /* leave the system call through iret_frame */
	bool woken;                     /* thread_wake() rather than a timeout     */
};

/* Bottom of every kernel stack: user FPU state, then a spare register frame. */
#define KSTACK_FX(t)    ((t)->stack)                            /* 512 bytes, 16-aligned */
#define KSTACK_IRET(t)  ((struct trap_frame *)((t)->stack + 512))
#define KSTACK_TOP(t)   ((t)->stack + KSTACK_SIZE)

void sched_init(void);                  /* the caller becomes thread 0 ("main") */
int  thread_create(void (*fn)(void *), void *arg, const char *name);   /* id, or -1 */
/* A user thread of `proc` that starts by IRETing to `regs` with FPU state `fx`. */
struct thread *thread_create_user(struct task *proc, const struct trap_frame *regs,
				  const u8 *fx);
bool thread_alive(int id);
void thread_yield(void);
void thread_sleep_ms(u32 ms);
void thread_exit(void) MOS_NORETURN;
struct thread *thread_current(void);
struct thread *thread_slot(int id);     /* NULL if out of range */

/*
 * Sleep until thread_wake(chan, aux) or `timeout` ticks (0 = forever).
 * Returns true when woken, false on timeout. Interrupts are saved/restored.
 */
bool thread_block(const void *chan, u64 aux, u64 timeout);
int  thread_wake(const void *chan, u64 aux, int max);  /* returns how many woke */
void thread_wake_proc(struct task *proc);              /* all blocked threads of proc */
void thread_kill(struct thread *t);     /* not the current thread: it never runs again */

/* User FPU state of the current thread, as the CPU holds it right now. */
void fpu_save(u8 *fx512);
void fpu_restore(const u8 *fx512);
void fpu_init_state(u8 *fx512);

u64  time_ticks(void);                  /* timer interrupts since boot */
void sched_tick(void);                  /* from the timer interrupt, after EOI */
int  sched_selftest(void);              /* tick rate, preemption, runaway-task kill */
void sched_print(void);

#endif /* MERGEDOS_SCHED_H */
