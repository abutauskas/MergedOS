/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * CPU exception handling. A fault in ring 3 terminates the offending task
 * and returns control to the kernel; a fault in ring 0 is a kernel bug.
 */
#include <mergedos/kernel.h>
#include <mergedos/cpu.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>

static const char *const trap_names[32] = {
	"#DE divide error", "#DB debug", "NMI", "#BP breakpoint", "#OF overflow",
	"#BR bound range", "#UD invalid opcode", "#NM device not available",
	"#DF double fault", "coprocessor overrun", "#TS invalid TSS",
	"#NP segment not present", "#SS stack fault", "#GP general protection",
	"#PF page fault", "reserved", "#MF x87 error", "#AC alignment check",
	"#MC machine check", "#XM SIMD error", "#VE virtualization", "#CP control protection",
	"reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
	"reserved", "#VC VMM communication", "#SX security", "reserved",
};

const char *trap_name(unsigned vector)
{
	return vector < 32 ? trap_names[vector] : "unknown";
}

/* Hardware interrupt. Only the timer (IRQ 0) is unmasked. */
static void irq_dispatch(struct trap_frame *tf)
{
	unsigned irq = (unsigned)tf->vector - 32;

	if (irq == 7 || irq == 15)
		return;                         /* spurious: no EOI */
	if (irq >= 8)
		outb(0xA0, 0x20);
	outb(0x20, 0x20);                       /* EOI first: we may not come straight back */
	if (irq != 0)
		return;
	if ((tf->cs & 3) == 3)
		task_user_tick();               /* may end a runaway task instead of returning */
	sched_tick();                           /* may switch to another thread */
}

void trap_handler(struct trap_frame *tf)
{
	u64 cr2;

	if (tf->vector >= 32) {
		irq_dispatch(tf);
		return;
	}
	cr2 = read_cr2();

	if ((tf->cs & 3) == 3)
		task_user_fault(tf, cr2);       /* does not return */

	panic("kernel trap %s (vector %u, error 0x%llx)\n"
	      "  rip=%016llx rsp=%016llx cr2=%016llx\n"
	      "  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n"
	      "  rsi=%016llx rdi=%016llx rbp=%016llx",
	      trap_name((unsigned)tf->vector), (unsigned)tf->vector,
	      (unsigned long long)tf->error,
	      (unsigned long long)tf->rip, (unsigned long long)tf->rsp,
	      (unsigned long long)cr2,
	      (unsigned long long)tf->rax, (unsigned long long)tf->rbx,
	      (unsigned long long)tf->rcx, (unsigned long long)tf->rdx,
	      (unsigned long long)tf->rsi, (unsigned long long)tf->rdi,
	      (unsigned long long)tf->rbp);
}
