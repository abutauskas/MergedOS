/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * hello.macho - a static macOS x86-64 Mach-O executable, written out by
 * hand (GNU binutils cannot link Mach-O, and no Apple code is involved).
 *
 * Load commands: __PAGEZERO (4 GiB guard), __TEXT, and LC_UNIXTHREAD, the
 * entry-point command used by statically linked Mach-O programs. System
 * calls use XNU's class-tagged numbers: 0x2000000 | n for the BSD class,
 * with errors signalled through the carry flag.
 *
 * Exit status = number of the first failed step:
 *   1  rsp points at argc == 1       3  bad fd -> CF=1, rax = EBADF (9)
 *   2  write(): CF=0, rax = length   4  getpid(): CF=0, rax > 0
 */
	.set    TEXT_BASE, 0x100000000          /* right above __PAGEZERO */
	.set    SYSCALL_CLASS_UNIX, 0x2000000

	.text
	.code64
mh:
	.long   0xFEEDFACF                      /* MH_MAGIC_64            */
	.long   0x01000007                      /* CPU_TYPE_X86_64        */
	.long   0x00000003                      /* CPU_SUBTYPE_X86_64_ALL */
	.long   2                               /* MH_EXECUTE             */
	.long   3                               /* ncmds                  */
	.long   cmds_end - cmds                 /* sizeofcmds             */
	.long   0x00000001                      /* MH_NOUNDEFS            */
	.long   0
cmds:
	.long   0x19, 72                        /* LC_SEGMENT_64 */
	.ascii  "__PAGEZERO\0\0\0\0\0\0"
	.quad   0, 0x100000000                  /* vmaddr, vmsize         */
	.quad   0, 0                            /* fileoff, filesize      */
	.long   0, 0, 0, 0                      /* maxprot, initprot, nsects, flags */

	.long   0x19, 72                        /* LC_SEGMENT_64 */
	.ascii  "__TEXT\0\0\0\0\0\0\0\0\0\0"
	.quad   TEXT_BASE, 0x1000
	.quad   0, file_end - mh
	.long   5, 5, 0, 0                      /* r-x */

	.long   0x5, 184                        /* LC_UNIXTHREAD */
	.long   4, 42                           /* x86_THREAD_STATE64, 42 words */
	.fill   16, 8, 0                        /* rax rbx rcx rdx rdi rsi rbp rsp r8-r15 */
	.quad   TEXT_BASE + (_start - mh)       /* rip */
	.fill   4, 8, 0                         /* rflags cs fs gs */
cmds_end:

_start:
	movl    $1, %r12d                       /* step 1: argc on the stack */
	cmpq    $1, (%rsp)
	jne     fail

	movl    $2, %r12d                       /* step 2: write(1, msg, len) */
	movl    $(SYSCALL_CLASS_UNIX | 4), %eax
	movl    $1, %edi
	leaq    msg(%rip), %rsi
	movl    $msg_len, %edx
	syscall
	jc      fail
	cmpq    $msg_len, %rax
	jne     fail

	movl    $3, %r12d                       /* step 3: carry-flag error convention */
	movl    $(SYSCALL_CLASS_UNIX | 4), %eax
	movl    $99, %edi
	leaq    msg(%rip), %rsi
	movl    $1, %edx
	syscall
	jnc     fail
	cmpq    $9, %rax                        /* EBADF */
	jne     fail

	movl    $4, %r12d                       /* step 4: getpid() */
	movl    $(SYSCALL_CLASS_UNIX | 20), %eax
	syscall
	jc      fail
	testq   %rax, %rax
	jle     fail

	movl    $(SYSCALL_CLASS_UNIX | 1), %eax /* exit(0) */
	xorl    %edi, %edi
	syscall
fail:
	movl    $(SYSCALL_CLASS_UNIX | 1), %eax /* exit(step) */
	movl    %r12d, %edi
	syscall

msg:
	.ascii  "Hello from a macOS Mach-O binary: write=0x2000004, exit=0x2000001, carry-flag errors!\n"
	.set    msg_len, . - msg
file_end:
