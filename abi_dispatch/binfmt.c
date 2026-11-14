/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Executable format probes and load planners for ELF, PE/COFF and Mach-O.
 * Written against the public format specifications; shares no code with
 * any of the operating systems whose binaries it recognises.
 */
#include "abi_dispatch/binfmt.h"

/* ======================================================================== */
/* ELF                                                                       */
/* ======================================================================== */

#define ELFCLASS64        2
#define ELFDATA2LSB       1
#define ELFOSABI_LINUX    3
#define ELFOSABI_FREEBSD  9
#define ET_EXEC           2
#define ET_DYN            3
#define EM_386            3
#define EM_X86_64         62
#define EM_AARCH64        183
#define PT_LOAD           1
#define PT_DYNAMIC        2
#define PT_INTERP         3
#define PT_NOTE           4
#define ELF64_EHDR_SIZE   64
#define ELF64_PHDR_SIZE   56

/* Walk one PT_NOTE segment looking for an OS brand note. */
static enum abi_personality elf_note_brand(const u8 *p, u64 len)
{
	u64 off = 0;

	while (mos_in_range(len, off, 12)) {
		u32 namesz = rd32le(p + off);
		u32 descsz = rd32le(p + off + 4);
		u32 type = rd32le(p + off + 8);
		u64 name_off = off + 12;
		u64 desc_off = name_off + ALIGN_UP(namesz, 4);

		if (!mos_in_range(len, name_off, namesz) || !mos_in_range(len, desc_off, descsz))
			break;
		if (type == 1 && namesz == 8 && !memcmp(p + name_off, "FreeBSD", 8))
			return ABI_FREEBSD;     /* NT_FREEBSD_ABI_TAG */
		if (type == 1 && namesz == 4 && !memcmp(p + name_off, "GNU", 4) &&
		    descsz >= 16 && rd32le(p + desc_off) == 0)
			return ABI_LINUX;       /* NT_GNU_ABI_TAG, OS = Linux */
		off = desc_off + ALIGN_UP(descsz, 4);
	}
	return ABI_NONE;
}

/*
 * Decide which kernel ABI an ELF64 image expects, the way a multi-ABI
 * kernel has to: explicit EI_OSABI first, then vendor notes, then the
 * requested dynamic linker, then a fallback brand.
 */
static enum abi_personality elf64_brand(const u8 *p, size_t len, u32 *flags)
{
	enum abi_personality brand = ABI_NONE;
	u64 phoff = rd64le(p + 32);
	u16 phentsize = rd16le(p + 54), phnum = rd16le(p + 56);

	if (p[7] == ELFOSABI_FREEBSD)
		brand = ABI_FREEBSD;
	else if (p[7] == ELFOSABI_LINUX)
		brand = ABI_LINUX;

	if (phentsize < ELF64_PHDR_SIZE || !mos_in_range(len, phoff, (u64)phnum * phentsize))
		return brand;

	for (u16 i = 0; i < phnum; i++) {
		const u8 *ph = p + phoff + (u64)i * phentsize;
		u32 type = rd32le(ph);
		u64 off = rd64le(ph + 8), filesz = rd64le(ph + 32);

		if (type == PT_DYNAMIC) {
			*flags |= BINFMT_F_DYNAMIC;
		} else if (type == PT_INTERP) {
			*flags |= BINFMT_F_DYNAMIC;
			if (brand == ABI_NONE && mos_in_range(len, off, filesz)) {
				const char *interp = (const char *)p + off;

				if (mos_memstr(interp, filesz, "ld-linux") ||
				    mos_memstr(interp, filesz, "ld-musl"))
					brand = ABI_LINUX;
				else if (mos_memstr(interp, filesz, "ld-elf"))
					brand = ABI_FREEBSD;
			}
		} else if (type == PT_NOTE && brand == ABI_NONE && mos_in_range(len, off, filesz)) {
			brand = elf_note_brand(p + off, filesz);
		}
	}
	return brand;
}

static int elf_probe(const u8 *p, size_t len, struct binfmt_info *out)
{
	u16 machine, type;

	if (len < 52)
		return -MERR_NOEXEC;
	out->format = p[4] == ELFCLASS64 ? BINFMT_ELF64 : BINFMT_ELF32;
	if (p[5] != ELFDATA2LSB)
		return 0;                       /* big-endian: recognised, not runnable */

	type = rd16le(p + 16);
	machine = rd16le(p + 18);
	out->arch = machine == EM_X86_64 ? BIN_ARCH_X86_64
		  : machine == EM_386 ? BIN_ARCH_I386
		  : machine == EM_AARCH64 ? BIN_ARCH_AARCH64 : BIN_ARCH_UNKNOWN;
	if (type == ET_DYN)
		out->flags |= BINFMT_F_PIE;

	if (out->format == BINFMT_ELF64 && len >= ELF64_EHDR_SIZE) {
		out->personality = elf64_brand(p, len, &out->flags);
	} else {
		out->personality = p[7] == ELFOSABI_FREEBSD ? ABI_FREEBSD
				 : p[7] == ELFOSABI_LINUX ? ABI_LINUX : ABI_NONE;
	}
	/*
	 * Unbranded System V images are overwhelmingly Linux binaries (the GNU
	 * toolchain leaves EI_OSABI at 0); this mirrors FreeBSD's
	 * kern.elf64.fallback_brand knob for its Linux ABI layer.
	 */
	if (out->personality == ABI_NONE)
		out->personality = ABI_LINUX;
	return 0;
}

static int elf_plan(const u8 *p, size_t len, struct exec_plan *plan)
{
	u64 phoff;
	u16 phentsize, phnum;

	if (plan->info.format != BINFMT_ELF64 || plan->info.arch != BIN_ARCH_X86_64)
		return -MERR_NOTSUP;
	if (len < ELF64_EHDR_SIZE)
		return -MERR_NOEXEC;
	if (rd16le(p + 16) != ET_EXEC && rd16le(p + 16) != ET_DYN)
		return -MERR_NOEXEC;
	plan->relative = rd16le(p + 16) == ET_DYN;

	phoff = rd64le(p + 32);
	phentsize = rd16le(p + 54);
	phnum = rd16le(p + 56);
	if (phentsize < ELF64_PHDR_SIZE || !mos_in_range(len, phoff, (u64)phnum * phentsize))
		return -MERR_NOEXEC;

	for (u16 i = 0; i < phnum; i++) {
		const u8 *ph = p + phoff + (u64)i * phentsize;
		struct exec_seg *s;
		u32 pflags;

		if (rd32le(ph) == PT_INTERP) {
			u64 off = rd64le(ph + 8), sz = rd64le(ph + 32);

			/* A path the kernel will open: NUL-terminated, sane length. */
			if (sz < 2 || sz > 128 || !mos_in_range(len, off, sz) || p[off + sz - 1])
				return -MERR_NOEXEC;
			for (u64 k = 0; k + 1 < sz; k++)
				if (!p[off + k])
					return -MERR_NOEXEC;
			plan->interp_off = off;
			plan->interp_len = (u32)sz - 1;
			continue;
		}
		if (rd32le(ph) != PT_LOAD)
			continue;
		if (plan->nsegs == EXEC_MAX_SEGS)
			return -MERR_RANGE;
		s = &plan->seg[plan->nsegs++];
		pflags = rd32le(ph + 4);
		s->fileoff = rd64le(ph + 8);
		s->vaddr = rd64le(ph + 16);
		s->filesz = rd64le(ph + 32);
		s->memsz = rd64le(ph + 40);
		s->prot = ((pflags & 4) ? EXEC_PROT_R : 0) | ((pflags & 2) ? EXEC_PROT_W : 0) |
			  ((pflags & 1) ? EXEC_PROT_X : 0);
		if (s->filesz > s->memsz || !mos_in_range(len, s->fileoff, s->filesz))
			return -MERR_NOEXEC;
	}
	plan->entry = rd64le(p + 24);
	/* Program headers as the C library will look for them through AT_PHDR. */
	for (u32 i = 0; i < plan->nsegs; i++) {
		const struct exec_seg *s = &plan->seg[i];

		if (phoff >= s->fileoff && phoff - s->fileoff < s->filesz) {
			plan->phdr_vaddr = s->vaddr + (phoff - s->fileoff);
			plan->phnum = phnum;
			plan->phentsize = phentsize;
		}
	}
	return plan->nsegs ? 0 : -MERR_NOEXEC;
}

/* ======================================================================== */
/* PE / COFF                                                                 */
/* ======================================================================== */

#define PE_MACHINE_I386    0x014C
#define PE_MACHINE_AMD64   0x8664
#define PE_MACHINE_ARM64   0xAA64
#define PE_CHAR_DLL        0x2000
#define PE_MAGIC_PE32      0x010B
#define PE_MAGIC_PE32PLUS  0x020B
#define PE_SUBSYSTEM_GUI   2
#define PE_DIR_IMPORT      1
#define PE_DIR_BASERELOC   5
#define PE_CHAR_RELOCS_STRIPPED 0x0001
#define PE_DLLCHAR_DYNAMIC_BASE 0x0040
#define PE_REL_ABSOLUTE    0
#define PE_REL_DIR64       10
#define PE_MAX_IMPORT_DLLS 256          /* sanity bounds on loops driven by image data */
#define PE_MAX_THUNKS      65536
#define PE_SCN_EXECUTE     0x20000000u
#define PE_SCN_READ        0x40000000u
#define PE_SCN_WRITE       0x80000000u
#define PE_COFF_SIZE       20
#define PE_SECTION_SIZE    40
#define PE32PLUS_OPT_MIN   112          /* optional header up to the data directories */

/* Returns the offset of the COFF header, or 0 if this is not a PE image. */
static u32 pe_coff_offset(const u8 *p, size_t len)
{
	u32 lfanew;

	if (len < 0x40)
		return 0;
	lfanew = rd32le(p + 0x3C);
	if (!mos_in_range(len, lfanew, 4 + PE_COFF_SIZE) || memcmp(p + lfanew, "PE\0\0", 4))
		return 0;
	return lfanew + 4;
}

