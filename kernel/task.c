/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * exec: turn an in-memory executable of any supported format into a running
 * ring-3 task with the matching ABI personality.
 *
 * The format-specific work happens in abi_dispatch/binfmt.c, which reduces
 * ELF, PE and Mach-O images to one neutral segment list. From here on the
 * only per-ABI difference is the initial register/stack contract.
 */
#include <mergedos/kernel.h>
#include <mergedos/compat.h>
#include <mergedos/cpu.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>

#define NT_THUNK_VA       0x000000007FFE1000ULL

extern const u8 nt_exit_thunk[], nt_exit_thunk_end[];

static struct task the_task;
static struct task *current;
static int next_pid = 1;

static struct exec_report history[TASK_HISTORY_MAX];
static u32 history_count;

struct task *task_current(void)
{
	return current;
}

static u32 to_vmm_prot(u32 prot)
{
	return ((prot & EXEC_PROT_R) ? VMM_R : 0) | ((prot & EXEC_PROT_W) ? VMM_W : 0) |
	       ((prot & EXEC_PROT_X) ? VMM_X : 0);
}

/* Copy bytes into a user address space through the kernel's direct map. */
static int copy_to_as(u64 pml4, u64 va, const void *src, u64 len)
{
	const u8 *s = src;

	while (len) {
		u64 phys = vmm_user_to_phys(pml4, va);
		u64 chunk = MOS_MIN(len, PAGE_SIZE - (va & (PAGE_SIZE - 1)));

		if (!phys)
			return -MERR_FAULT;
		memcpy(phys_to_virt(phys), s, chunk);
		va += chunk;
		s += chunk;
		len -= chunk;
	}
	return 0;
}

int task_map_range(struct task *t, u64 va, u64 len, u32 prot)
{
	u64 end = va + len;

	if (end < va || va < USER_MIN || end > USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)
		return -MERR_NOEXEC;
	for (u64 page = ALIGN_DOWN(va, PAGE_SIZE); page < end; page += PAGE_SIZE) {
		if (t->pages >= TASK_MAX_PAGES)
			return -MERR_NOMEM;
		if (!vmm_user_to_phys(t->pml4, page))
			t->pages++;
		if (!vmm_map_user(t->pml4, page, prot))
			return -MERR_NOMEM;
	}
	return 0;
}

static int load_segments(struct task *t, const u8 *image, const struct exec_plan *plan)
{
	for (u32 i = 0; i < plan->nsegs; i++) {
		const struct exec_seg *s = &plan->seg[i];
		int rc;

		if (s->memsz == 0)
			continue;
		rc = task_map_range(t, s->vaddr, s->memsz, to_vmm_prot(s->prot));
		if (!rc)
			rc = copy_to_as(t->pml4, s->vaddr, image + s->fileoff, s->filesz);
		if (rc)
			return rc;
		/* Frames come zeroed from the allocator, so .bss needs no extra work. */
	}
	return 0;
}

/*
 * Build the initial stack and registers.
 *
 *   Linux / XNU : rsp -> argc, argv[], NULL, envp[], NULL, auxv / apple[]
 *   FreeBSD     : same vector, but its address is passed in rdi and rsp is
 *                 aligned as if _start had been CALLed
 *   NT          : the entry point is CALLed by a thread-start routine, so
 *                 rsp holds a return address (our exit thunk) and 32 bytes
 *                 of home space; rcx would carry the PEB pointer
 */
static int build_stack(struct task *t, const struct exec_plan *plan, struct user_start *start)
{
	const u64 base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
	u64 sp;

	for (u64 page = base; page < USER_STACK_TOP; page += PAGE_SIZE)
		if (!vmm_map_user(t->pml4, page, VMM_R | VMM_W))
			return -MERR_NOMEM;
	t->pages += USER_STACK_PAGES;

	memset(start, 0, sizeof(*start));
	start->rip = plan->entry;

	if (plan->info.personality == ABI_NT) {
		u64 ret = NT_THUNK_VA;
		int rc;

		if (!vmm_map_user(t->pml4, NT_THUNK_VA, VMM_R | VMM_X))
			return -MERR_NOMEM;
		t->pages++;
		rc = copy_to_as(t->pml4, NT_THUNK_VA, nt_exit_thunk,
				(u64)(nt_exit_thunk_end - nt_exit_thunk));
		sp = USER_STACK_TOP - 0x100 - 8;        /* rsp % 16 == 8, as after a CALL */
		if (!rc)
			rc = copy_to_as(t->pml4, sp, &ret, sizeof(ret));
		start->rsp = sp;
		start->rcx = 0;                         /* PEB pointer: not provided yet */
		return rc;
	} else {
		u8 strs[80] = { 0 };            /* argv[0] (64 bytes) + AT_RANDOM bytes (16) */
		u64 vec[20] = { 0 };
		const u64 str_va = USER_STACK_TOP - sizeof(strs);
		u64 seed = ((u64)t->pid << 32) ^ time_ticks();
		int rc;

		mos_strlcpy((char *)strs, t->name, 64);
		/* Seed for the C library's stack protector. Cycle counter mixed with a
		 * xorshift: unpredictable enough for a canary, not a key source. */
		{
			u32 lo, hi;

			__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
			seed ^= ((u64)hi << 32) | lo;
		}
		for (int i = 64; i < 80; i++) {
			seed ^= seed << 13;
			seed ^= seed >> 7;
			seed ^= seed << 17;
			strs[i] = (u8)seed;
		}
		sp = str_va - sizeof(vec);              /* 16-byte aligned */
		vec[0] = 1;                             /* argc; then argv[0], NULL, envp NULL */
		vec[1] = str_va;
		if (plan->info.personality == ABI_LINUX) {
			/* ELF auxiliary vector: what a static C library needs to start. */
			const u64 aux[][2] = {
				{ 3, plan->phdr_vaddr }, { 4, plan->phentsize }, { 5, plan->phnum },
				{ 6, PAGE_SIZE }, { 9, plan->entry }, { 23, 0 }, { 25, str_va + 64 },
			};

			memcpy(&vec[4], aux, sizeof(aux));
		}
		rc = copy_to_as(t->pml4, str_va, strs, sizeof(strs));
		if (!rc)
			rc = copy_to_as(t->pml4, sp, vec, sizeof(vec));
		if (plan->info.personality == ABI_FREEBSD) {
			start->rdi = sp;
			start->rsp = ((sp - 8) & ~0xFULL) + 8;
		} else {
			start->rsp = sp;
		}
		return rc;
	}
}

