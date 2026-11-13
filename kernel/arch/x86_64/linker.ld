/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS kernel image layout.
 *
 * The 32-bit boot stub lives (and runs) at its physical address just above
 * 1 MiB. Everything else is linked at KERNEL_VMA + physical address
 * ("higher half") and loaded at the physical address via AT().
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
	. = 0x100000;
	__kernel_start = . + KERNEL_VMA;

	.boot : {
		KEEP(*(.multiboot))             /* must sit in the first 8 KiB of the file */
		*(.boot.text)
		*(.boot.data)
	}

	.boot_bss (NOLOAD) : ALIGN(4096) {
		*(.boot.bss)
	}

	. = ALIGN(4096);
	. += KERNEL_VMA;

	.text : AT(ADDR(.text) - KERNEL_VMA) {
		__text_start = .;
		*(.text .text.*)
		__text_end = .;
	}

	.rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA) {
		__rodata_start = .;
		*(.rodata .rodata.*)
		__rodata_end = .;
	}

	.data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA) {
		__data_start = .;
		*(.data .data.*)
		__data_end = .;
	}

	.bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VMA) {
		__bss_start = .;
		*(COMMON)
		*(.bss .bss.*)
		. = ALIGN(8);
		__bss_end = .;
	}

	. = ALIGN(4096);
	__kernel_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note .note.*)
		*(.eh_frame .eh_frame_hdr)
	}
}