static int pe_probe(const u8 *p, size_t len, struct binfmt_info *out)
{
	u32 coff = pe_coff_offset(p, len);
	u16 machine, opt_size, magic;
	const u8 *opt;

	if (!coff)
		return -MERR_NOEXEC;            /* plain MS-DOS executable */
	machine = rd16le(p + coff);
	opt_size = rd16le(p + coff + 16);
	opt = p + coff + PE_COFF_SIZE;
	if (opt_size < 2 || !mos_in_range(len, coff + PE_COFF_SIZE, opt_size))
		return -MERR_NOEXEC;

	magic = rd16le(opt);
	out->format = magic == PE_MAGIC_PE32PLUS ? BINFMT_PE32PLUS : BINFMT_PE32;
	out->arch = machine == PE_MACHINE_AMD64 ? BIN_ARCH_X86_64
		  : machine == PE_MACHINE_I386 ? BIN_ARCH_I386
		  : machine == PE_MACHINE_ARM64 ? BIN_ARCH_AARCH64 : BIN_ARCH_UNKNOWN;
	out->personality = ABI_NT;
	if (rd16le(p + coff + 18) & PE_CHAR_DLL)
		out->flags |= BINFMT_F_LIBRARY;

	if (magic == PE_MAGIC_PE32PLUS && opt_size >= PE32PLUS_OPT_MIN) {
		u32 ndirs = rd32le(opt + 108);

		if (rd16le(opt + 68) == PE_SUBSYSTEM_GUI)
			out->flags |= BINFMT_F_GUI;
		if (rd16le(opt + 70) & PE_DLLCHAR_DYNAMIC_BASE)
			out->flags |= BINFMT_F_ASLR;
		/* A non-empty import directory means the image needs DLLs resolved. */
		if (ndirs > PE_DIR_IMPORT && opt_size >= PE32PLUS_OPT_MIN + (PE_DIR_IMPORT + 1) * 8 &&
		    rd32le(opt + PE32PLUS_OPT_MIN + PE_DIR_IMPORT * 8 + 4) != 0)
			out->flags |= BINFMT_F_DYNAMIC;
		if (ndirs > PE_DIR_BASERELOC &&
		    opt_size >= PE32PLUS_OPT_MIN + (PE_DIR_BASERELOC + 1) * 8 &&
		    rd32le(opt + PE32PLUS_OPT_MIN + PE_DIR_BASERELOC * 8 + 4) != 0 &&
		    !(rd16le(p + coff + 18) & PE_CHAR_RELOCS_STRIPPED))
			out->flags |= BINFMT_F_RELOC;
	}
	return 0;
}

static int pe_plan(const u8 *p, size_t len, struct exec_plan *plan)
{
	u32 coff = pe_coff_offset(p, len);
	u16 nsect, opt_size;
	const u8 *opt, *sect;
	u64 image_base;
	u32 headers_size;

	if (plan->info.format != BINFMT_PE32PLUS || plan->info.arch != BIN_ARCH_X86_64)
		return -MERR_NOTSUP;
	if (plan->info.flags & BINFMT_F_LIBRARY)
		return -MERR_NOTSUP;            /* a DLL is not a program */
	/* Imports (BINFMT_F_DYNAMIC) are bound by the kernel's NT loader. */

	nsect = rd16le(p + coff + 2);
	opt_size = rd16le(p + coff + 16);
	opt = p + coff + PE_COFF_SIZE;
	sect = opt + opt_size;
	if (opt_size < PE32PLUS_OPT_MIN ||
	    !mos_in_range(len, (u64)coff + PE_COFF_SIZE + opt_size, (u64)nsect * PE_SECTION_SIZE))
		return -MERR_NOEXEC;

	image_base = rd64le(opt + 24);
	headers_size = rd32le(opt + 60);
	plan->image_base = image_base;
	plan->image_size = rd32le(opt + 56);
	if (image_base & (PAGE_SIZE - 1))
		return -MERR_NOEXEC;
	if (headers_size > len || nsect + 1u > EXEC_MAX_SEGS)
		return -MERR_NOEXEC;

	/* The headers themselves are mapped read-only at the image base. */
	plan->seg[plan->nsegs++] = (struct exec_seg){
		.vaddr = image_base, .memsz = headers_size, .fileoff = 0,
		.filesz = headers_size, .prot = EXEC_PROT_R,
	};

	for (u16 i = 0; i < nsect; i++) {
		const u8 *sh = sect + (u64)i * PE_SECTION_SIZE;
		u32 vsize = rd32le(sh + 8), rva = rd32le(sh + 12);
		u32 rawsize = rd32le(sh + 16), rawptr = rd32le(sh + 20);
		u32 chars = rd32le(sh + 36);
		struct exec_seg *s = &plan->seg[plan->nsegs++];

		if (vsize == 0)
			vsize = rawsize;
		s->vaddr = image_base + rva;
		s->memsz = vsize;
		s->fileoff = rawptr;
		s->filesz = MOS_MIN(rawsize, vsize);
		s->prot = ((chars & PE_SCN_READ) ? EXEC_PROT_R : 0) |
			  ((chars & PE_SCN_WRITE) ? EXEC_PROT_W : 0) |
			  ((chars & PE_SCN_EXECUTE) ? EXEC_PROT_X : 0);
		if (!mos_in_range(len, s->fileoff, s->filesz))
			return -MERR_NOEXEC;
	}
	plan->entry = image_base + rd32le(opt + 16);
	return 0;
}

/*
 * Locate the data directory `idx`. Returns false when it is absent or empty.
 * The image must already have passed pe_probe().
 */
