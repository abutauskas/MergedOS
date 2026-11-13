/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The boot ramdisk, linked straight into the kernel image as read-only data.
 * INITRD_FILE is supplied by the Makefile (build/initrd.tar).
 */
	.section .rodata
	.balign 512
	.globl  initrd_start, initrd_end
initrd_start:
	.incbin INITRD_FILE
initrd_end:
