/* SPDX-License-Identifier: BSD-2-Clause */
#include <mergedos/kernel.h>
#include <mergedos/initrd.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>

enum thread_state { T_FREE = 0, T_READY, T_RUNNING, T_SLEEPING, T_DEAD };

struct thread {
	u64 *rsp;                       /* saved stack pointer while not running */
	enum thread_state state;
	const char *name;
	void (*fn)(void *);
	void *arg;
	u64 wake_tick, run_ticks;
	int id;
};

#define IDLE_ID 1

extern void switch_context(u64 **save_rsp, u64 *load_rsp);

static struct thread threads[SCHED_MAX_THREADS];
/* Thread 0 keeps the boot stack; the others get one of these. */
static u8 stacks[SCHED_MAX_THREADS - 1][THREAD_STACK_SIZE] __attribute__((aligned(16)));
static struct thread *cur;
static volatile u64 ticks;

u64 time_ticks(void)
{
	return ticks;
}

/* Pick the next runnable thread and switch to it. Interrupts must be off. */
static void schedule(void)
{
	struct thread *prev = cur, *next = NULL;

	for (int i = 1; i <= SCHED_MAX_THREADS && !next; i++) {
		struct thread *t = &threads[(prev->id + i) % SCHED_MAX_THREADS];

		if (t->state == T_SLEEPING && ticks >= t->wake_tick)
			t->state = T_READY;
		if (t->state == T_READY && t->id != IDLE_ID)
			next = t;
	}
	if (!next) {
		if (prev->state == T_RUNNING)
			return;                 /* nobody else wants the CPU */
		next = &threads[IDLE_ID];
	}
	if (prev->state == T_RUNNING)
		prev->state = T_READY;
	next->state = T_RUNNING;
	cur = next;
	switch_context(&prev->rsp, next->rsp);
}

void sched_tick(void)
{
	ticks++;
	if (!cur)
		return;
	cur->run_ticks++;
	schedule();
}

static void thread_entry(void)
{
	irq_enable();                           /* a new thread starts inside schedule() */
	cur->fn(cur->arg);
	thread_exit();
}

static void idle(void *arg)
{
	(void)arg;
	for (;;)
		cpu_halt();                     /* sleep until the next interrupt */
}

int thread_create(void (*fn)(void *), void *arg, const char *name)
{
	u64 flags = irq_save();
	int id = -1;

	for (int i = 1; i < SCHED_MAX_THREADS && id < 0; i++)
		if (threads[i].state == T_FREE || (threads[i].state == T_DEAD && &threads[i] != cur))
			id = i;
	if (id > 0) {
		struct thread *t = &threads[id];
		u64 *sp = (u64 *)(stacks[id - 1] + THREAD_STACK_SIZE);

		/* What switch_context() pops: six callee-saved registers, then a return
		 * address. The spare slot keeps the stack aligned as after a CALL. */
		*--sp = 0;
		*--sp = (u64)thread_entry;
		for (int r = 0; r < 6; r++)
			*--sp = 0;
		*t = (struct thread){ .rsp = sp, .state = T_READY, .name = name, .fn = fn,
				      .arg = arg, .id = id };
	}
	irq_restore(flags);
	return id;
}

void sched_init(void)
{
	threads[0] = (struct thread){ .state = T_RUNNING, .name = "main", .id = 0 };
	cur = &threads[0];
	thread_create(idle, NULL, "idle");
}

bool thread_alive(int id)
{
	return id >= 0 && id < SCHED_MAX_THREADS && threads[id].state != T_FREE &&
	       threads[id].state != T_DEAD;
}

void thread_yield(void)
{
	u64 flags = irq_save();

	schedule();
	irq_restore(flags);
}

void thread_sleep_ms(u32 ms)
{
	u64 flags;

	if (!cur)                               /* before sched_init(): nothing to wait for */
		return;
	flags = irq_save();

	cur->wake_tick = ticks + (u64)ms * SCHED_HZ / 1000 + 1;
	cur->state = T_SLEEPING;
	schedule();
	irq_restore(flags);
}

void thread_exit(void)
{
	(void)irq_save();
	cur->state = T_DEAD;
	schedule();
	for (;;)
		cpu_halt();
}

void sched_print(void)
{
	static const char *const states[] = { "free", "ready", "running", "sleeping", "dead" };

	kprintf("  uptime %llu.%02llu s, timer %u Hz\n", (unsigned long long)(ticks / SCHED_HZ),
		(unsigned long long)(ticks % SCHED_HZ), SCHED_HZ);
	for (int i = 0; i < SCHED_MAX_THREADS; i++)
		if (threads[i].state != T_FREE)
			kprintf("  thread %d  %-8s %-8s %llu ticks\n", i, threads[i].name,
				states[threads[i].state], (unsigned long long)threads[i].run_ticks);
}

/* ---- self-test ---------------------------------------------------------------- */

static volatile u64 work[2];

/* Never yields, never sleeps: only preemption can take the CPU away from it. */
static void spinner(void *arg)
{
	volatile u64 *counter = arg;
	u64 end = ticks + 20;

	while (ticks < end)
		(*counter)++;
}

int sched_selftest(void)
{
	struct exec_report rep = { 0 };
	const u8 *img;
	size_t len;
	u64 t0 = ticks, dt;
	int a, b, bad = 0;

	/* 1. Tick rate against an independent clock (PIT channel 2, polled). */
	time_delay_ms(200);
	dt = ticks - t0;
	bad += dt < 17 || dt > 23;

	/* 2. Two CPU hogs plus this thread: all three must make progress. */
	work[0] = work[1] = 0;
	a = thread_create(spinner, (void *)&work[0], "spin-a");
	b = thread_create(spinner, (void *)&work[1], "spin-b");
	thread_sleep_ms(600);
	bad += a < 0 || b < 0 || thread_alive(a) || thread_alive(b);
	bad += !work[0] || !work[1] || work[0] > work[1] * 4 || work[1] > work[0] * 4;

	/* 3. A user program that never returns to the kernel is stopped by the timer. */
	task_set_tick_limit(30);
	if (initrd_find("bin/spin.linux", &img, &len) || task_exec("bin/spin.linux", img, len, &rep) ||
	    rep.exit_code != TASK_EXIT_TIMEOUT)
		bad++;
	task_set_tick_limit(TASK_DEFAULT_TICK_LIMIT);

	kprintf("[sched] timer %llu ticks in 200 ms; preempted hogs ran %llu / %llu loops; "
		"runaway task exit %d: %s\n", (unsigned long long)dt, (unsigned long long)work[0],
		(unsigned long long)work[1], rep.exit_code, bad ? "FAILED" : "ok");
	return bad;
}