static bool pe_data_dir(const u8 *p, size_t len, u32 idx, u32 *rva, u32 *size)
{
	u32 coff = pe_coff_offset(p, len);
	u16 opt_size;
	const u8 *opt;

	if (!coff)
		return false;
	opt_size = rd16le(p + coff + 16);
	opt = p + coff + PE_COFF_SIZE;
	if (opt_size < PE32PLUS_OPT_MIN + (idx + 1) * 8 || rd16le(opt) != PE_MAGIC_PE32PLUS ||
	    !mos_in_range(len, (u64)coff + PE_COFF_SIZE, opt_size) || rd32le(opt + 108) <= idx)
		return false;
	*rva = rd32le(opt + PE32PLUS_OPT_MIN + idx * 8);
	*size = rd32le(opt + PE32PLUS_OPT_MIN + idx * 8 + 4);
	return *size != 0;
}

/*
 * Translate [rva, rva + span) to a file offset. The whole span must be
 * backed by raw data of one section (or lie inside the headers).
 * Returns false otherwise.
 */
static bool pe_rva_to_off(const u8 *p, size_t len, u32 rva, u32 span, u64 *off)
{
	u32 coff = pe_coff_offset(p, len);
	u16 nsect, opt_size;
	u64 sect;

	if (!coff)
		return false;
	nsect = rd16le(p + coff + 2);
	opt_size = rd16le(p + coff + 16);
	sect = (u64)coff + PE_COFF_SIZE + opt_size;
	if (!mos_in_range(len, sect, (u64)nsect * PE_SECTION_SIZE))
		return false;
	for (u16 i = 0; i < nsect; i++) {
		const u8 *sh = p + sect + (u64)i * PE_SECTION_SIZE;
		u32 va = rd32le(sh + 12), rawsize = rd32le(sh + 16), rawptr = rd32le(sh + 20);
		u32 vsize = rd32le(sh + 8);
		u64 limit = vsize ? MOS_MIN(vsize, rawsize) : rawsize;

		if (rva >= va && (u64)rva - va + span <= limit) {
			*off = (u64)rawptr + (rva - va);
			return mos_in_range(len, *off, span);
		}
	}
	if (opt_size >= PE32PLUS_OPT_MIN && (u64)rva + span <= rd32le(p + coff + PE_COFF_SIZE + 60)) {
		*off = rva;
		return mos_in_range(len, rva, span);
	}
	return false;
}

/* A NUL-terminated string at `rva`, wholly inside one section's raw data. */
static const char *pe_str(const u8 *p, size_t len, u32 rva)
{
	u64 off;
	u32 span = 1;

	if (!pe_rva_to_off(p, len, rva, 1, &off))
		return NULL;
	/* Grow the span until it reaches the terminator or leaves the section. */
	while (p[off + span - 1] != '\0') {
		if (span == 4096 || !pe_rva_to_off(p, len, rva, span + 1, &off))
			return NULL;
		span++;
	}
	return (const char *)p + off;
}

int binfmt_pe_imports(const void *image, size_t len, binfmt_pe_import_fn fn, void *arg)
{
	const u8 *p = image;
	struct binfmt_info info;
	u32 dir_rva, dir_size;

	if (binfmt_probe(p, len, &info) || info.format != BINFMT_PE32PLUS)
		return -MERR_NOEXEC;
	if (!pe_data_dir(p, len, PE_DIR_IMPORT, &dir_rva, &dir_size))
		return 0;                       /* no imports at all */

	for (u32 d = 0;; d++) {
		u64 doff, toff;
		u32 ilt, name_rva, iat;
		const char *dll;

		if (d == PE_MAX_IMPORT_DLLS || !pe_rva_to_off(p, len, dir_rva + d * 20, 20, &doff))
			return -MERR_NOEXEC;
		ilt = rd32le(p + doff);
		name_rva = rd32le(p + doff + 12);
		iat = rd32le(p + doff + 16);
		if (name_rva == 0 && iat == 0)
			return 0;               /* the all-zero terminator */
		dll = pe_str(p, len, name_rva);
		if (!dll || iat == 0)
			return -MERR_NOEXEC;
		if (ilt == 0)
			ilt = iat;              /* old linkers: the IAT doubles as lookup table */

		for (u32 i = 0;; i++) {
			u64 entry;
			int rc;

			if (i == PE_MAX_THUNKS || !pe_rva_to_off(p, len, ilt + i * 8, 8, &toff))
				return -MERR_NOEXEC;
			entry = rd64le(p + toff);
			if (entry == 0)
				break;
			if (entry >> 63) {
				rc = fn(dll, NULL, (u16)entry, iat + i * 8, arg);
			} else {
				/* Hint/name entry: a 16-bit hint, then the name. */
				const char *name = entry > 0xFFFFFFF0ull ? NULL
						 : pe_str(p, len, (u32)entry + 2);

				if (!name)
					return -MERR_NOEXEC;
				rc = fn(dll, name, 0, iat + i * 8, arg);
			}
			if (rc)
				return rc;
		}
	}
}

