/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * NT loader, kernel half: what the kernel's image mapper does on Windows,
 * for the program image plus MergedOS's ntdll (the built-in runtime).
 *
 *   1. place  - keep the preferred base, unless the image asks for ASLR or
 *               collides with the runtime area; then rebase it
 *   2. relocate - walk .reloc and adjust every DIR64 word
 *   3. map the built-in Win32 runtime (src/ntrt, embedded flat binary)
 *   4. hand the runtime (MergedOS's ntdll) its parameters page and a
 *      KUSER_SHARED_DATA page; the runtime's own loader (src/ntrt/ldr.c)
 *      binds the imports, loading DLL files from disk where they exist
 *
 * Everything else a Windows loader does happens in user space.
 */
#include <mergedos/kernel.h>
#include <mergedos/compat.h>
#include <mergedos/cpu.h>
#include <mergedos/initrd.h>
#include <mergedos/mm.h>
#include <mergedos/task.h>
#include "src/ntrt/ntrt_abi.h"


/* Lowest address the runtime's fixed areas use, and their end. */
#define NT_FIXED_LO  NTRT_BASE
#define NT_FIXED_HI  (NTRT_KUSER_VA + 2 * PAGE_SIZE)    /* KUSER + exit thunk */

static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }     /* x86: little-endian */

static bool overlaps(u64 a, u64 alen, u64 b, u64 blen)
{
	return a < b + blen && b < a + alen;
}

int ntload_place(struct exec_plan *plan, u64 *delta)
{
	u64 base = plan->image_base, size = ALIGN_UP(plan->image_size, PAGE_SIZE);
	bool clash = base < USER_MIN || size == 0 || base + size < base ||
		     base + size > TASK_MMAP_BASE ||
		     overlaps(base, size, NT_FIXED_LO, NT_FIXED_HI - NT_FIXED_LO);
	u64 d;

	*delta = 0;
	for (u32 i = 0; i < plan->nsegs; i++)
		if (plan->seg[i].vaddr < base || plan->seg[i].vaddr + plan->seg[i].memsz > base + size)
			return -MERR_NOEXEC;    /* sections must lie inside SizeOfImage */
	if (!(plan->info.flags & BINFMT_F_RELOC)) {
		/* Fixed-address image: it either fits where it wants to be or not at all. */
		return clash ? -MERR_NOEXEC : 0;
	}
	if (!clash && (!(plan->info.flags & BINFMT_F_ASLR) || base == NTRT_REBASE_VA))
		return 0;
	if (size > TASK_MMAP_BASE - NTRT_REBASE_VA)
		return -MERR_NOEXEC;
	/* DYNAMIC_BASE images move, as on Windows: this also keeps .reloc tested. */
	d = NTRT_REBASE_VA - base;
	for (u32 i = 0; i < plan->nsegs; i++)
		plan->seg[i].vaddr += d;
	plan->entry += d;
	plan->image_base += d;
	*delta = d;
	return 0;
}

/* Copy between the kernel and t's address space through the direct map. */
static int as_copy(struct task *t, u64 va, void *buf, u64 len, bool to_user)
{
	u8 *b = buf;

	while (len) {
		u64 phys = vmm_user_to_phys(t->pml4, va);
		u64 chunk = MOS_MIN(len, PAGE_SIZE - (va & (PAGE_SIZE - 1)));

		if (!phys)
			return -MERR_FAULT;
		if (to_user)
			memcpy(phys_to_virt(phys), b, chunk);
		else
			memcpy(b, phys_to_virt(phys), chunk);
		va += chunk;
		b += chunk;
		len -= chunk;
	}
	return 0;
}

static int as_write(struct task *t, u64 va, const void *buf, u64 len)
{
	return as_copy(t, va, (void *)buf, len, true);
}

struct reloc_ctx {
	struct task *t;
	const struct exec_plan *plan;
	u64 delta;
};

static int apply_reloc(u32 rva, void *arg)
{
	struct reloc_ctx *c = arg;
	u64 va = c->plan->image_base + rva, v;

	if ((u64)rva + 8 > c->plan->image_size || as_copy(c->t, va, &v, 8, false))
		return -MERR_NOEXEC;
	v += c->delta;
	return as_write(c->t, va, &v, 8);
}

