/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * exec: turn an in-memory executable of any supported format into a running
 * ring-3 process with the matching ABI personality; the process list; and
 * task_exec(), the kernel's synchronous "run this program" entry point.
 *
 * The format-specific work happens in abi_dispatch/binfmt.c, which reduces
 * ELF, PE and Mach-O images to one neutral segment list. From here on the
 * only per-ABI difference is the initial register/stack contract.
 */
#include <mergedos/kernel.h>
#include <mergedos/compat.h>
#include <mergedos/console.h>
#include <mergedos/cpu.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>

#define NT_THUNK_VA       0x000000007FFE1000ULL

extern const u8 nt_exit_thunk[], nt_exit_thunk_end[];

static struct task *procs;
static int next_pid = 1;
static bool cleanup_pending;

static struct exec_report history[TASK_HISTORY_MAX];
static u32 history_count;

struct task *task_current(void)
{
	struct thread *th = thread_current();

	return th ? th->proc : NULL;
}

u64 task_pml4(const struct task *t)
{
	return t->pml4;
}

const char *task_name(const struct task *t)
{
	return t->name;
}

struct task *task_first(void)
{
	return procs;
}

struct task *task_find(int pid)
{
	for (struct task *t = procs; t; t = t->next)
		if (t->pid == pid)
			return t;
	return NULL;
}

int task_new_pid(void)
{
	return next_pid++;
}

struct task *task_new(const char *name)
{
	struct task *t = kzalloc(sizeof(*t));

	if (!t)
		return NULL;
	t->pid = task_new_pid();
	mos_strlcpy(t->name, name, sizeof(t->name));
	t->next = procs;
	procs = t;
	return t;
}

/* Address space of a process that has no threads left. */
static void release_mm(struct task *t)
{
	if (t->pml4 && !t->mm_borrowed) {
		if ((read_cr3() & ~0xFFFULL) == t->pml4)
			vmm_switch(vmm_kernel_pml4());
		vmm_as_destroy(t->pml4);
	}
	t->pml4 = 0;
}

void task_free(struct task *t)
{
	struct task **pp = &procs;

	while (*pp && *pp != t)
		pp = &(*pp)->next;
	if (*pp)
		*pp = t->next;
	fd_close_all(t);
	release_mm(t);
	kfree(t);
}

void task_mark_cleanup(void)
{
	cleanup_pending = true;
}

/* After every context switch: zombies give back their memory, orphans go away. */
void task_deferred_cleanup(void)
{
	struct task *t = procs, *next;

	if (!cleanup_pending)
		return;
	cleanup_pending = false;
	for (; t; t = next) {
		next = t->next;
		if (!t->zombie)
			continue;
		if (t->autoreap)
			task_free(t);
		else
			release_mm(t);
	}
}

static u32 to_vmm_prot(u32 prot)
{
	return ((prot & EXEC_PROT_R) ? VMM_R : 0) | ((prot & EXEC_PROT_W) ? VMM_W : 0) |
	       ((prot & EXEC_PROT_X) ? VMM_X : 0);
}

/* Copy bytes into a user address space through the kernel's direct map. */
int task_copy_to_as(u64 pml4, u64 va, const void *src, u64 len)
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

	if (end < va || va < USER_MIN || end > USER_STACK_TOP - USER_STACK_MAX)
		return -MERR_NOEXEC;
	for (u64 page = ALIGN_DOWN(va, PAGE_SIZE); page < end; page += PAGE_SIZE) {
		if (t->pages >= TASK_VM_LIMIT / PAGE_SIZE)
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
			rc = task_copy_to_as(t->pml4, s->vaddr, image + s->fileoff, s->filesz);
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
static int build_stack(struct task *t, const struct exec_plan *plan, const struct exec_args *args,
		       struct user_start *start)
{
	const u64 base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
	u64 sp;

	for (u64 page = base; page < USER_STACK_TOP; page += PAGE_SIZE)
		if (!vmm_map_user(t->pml4, page, VMM_R | VMM_W))
			return -MERR_NOMEM;
	t->pages += USER_STACK_PAGES;
	/* The rest of the stack is reserved and filled in on demand. */
	t->vma[t->nvma++] = (struct vma){ USER_STACK_TOP - USER_STACK_MAX, base, VMM_R | VMM_W };

	memset(start, 0, sizeof(*start));
	start->rip = plan->entry;

	if (plan->info.personality == ABI_NT) {
		u64 ret = NT_THUNK_VA;
		int rc;

		if (!vmm_map_user(t->pml4, NT_THUNK_VA, VMM_R | VMM_X))
			return -MERR_NOMEM;
		t->pages++;
		rc = task_copy_to_as(t->pml4, NT_THUNK_VA, nt_exit_thunk,
				     (u64)(nt_exit_thunk_end - nt_exit_thunk));
		sp = USER_STACK_TOP - 0x100 - 8;        /* rsp % 16 == 8, as after a CALL */
		if (!rc)
			rc = task_copy_to_as(t->pml4, sp, &ret, sizeof(ret));
		start->rsp = sp;
		start->rcx = 0;                         /* PEB pointer: not provided yet */
		return rc;
	} else {
		u8 rnd[16];
		u64 vec[2 * EXEC_ARGS_MAX + 24];
		const u64 str_va = ALIGN_DOWN(USER_STACK_TOP - args->len - sizeof(rnd), 16);
		const u64 rnd_va = str_va + args->len;
		u64 seed = ((u64)t->pid << 32) ^ time_ticks(), s = str_va;
		u32 n = 0, words;
		int rc;

		/* Seed for the C library's stack protector. Cycle counter mixed with a
		 * xorshift: unpredictable enough for a canary, not a key source. */
		{
			u32 lo, hi;

			__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
			seed ^= ((u64)hi << 32) | lo;
		}
		for (int i = 0; i < 16; i++) {
			seed ^= seed << 13;
			seed ^= seed >> 7;
			seed ^= seed << 17;
			rnd[i] = (u8)seed;
		}
		vec[n++] = args->argc;
		for (u32 i = 0; i < args->argc + args->envc; i++) {
			vec[n++] = s;
			s += strlen(args->buf + (s - str_va)) + 1;
			if (i + 1 == args->argc)
				vec[n++] = 0;
		}
		vec[n++] = 0;
		if (plan->info.personality == ABI_LINUX) {
			/* ELF auxiliary vector: what a static C library needs to start. */
			const u64 aux[][2] = {
				{ 3, plan->phdr_vaddr }, { 4, plan->phentsize }, { 5, plan->phnum },
				{ 6, PAGE_SIZE }, { 9, plan->entry }, { 23, 0 }, { 25, rnd_va },
			};

			memcpy(&vec[n], aux, sizeof(aux));
			n += 2 * ARRAY_SIZE(aux);
		}
		vec[n++] = 0;                           /* AT_NULL */
		vec[n++] = 0;
		words = n;
		sp = ALIGN_DOWN(str_va - words * 8, 16);
		rc = task_copy_to_as(t->pml4, str_va, args->buf, args->len);
		if (!rc)
			rc = task_copy_to_as(t->pml4, rnd_va, rnd, sizeof(rnd));
		if (!rc)
			rc = task_copy_to_as(t->pml4, sp, vec, words * 8);
		if (plan->info.personality == ABI_FREEBSD) {
			start->rdi = sp;
			start->rsp = ((sp - 8) & ~0xFULL) + 8;
		} else {
			start->rsp = sp;
		}
		return rc;
	}
}

/*
 * Give `t` a fresh address space holding `image`. On failure nothing of the
 * new space is left and t->pml4 is 0; the caller keeps or restores the old one.
 */
int task_load_image(struct task *t, const u8 *image, size_t len, const struct exec_args *args,
		    struct user_start *start)
{
	struct exec_plan plan;
	bool nt_dynamic;
	u64 delta = 0;
	int rc = binfmt_load_plan(image, len, &plan);

	t->format = plan.info.format;
	t->abi.personality = plan.info.personality;
	if (rc)
		return rc;
	nt_dynamic = plan.info.personality == ABI_NT && (plan.info.flags & BINFMT_F_DYNAMIC);
	abi_ctx_init(&t->abi, plan.info.personality, t);

	if (nt_dynamic) {
		rc = ntload_place(&plan, &delta);
		if (rc)
			return rc;
	}
	t->pml4 = vmm_as_create();
	t->mm_borrowed = false;
	t->pages = 0;
	t->nvma = 0;
	t->vm_reserved = 0;
	t->gs_base = 0;
	t->brk_start = 0;
	if (!t->pml4)
		return -MERR_NOMEM;
	rc = load_segments(t, image, &plan);
	for (u32 i = 0; i < plan.nsegs; i++)            /* the heap starts after the image */
		t->brk_start = MOS_MAX(t->brk_start,
				       ALIGN_UP(plan.seg[i].vaddr + plan.seg[i].memsz, PAGE_SIZE));
	t->brk = t->brk_start;
	t->mmap_next = TASK_MMAP_BASE;
	if (!rc)
		rc = build_stack(t, &plan, args, start);
	if (!rc && nt_dynamic)
		rc = ntload_link(t, image, len, &plan, delta, args, start);
	if (rc) {
		vmm_as_destroy(t->pml4);
		t->pml4 = 0;
	}
	return rc;
}

/* argv for a program started by the kernel: just its name. */
void exec_args_init(struct exec_args *a, const char *argv0)
{
	memset(a, 0, sizeof(*a));
	a->argc = 1;
	a->len = (u32)mos_strlcpy(a->buf, argv0, 64) + 1;
	if (a->len > 64)
		a->len = 64;
}

static void record(const struct task *t, struct exec_report *rep)
{
	rep->pid = t->pid;
	mos_strlcpy(rep->name, t->name, sizeof(rep->name));
	rep->format = t->format;
	rep->personality = t->abi.personality;
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

void trap_frame_from_start(struct trap_frame *tf, const struct user_start *s)
{
	memset(tf, 0, sizeof(*tf));
	tf->rip = s->rip;
	tf->rsp = s->rsp;
	tf->rdi = s->rdi;
	tf->rsi = s->rsi;
	tf->rdx = s->rdx;
	tf->rcx = s->rcx;
	tf->rflags = 0x202;
	tf->cs = USER_CS;
	tf->ss = USER_DS;
}

/*
 * Run a program to completion as the foreground job. The calling kernel
 * thread waits, meanwhile turning keyboard input into the job's stdin.
 */
int task_exec(const char *name, const void *image, size_t len, struct exec_report *rep)
{
	return task_exec_args(name, image, len, NULL, rep);
}

/* As task_exec(), with further arguments (a NULL-terminated list) after argv[0]. */
int task_exec_args(const char *name, const void *image, size_t len, const char *const *argv,
		   struct exec_report *rep)
{
	struct exec_args *args = kzalloc(sizeof(*args));
	struct user_start start;
	struct trap_frame tf;
	struct thread *th;
	struct file *con;
	struct task *t;
	int rc, prev_fg = fg_pgid;

	memset(rep, 0, sizeof(*rep));
	t = args ? task_new(name) : NULL;
	if (!t) {
		kfree(args);
		return -MERR_NOMEM;
	}
	exec_args_init(args, name);
	for (; argv && *argv && args->argc < EXEC_ARGS_MAX; argv++) {
		size_t n = strlen(*argv) + 1;

		if (args->len + n > EXEC_ARGS_BUF)
			break;
		memcpy(args->buf + args->len, *argv, n);
		args->len += (u32)n;
		args->argc++;
	}
	rc = task_load_image(t, image, len, args, &start);
	kfree(args);
	rep->format = t->format;
	rep->personality = t->abi.personality;
	con = rc ? NULL : console_file();
	if (!rc && !con)
		rc = -MERR_NOMEM;
	if (rc) {
		task_free(t);
		return rc;
	}
	t->fd[0] = con;
	t->fd[1] = file_get(con);
	t->fd[2] = file_get(con);
	t->pgid = t->pid;
	t->kernel_wait = true;

	trap_frame_from_start(&tf, &start);
	th = thread_create_user(t, &tf, NULL);
	if (!th) {
		task_free(t);
		return -MERR_NOMEM;
	}
	th->tid = t->pid;
	th->gs_base = t->gs_base;
	t->nthreads = 1;

	fg_pgid = t->pgid;
	while (!t->zombie) {
		task_keyboard_pump(t);
		thread_block(t, 0, 2);
	}
	fg_pgid = prev_fg;
	stdin_drain_to_console();
	record(t, rep);
	task_free(t);
	return 0;
}

/* Keyboard and serial input while a foreground job runs. */
void task_keyboard_pump(struct task *fg)
{
	int c;

	while ((c = console_poll()) >= 0) {
		if (c == 3) {                   /* Ctrl-C */
			console_write("^C\n", 3);
			for (struct task *t = procs; t; t = t->next)
				if (t->pgid == fg->pgid)
					task_signal(t, 2);      /* SIGINT */
		} else {
			stdin_feed(c);
		}
	}
}

static int fault_signal(u32 vector)
{
	switch (vector) {
	case 0: case 16: case 19: return 8;     /* SIGFPE  */
	case 1: case 3:           return 5;     /* SIGTRAP */
	case 6:                   return 4;     /* SIGILL  */
	case 17:                  return 7;     /* SIGBUS  */
	default:                  return 11;    /* SIGSEGV */
	}
}

/* A trap in ring 3: demand paging, or the end of the process. */
void task_user_fault(struct trap_frame *tf, u64 fault_addr)
{
	struct task *t = task_current();

	if (!t)
		panic("ring-3 trap %u with no current task", (unsigned)tf->vector);
	if (tf->vector == 14 && !(tf->error & 1) && !task_fault_in(t, fault_addr, tf->error & 2))
		return;                         /* a reserved page, now backed */
	t->faulted = true;
	t->fault_vector = (u32)tf->vector;
	t->fault_rip = tf->rip;
	t->fault_addr = fault_addr;
	kprintf("[task] pid %d (%s) killed: %s at rip=%llx addr=%llx\n", t->pid, t->name,
		trap_name((unsigned)tf->vector), (unsigned long long)tf->rip,
		(unsigned long long)fault_addr);
	task_exit_group(t, 128 + (int)tf->vector, fault_signal((u32)tf->vector));
}

static u32 tick_limit = TASK_DEFAULT_TICK_LIMIT;

void task_set_tick_limit(u32 limit)
{
	tick_limit = limit;
}

void task_user_tick(void)
{
	struct task *t = task_current();

	if (!t || !tick_limit || ++t->cpu_ticks <= tick_limit)
		return;
	kprintf("[task] pid %d (%s) stopped: CPU budget of %u ticks used up\n", t->pid, t->name,
		tick_limit);
	task_exit_group(t, TASK_EXIT_TIMEOUT, 24);      /* SIGXCPU */
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