int binfmt_pe_relocs(const void *image, size_t len, binfmt_pe_reloc_fn fn, void *arg)
{
	const u8 *p = image;
	struct binfmt_info info;
	u32 dir_rva, dir_size, pos = 0;
	u64 off;

	if (binfmt_probe(p, len, &info) || info.format != BINFMT_PE32PLUS)
		return -MERR_NOEXEC;
	if (!pe_data_dir(p, len, PE_DIR_BASERELOC, &dir_rva, &dir_size))
		return 0;
	if (!pe_rva_to_off(p, len, dir_rva, dir_size, &off))
		return -MERR_NOEXEC;

	while (dir_size - pos >= 8) {
		u32 page = rd32le(p + off + pos), block = rd32le(p + off + pos + 4);

		if (block < 8 || block > dir_size - pos || (block & 1))
			return -MERR_NOEXEC;
		for (u32 e = 8; e + 2 <= block; e += 2) {
			u16 ent = rd16le(p + off + pos + e);
			int rc;

			if ((ent >> 12) == PE_REL_ABSOLUTE)
				continue;       /* padding */
			if ((ent >> 12) != PE_REL_DIR64)
				return -MERR_NOTSUP;
			rc = fn(page + (ent & 0xFFF), arg);
			if (rc)
				return rc;
		}
		pos += block;
	}
	return 0;
}

/* ======================================================================== */
/* Mach-O                                                                    */
/* ======================================================================== */

#define MH_MAGIC_32          0xFEEDFACEu
#define MH_MAGIC_64          0xFEEDFACFu
#define FAT_MAGIC_BE         0xCAFEBABEu
#define MACHO_CPU_X86_64     0x01000007u
#define MACHO_CPU_I386       0x00000007u
#define MACHO_CPU_ARM64      0x0100000Cu
#define MH_EXECUTE           2
#define MH_DYLIB             6
#define MH_DYLINKER          7
#define LC_UNIXTHREAD        0x5
#define LC_LOAD_DYLINKER     0xE
#define LC_SEGMENT_64        0x19
#define LC_MAIN              0x80000028u
#define X86_THREAD_STATE64   4
#define MACHO_HDR64_SIZE     32
#define MACHO_SEG64_SIZE     72
#define FAT_MAX_ARCHS        20         /* above this 0xCAFEBABE is a Java class file */

bool binfmt_macho_slice(const void *image, size_t len, const u8 **slice, size_t *slice_len)
{
	const u8 *p = image;

	if (len < MACHO_HDR64_SIZE)
		return false;
	if (rd32le(p) == MH_MAGIC_64 && rd32le(p + 4) == MACHO_CPU_X86_64) {
		*slice = p;
		*slice_len = len;
		return true;
	}
	if (rd32be(p) == FAT_MAGIC_BE) {
		u32 narch = rd32be(p + 4);

		if (narch > FAT_MAX_ARCHS || !mos_in_range(len, 8, (u64)narch * 20))
			return false;
		for (u32 i = 0; i < narch; i++) {
			const u8 *a = p + 8 + i * 20;
			u32 off = rd32be(a + 8), size = rd32be(a + 12);

			if (rd32be(a) == MACHO_CPU_X86_64 && size >= MACHO_HDR64_SIZE &&
			    mos_in_range(len, off, size) && rd32le(p + off) == MH_MAGIC_64) {
				*slice = p + off;
				*slice_len = size;
				return true;
			}
		}
	}
	return false;
}

/* Invoke fn for every load command of a thin 64-bit Mach-O. */
typedef int (*macho_lc_fn)(u32 cmd, const u8 *lc, u32 cmdsize, void *arg);

static int macho_for_each_lc(const u8 *p, size_t len, macho_lc_fn fn, void *arg)
{
	u32 ncmds = rd32le(p + 16), sizeofcmds = rd32le(p + 20);
	u64 off = MACHO_HDR64_SIZE;

	if (!mos_in_range(len, off, sizeofcmds))
		return -MERR_NOEXEC;
	for (u32 i = 0; i < ncmds; i++) {
		u32 cmd, cmdsize;
		int rc;

		if (!mos_in_range((u64)MACHO_HDR64_SIZE + sizeofcmds, off, 8))
			return -MERR_NOEXEC;
		cmd = rd32le(p + off);
		cmdsize = rd32le(p + off + 4);
		if (cmdsize < 8 || !mos_in_range((u64)MACHO_HDR64_SIZE + sizeofcmds, off, cmdsize))
			return -MERR_NOEXEC;
		rc = fn(cmd, p + off, cmdsize, arg);
		if (rc)
			return rc;
		off += cmdsize;
	}
	return 0;
}

static int macho_probe_lc(u32 cmd, const u8 *lc, u32 cmdsize, void *arg)
{
	u32 *flags = arg;

	(void)lc;
	(void)cmdsize;
	if (cmd == LC_LOAD_DYLINKER || cmd == LC_MAIN)
		*flags |= BINFMT_F_DYNAMIC;     /* both imply dyld */
	return 0;
}

static int macho_probe(const u8 *p, size_t len, struct binfmt_info *out)
{
	const u8 *slice;
	size_t slice_len;
	u32 magic_le = rd32le(p);

	if (rd32be(p) == FAT_MAGIC_BE) {
		if (len < 8 || rd32be(p + 4) > FAT_MAX_ARCHS)
			return -MERR_NOEXEC;    /* Java class file, not a universal binary */
		out->format = BINFMT_MACHO_FAT;
	} else if (magic_le == MH_MAGIC_64) {
		out->format = BINFMT_MACHO64;
	} else {
		out->format = BINFMT_MACHO32;
		out->arch = (len >= 8 && rd32le(p + 4) == MACHO_CPU_I386) ? BIN_ARCH_I386
									    : BIN_ARCH_UNKNOWN;
		out->personality = ABI_XNU;
		return 0;
	}
	out->personality = ABI_XNU;

	if (!binfmt_macho_slice(p, len, &slice, &slice_len)) {
		/* Recognised container, but no x86-64 code inside (e.g. arm64-only). */
		if (out->format == BINFMT_MACHO64 && len >= 8)
			out->arch = rd32le(p + 4) == MACHO_CPU_ARM64 ? BIN_ARCH_AARCH64
								     : BIN_ARCH_UNKNOWN;
		return 0;
	}
	out->arch = BIN_ARCH_X86_64;
	if (rd32le(slice + 12) == MH_DYLIB)
		out->flags |= BINFMT_F_LIBRARY;
	(void)macho_for_each_lc(slice, slice_len, macho_probe_lc, &out->flags);
	return 0;
}