/* NT command line from argv: words joined by spaces, quoted when they hold one. */
static void nt_cmdline(char *out, size_t cap, const struct exec_args *args)
{
	const char *s = args->buf;
	size_t n = 0;

	for (u32 i = 0; i < args->argc && n + 4 < cap; i++) {
		bool q = !*s || strchr(s, ' ');

		if (i)
			out[n++] = ' ';
		if (q)
			out[n++] = '"';
		if (!i && *s != '/' && !strchr(s, ':')) {       /* the program: a DOS path on C: */
			out[n++] = 'C';
			out[n++] = ':';
			out[n++] = '\\';
		}
		for (; *s && n + 2 < cap; s++)
			out[n++] = (!i && *s == '/') ? '\\' : *s;
		if (q)
			out[n++] = '"';
		s += strlen(s) + 1;
	}
	out[MOS_MIN(n, cap - 1)] = '\0';
}

int ntload_link(struct task *t, const u8 *image, size_t len, const struct exec_plan *plan,
		u64 delta, const struct exec_args *args, struct user_start *start)
{
	const u8 *ntrt_blob_start;
	size_t blob_len;
	const struct ntrt_header *hdr;
	const struct abi_identity *id = abi_identity(ABI_NT);
	struct ntrt_params params = { 0 };
	u8 kuser[0x280] = { 0 };
	int rc;

	/* The runtime is a file of the boot module (user space, not kernel image). */
	if (initrd_find("sys/ntrt.bin", &ntrt_blob_start, &blob_len)) {
		kprintf("[nt] sys/ntrt.bin not found: no boot module with the NT runtime\n");
		return -MERR_NOENT;
	}
	hdr = (const void *)ntrt_blob_start;
	if (blob_len < sizeof(*hdr) || hdr->magic != NTRT_MAGIC || hdr->text_end <= NTRT_BASE ||
	    hdr->image_end > NTRT_LIMIT || hdr->text_end > hdr->image_end ||
	    NTRT_BASE + blob_len > hdr->image_end)
		panic("built-in Win32 runtime image is malformed");

	/* 2. Base relocations, while the image still holds its on-disk values. */
	if (delta) {
		struct reloc_ctx rcx = { t, plan, delta };

		rc = binfmt_pe_relocs(image, len, apply_reloc, &rcx);
		if (rc)
			return rc;
	}

	/* 3. The runtime: code and constants read-only, data and bss writable. */
	rc = task_map_range(t, NTRT_BASE, hdr->text_end - NTRT_BASE, VMM_R | VMM_X);
	if (!rc)
		rc = task_map_range(t, hdr->text_end, hdr->image_end - hdr->text_end, VMM_R | VMM_W);
	if (!rc)
		rc = as_write(t, NTRT_BASE, ntrt_blob_start, blob_len);
	if (rc)
		return rc;

	/* 4. Imports are bound by the runtime's own loader (src/ntrt/ldr.c). */
	/* 5. Parameters and the version fields of KUSER_SHARED_DATA. */
	params.image_base = plan->image_base;
	params.entry = plan->entry;
	params.stack_base = USER_STACK_TOP;
	params.stack_limit = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
	params.pid = (u32)t->pid * 4;           /* NT ids are multiples of four */
	params.tid = (u32)t->pid * 4 + 4;
	mos_strlcpy(params.computer_name, id->nodename, sizeof(params.computer_name));
	mos_strlcpy(params.image_path, t->name, sizeof(params.image_path));
	nt_cmdline(params.command_line, sizeof(params.command_line), args);
	params.image_size = plan->image_size;
	put32(kuser + KUSER_NT_BUILD_NUMBER, id->build);
	put32(kuser + KUSER_NT_PRODUCT_TYPE, 1);       /* NtProductWinNt */
	kuser[KUSER_PRODUCT_VALID] = 1;
	put32(kuser + KUSER_NT_MAJOR_VERSION, id->major);
	put32(kuser + KUSER_NT_MINOR_VERSION, id->minor);
	rc = task_map_range(t, NTRT_PARAMS_VA, PAGE_SIZE, VMM_R | VMM_W);
	if (!rc)
		rc = task_map_range(t, NTRT_KUSER_VA, PAGE_SIZE, VMM_R);
	if (!rc)
		rc = as_write(t, NTRT_PARAMS_VA, &params, sizeof(params));
	if (!rc)
		rc = as_write(t, NTRT_KUSER_VA, kuser, sizeof(kuser));
	if (rc)
		return rc;

	/* The runtime's start-up runs first; it calls the image's entry point. */
	start->rip = hdr->start;
	start->rdi = NTRT_PARAMS_VA;
	t->gs_base = hdr->teb;
	return 0;
}
