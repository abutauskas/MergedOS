/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Built-in Win32 runtime: linked at a fixed user address (NTRT_BASE in
 * ntrt_abi.h) and turned into a flat binary. The header comes first; code
 * and read-only data end on a page boundary so the loader can map them
 * RX and everything after RW.
 */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(ntrt_start)

SECTIONS
{
	. = 0x7F000000;
	.text : {
		KEEP(*(.ntrt_header))
		*(.text .text.*)
	}
	.rodata : {
		*(.rodata .rodata.*)
		. = ALIGN(8);
		__exports_start = .;
		KEEP(*(.ntrt_exports))
		__exports_end = .;
	}
	. = ALIGN(4096);
	__text_end = .;
	.data : {
		*(.data .data.*)
		/* Never empty, so the flat binary always reaches __file_end. */
		QUAD(0)
	}
	__file_end = .;
	.bss : {
		*(.bss .bss.* COMMON)
	}
	. = ALIGN(4096);
	__image_end = .;
	/DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