struct macho_plan_ctx {
	struct exec_plan *plan;
	u64 slice_off, slice_len;
	u64 text_va, main_off;
	const u8 *slice_base;
	bool have_entry, have_text, have_main;
};

static int macho_plan_lc(u32 cmd, const u8 *lc, u32 cmdsize, void *arg)
{
	struct macho_plan_ctx *ctx = arg;
	struct exec_plan *plan = ctx->plan;

	if (cmd == LC_SEGMENT_64) {
		struct exec_seg *s;
		u32 initprot;

		if (cmdsize < MACHO_SEG64_SIZE)
			return -MERR_NOEXEC;
		initprot = rd32le(lc + 60);
		if (initprot == 0)
			return 0;               /* __PAGEZERO: a reservation, nothing to map */
		if (plan->nsegs == EXEC_MAX_SEGS)
			return -MERR_RANGE;
		s = &plan->seg[plan->nsegs++];
		s->vaddr = rd64le(lc + 24);
		s->memsz = rd64le(lc + 32);
		s->fileoff = rd64le(lc + 40);
		s->filesz = rd64le(lc + 48);
		s->prot = initprot & (EXEC_PROT_R | EXEC_PROT_W | EXEC_PROT_X);
		if (s->filesz > s->memsz || !mos_in_range(ctx->slice_len, s->fileoff, s->filesz))
			return -MERR_NOEXEC;
		if (s->fileoff == 0 && s->filesz && !ctx->have_text) {
			ctx->text_va = s->vaddr;        /* __TEXT: where the mach_header lives */
			ctx->have_text = true;
		}
		s->fileoff += ctx->slice_off;   /* make it relative to the whole image */
	} else if (cmd == LC_LOAD_DYLINKER) {
		u32 no = cmdsize >= 12 ? rd32le(lc + 8) : 0, n = 0;

		if (no < 12 || no >= cmdsize)
			return -MERR_NOEXEC;
		while (no + n < cmdsize && lc[no + n])
			n++;
		if (no + n >= cmdsize || n == 0)
			return -MERR_NOEXEC;
		plan->interp_off = (u64)(lc - ctx->slice_base) + no + ctx->slice_off;
		plan->interp_len = n;
	} else if (cmd == LC_MAIN) {
		if (cmdsize < 24)
			return -MERR_NOEXEC;
		ctx->main_off = rd64le(lc + 8);
		ctx->have_main = true;
	} else if (cmd == LC_UNIXTHREAD) {
		/* cmd, cmdsize, flavor, count, then x86_thread_state64 (rip is word 16). */
		if (cmdsize < 16 + 21 * 8 || rd32le(lc + 8) != X86_THREAD_STATE64)
			return -MERR_NOEXEC;
		plan->entry = rd64le(lc + 16 + 16 * 8);
		ctx->have_entry = true;
	}
	return 0;
}

static int macho_plan(const u8 *p, size_t len, struct exec_plan *plan)
{
	struct macho_plan_ctx ctx = { .plan = plan };
	const u8 *slice;
	size_t slice_len;
	int rc;

	if (plan->info.arch != BIN_ARCH_X86_64 || !binfmt_macho_slice(p, len, &slice, &slice_len))
		return -MERR_NOTSUP;
	if (plan->info.flags & BINFMT_F_LIBRARY)
		return -MERR_NOTSUP;
	if (rd32le(slice + 12) != MH_EXECUTE && rd32le(slice + 12) != MH_DYLINKER)
		return -MERR_NOEXEC;

	ctx.slice_off = (u64)(slice - p);
	ctx.slice_len = slice_len;
	ctx.slice_base = slice;
	rc = macho_for_each_lc(slice, slice_len, macho_plan_lc, &ctx);
	if (rc)
		return rc;
	if (ctx.have_main && ctx.have_text) {
		plan->entry = ctx.text_va + ctx.main_off;
		ctx.have_entry = true;
	}
	plan->image_base = ctx.text_va;
	plan->lc_main = ctx.have_main;
	if (!ctx.have_entry || !plan->nsegs)
		return -MERR_NOEXEC;
	return 0;
}

#define LC_LOAD_DYLIB       0xC
#define LC_LOAD_WEAK_DYLIB  0x80000018u

struct macho_dylib_ctx {
	binfmt_dylib_fn fn;
	void *arg;
};

static int macho_dylib_lc(u32 cmd, const u8 *lc, u32 cmdsize, void *arg)
{
	struct macho_dylib_ctx *ctx = arg;
	u32 name_off;

	if (cmd != LC_LOAD_DYLIB && cmd != LC_LOAD_WEAK_DYLIB)
		return 0;
	/* dylib_command: cmd, cmdsize, name offset, timestamp, versions; then the name. */
	if (cmdsize < 24)
		return -MERR_NOEXEC;
	name_off = rd32le(lc + 8);
	if (name_off >= cmdsize || !memchr(lc + name_off, '\0', cmdsize - name_off))
		return -MERR_NOEXEC;
	ctx->fn((const char *)lc + name_off, ctx->arg);
	return 0;
}

int binfmt_macho_dylibs(const void *image, size_t len, binfmt_dylib_fn fn, void *arg)
{
	struct macho_dylib_ctx ctx = { fn, arg };
	const u8 *slice;
	size_t slice_len;

	if (!binfmt_macho_slice(image, len, &slice, &slice_len))
		return -MERR_NOEXEC;
	return macho_for_each_lc(slice, slice_len, macho_dylib_lc, &ctx);
}

/* ---- imported symbols (what dyld will have to bind) ------------------------ */

#define LC_REEXPORT_DYLIB_  0x8000001Fu
#define LC_LOAD_UPWARD_     0x80000023u
#define LC_DYLD_INFO_       0x22
#define LC_DYLD_INFO_ONLY_  0x80000022u
#define LC_DYLD_CHAINED_    0x80000034u
#define MACHO_MAX_LIBS      32

struct macho_imp_ctx {
	const u8 *s;
	size_t len;
	const char *lib[MACHO_MAX_LIBS];
	u32 nlib;
	u32 bind_off[2], bind_size[2];
	u32 chain_off, chain_size;
	binfmt_import_fn fn;
	void *arg;
};

static int macho_imp_lc(u32 cmd, const u8 *lc, u32 cmdsize, void *arg)
{
	struct macho_imp_ctx *c = arg;

	if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB || cmd == LC_REEXPORT_DYLIB_ ||
	    cmd == LC_LOAD_UPWARD_) {
		u32 no;

		if (cmdsize < 24)
			return -MERR_NOEXEC;
		no = rd32le(lc + 8);
		if (no >= cmdsize || !memchr(lc + no, '\0', cmdsize - no))
			return -MERR_NOEXEC;
		if (c->nlib < MACHO_MAX_LIBS)
			c->lib[c->nlib++] = (const char *)lc + no;
	} else if (cmd == LC_DYLD_INFO_ || cmd == LC_DYLD_INFO_ONLY_) {
		if (cmdsize < 40)
			return -MERR_NOEXEC;
		c->bind_off[0] = rd32le(lc + 16);
		c->bind_size[0] = rd32le(lc + 20);
		c->bind_off[1] = rd32le(lc + 32);       /* lazy */
		c->bind_size[1] = rd32le(lc + 36);
	} else if (cmd == LC_DYLD_CHAINED_) {
		if (cmdsize < 16)
			return -MERR_NOEXEC;
		c->chain_off = rd32le(lc + 8);
		c->chain_size = rd32le(lc + 12);
	}
	return 0;
}

static bool macho_uleb(const u8 **pp, const u8 *end)
{
	while (*pp < end)
		if (!(*(*pp)++ & 0x80))
			return true;
	return false;
}

static const char *macho_lib(const struct macho_imp_ctx *c, int ord)
{
	return ord > 0 && (u32)ord <= c->nlib ? c->lib[ord - 1] : NULL;
}

/* Classic bind / lazy-bind opcode stream: report each DO_BIND's library and symbol. */
static int macho_bind_stream(struct macho_imp_ctx *c, u32 off, u32 size)
{
	const u8 *p, *end;
	const char *sym = NULL;
	bool weak = false;
	int ord = 0;

	if (!size)
		return 0;
	if (!mos_in_range(c->len, off, size))
		return -MERR_NOEXEC;
	p = c->s + off;
	end = p + size;
	while (p < end) {
		u8 b = *p++, op = b & 0xF0, imm = b & 0x0F;
		bool bind = false;

		switch (op) {
		case 0x00: case 0x50: break;
		case 0x10: ord = imm; break;
		case 0x20: {
			const u8 *q = p;
			u64 v = 0;
			int sh = 0;

			do {
				if (q >= end)
					return -MERR_NOEXEC;
				v |= (u64)(*q & 0x7F) << (sh < 63 ? sh : 63);
				sh += 7;
			} while (*q++ & 0x80);
			p = q;
			ord = v > 0xFFFF ? 0 : (int)v;
			break;
		}
		case 0x30: ord = imm ? -1 : 0; break;   /* special: not a library */
		case 0x40: {
			const u8 *nul = memchr(p, '\0', (size_t)(end - p));

			if (!nul)
				return -MERR_NOEXEC;
			sym = (const char *)p;
			weak = imm & 1;
			p = nul + 1;
			break;
		}
		case 0x60: case 0x70: case 0x80:
			if (!macho_uleb(&p, end))
				return -MERR_NOEXEC;
			break;
		case 0x90: case 0xB0: bind = true; break;
		case 0xA0:
			if (!macho_uleb(&p, end))
				return -MERR_NOEXEC;
			bind = true;
			break;
		case 0xC0:
			if (!macho_uleb(&p, end) || !macho_uleb(&p, end))
				return -MERR_NOEXEC;
			bind = true;
			break;
		default:
			return -MERR_NOTSUP;            /* threaded binds (arm64e) */
		}
		if (bind && sym)
			c->fn(macho_lib(c, ord), sym, weak, c->arg);
	}
	return 0;
}

/* LC_DYLD_CHAINED_FIXUPS: the imports table lists every symbol directly. */
static int macho_chained_imports(struct macho_imp_ctx *c)
{
	const u8 *h;
	u32 imp_off, sym_off, count, fmt, esz;

	if (!c->chain_size)
		return 0;
	if (c->chain_size < 28 || !mos_in_range(c->len, c->chain_off, c->chain_size))
		return -MERR_NOEXEC;
	h = c->s + c->chain_off;
	imp_off = rd32le(h + 8);
	sym_off = rd32le(h + 12);
	count = rd32le(h + 16);
	fmt = rd32le(h + 20);
	esz = fmt == 1 ? 4 : fmt == 2 ? 8 : fmt == 3 ? 16 : 0;
	if (!esz)
		return -MERR_NOTSUP;
	if (sym_off > c->chain_size || !mos_in_range(c->chain_size, imp_off, (u64)count * esz))
		return -MERR_NOEXEC;
	for (u32 i = 0; i < count; i++) {
		u32 name, ord;
		bool weak;
		const u8 *e = h + imp_off + (u64)i * esz;

		if (fmt == 3) {
			u64 v = rd64le(e);

			ord = (u32)(v & 0xFFFF);
			ord = ord >= 0xFFF0 ? 0 : ord;
			weak = (v >> 16) & 1;
			name = (u32)(v >> 32);
		} else {
			u32 v = rd32le(e);

			ord = v & 0xFF;
			ord = ord >= 0xF0 ? 0 : ord;
			weak = (v >> 8) & 1;
			name = v >> 9;
		}
		if (name >= c->chain_size - sym_off ||
		    !memchr(h + sym_off + name, '\0', c->chain_size - sym_off - name))
			return -MERR_NOEXEC;
		c->fn(macho_lib(c, (int)ord), (const char *)h + sym_off + name, weak, c->arg);
	}
	return 0;
}

int binfmt_macho_imports(const void *image, size_t len, binfmt_import_fn fn, void *arg)
{
	struct macho_imp_ctx c = { .fn = fn, .arg = arg };
	size_t slice_len;
	int rc;

	if (!binfmt_macho_slice(image, len, &c.s, &slice_len))
		return -MERR_NOEXEC;
	c.len = slice_len;
	rc = macho_for_each_lc(c.s, slice_len, macho_imp_lc, &c);
	if (!rc)
		rc = macho_bind_stream(&c, c.bind_off[0], c.bind_size[0]);
	if (!rc)
		rc = macho_bind_stream(&c, c.bind_off[1], c.bind_size[1]);
	if (!rc)
		rc = macho_chained_imports(&c);
	return rc;
}

/* ======================================================================== */
/* Front end                                                                 */
/* ======================================================================== */

int binfmt_probe(const void *image, size_t len, struct binfmt_info *out)
{
	const u8 *p = image;

	memset(out, 0, sizeof(*out));
	if (len < 4)
		return -MERR_NOEXEC;

	if (!memcmp(p, "\177ELF", 4))
		return elf_probe(p, len, out);
	if (p[0] == 'M' && p[1] == 'Z')
		return pe_probe(p, len, out);
	if (rd32le(p) == MH_MAGIC_64 || rd32le(p) == MH_MAGIC_32 || rd32be(p) == FAT_MAGIC_BE)
		return macho_probe(p, len, out);
	if (p[0] == '#' && p[1] == '!') {
		out->format = BINFMT_SCRIPT;
		return 0;
	}
	return -MERR_NOEXEC;
}

int binfmt_load_plan(const void *image, size_t len, struct exec_plan *out)
{
	int rc;

	memset(out, 0, sizeof(*out));
	rc = binfmt_probe(image, len, &out->info);
	if (rc)
		return rc;

	switch (out->info.format) {
	case BINFMT_ELF32:
	case BINFMT_ELF64:
		rc = elf_plan(image, len, out);
		break;
	case BINFMT_PE32:
	case BINFMT_PE32PLUS:
		rc = pe_plan(image, len, out);
		break;
	case BINFMT_MACHO32:
	case BINFMT_MACHO64:
	case BINFMT_MACHO_FAT:
		rc = macho_plan(image, len, out);
		break;
	default:
		rc = -MERR_NOTSUP;
		break;
	}
	if (rc)
		out->nsegs = 0;
	return rc;
}

const char *binfmt_format_name(enum bin_format f)
{
	switch (f) {
	case BINFMT_ELF32:     return "ELF32";
	case BINFMT_ELF64:     return "ELF64";
	case BINFMT_PE32:      return "PE32";
	case BINFMT_PE32PLUS:  return "PE32+";
	case BINFMT_MACHO32:   return "Mach-O 32";
	case BINFMT_MACHO64:   return "Mach-O 64";
	case BINFMT_MACHO_FAT: return "Mach-O universal";
	case BINFMT_SCRIPT:    return "script";
	default:               return "unknown";
	}
}

const char *binfmt_arch_name(enum bin_arch a)
{
	switch (a) {
	case BIN_ARCH_X86_64:  return "x86_64";
	case BIN_ARCH_I386:    return "i386";
	case BIN_ARCH_AARCH64: return "aarch64";
	default:               return "unknown";
	}
}