static void record(const struct task *t, const struct exec_plan *plan, struct exec_report *rep)
{
	rep->pid = t->pid;
	mos_strlcpy(rep->name, t->name, sizeof(rep->name));
	rep->format = plan->info.format;
	rep->personality = plan->info.personality;
	rep->exit_code = t->exit_code;
	rep->faulted = t->faulted;
	rep->fault_vector = t->fault_vector;
	rep->pages = t->pages;
	rep->syscalls = t->abi.stats.calls;
	rep->unknown_syscalls = t->abi.stats.unknown;
	memcpy(rep->out, t->out, sizeof(rep->out));
	rep->out_len = t->out_len;

	history[history_count % TASK_HISTORY_MAX] = *rep;
	history_count++;
}

int task_exec(const char *name, const void *image, size_t len, struct exec_report *rep)
{
	struct task *t = &the_task;
	struct exec_plan plan;
	struct user_start start;
	bool nt_dynamic;
	u64 delta = 0;
	int rc;

	memset(rep, 0, sizeof(*rep));
	rc = binfmt_load_plan(image, len, &plan);
	nt_dynamic = plan.info.personality == ABI_NT && (plan.info.flags & BINFMT_F_DYNAMIC);
	rep->format = plan.info.format;
	rep->personality = plan.info.personality;
	if (rc)
		return rc;
	if (current)
		return -MERR_AGAIN;             /* no nested exec in the PoC */

	memset(t, 0, sizeof(*t));
	t->pid = next_pid++;
	mos_strlcpy(t->name, name, sizeof(t->name));
	abi_ctx_init(&t->abi, plan.info.personality, t);

	if (nt_dynamic) {
		rc = ntload_place(&plan, &delta);
		if (rc)
			return rc;
	}
	t->pml4 = vmm_as_create();
	if (!t->pml4)
		return -MERR_NOMEM;
	rc = load_segments(t, image, &plan);
	for (u32 i = 0; i < plan.nsegs; i++)            /* the heap starts after the image */
		t->brk_start = MOS_MAX(t->brk_start,
				       ALIGN_UP(plan.seg[i].vaddr + plan.seg[i].memsz, PAGE_SIZE));
	t->brk = t->brk_start;
	t->mmap_next = TASK_MMAP_BASE;
	if (!rc)
		rc = build_stack(t, &plan, &start);
	if (!rc && nt_dynamic)
		rc = ntload_link(t, image, len, &plan, delta, &start);
	if (rc) {
		vmm_as_destroy(t->pml4);
		return rc;
	}

	current = t;
	vmm_switch(t->pml4);
	wrmsr(0xC0000100, 0);                   /* FS base: no thread pointer yet */
	wrmsr(0xC0000101, t->gs_base);          /* GS base: the NT TEB (the kernel never uses gs) */
	user_enter(&start);                     /* returns via user_exit_resume() */
	irq_enable();                           /* we come back from a syscall or trap: IF is off */
	wrmsr(0xC0000101, 0);
	vmm_switch(vmm_kernel_pml4());
	current = NULL;

	vmm_as_destroy(t->pml4);
	record(t, &plan, rep);
	return 0;
}

void task_user_fault(struct trap_frame *tf, u64 fault_addr)
{
	struct task *t = current;

	if (!t)
		panic("ring-3 trap %u with no current task", (unsigned)tf->vector);
	t->faulted = true;
	t->fault_vector = (u32)tf->vector;
	t->fault_rip = tf->rip;
	t->fault_addr = fault_addr;
	t->exit_code = 128 + (int)tf->vector;
	kprintf("[task] pid %d (%s) killed: %s at rip=%llx addr=%llx\n", t->pid, t->name,
		trap_name((unsigned)tf->vector), (unsigned long long)tf->rip,
		(unsigned long long)fault_addr);
	user_exit_resume(t->exit_code);
}

static u32 tick_limit = TASK_DEFAULT_TICK_LIMIT;

void task_set_tick_limit(u32 limit)
{
	tick_limit = limit;
}

void task_user_tick(void)
{
	struct task *t = current;

	if (!t || !tick_limit || ++t->cpu_ticks <= tick_limit)
		return;
	t->exit_code = TASK_EXIT_TIMEOUT;
	kprintf("[task] pid %d (%s) stopped: CPU budget of %u ticks used up\n", t->pid, t->name,
		tick_limit);
	user_exit_resume(t->exit_code);
}

u32 task_history(struct exec_report *out, u32 max)
{
	u32 n = history_count < TASK_HISTORY_MAX ? history_count : TASK_HISTORY_MAX;
	u32 first = history_count - n;

	if (n > max)
		n = max;
	for (u32 i = 0; i < n; i++)
		out[i] = history[(first + i) % TASK_HISTORY_MAX];
	return n;
}
