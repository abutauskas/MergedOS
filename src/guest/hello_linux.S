/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * hello.linux - a complete static Linux x86-64 ELF executable, headers
 * included, written out by hand so the build needs no Linux libc.
 *
 * It is a genuine Linux binary: it runs unmodified on a real Linux kernel
 * (`./build/guest/hello.linux`) and, byte for byte the same file, on
 * MergedOS through the Linux personality.
 *
 * Beyond printing, it checks the things an ABI layer tends to get wrong and
 * exits with the number of the first failed step:
 *   1  .bss is zero-filled            4  bad fd  -> rax = -EBADF  (-9)
 *   2  .data is writable              5  bad nr  -> rax = -ENOSYS (-38)
 *   3  write() returns the length     6  getpid() > 0
 *   7  uname() succeeds and names the system "Linux"
 *
 * Assembled as a flat binary:  as -> objcopy -O binary
 */
	.set    TEXT_BASE, 0x400000
	.set    DATA_BASE, 0x600000             /* + file offset, so vaddr == offset (mod 4K) */
	.set    BSS_SIZE,  512

	.text
	.code64
ehdr:
	.byte   0x7F, 'E', 'L', 'F'
	.byte   2                               /* ELFCLASS64  */
	.byte   1                               /* ELFDATA2LSB */
	.byte   1                               /* EV_CURRENT  */
	.byte   0                               /* ELFOSABI_NONE: what GNU ld emits for Linux */
	.quad   0
	.word   2                               /* ET_EXEC     */
	.word   62                              /* EM_X86_64   */
	.long   1
	.quad   TEXT_BASE + (_start - ehdr)     /* e_entry     */
	.quad   phdr - ehdr                     /* e_phoff     */
	.quad   0                               /* e_shoff     */
	.long   0                               /* e_flags     */
	.word   64                              /* e_ehsize    */
	.word   56                              /* e_phentsize */
	.word   2                               /* e_phnum     */
	.word   64, 0, 0                        /* no section headers */
phdr:
	.long   1                               /* PT_LOAD: text */
	.long   5                               /* PF_R | PF_X   */
	.quad   0
	.quad   TEXT_BASE, TEXT_BASE
	.quad   text_end - ehdr, text_end - ehdr
	.quad   0x1000

	.long   1                               /* PT_LOAD: data + bss */
	.long   6                               /* PF_R | PF_W         */
	.quad   data - ehdr
	.quad   DATA_BASE + (data - ehdr), DATA_BASE + (data - ehdr)
	.quad   data_end - data
	.quad   data_end - data + BSS_SIZE
	.quad   0x1000

	.set    MSG_VA, DATA_BASE + (msg - ehdr)
	.set    BSS_VA, DATA_BASE + (data_end - ehdr)

_start:
	movl    $1, %r12d                       /* step 1: .bss reads as zero */
	movl    $BSS_VA, %ebx
	cmpq    $0, (%rbx)
	jne     fail

	movl    $2, %r12d                       /* step 2: data segment is writable */
	movq    $0x1122334455667788, %rax
	movq    %rax, (%rbx)
	cmpq    %rax, (%rbx)
	jne     fail
	movl    $(MSG_VA + msg_mark - msg), %ebx
	movb    $'!', (%rbx)                    /* patch the '?' in the message */

	movl    $3, %r12d                       /* step 3: write(1, msg, len) */
	movl    $1, %eax
	movl    $1, %edi
	movl    $MSG_VA, %esi
	movl    $msg_len, %edx
	syscall
	cmpq    $msg_len, %rax
	jne     fail

	movl    $4, %r12d                       /* step 4: Linux reports errors as -errno */
	movl    $1, %eax
	movl    $99, %edi
	movl    $MSG_VA, %esi
	movl    $1, %edx
	syscall
	cmpq    $-9, %rax                       /* -EBADF */
	jne     fail

	movl    $5, %r12d                       /* step 5: unassigned syscall number */
	movl    $511, %eax
	syscall
	cmpq    $-38, %rax                      /* -ENOSYS (Linux numbering) */
	jne     fail

	movl    $6, %r12d                       /* step 6: getpid() */
	movl    $39, %eax
	syscall
	testq   %rax, %rax
	jle     fail

	movl    $7, %r12d                       /* step 7: uname() into .bss */
	movl    $63, %eax
	movl    $(BSS_VA + 64), %edi
	syscall
	testq   %rax, %rax
	jnz     fail
	movl    $(BSS_VA + 64), %ebx
	cmpl    $0x756E694C, (%rbx)             /* "Linu" */
	jne     fail

	movl    $231, %eax                      /* exit_group(0) */
	xorl    %edi, %edi
	syscall
fail:
	movl    $60, %eax                       /* exit(step) */
	movl    %r12d, %edi
	syscall
text_end:

data:
msg:
	.ascii  "Hello from a Linux ELF64 binary: write=1, exit_group=231, errors as -errno"
msg_mark:
	.ascii  "?\n"
	.set    msg_len, . - msg
data_end:
