/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The built-in Win32 runtime (src/ntrt), linked at its fixed user address
 * and embedded as a flat binary. kernel/ntload.c copies it into every NT
 * process that imports DLLs. NTRT_FILE is supplied by the Makefile.
 */
	.section .rodata
	.balign 4096
	.globl  ntrt_blob_start, ntrt_blob_end
ntrt_blob_start:
	.incbin NTRT_FILE
ntrt_blob_end:

	.section .note.GNU-stack, "", @progbits
