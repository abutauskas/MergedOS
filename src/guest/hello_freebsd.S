/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * hello.freebsd - a static FreeBSD/amd64 ELF executable (EI_OSABI = 9).
 *
 * Same container format as the Linux test program, different kernel ABI:
 * write is syscall 4 (not 1), exit is 1 (not 60), the argument vector
 * arrives in %rdi, and errors are signalled by the carry flag with a
 * *positive* errno in %rax.
 *
 * Exit status = number of the first failed step:
 *   1  %rdi points at argc == 1      3  bad fd -> CF=1, rax = EBADF (9)
 *   2  write(): CF=0, rax = length   4  getpid(): CF=0, rax > 0
 *
 * (An unassigned syscall number is not probed here: real FreeBSD raises
 * SIGSYS for it, and MergedOS has no signal delivery yet.)
 */
	.set    TEXT_BASE, 0x200000

	.text
	.code64
ehdr:
	.byte   0x7F, 'E', 'L', 'F'
	.byte   2, 1, 1
	.byte   9                               /* ELFOSABI_FREEBSD */
	.quad   0
	.word   2                               /* ET_EXEC   */
	.word   62                              /* EM_X86_64 */
	.long   1
	.quad   TEXT_BASE + (_start - ehdr)
	.quad   phdr - ehdr
	.quad   0
	.long   0
	.word   64, 56, 1
	.word   64, 0, 0
phdr:
	.long   1                               /* PT_LOAD   */
	.long   5                               /* PF_R | PF_X */
	.quad   0
	.quad   TEXT_BASE, TEXT_BASE
	.quad   file_end - ehdr, file_end - ehdr
	.quad   0x1000

_start:
	movl    $1, %r12d                       /* step 1: FreeBSD passes &argc in %rdi */
	cmpq    $1, (%rdi)
	jne     fail

	movl    $2, %r12d                       /* step 2: write(1, msg, len) */
	movl    $4, %eax
	movl    $1, %edi
	leaq    msg(%rip), %rsi
	movl    $msg_len, %edx
	syscall
	jc      fail
	cmpq    $msg_len, %rax
	jne     fail

	movl    $3, %r12d                       /* step 3: errors come back in the carry flag */
	movl    $4, %eax
	movl    $99, %edi
	leaq    msg(%rip), %rsi
	movl    $1, %edx
	syscall
	jnc     fail
	cmpq    $9, %rax                        /* EBADF, positive */
	jne     fail

	movl    $4, %r12d                       /* step 4: getpid() */
	movl    $20, %eax
	syscall
	jc      fail
	testq   %rax, %rax
	jle     fail

	movl    $1, %eax                        /* exit(0) */
	xorl    %edi, %edi
	syscall
fail:
	movl    $1, %eax                        /* exit(step) */
	movl    %r12d, %edi
	syscall

msg:
	.ascii  "Hello from a FreeBSD ELF64 binary: write=4, exit=1, errors via carry flag!\n"
	.set    msg_len, . - msg
file_end:
