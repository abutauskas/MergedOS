/* SPDX-License-Identifier: BSD-2-Clause */
#include <mergedos/kernel.h>
#include <mergedos/initrd.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>

#define IDLE_ID 1

extern void switch_context(u64 **save_rsp, u64 *load_rsp);
extern void user_thread_start(void);
extern u64 syscall_kernel_rsp;
void sched_thread_begin(void);

static struct thread threads[SCHED_MAX_THREADS];
static struct thread *cur;
static volatile u64 ticks;
static bool reap_pending;

u64 time_ticks(void)
{
	return ticks;
}

struct thread *thread_current(void)
{
	return cur;
}

struct thread *thread_slot(int id)
{
	return id >= 0 && id < SCHED_MAX_THREADS ? &threads[id] : NULL;
}

void fpu_save(u8 *fx)
{
	__asm__ volatile("fxsave64 (%0)" : : "r"(fx) : "memory");
}

void fpu_restore(const u8 *fx)
{
	__asm__ volatile("fxrstor64 (%0)" : : "r"(fx) : "memory");
}

void fpu_init_state(u8 *fx)
{
	memset(fx, 0, 512);
	fx[0] = 0x7F;                           /* FCW 0x037F: x87 exceptions masked */
	fx[1] = 0x03;
	fx[24] = 0x80;                          /* MXCSR 0x1F80: SSE exceptions masked */
	fx[25] = 0x1F;
}

/* Load what the next thread runs on: address space, TLS bases, ring-0 stack, FPU. */
static void switch_mm(struct thread *next)
{
	u64 want = next->proc ? task_pml4(next->proc) : vmm_kernel_pml4();

	if ((read_cr3() & ~0xFFFULL) != want)
		vmm_switch(want);
	if (!next->proc)
		return;
	wrmsr(0xC0000100, next->fs_base);
	wrmsr(0xC0000101, next->gs_base);
	syscall_kernel_rsp = (u64)KSTACK_TOP(next);
	cpu_set_rsp0((u64)KSTACK_TOP(next));
	fpu_restore(KSTACK_FX(next));
}

/*
 * Runs on the new thread right after every switch: free what dead threads
 * left behind. A thread can never free the stack it is running on, so this
 * is always somebody else's job.
 */
static void finish_switch(void)
{
	if (!reap_pending)
		return;
	reap_pending = false;
	for (int i = 1; i < SCHED_MAX_THREADS; i++) {
		struct thread *t = &threads[i];

		if (t->state != T_DEAD || t == cur || !t->stack)
			continue;
		pmm_free_contig((u64)t->stack - DMAP_BASE, KSTACK_SIZE / PAGE_SIZE);
		t->stack = NULL;
		t->state = T_FREE;
	}
	task_deferred_cleanup();
}

