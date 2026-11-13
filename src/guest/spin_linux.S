/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * spin.linux - a Linux ELF that loops forever without a single system call.
 * Used to prove that the kernel keeps control: the timer interrupt preempts
 * it and the per-task CPU budget ends it.
 */
	.set    TEXT_BASE, 0x400000

	.text
	.code64
ehdr:
	.byte   0x7F, 'E', 'L', 'F', 2, 1, 1, 0
	.quad   0
	.word   2, 62
	.long   1
	.quad   TEXT_BASE + (_start - ehdr)
	.quad   phdr - ehdr
	.quad   0
	.long   0
	.word   64, 56, 1
	.word   64, 0, 0
phdr:
	.long   1, 5
	.quad   0
	.quad   TEXT_BASE, TEXT_BASE
	.quad   file_end - ehdr, file_end - ehdr
	.quad   0x1000
_start:
	jmp     _start
file_end:
