/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS x86_64 boot path.
 *
 * A Multiboot 1 loader (GRUB, QEMU -kernel) drops us in 32-bit protected
 * mode with paging off. This file:
 *   1. checks that the CPU supports long mode,
 *   2. builds the boot page tables (identity map, direct map, kernel map),
 *   3. enters 64-bit mode and jumps to the higher-half kernel.
 *
 * Boot page tables (2 MiB pages, 32 KiB total):
 *   PML4[0]   -> identity map of physical 0..4 GiB (dropped again in vmm_init)
 *   PML4[256] -> direct map   of physical 0..4 GiB at 0xFFFF800000000000
 *   PML4[511] -> kernel image, physical 0..1 GiB at 0xFFFFFFFF80000000
 */

#define MB1_MAGIC       0x1BADB002
#define MB1_FLAGS       0x00000007      /* page-align modules, memory map, video mode */

#define MSR_EFER        0xC0000080
#define EFER_LME        0x00000100

	.section .multiboot, "a", @progbits
	.balign 4
	.long   MB1_MAGIC
	.long   MB1_FLAGS
	.long   -(MB1_MAGIC + MB1_FLAGS)
	.long   0, 0, 0, 0, 0                   /* load addresses: unused for ELF */
	.long   0                               /* mode_type: linear graphics    */
	.long   1024, 768, 32                   /* preferred width, height, depth */

/* ------------------------------------------------------------------------ */
	.section .boot.text, "ax", @progbits
	.code32
	.globl  _start
	.type   _start, @function
_start:
	cli
	cld
	movl    %eax, boot_mb_magic
	movl    %ebx, boot_mb_info
	movl    $boot_stack_top, %esp

	/* Long mode available? (CPUID.80000001h:EDX[29]) */
	movl    $0x80000000, %eax
	cpuid
	cmpl    $0x80000001, %eax
	jb      .Lno_long_mode
	movl    $0x80000001, %eax
	cpuid
	testl   $0x20000000, %edx
	jz      .Lno_long_mode

	/* Do not trust the loader to have zeroed .bss: clear all 8 table pages. */
	movl    $boot_pml4, %edi
	xorl    %eax, %eax
	movl    $(8 * 4096 / 4), %ecx
	rep stosl

	/* Four page directories: 2048 x 2 MiB pages = physical 0..4 GiB. */
	movl    $boot_pd, %edi
	xorl    %ecx, %ecx
1:	movl    %ecx, %eax
	shll    $21, %eax
	orl     $0x83, %eax                     /* present | writable | 2 MiB */
	movl    %eax, (%edi,%ecx,8)
	incl    %ecx
	cmpl    $2048, %ecx
	jne     1b

	/* PDPT entries 0..3 of the identity map and of the direct map. */
	xorl    %ecx, %ecx
2:	movl    %ecx, %eax
	shll    $12, %eax
	addl    $boot_pd, %eax
	orl     $0x3, %eax
	movl    %eax, boot_pdpt_low(,%ecx,8)
	movl    %eax, boot_pdpt_dmap(,%ecx,8)
	incl    %ecx
	cmpl    $4, %ecx
	jne     2b

	/* Kernel map: PDPT[510] covers 0xFFFFFFFF80000000 + 1 GiB. */
	movl    $(boot_pd + 0x3), boot_pdpt_kern + 510 * 8

	movl    $(boot_pdpt_low  + 0x3), boot_pml4 + 0 * 8
	movl    $(boot_pdpt_dmap + 0x3), boot_pml4 + 256 * 8
	movl    $(boot_pdpt_kern + 0x3), boot_pml4 + 511 * 8

	/* PAE + SSE state management (user programs may use SSE; the kernel does not). */
	movl    %cr4, %eax
	orl     $0x00000620, %eax               /* PAE | OSFXSR | OSXMMEXCPT */
	movl    %eax, %cr4

	movl    $boot_pml4, %eax
	movl    %eax, %cr3

	movl    $MSR_EFER, %ecx
	rdmsr
	orl     $EFER_LME, %eax
	wrmsr

	movl    %cr0, %eax
	andl    $0xFFFFFFFB, %eax               /* clear EM */
	orl     $0x80010002, %eax               /* PG | WP | MP */
	movl    %eax, %cr0

	lgdt    boot_gdt_ptr
	ljmp    $0x08, $.Llong_mode

.Lno_long_mode:
	movl    $boot_no_lm_msg, %esi
	movl    $0xB8000, %edi
3:	lodsb
	testb   %al, %al
	jz      4f
	movb    %al, (%edi)
	movb    $0x4F, 1(%edi)                  /* white on red */
	addl    $2, %edi
	jmp     3b
4:	hlt
	jmp     4b

	.code64
.Llong_mode:
	movw    $0x10, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	xorl    %eax, %eax
	movw    %ax, %fs
	movw    %ax, %gs
	movl    boot_mb_magic, %edi             /* kmain(magic, mbi) */
	movl    boot_mb_info, %esi
	movabsq $kernel_entry64, %rax
	jmp     *%rax

/* ------------------------------------------------------------------------ */
	.section .boot.data, "aw", @progbits
	.balign 16
boot_gdt:
	.quad   0
	.quad   0x00AF9A000000FFFF              /* 0x08: 64-bit code */
	.quad   0x00CF92000000FFFF              /* 0x10: data        */
boot_gdt_end:
boot_gdt_ptr:
	.word   boot_gdt_end - boot_gdt - 1
	.long   boot_gdt
boot_mb_magic:
	.long   0
boot_mb_info:
	.long   0
boot_no_lm_msg:
	.asciz  "MergedOS requires an x86-64 (long mode) CPU."

	.section .boot.bss, "aw", @nobits
	.balign 4096
boot_pml4:      .skip 4096
boot_pdpt_low:  .skip 4096
boot_pdpt_dmap: .skip 4096
boot_pdpt_kern: .skip 4096
boot_pd:        .skip 4 * 4096
boot_stack:     .skip 4096
boot_stack_top:

/* ------------------------------------------------------------------------ */
	.section .text
	.code64
	.globl  kernel_entry64
	.type   kernel_entry64, @function
kernel_entry64:
	movq    %rdi, %r8
	movq    %rsi, %r9

	/* Zero .bss ourselves (start and end are 8-byte aligned by the linker script). */
	leaq    __bss_start(%rip), %rdi
	leaq    __bss_end(%rip), %rcx
	subq    %rdi, %rcx
	shrq    $3, %rcx
	xorl    %eax, %eax
	rep stosq

	movq    %r8, %rdi
	movq    %r9, %rsi
	leaq    kernel_stack_top(%rip), %rsp
	xorl    %ebp, %ebp
	call    kmain
5:	cli
	hlt
	jmp     5b

	.section .bss
	.balign 16
kernel_stack:
	.skip   16384
kernel_stack_top:
