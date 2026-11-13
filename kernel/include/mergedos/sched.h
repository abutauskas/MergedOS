/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_SCHED_H
#define MERGEDOS_SCHED_H

/*
 * Kernel threads, preempted by the timer.
 *
 * One CPU, round-robin, one tick (10 ms) per turn. Every thread has its own
 * kernel stack; a switch saves the callee-saved registers on the old stack
 * and resumes the new one. Preemption happens on the way out of the timer
 * interrupt, so the interrupted context is restored by an ordinary IRET
 * when its thread runs again.
 *
 * Code that must not be interleaved (console output, the frame allocator)
 * brackets itself with irq_save()/irq_restore(); on one CPU that is a lock.
 */
#include <mergedos/types.h>

#define SCHED_HZ          100
#define SCHED_MAX_THREADS 6
#define THREAD_STACK_SIZE 8192

void sched_init(void);                  /* the caller becomes thread 0 ("main") */
int  thread_create(void (*fn)(void *), void *arg, const char *name);   /* id, or -1 */
bool thread_alive(int id);
void thread_yield(void);
void thread_sleep_ms(u32 ms);
void thread_exit(void) MOS_NORETURN;

u64  time_ticks(void);                  /* timer interrupts since boot */
void sched_tick(void);                  /* from the timer interrupt, after EOI */
int  sched_selftest(void);              /* tick rate, preemption, runaway-task kill */
void sched_print(void);

#endif /* MERGEDOS_SCHED_H */
