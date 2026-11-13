/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * NT loader: what ntdll's LdrInitializeThunk and the kernel's image mapper
 * do together on Windows, reduced to one static image plus one built-in
 * runtime.
 *
 *   1. place  - keep the preferred base, unless the image asks for ASLR or
 *               collides with the runtime area; then rebase it
 *   2. relocate - walk .reloc and adjust every DIR64 word
 *   3. map the built-in Win32 runtime (src/ntrt, embedded flat binary)
 *   4. bind  - fill each IAT slot with the runtime's implementation, or with
 *              a stub that reports "missing import DLL!Name" and exits with
 *              STATUS_ENTRYPOINT_NOT_FOUND when called
 *   5. hand the runtime its parameters page and a KUSER_SHARED_DATA page
 *
 * No DLL files are read from disk: every supported DLL is the runtime.
 */
#include <mergedos/kernel.h>
#include <mergedos/compat.h>
#include <mergedos/cpu.h>
#include <mergedos/mm.h>
#include <mergedos/task.h>
#include "src/ntrt/ntrt_abi.h"

extern const u8 ntrt_blob_start[], ntrt_blob_end[];

/* Lowest address the runtime's fixed areas use, and their end. */
#define NT_FIXED_LO  NTRT_BASE
#define NT_FIXED_HI  (NTRT_KUSER_VA + 2 * PAGE_SIZE)    /* KUSER + exit thunk */

static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }     /* x86: little-endian */
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }

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

/* DLL names the runtime answers for, without the ".dll" suffix. */
static bool runtime_dll(const char *dll)
{
	static const char *const names[] = {
		"kernel32", "kernelbase", "ntdll", "msvcrt", "ucrtbase", "advapi32", "secur32",
	};
	char base[24];
	size_t n = 0;

	while (dll[n] && dll[n] != '.' && n < sizeof(base) - 1) {
		base[n] = (char)mos_tolower((u8)dll[n]);
		n++;
	}
	base[n] = '\0';
	if (dll[n] && mos_strcasecmp(dll + n, ".dll"))
		return false;
	if (!strncmp(base, "api-ms-win-", 11))
		return true;            /* API sets: thin forwarders on Windows too */
	for (size_t i = 0; i < ARRAY_SIZE(names); i++)
		if (!strcmp(base, names[i]))
			return true;
	return false;
}

struct bind_ctx {
	struct task *t;
	const struct exec_plan *plan;
	const struct ntrt_header *hdr;
	u32 bound, missing;
	u64 stub_next;
};

/* Find `name` in the runtime's export table (read from the kernel's copy). */
static u64 runtime_lookup(const struct ntrt_header *hdr, const char *name)
{
	const u64 blob_len = (u64)(ntrt_blob_end - ntrt_blob_start);
	const struct ntrt_export *e = (const void *)(ntrt_blob_start + (hdr->exports - NTRT_BASE));
	const struct ntrt_export *end = (const void *)(ntrt_blob_start + (hdr->exports_end - NTRT_BASE));

	for (; e < end; e++) {
		u64 off = e->name - NTRT_BASE;

		if (off < blob_len && !strncmp((const char *)ntrt_blob_start + off, name,
					       blob_len - off))
			return e->addr;
	}
	return 0;
}

/*
 * A stub for an unresolved import:
 *   lea  rcx, [rip + name]     ; 48 8D 0D <rel32>
 *   jmp  [rip + 0]             ; FF 25 00 00 00 00
 *   dq   runtime missing()
 *   db   "DLL!Name", 0
 */
static int make_stub(struct bind_ctx *c, const char *dll, const char *name, u16 ord, u64 *addr)
{
	u8 code[21 + 96];
	char *text = (char *)code + 21;
	size_t n;

	if (c->stub_next == 0) {
		int rc = task_map_range(c->t, NTRT_STUB_VA, NTRT_STUB_PAGES * PAGE_SIZE,
					VMM_R | VMM_X);

		if (rc)
			return rc;
		c->stub_next = NTRT_STUB_VA;
	}
	n = mos_strlcpy(text, dll, 48);
	text[MOS_MIN(n, 47)] = '!';
	if (name) {
		mos_strlcpy(text + MOS_MIN(n, 47) + 1, name, 96 - 48);
	} else {
		char *p = text + MOS_MIN(n, 47) + 1;

		*p++ = '#';
		for (u32 div = 10000; div; div /= 10)
			if (ord >= div || div == 1)
				*p++ = (char)('0' + ord / div % 10);
		*p = '\0';
	}
	n = 21 + strlen(text) + 1;
	if (c->stub_next + n > NTRT_STUB_VA + NTRT_STUB_PAGES * PAGE_SIZE)
		return -MERR_RANGE;
	code[0] = 0x48; code[1] = 0x8D; code[2] = 0x0D;
	put32(code + 3, 14);                   /* from the end of lea to the text */
	code[7] = 0xFF; code[8] = 0x25;
	put32(code + 9, 0);
	put64(code + 13, c->hdr->missing);
	*addr = c->stub_next;
	c->stub_next = ALIGN_UP(c->stub_next + n, 16);
	return as_write(c->t, *addr, code, n);
}

static int bind_import(const char *dll, const char *name, u16 ord, u32 iat_rva, void *arg)
{
	struct bind_ctx *c = arg;
	u64 addr = 0;
	int rc;

	if ((u64)iat_rva + 8 > c->plan->image_size)
		return -MERR_NOEXEC;
	if (name && runtime_dll(dll))
		addr = runtime_lookup(c->hdr, name);
	if (addr) {
		c->bound++;
	} else {
		rc = make_stub(c, dll, name, ord, &addr);
		if (rc)
			return rc;
		c->missing++;
	}
	return as_write(c->t, c->plan->image_base + iat_rva, &addr, 8);
}

int ntload_link(struct task *t, const u8 *image, size_t len, const struct exec_plan *plan,
		u64 delta, struct user_start *start)
{
	const struct ntrt_header *hdr = (const void *)ntrt_blob_start;
	const u64 blob_len = (u64)(ntrt_blob_end - ntrt_blob_start);
	const struct abi_identity *id = abi_identity(ABI_NT);
	struct bind_ctx bc = { .t = t, .plan = plan, .hdr = hdr };
	struct ntrt_params params = { 0 };
	u8 kuser[0x280] = { 0 };
	int rc;

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

	/* 4. Imports. */
	rc = binfmt_pe_imports(image, len, bind_import, &bc);
	if (rc)
		return rc;
	kprintf("[nt] %s: %u imports bound to the built-in runtime, %u missing%s\n", t->name,
		bc.bound, bc.missing, delta ? ", image rebased" : "");

	/* 5. Parameters and the version fields of KUSER_SHARED_DATA. */
	params.image_base = plan->image_base;
	params.entry = plan->entry;
	params.stack_base = USER_STACK_TOP;
	params.stack_limit = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
	params.pid = (u32)t->pid * 4;           /* NT ids are multiples of four */
	params.tid = (u32)t->pid * 4 + 4;
	mos_strlcpy(params.computer_name, id->nodename, sizeof(params.computer_name));
	mos_strlcpy(params.command_line, t->name, sizeof(params.command_line));
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
