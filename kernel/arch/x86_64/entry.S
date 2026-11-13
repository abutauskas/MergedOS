/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS x86_64 low-level entry points:
 *   - gdt_load:          install the runtime GDT and reload segment registers
 *   - isr_stub_N:        CPU exception entry (vectors 0..31)
 *   - syscall_entry:     SYSCALL target shared by *all* ABI personalities
 *   - user_enter / user_exit_resume: enter ring 3 and unwind back out of it
 *
 * Linux, FreeBSD, XNU and NT x86-64 binaries all enter the kernel with the
 * same instruction (SYSCALL); they differ only in what the registers mean.
 * This file therefore saves a single, ABI-neutral register frame and leaves
 * interpretation to abi_dispatch/.
 *
 * Uniprocessor PoC: the kernel-stack pointer for SYSCALL lives in a global.
 * The SMP milestone moves it behind %gs (swapgs) as a per-CPU variable.
 */

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   0x1B          /* 0x18 | RPL 3 */
#define USER_CS   0x23          /* 0x20 | RPL 3 */

/* Layout must match struct syscall_frame / struct trap_frame (abi.h, cpu.h). */
.macro PUSH_GPRS
	pushq   %rax
	pushq   %rbx
	pushq   %rcx
	pushq   %rdx
	pushq   %rsi
	pushq   %rdi
	pushq   %rbp
	pushq   %r8
	pushq   %r9
	pushq   %r10
	pushq   %r11
	pushq   %r12
	pushq   %r13
	pushq   %r14
	pushq   %r15
.endm

.macro POP_GPRS
	popq    %r15
	popq    %r14
	popq    %r13
	popq    %r12
	popq    %r11
	popq    %r10
	popq    %r9
	popq    %r8
	popq    %rbp
	popq    %rdi
	popq    %rsi
	popq    %rdx
	popq    %rcx
	popq    %rbx
	popq    %rax
.endm

	.text
	.code64

/* void gdt_load(const struct desc_ptr *gdtr); */
	.globl  gdt_load
	.type   gdt_load, @function
gdt_load:
	lgdt    (%rdi)
	movw    $KERNEL_DS, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	xorl    %eax, %eax
	movw    %ax, %fs
	movw    %ax, %gs
	popq    %rax                            /* return address */
	pushq   $KERNEL_CS
	pushq   %rax
	lretq                                   /* reload %cs */

/* ---- CPU exceptions ------------------------------------------------------ */

.macro ISR_NOERR vec
	.balign 16
isr_stub_\vec:
	pushq   $0                              /* fake error code */
	pushq   $\vec
	jmp     isr_common
.endm

.macro ISR_ERR vec
	.balign 16
isr_stub_\vec:
	pushq   $\vec                           /* CPU already pushed the error code */
	jmp     isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* Hardware interrupts: PIC lines 0..15 arrive as vectors 32..47. */
.irp vec, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47
ISR_NOERR \vec
.endr

isr_common:
	PUSH_GPRS
	movq    %rsp, %rdi                      /* struct trap_frame * */
	cld
	call    trap_handler
	POP_GPRS
	addq    $16, %rsp                       /* vector + error code */
	iretq

	.section .rodata
	.balign 8
	.globl  isr_stub_table
isr_stub_table:
.altmacro
.macro ISR_ADDR n
	.quad   isr_stub_\n
.endm
.set i, 0
.rept 48
	ISR_ADDR %i
	.set i, i + 1
.endr
.noaltmacro

/* ---- SYSCALL entry (all personalities) ----------------------------------- */

	.text
	.globl  syscall_entry
	.type   syscall_entry, @function
syscall_entry:
	/* In: rcx = user rip, r11 = user rflags, rsp = user stack. IF is masked. */
	movq    %rsp, syscall_user_rsp(%rip)
	movq    syscall_kernel_rsp(%rip), %rsp
	pushq   syscall_user_rsp(%rip)          /* frame.rsp    */
	pushq   %r11                            /* frame.rflags */
	pushq   %rcx                            /* frame.rip    */
	PUSH_GPRS
	movq    %rsp, %rdi                      /* struct syscall_frame * */
	cld
	call    syscall_handler                 /* does not return if the task exited */
	POP_GPRS
	popq    %rcx                            /* rip    -> rcx for SYSRET */
	popq    %r11                            /* rflags -> r11 for SYSRET */
	popq    %rsp
	sysretq