/* Pick the next runnable thread and switch to it. Interrupts must be off. */
static void schedule(void)
{
	struct thread *prev = cur, *next = NULL;

	for (int i = 1; i <= SCHED_MAX_THREADS && !next; i++) {
		struct thread *t = &threads[(prev->id + i) % SCHED_MAX_THREADS];

		if (t->state == T_BLOCKED && t->wake_tick && ticks >= t->wake_tick)
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
	if (next == prev)
		return;
	if (prev->proc && prev->state != T_DEAD)
		fpu_save(KSTACK_FX(prev));
	cur = next;
	switch_mm(next);
	switch_context(&prev->rsp, next->rsp);
	finish_switch();
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
	finish_switch();
	irq_enable();                           /* a new thread starts inside schedule() */
	cur->fn(cur->arg);
	thread_exit();
}

/* First C code of a new user thread, on its way to IRET into ring 3. */
void sched_thread_begin(void)
{
	finish_switch();
}

static void idle(void *arg)
{
	(void)arg;
	for (;;)
		cpu_halt();                     /* sleep until the next interrupt */
}

static struct thread *alloc_thread(void)
{
	for (int i = 1; i < SCHED_MAX_THREADS; i++) {
		struct thread *t = &threads[i];

		if (t->state == T_FREE) {
			u64 phys = pmm_alloc_contig(KSTACK_SIZE / PAGE_SIZE);

			if (!phys)
				return NULL;
			memset(t, 0, sizeof(*t));
			t->id = i;
			t->stack = phys_to_virt(phys);
			return t;
		}
	}
	return NULL;
}

int thread_create(void (*fn)(void *), void *arg, const char *name)
{
	u64 flags = irq_save();
	struct thread *t = alloc_thread();
	int id = -1;

	if (t) {
		u64 *sp = (u64 *)KSTACK_TOP(t);

		/* What switch_context() pops: six callee-saved registers, then a return
		 * address. The spare slot keeps the stack aligned as after a CALL. */
		*--sp = 0;
		*--sp = (u64)thread_entry;
		for (int r = 0; r < 6; r++)
			*--sp = 0;
		t->rsp = sp;
		t->name = name;
		t->fn = fn;
		t->arg = arg;
		t->state = T_READY;
		id = t->id;
	}
	irq_restore(flags);
	return id;
}

struct thread *thread_create_user(struct task *proc, const struct trap_frame *regs, const u8 *fx)
{
	u64 flags = irq_save();
	struct thread *t = alloc_thread();

	if (t) {
		struct trap_frame *tf = (struct trap_frame *)(KSTACK_TOP(t) - sizeof(*tf));
		u64 *sp = (u64 *)tf;

		*tf = *regs;
		tf->cs = USER_CS;
		tf->ss = USER_DS;
		tf->rflags = (tf->rflags & 0xCD5) | 0x202;   /* user flags only, IF on */
		*--sp = (u64)user_thread_start;
		for (int r = 0; r < 6; r++)
			*--sp = 0;
		t->rsp = sp;
		t->proc = proc;
		t->name = task_name(proc);
		if (fx)
			memcpy(KSTACK_FX(t), fx, 512);
		else
			fpu_init_state(KSTACK_FX(t));
		t->state = T_READY;
	}
	irq_restore(flags);
	return t;
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

bool thread_block(const void *chan, u64 aux, u64 timeout)
{
	u64 flags;
	bool woken;

	if (!cur)                               /* before sched_init(): nothing to wait for */
		return false;
	flags = irq_save();
	cur->chan = chan;
	cur->chan_aux = aux;
	cur->woken = false;
	cur->wake_tick = timeout ? ticks + timeout : 0;
	cur->state = T_BLOCKED;
	schedule();
	woken = cur->woken;
	cur->chan = NULL;
	irq_restore(flags);
	return woken;
}

int thread_wake(const void *chan, u64 aux, int max)
{
	u64 flags = irq_save();
	int n = 0;

	for (int i = 0; i < SCHED_MAX_THREADS && n < max; i++) {
		struct thread *t = &threads[i];

		if (t->state == T_BLOCKED && t->chan == chan && t->chan_aux == aux) {
			t->state = T_READY;
			t->woken = true;
			n++;
		}
	}
	irq_restore(flags);
	return n;
}

void thread_wake_proc(struct task *proc)
{
	u64 flags = irq_save();

	for (int i = 0; i < SCHED_MAX_THREADS; i++)
		if (threads[i].state == T_BLOCKED && threads[i].proc == proc) {
			threads[i].state = T_READY;
			threads[i].woken = true;
		}
	irq_restore(flags);
}

void thread_kill(struct thread *t)
{
	u64 flags = irq_save();

	if (t != cur && t->state != T_FREE && t->state != T_DEAD) {
		t->state = T_DEAD;
		reap_pending = true;
	}
	irq_restore(flags);
}

void thread_sleep_ms(u32 ms)
{
	thread_block(NULL, 0, (u64)ms * SCHED_HZ / 1000 + 1);
}

void thread_exit(void)
{
	(void)irq_save();
	cur->state = T_DEAD;
	reap_pending = true;
	schedule();
	for (;;)
		cpu_halt();
}

void sched_print(void)
{
	static const char *const states[] = { "free", "ready", "running", "blocked", "dead" };

	kprintf("  uptime %llu.%02llu s, timer %u Hz\n", (unsigned long long)(ticks / SCHED_HZ),
		(unsigned long long)(ticks % SCHED_HZ), SCHED_HZ);
	for (int i = 0; i < SCHED_MAX_THREADS; i++)
		if (threads[i].state != T_FREE && threads[i].state != T_DEAD)
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
