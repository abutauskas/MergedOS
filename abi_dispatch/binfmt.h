/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_BINFMT_H
#define MERGEDOS_BINFMT_H

/*
 * Executable format detection and load planning.
 *
 * binfmt_probe() answers "what is this file and which ABI personality does
 * it need?" purely from its headers. binfmt_load_plan() additionally turns
 * ELF program headers, PE sections or Mach-O load commands into one neutral
 * list of segments, so the kernel has a single mapping routine for all
 * three container formats.
 *
 * Both functions are pure (no allocation, no globals) and never trust the
 * image: every offset is bounds-checked against the buffer.
 */
#include "abi_dispatch/abi.h"

enum bin_format {
	BINFMT_UNKNOWN = 0,
	BINFMT_ELF32,
	BINFMT_ELF64,
	BINFMT_PE32,
	BINFMT_PE32PLUS,
	BINFMT_MACHO32,
	BINFMT_MACHO64,
	BINFMT_MACHO_FAT,
	BINFMT_SCRIPT,          /* "#!" - handed to the shell layer */
};

enum bin_arch {
	BIN_ARCH_UNKNOWN = 0,
	BIN_ARCH_X86_64,
	BIN_ARCH_I386,
	BIN_ARCH_AARCH64,
};

#define BINFMT_F_DYNAMIC  0x01  /* needs a dynamic linker / imported libraries */
#define BINFMT_F_GUI      0x02  /* PE GUI subsystem                            */
#define BINFMT_F_LIBRARY  0x04  /* DLL / shared object / dylib, not a program  */
#define BINFMT_F_PIE      0x08  /* position independent (ET_DYN executable)    */
#define BINFMT_F_RELOC    0x10  /* PE: carries base relocations, may be rebased */
#define BINFMT_F_ASLR     0x20  /* PE: asks to be loaded at a randomised base   */

struct binfmt_info {
	enum bin_format format;
	enum bin_arch arch;
	enum abi_personality personality;
	u32 flags;
};

#define EXEC_MAX_SEGS 24
#define EXEC_PROT_R   0x1
#define EXEC_PROT_W   0x2
#define EXEC_PROT_X   0x4

struct exec_seg {
	u64 vaddr;      /* where the segment starts in the new address space     */
	u64 memsz;      /* bytes of address space (zero-filled beyond filesz)    */
	u64 fileoff;    /* offset of its contents within the image buffer        */
	u64 filesz;
	u32 prot;
};

struct exec_plan {
	struct binfmt_info info;
	u64 entry;
	u64 phdr_vaddr;         /* ELF: where the program headers land in memory (auxv) */
	u16 phnum, phentsize;
	u64 image_base, image_size;     /* PE: preferred base and SizeOfImage           */
	u32 nsegs;
	struct exec_seg seg[EXEC_MAX_SEGS];
};

/* Returns 0 if the format was recognised, -MERR_NOEXEC otherwise. */
int binfmt_probe(const void *image, size_t len, struct binfmt_info *out);

/*
 * Returns 0 on success; -MERR_NOEXEC for malformed images and -MERR_NOTSUP
 * for well-formed images this build cannot run yet (dynamic linking,
 * foreign CPU architecture, ...). `out->info` is valid in both cases.
 */
int binfmt_load_plan(const void *image, size_t len, struct exec_plan *out);

/*
 * Locate the x86-64 Mach-O inside `image` (which may be a thin Mach-O or a
 * fat/universal container). Returns false if there is none.
 */
bool binfmt_macho_slice(const void *image, size_t len, const u8 **slice, size_t *slice_len);

/* Report the install name of every dylib a Mach-O image links against. */
typedef void (*binfmt_dylib_fn)(const char *install_name, void *arg);
int binfmt_macho_dylibs(const void *image, size_t len, binfmt_dylib_fn fn, void *arg);

/*
 * PE32+ import and base-relocation walkers, for the kernel's NT loader.
 *
 * binfmt_pe_imports() calls fn once per imported symbol, in IAT order:
 * `name` is the symbol name, or NULL for an import by ordinal. `iat_rva` is
 * the RVA of the 8-byte IAT slot the loader must fill. A non-zero return
 * from fn stops the walk and is passed back.
 *
 * binfmt_pe_relocs() calls fn with the RVA of every 64-bit word that must
 * be adjusted when the image is loaded away from its preferred base.
 * Relocation types other than ABSOLUTE and DIR64 yield -MERR_NOTSUP.
 *
 * Both validate every table, name and RVA against `len`: a truncated or
 * corrupt image yields -MERR_NOEXEC, never an out-of-bounds read.
 */
typedef int (*binfmt_pe_import_fn)(const char *dll, const char *name, u16 ordinal,
				   u32 iat_rva, void *arg);
typedef int (*binfmt_pe_reloc_fn)(u32 rva, void *arg);
int binfmt_pe_imports(const void *image, size_t len, binfmt_pe_import_fn fn, void *arg);
int binfmt_pe_relocs(const void *image, size_t len, binfmt_pe_reloc_fn fn, void *arg);

const char *binfmt_format_name(enum bin_format f);
const char *binfmt_arch_name(enum bin_arch a);

#endif /* MERGEDOS_BINFMT_H */
