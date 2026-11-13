/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Contract between the kernel's NT loader (kernel/ntload.c) and the
 * built-in Win32 runtime (src/ntrt/), which is linked at a fixed address
 * and embedded in the kernel image as a flat binary.
 *
 * Only plain C types here: the header is compiled both freestanding in the
 * kernel and in the user-mode runtime.
 *
 * User address-space layout of an NT process that imports DLLs:
 *
 *   image (preferred base, else NTRT_REBASE_VA)
 *   0x7F000000  runtime: header, code (RX), then data + bss (RW)
 *   0x7FFC0000  stubs for imports the runtime does not provide (RX)
 *   0x7FFD0000  struct ntrt_params, filled by the loader (RW)
 *   0x7FFE0000  KUSER_SHARED_DATA: version fields only (R)
 *   0x7FFE1000  exit thunk (see entry.S)
 */
#ifndef MERGEDOS_NTRT_ABI_H
#define MERGEDOS_NTRT_ABI_H

#define NTRT_BASE        0x7F000000ULL
#define NTRT_LIMIT       0x7F400000ULL          /* runtime must end below this */
#define NTRT_STUB_VA     0x7FFC0000ULL
#define NTRT_STUB_PAGES  4
#define NTRT_PARAMS_VA   0x7FFD0000ULL
#define NTRT_KUSER_VA    0x7FFE0000ULL
#define NTRT_REBASE_VA   0x200000000ULL         /* where a clashing image is moved */
#define NTRT_MAGIC       0x5452544Eu            /* "NTRT" */

/* KUSER_SHARED_DATA offsets used (same as on Windows x64). */
#define KUSER_NT_BUILD_NUMBER   0x260
#define KUSER_NT_PRODUCT_TYPE   0x264
#define KUSER_PRODUCT_VALID     0x268
#define KUSER_NT_MAJOR_VERSION  0x26C
#define KUSER_NT_MINOR_VERSION  0x270

/* Exit status of a process that called an import nobody implements. */
#define NTRT_STATUS_MISSING_IMPORT 0xC0000139u  /* STATUS_ENTRYPOINT_NOT_FOUND */

/* First bytes of the runtime blob. All addresses are absolute VAs. */
struct ntrt_header {
	unsigned int magic, reserved;
	unsigned long long text_end;            /* [NTRT_BASE, text_end) is code: RX */
	unsigned long long image_end;           /* end of data + bss: RW             */
	unsigned long long file_end;            /* end of the bytes in the blob      */
	unsigned long long start;               /* void start(struct ntrt_params *)  */
	unsigned long long missing;             /* ms_abi void missing(const char *) */
	unsigned long long teb;                 /* GS base for the (only) thread     */
	unsigned long long exports, exports_end; /* array of struct ntrt_export      */
};

/* One function or variable the runtime provides, under its Win32 name. */
struct ntrt_export {
	unsigned long long name;                /* VA of a NUL-terminated name */
	unsigned long long addr;
};

/* Written by the kernel at NTRT_PARAMS_VA before the runtime starts. */
struct ntrt_params {
	unsigned long long image_base, entry;
	unsigned long long stack_base, stack_limit;     /* top (exclusive) and bottom */
	unsigned int pid, tid;
	char computer_name[16];
	char command_line[256];
};

#endif /* MERGEDOS_NTRT_ABI_H */