/* ---- Ring 3 entry / exit ---------------------------------------------------
 *
 * long user_enter(const struct user_start *s);
 *
 * Saves the callee-saved kernel context, then IRETs to ring 3. It "returns"
 * when some other kernel path (exit syscall, fatal trap) calls
 * user_exit_resume(), which unwinds straight back to user_enter's caller.
 */
	.globl  user_enter
	.type   user_enter, @function
user_enter:
	pushq   %rbp
	pushq   %rbx
	pushq   %r12
	pushq   %r13
	pushq   %r14
	pushq   %r15
	movq    %rsp, kernel_resume_rsp(%rip)

	pushq   $USER_DS                        /* ss     */
	pushq   8(%rdi)                         /* rsp    */
	pushq   $0x202                          /* rflags: IF=1, user code is preemptible */
	pushq   $USER_CS                        /* cs     */
	pushq   0(%rdi)                         /* rip    */

	movq    24(%rdi), %rsi
	movq    32(%rdi), %rdx
	movq    40(%rdi), %rcx
	movq    16(%rdi), %rdi
	xorl    %eax, %eax                      /* do not leak kernel values to user mode */
	xorl    %ebx, %ebx
	xorl    %ebp, %ebp
	xorl    %r8d, %r8d
	xorl    %r9d, %r9d
	xorl    %r10d, %r10d
	xorl    %r11d, %r11d
	xorl    %r12d, %r12d
	xorl    %r13d, %r13d
	xorl    %r14d, %r14d
	xorl    %r15d, %r15d
	iretq

/* void user_exit_resume(long status) __attribute__((noreturn)); */
	.globl  user_exit_resume
	.type   user_exit_resume, @function
user_exit_resume:
	movq    kernel_resume_rsp(%rip), %rsp
	movq    %rdi, %rax
	popq    %r15
	popq    %r14
	popq    %r13
	popq    %r12
	popq    %rbx
	popq    %rbp
	ret

/*
 * void switch_context(u64 **save_rsp, u64 *load_rsp);
 *
 * Kernel thread switch. Everything the C ABI lets a callee clobber is
 * already dead at a call, so only the callee-saved registers go on the old
 * stack. Called with interrupts off.
 */
	.text
	.globl  switch_context
	.type   switch_context, @function
switch_context:
	pushq   %rbp
	pushq   %rbx
	pushq   %r12
	pushq   %r13
	pushq   %r14
	pushq   %r15
	movq    %rsp, (%rdi)
	movq    %rsi, %rsp
	popq    %r15
	popq    %r14
	popq    %r13
	popq    %r12
	popq    %rbx
	popq    %rbp
	ret

/* ---- NT thread-exit thunk --------------------------------------------------
 *
 * Position-independent code copied into every NT-personality process. A PE
 * entry point that simply returns lands here, mirroring what
 * ntdll!RtlUserThreadStart does on Windows: NtTerminateProcess(-1, eax).
 */
	.section .rodata
	.globl  nt_exit_thunk, nt_exit_thunk_end
nt_exit_thunk:
	movq    $-1, %r10                       /* arg1: NtCurrentProcess() */
	movl    %eax, %edx                      /* arg2: exit status        */
	movl    $0x2C, %eax                     /* NtTerminateProcess       */
	syscall
	ud2
nt_exit_thunk_end:

/* ---- Data ------------------------------------------------------------------ */

	.data
	.balign 8
	.globl  syscall_kernel_rsp
syscall_kernel_rsp:
	.quad   ring0_stack_top
syscall_user_rsp:
	.quad   0
kernel_resume_rsp:
	.quad   0

	.bss
	.balign 16
ring0_stack:
	.skip   16384
	.globl  ring0_stack_top
ring0_stack_top:
