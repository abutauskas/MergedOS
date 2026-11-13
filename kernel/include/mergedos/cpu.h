/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_CPU_H
#define MERGEDOS_CPU_H

#include <mergedos/types.h>

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1B
#define USER_CS   0x23
#define TSS_SEL   0x28

/* Register frame built by isr_common in entry.S (lowest address first). */
struct trap_frame {
	u64 r15, r14, r13, r12, r11, r10, r9, r8;
	u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
	u64 vector, error;
	u64 rip, cs, rflags, rsp, ss;
};

/* Initial user-mode register state consumed by user_enter(). */
struct user_start {
	u64 rip, rsp;
	u64 rdi, rsi, rdx, rcx;
};

void cpu_init(void);
bool cpu_nx_enabled(void);

long user_enter(const struct user_start *s);
void user_exit_resume(long status) MOS_NORETURN;

void trap_handler(struct trap_frame *tf);

#endif /* MERGEDOS_CPU_H */
