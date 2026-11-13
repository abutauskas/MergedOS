/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The user-space image loader of MergedOS's ntdll: loads PE32+ DLL files
 * from disk, relocates them, binds imports (by name and ordinal, through
 * forwarders and API sets), sets up static TLS and runs TLS callbacks and
 * DllMain in dependency order. Spec: docs/NT_USERSPACE.md, part 1.
 *
 * Written from the PE/COFF specification. The program image itself was
 * mapped (and relocated) by the kernel; every DLL is mapped here.
 */
#include "src/ntrt/nt.h"

#define DIR_EXPORT    0
#define DIR_IMPORT    1
#define DIR_RESOURCE  2
#define DIR_RELOC     5
#define DIR_TLS       9
#define DIR_DELAY     13
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define MAX_TLS       32
#define MAX_INIT      64

/* An LDR_DATA_TABLE_ENTRY (x64 layout) followed by private fields. */
struct ldr_mod {
	LIST_ENTRY load_links;          /* 0x00 */
	LIST_ENTRY mem_links;           /* 0x10 */
	LIST_ENTRY init_links;          /* 0x20 */
	u64 base;                       /* 0x30 */
	u64 entry;                      /* 0x38 */
	u32 size, pad0;                 /* 0x40 */
	UNICODE_STRING full;            /* 0x48 */
	UNICODE_STRING basename;        /* 0x58 */
	u32 flags;                      /* 0x68 */
	u16 load_count;                 /* 0x6C */
	u16 tls_index;                  /* 0x6E */
	LIST_ENTRY hash_links;          /* 0x70 */
	u32 timestamp;                  /* 0x80 */
	u8 reserved[0x120 - 0x84];
	/* private */
	struct ldr_mod *next;
	wchar fullbuf[160];
	wchar basebuf[64];
	char name[64];                  /* lower-case base name, with extension */
	bool builtin, bound, initialised, has_tls;
	u32 exp_rva, exp_size;
};
_Static_assert(__builtin_offsetof(struct ldr_mod, base) == 0x30, "LDR_DATA_TABLE_ENTRY");
_Static_assert(__builtin_offsetof(struct ldr_mod, basename) == 0x58, "LDR_DATA_TABLE_ENTRY");

/* PEB_LDR_DATA */
static struct {
	u32 length;
	u8 initialized, pad[3];
	void *ss_handle;
	LIST_ENTRY load, mem, init;     /* 0x10, 0x20, 0x30 */
	void *entry_in_progress;
	u8 shutdown;
	void *shutdown_tid;
} ldr_data;

static struct ldr_mod *mods, *exe_mod, *ntdll_mod;
static struct ldr_mod *init_queue[MAX_INIT];
static u32 n_init;
static struct ldr_mod *tls_mods[MAX_TLS];
static u32 n_tls;
static bool debug;
static volatile u32 ldr_lock;

/* ---- small helpers ------------------------------------------------------------- */

static void dbg(const char *a, const char *b, u64 v)
{
	char buf[160], *p = buf;
	static const char hex[] = "0123456789abcdef";

	if (!debug)
		return;
	for (const char *s = "[ldr] "; *s; s++)
		*p++ = *s;
	for (; *a && p < buf + 60; a++)
		*p++ = *a;
	*p++ = ' ';
	for (; b && *b && p < buf + 120; b++)
		*p++ = *b;
	*p++ = ' ';
	for (int i = 60; i >= 0; i -= 4)
		if (v >> i || !i)
			*p++ = hex[(v >> i) & 15];
	*p++ = '\n';
	rt_write(2, buf, (size_t)(p - buf));
}

static u8 *pe_nt(u64 base)
{
	u32 l;

	if (!base || *(u16 *)base != 0x5A4D)
		return NULL;
	l = *(u32 *)(base + 0x3C);
	if (l > 0x1000 || *(u32 *)(base + l) != 0x4550)
		return NULL;
	return (u8 *)base + l;
}

static u8 *pe_opt(u64 base)
{
	u8 *nt = pe_nt(base);

	return nt && *(u16 *)(nt + 24) == 0x20B ? nt + 24 : NULL;
}

/* Data directory i: RVA and size, or 0. */
static u32 pe_dir(u64 base, u32 i, u32 *size)
{
	u8 *opt = pe_opt(base);

	if (size)
		*size = 0;
	if (!opt || *(u32 *)(opt + 108) <= i)
		return 0;
	if (size)
		*size = *(u32 *)(opt + 112 + i * 8 + 4);
	return *(u32 *)(opt + 112 + i * 8);
}

static void lower_name(char *out, size_t cap, const char *in, bool add_dll)
{
	size_t n = 0;
	bool dot = false;

	for (; *in && n + 5 < cap; in++) {
		char c = *in;

		if (c == '/' || c == '\\') {
			n = 0;
			dot = false;
			continue;
		}
		if (c == '.')
			dot = true;
		out[n++] = (char)rt_lower((u8)c);
	}
	if (add_dll && !dot) {
		memcpy(out + n, ".dll", 4);
		n += 4;
	}
	out[n] = 0;
}

static bool starts(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return false;
	return true;
}

/* ---- module list --------------------------------------------------------------- */

static void list_add(LIST_ENTRY *head, LIST_ENTRY *e)
{
	e->next = head;
	e->prev = head->prev;
	head->prev->next = e;
	head->prev = e;
}

static struct ldr_mod *mod_new(const char *name8, const wchar *full, u64 base, u32 size)
{
	struct ldr_mod *m = nt_alloc(sizeof(*m));
	size_t n = 0;

	if (!m)
		return NULL;
	lower_name(m->name, sizeof(m->name), name8, false);
	for (; full[n] && n + 1 < 160; n++)
		m->fullbuf[n] = full[n];
	{
		size_t b = n;

		while (b && m->fullbuf[b - 1] != '\\')
			b--;
		for (size_t i = 0; b + i < n && i + 1 < 64; i++)
			m->basebuf[i] = m->fullbuf[b + i];
	}
	nt_init_ustr(&m->full, m->fullbuf);
	nt_init_ustr(&m->basename, m->basebuf);
	m->base = base;
	m->size = size;
	m->load_count = 1;
	m->flags = 0x4;                 /* LDRP_IMAGE_DLL */
	m->next = mods;
	mods = m;
	list_add(&ldr_data.load, &m->load_links);
	list_add(&ldr_data.mem, &m->mem_links);
	m->exp_rva = pe_dir(base, DIR_EXPORT, &m->exp_size);
	{
		u8 *opt = pe_opt(base);

		if (opt && *(u32 *)(opt + 16))
			m->entry = base + *(u32 *)(opt + 16);
	}
	return m;
}

bool file_exists_sys(const char *lname);

static struct ldr_mod *find_loaded(const char *lname)
{
	for (struct ldr_mod *m = mods; m; m = m->next)
		if (!nt_strcmp(m->name, lname))
			return m;
	return NULL;
}

static struct ldr_mod *find_by_base(u64 base)
{
	for (struct ldr_mod *m = mods; m; m = m->next)
		if (m->base == base && (!m->builtin || base != NTRT_BASE || m == ntdll_mod))
			return m;
	return NULL;
}

u64 ldr_module_of(u64 addr)
{
	for (struct ldr_mod *m = mods; m; m = m->next)
		if (!m->builtin && addr >= m->base && addr < m->base + m->size)
			return m->base;
	if (addr >= NTRT_BASE && addr < NTRT_LIMIT)
		return NTRT_BASE;
	return 0;
}

/* DLLs the built-in table stands in for when no file is found. */
static bool builtin_name(const char *lname)
{
	static const char *const names[] = {
		"kernel32.dll", "kernelbase.dll", "ntdll.dll", "msvcrt.dll", "ucrtbase.dll",
		"advapi32.dll", "secur32.dll", "user32.dll", "shell32.dll",
	};

	if (starts(lname, "api-ms-win-") || starts(lname, "ext-ms-"))
		return true;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!nt_strcmp(lname, names[i]))
			return true;
	return false;
}

static struct ldr_mod *builtin_mod(const char *lname)
{
	struct ldr_mod *m = find_loaded(lname);
	wchar full[96];
	char path[96];

	if (m)
		return m;
	memcpy(path, "C:\\windows\\system32\\", 20);
	nt_strlcpy(path + 20, lname, sizeof(path) - 20);
	nt_utow(full, 96, path, rt_strlen(path) + 1);
	m = mod_new(lname, full, NTRT_BASE, (u32)(NTRT_LIMIT - NTRT_BASE));
	if (m) {
		m->builtin = true;
		m->bound = m->initialised = true;
	}
	return m;
}

/* API sets: the contract name says which DLL implements it. */
static void apiset_redirect(char *lname, size_t cap)
{
	static const struct {
		const char *prefix, *host;
	} map[] = {
		{ "api-ms-win-crt-", "ucrtbase.dll" },
		{ "api-ms-win-core-rtlsupport-", "ntdll.dll" },
		{ "api-ms-win-security-sddl-", "sechost.dll" },
		{ "api-ms-win-service-", "sechost.dll" },
		{ "api-ms-win-eventing-", "advapi32.dll" },
		{ "api-ms-win-security-lsalookup-", "advapi32.dll" },
		{ "api-ms-win-downlevel-advapi32-", "advapi32.dll" },
		{ "api-ms-win-downlevel-user32-", "user32.dll" },
		{ "api-ms-win-downlevel-shlwapi-", "shlwapi.dll" },
		{ "api-ms-win-core-", "kernelbase.dll" },
		{ "api-ms-win-security-", "kernelbase.dll" },
	};

	if (!starts(lname, "api-ms-win-"))
		return;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (starts(lname, map[i].prefix)) {
			nt_strlcpy(lname, map[i].host, cap);
			return;
		}
}

/* ---- mapping a DLL file -------------------------------------------------------- */

static long read_at(long fd, u64 off, void *buf, u64 n)
{
	u64 got = 0;

	if (mos_seek(fd, (s64)off, 0) < 0)
		return -1;
	while (got < n) {
		long r = mos_read(fd, (u8 *)buf + got, n - got);

		if (r <= 0)
			break;
		got += (u64)r;
	}
	return (long)got;
}

static NTSTATUS relocate(u64 base, s64 delta)
{
	u32 size, rva = pe_dir(base, DIR_RELOC, &size);
	u8 *p = (u8 *)base + rva, *end = p + size;

	if (!delta)
		return 0;
	if (!rva)
		return STATUS_INVALID_IMAGE_FORMAT;     /* must move, cannot */
	while (p + 8 <= end) {
		u32 page = *(u32 *)p, len = *(u32 *)(p + 4);
		u16 *e = (u16 *)(p + 8);

		if (len < 8 || p + len > end)
			break;
		for (u32 i = 0; i < (len - 8) / 2; i++) {
			u8 *at = (u8 *)base + page + (e[i] & 0xFFF);

			switch (e[i] >> 12) {
			case 0:
				break;
			case 3:
				*(u32 *)at += (u32)delta;
				break;
			case 10:
				*(u64 *)at += (u64)delta;
				break;
			default:
				return STATUS_INVALID_IMAGE_FORMAT;
			}
		}
		p += len;
	}
	return 0;
}

static NTSTATUS map_file(const char *path, u64 *out_base, u32 *out_size)
{
	u8 hdr[1024];
	long fd = mos_open(path, MOS_O_RDONLY), n;
	u8 *nt, *opt, *sec;
	u32 size, hsize, nsec;
	void *base = NULL;
	NTSTATUS st = STATUS_INVALID_IMAGE_FORMAT;

	if (fd < 0)
		return STATUS_DLL_NOT_FOUND;
	n = read_at(fd, 0, hdr, sizeof(hdr));
	if (n < 0x200 || *(u16 *)hdr != 0x5A4D || *(u32 *)(hdr + 0x3C) > (u32)n - 0x108)
		goto out;
	nt = hdr + *(u32 *)(hdr + 0x3C);
	opt = nt + 24;
	if (*(u32 *)nt != 0x4550 || *(u16 *)(nt + 4) != 0x8664 || *(u16 *)opt != 0x20B)
		goto out;
	size = *(u32 *)(opt + 56);
	hsize = *(u32 *)(opt + 60);
	nsec = *(u16 *)(nt + 6);
	if (!size || size > (512u << 20) || hsize > size || nsec > 96)
		goto out;
	st = nt_vm_alloc(&base, size);
	if (st)
		goto out;
	st = STATUS_INVALID_IMAGE_FORMAT;
	if (read_at(fd, 0, base, hsize) < 0)
		goto out;
	nt = pe_nt((u64)base);
	if (!nt)
		goto out;
	sec = nt + 24 + *(u16 *)(nt + 20);
	if (sec + nsec * 40 > (u8 *)base + hsize)
		goto out;
	for (u32 i = 0; i < nsec; i++, sec += 40) {
		u32 vsize = *(u32 *)(sec + 8), va = *(u32 *)(sec + 12);
		u32 raw = *(u32 *)(sec + 16), off = *(u32 *)(sec + 20);

		if (!memcmp(sec, ".debug", 6) || !raw)
			continue;
		if (vsize && raw > vsize)
			raw = vsize;
		if (va >= size || raw > size - va)
			goto out;
		if (read_at(fd, off, (u8 *)base + va, raw) != (long)raw)
			goto out;
	}
	st = relocate((u64)base, (s64)((u64)base - *(u64 *)(nt + 24 + 24)));
	if (!st) {
		size_t sz = size;
		void *b = base;
		u32 old;

		NtProtectVirtualMemory(~0ull, &b, &sz, 0x40 /* PAGE_EXECUTE_READWRITE */, &old);
		*out_base = (u64)base;
		*out_size = size;
	}
out:
	mos_close(fd);
	return st;
}

/* ---- exports and imports ------------------------------------------------------- */

static struct ldr_mod *load_dll(const char *name, const struct ldr_mod *importer, bool dynamic,
				NTSTATUS *status);
static NTSTATUS bind_imports(struct ldr_mod *m);

static u64 mod_export(struct ldr_mod *m, const char *name, u16 ord, u16 hint, int depth);

static u64 forward(const char *fwd, int depth)
{
	char dll[64], fn[128];
	size_t i = 0, j = 0;
	struct ldr_mod *t;
	NTSTATUS st;

	while (fwd[i] && fwd[i] != '.' && i + 5 < sizeof(dll)) {
		dll[i] = fwd[i];
		i++;
	}
	if (fwd[i] != '.')
		return 0;
	dll[i] = 0;
	for (i++; fwd[i] && j + 1 < sizeof(fn); )
		fn[j++] = fwd[i++];
	fn[j] = 0;
	t = load_dll(dll, NULL, false, &st);
	dbg("forward", fwd, t ? t->base : 0);
	if (!t)
		return 0;
	if (fn[0] == '#') {
		u32 o = 0;

		for (j = 1; fn[j] >= '0' && fn[j] <= '9'; j++)
			o = o * 10 + (u32)(fn[j] - '0');
		return mod_export(t, NULL, (u16)o, 0, depth + 1);
	}
	return mod_export(t, fn, 0, 0, depth + 1);
}

static u64 mod_export(struct ldr_mod *m, const char *name, u16 ord, u16 hint, int depth)
{
	u8 *dir;
	u32 nnames, nfuncs, obase, *funcs, *names, rva;
	u16 *ords;
	s64 idx = -1;

	if (!m || depth > 8)
		return 0;
	if (m->builtin)
		return name ? rt_lookup(name) : 0;
	if (!m->exp_rva)
		return 0;
	dir = (u8 *)m->base + m->exp_rva;
	obase = *(u32 *)(dir + 16);
	nfuncs = *(u32 *)(dir + 20);
	nnames = *(u32 *)(dir + 24);
	funcs = (u32 *)(m->base + *(u32 *)(dir + 28));
	names = (u32 *)(m->base + *(u32 *)(dir + 32));
	ords = (u16 *)(m->base + *(u32 *)(dir + 36));
	if (name) {
		if (hint < nnames && !nt_strcmp((char *)m->base + names[hint], name)) {
			idx = ords[hint];
		} else {
			u32 lo = 0, hi = nnames;

			while (lo < hi) {
				u32 mid = (lo + hi) / 2;
				int c = nt_strcmp((char *)m->base + names[mid], name);

				if (!c) {
					idx = ords[mid];
					break;
				}
				if (c < 0)
					lo = mid + 1;
				else
					hi = mid;
			}
		}
	} else if (ord >= obase) {
		idx = ord - obase;
	}
	if (idx < 0 || (u64)idx >= nfuncs || !(rva = funcs[idx]))
		return 0;
	if (rva >= m->exp_rva && rva < m->exp_rva + m->exp_size)
		return forward((char *)m->base + rva, depth);
	return m->base + rva;
}

/*
 * Stub for an import nobody provides:
 *   lea rcx, [rip + 14] ; jmp [rip + 0] ; dq missing ; db "DLL!Name", 0
 */
static u64 make_stub(const char *dll, const char *name, u16 ord)
{
	static u8 *cur, *end;
	char text[128];
	size_t n = 0, need;
	u8 *p;

	for (; *dll && n < 48; dll++)
		text[n++] = *dll;
	text[n++] = '!';
	if (name) {
		for (; *name && n + 1 < sizeof(text); name++)
			text[n++] = *name;
	} else {
		text[n++] = '#';
		for (u32 div = 10000; div; div /= 10)
			if (ord >= div || div == 1)
				text[n++] = (char)('0' + ord / div % 10);
	}
	text[n++] = 0;
	need = (21 + n + 15) & ~15ull;
	if (!cur || cur + need > end) {
		void *b = NULL;
		size_t sz = 0x4000;
		u32 old;

		if (nt_vm_alloc(&b, sz))
			return 0;
		NtProtectVirtualMemory(~0ull, &b, &sz, 0x40, &old);
		cur = b;
		end = cur + 0x4000;
	}
	p = cur;
	cur += need;
	p[0] = 0x48; p[1] = 0x8D; p[2] = 0x0D;
	*(u32 *)(p + 3) = 14;
	p[7] = 0xFF; p[8] = 0x25;
	*(u32 *)(p + 9) = 0;
	*(u64 *)(p + 13) = ((const struct ntrt_header *)NTRT_BASE)->missing;
	memcpy(p + 21, text, n);
	return (u64)p;
}

/* Fill one import address table from its name table. */
static void bind_thunks(struct ldr_mod *m, struct ldr_mod *t, const char *dll, u64 *iat,
			const u64 *names)
{
	for (; *names; names++, iat++) {
		u64 addr;

		if (*names >> 63) {
			u16 ord = (u16)*names;

			addr = mod_export(t, NULL, ord, 0, 0);
			if (!addr)
				addr = make_stub(dll, NULL, ord);
		} else {
			const u8 *hn = (const u8 *)m->base + (u32)*names;
			const char *fn = (const char *)hn + 2;

			addr = mod_export(t, fn, 0, *(const u16 *)hn, 0);
			if (!addr)
				addr = make_stub(dll, fn, 0);
		}
		*iat = addr;
	}
}

static NTSTATUS bind_imports(struct ldr_mod *m)
{
	u32 size, rva;
	u32 *d;

	if (m->bound)
		return 0;
	m->bound = true;
	rva = pe_dir(m->base, DIR_IMPORT, &size);
	if (!rva)
		return 0;
	for (d = (u32 *)(m->base + rva); d[3]; d += 5) {
		const char *dll = (const char *)m->base + d[3];
		NTSTATUS st;
		struct ldr_mod *t = load_dll(dll, m, false, &st);
		u64 *iat = (u64 *)(m->base + d[4]);
		const u64 *names = (const u64 *)(m->base + (d[0] ? d[0] : d[4]));

		if (!t && st != STATUS_DLL_NOT_FOUND)
			return st;
		bind_thunks(m, t, dll, iat, names);
	}
	return 0;
}

static void queue_init(struct ldr_mod *m)
{
	if (n_init < MAX_INIT)
		init_queue[n_init++] = m;
}

static void tls_register(struct ldr_mod *m)
{
	u32 size, rva = pe_dir(m->base, DIR_TLS, &size);
	u64 *dir;

	if (!rva || size < 40 || n_tls >= MAX_TLS)
		return;
	dir = (u64 *)(m->base + rva);
	m->has_tls = true;
	m->tls_index = (u16)n_tls;
	tls_mods[n_tls++] = m;
	if (dir[2])
		*(u32 *)dir[2] = m->tls_index;
}

/* Allocate this thread's static TLS blocks (ThreadLocalStoragePointer). */
static void tls_alloc_thread(void)
{
	u8 *teb = nt_teb();
	void **arr = *(void ***)(teb + 0x58);

	if (!n_tls)
		return;
	if (!arr) {
		arr = nt_alloc(MAX_TLS * sizeof(void *));
		*(void ***)(teb + 0x58) = arr;
	}
	for (u32 i = 0; i < n_tls; i++) {
		u64 *dir;
		size_t raw, zero;

		if (!arr || arr[i])
			continue;
		dir = (u64 *)(tls_mods[i]->base + pe_dir(tls_mods[i]->base, DIR_TLS, NULL));
		raw = dir[1] > dir[0] ? dir[1] - dir[0] : 0;
		zero = *(u32 *)&dir[4];
		arr[i] = nt_alloc(raw + zero + 16);
		if (arr[i] && raw)
			memcpy(arr[i], (void *)dir[0], raw);
	}
}

typedef API void (*tls_cb)(void *base, u32 reason, void *reserved);
typedef API BOOL (*dll_main)(void *base, u32 reason, void *reserved);

static void tls_callbacks(struct ldr_mod *m, u32 reason)
{
	u32 size, rva = pe_dir(m->base, DIR_TLS, &size);
	tls_cb *cb;

	if (!rva || size < 40)
		return;
	for (cb = (tls_cb *)((u64 *)(m->base + rva))[3]; cb && *cb; cb++)
		(*cb)((void *)m->base, reason, NULL);
}

static NTSTATUS run_inits(void)
{
	NTSTATUS st = 0;

	tls_alloc_thread();
	for (u32 i = 0; i < n_init; i++) {
		struct ldr_mod *m = init_queue[i];

		if (m->initialised)
			continue;
		m->initialised = true;
		list_add(&ldr_data.init, &m->init_links);
		dbg("init", m->name, m->base);
		tls_callbacks(m, DLL_PROCESS_ATTACH);
		if (m->entry && m != exe_mod &&
		    !((dll_main)m->entry)((void *)m->base, DLL_PROCESS_ATTACH, (void *)1)) {
			dbg("DllMain failed", m->name, 0);
			st = STATUS_DLL_INIT_FAILED;
			break;
		}
		m->flags |= 0x80000;    /* LDRP_PROCESS_ATTACH_CALLED */
	}
	n_init = 0;
	return st;
}

static bool file_exists(const char *path)
{
	u64 st[2];

	return mos_stat(path, st) >= 0 && st[1] != MOS_ST_DIR;
}

/* Directory part of the program's VFS path, with a trailing slash. */
static size_t exe_dir(char *out, size_t cap)
{
	const char *p = rt_params->image_path;
	size_t n = 0, slash = 0;

	out[n++] = '/';
	for (; *p && n + 1 < cap; p++) {
		out[n++] = *p;
		if (*p == '/')
			slash = n;
	}
	if (*p == 0 && rt_params->image_path[0] == '/')
		memmove(out, out + 1, n--);
	if (rt_params->image_path[0] == '/' && slash)
		slash--;
	out[slash ? slash : 1] = 0;
	return slash ? slash : 1;
}

static struct ldr_mod *load_dll(const char *name, const struct ldr_mod *importer, bool dynamic,
				NTSTATUS *status)
{
	char lname[64], path[192];
	struct ldr_mod *m;
	u64 base;
	u32 size;
	NTSTATUS st;

	(void)importer;
	*status = 0;
	lower_name(lname, sizeof(lname), name, true);
	apiset_redirect(lname, sizeof(lname));
	if (!nt_strcmp(lname, "ntdll.dll"))
		return ntdll_mod;
	m = find_loaded(lname);
	if (m) {
		if (dynamic)
			m->load_count++;
		return m;
	}
	/* Paths with a directory are used as given (C:\x\y.dll or /x/y.dll). */
	path[0] = 0;
	if (nt_strchr(name, '\\') || nt_strchr(name, '/')) {
		wchar w[192];
		UNICODE_STRING u;

		nt_utow(w, 192, name, rt_strlen(name) + 1);
		nt_init_ustr(&u, w);
		if (nt_unix_path(&u, 0, path, sizeof(path)) || !file_exists(path))
			path[0] = 0;
	}
	if (!path[0]) {
		memcpy(path, "/windows/system32/", 18);
		nt_strlcpy(path + 18, lname, sizeof(path) - 18);
		if (!file_exists(path)) {
			size_t d = exe_dir(path, sizeof(path));

			nt_strlcpy(path + d, lname, sizeof(path) - d);
			if (!file_exists(path))
				path[0] = 0;
		}
	}
	if (!path[0]) {
		if (dynamic && !builtin_name(lname)) {
			*status = STATUS_DLL_NOT_FOUND;
			return NULL;
		}
		return builtin_mod(lname);
	}
	st = map_file(path, &base, &size);
	if (st) {
		dbg("cannot map", path, st);
		*status = st;
		return NULL;
	}
	{
		wchar full[192];
		char dos[200];

		dos[0] = 'C';
		dos[1] = ':';
		nt_strlcpy(dos + 2, path, sizeof(dos) - 2);
		for (char *p = dos; *p; p++)
			if (*p == '/')
				*p = '\\';
		nt_utow(full, 192, dos, rt_strlen(dos) + 1);
		m = mod_new(lname, full, base, size);
	}
	if (!m) {
		*status = STATUS_NO_MEMORY;
		return NULL;
	}
	dbg("loaded", path, base);
	tls_register(m);
	st = bind_imports(m);
	if (st) {
		*status = st;
		return NULL;
	}
	queue_init(m);
	return m;
}

/* ---- process start-up ---------------------------------------------------------- */

void ldr_init(void)
{
	ldr_data.length = sizeof(ldr_data);
	ldr_data.initialized = 1;
	ldr_data.load.next = ldr_data.load.prev = &ldr_data.load;
	ldr_data.mem.next = ldr_data.mem.prev = &ldr_data.mem;
	ldr_data.init.next = ldr_data.init.prev = &ldr_data.init;
	*(void **)(nt_peb + 0x18) = &ldr_data;
}

u64 ldr_image_base(void)
{
	return rt_params->image_base;
}

const wchar *ldr_image_path(void)
{
	return exe_mod ? exe_mod->fullbuf : (const wchar *)L"";
}

void ldr_set_debug(bool on)
{
	debug = on;
}

void ldr_process_start(void)
{
	char dos[96];
	wchar full[96];
	u64 base = rt_params->image_base;
	NTSTATUS st;
	u32 rc;

	dos[0] = 'C';
	dos[1] = ':';
	dos[2] = '\\';
	nt_strlcpy(dos + 3, rt_params->image_path[0] == '/' ? rt_params->image_path + 1 :
		     rt_params->image_path, sizeof(dos) - 3);
	for (char *p = dos; *p; p++)
		if (*p == '/')
			*p = '\\';
	nt_utow(full, 96, dos, rt_strlen(dos) + 1);
	ntdll_mod = builtin_mod("ntdll.dll");
	exe_mod = mod_new(dos, full, base, (u32)rt_params->image_size);
	exe_mod->flags = 0;
	exe_mod->entry = rt_params->entry;
	nt_set_image_path(exe_mod->fullbuf);
	{
		void *b = (void *)base;
		size_t sz = rt_params->image_size;
		u32 old;

		NtProtectVirtualMemory(~0ull, &b, &sz, 0x40, &old);
	}
	tls_register(exe_mod);
	st = bind_imports(exe_mod);
	if (st) {
		dbg("import binding failed", exe_mod->name, st);
		rt_exit(st);
	}
	queue_init(exe_mod);
	st = run_inits();
	if (st)
		rt_exit(st);
	ldr_data.initialized = 1;
	rc = ((API u32 (*)(void *))rt_params->entry)(nt_peb);
	rt_exit(rc);            /* like RtlUserThreadStart: return value = exit code */
}

/* A new thread: static TLS and DLL_THREAD_ATTACH. */
void ldr_thread_attach(void)
{
	tls_alloc_thread();
	for (struct ldr_mod *m = mods; m; m = m->next) {
		if (m->builtin || !m->initialised || (m->flags & 0x40000 /* no thread calls */))
			continue;
		tls_callbacks(m, DLL_THREAD_ATTACH);
		if (m->entry && m != exe_mod)
			((dll_main)m->entry)((void *)m->base, DLL_THREAD_ATTACH, NULL);
	}
}

/* ---- Ldr* API ------------------------------------------------------------------ */

static void ustr_to_utf8(const UNICODE_STRING *u, char *out, size_t cap)
{
	size_t n = nt_wtou(out, cap - 1, u->buf, u->len / 2);

	out[n < cap ? n : cap - 1] = 0;
}

u64 ldr_load(const char *name, NTSTATUS *st)
{
	struct ldr_mod *m;

	nt_lock(&ldr_lock);
	m = load_dll(name, NULL, true, st);
	if (m && !*st)
		*st = run_inits();
	nt_unlock(&ldr_lock);
	return m && !*st ? m->base : 0;
}

static API NTSTATUS w_LdrLoadDll(const wchar *path, u32 *flags, const UNICODE_STRING *name,
				 u64 *handle)
{
	char n8[192];
	NTSTATUS st;

	(void)path;
	(void)flags;
	ustr_to_utf8(name, n8, sizeof(n8));
	*handle = ldr_load(n8, &st);
	return st;
}
EXPORT(LdrLoadDll);

u64 ldr_handle(const char *name)
{
	char lname[64];
	struct ldr_mod *m;

	if (!name)
		return rt_params->image_base;
	lower_name(lname, sizeof(lname), name, true);
	apiset_redirect(lname, sizeof(lname));
	m = find_loaded(lname);
	if (!m && builtin_name(lname) && !file_exists_sys(lname))
		m = builtin_mod(lname);
	return m ? m->base : 0;
}

bool file_exists_sys(const char *lname)
{
	char path[96];

	memcpy(path, "/windows/system32/", 18);
	nt_strlcpy(path + 18, lname, sizeof(path) - 18);
	return file_exists(path);
}

static API NTSTATUS w_LdrGetDllHandle(const wchar *path, u32 *flags, const UNICODE_STRING *name,
				      u64 *handle)
{
	char n8[192];

	(void)path;
	(void)flags;
	if (!name) {
		*handle = rt_params->image_base;
		return 0;
	}
	ustr_to_utf8(name, n8, sizeof(n8));
	*handle = ldr_handle(n8);
	dbg("gethandle", n8, *handle);
	return *handle ? 0 : STATUS_DLL_NOT_FOUND;
}
EXPORT(LdrGetDllHandle);

static API NTSTATUS w_LdrGetDllHandleEx(u32 flags, const wchar *path, u32 *chars,
					const UNICODE_STRING *name, u64 *handle)
{
	(void)flags;
	return w_LdrGetDllHandle(path, chars, name, handle);
}
EXPORT(LdrGetDllHandleEx);

static API NTSTATUS w_LdrGetDllHandleByName(const UNICODE_STRING *base, const UNICODE_STRING *full,
					    u64 *handle)
{
	return w_LdrGetDllHandle(NULL, NULL, base ? base : full, handle);
}
EXPORT(LdrGetDllHandleByName);

u64 ldr_proc(u64 module, const char *name, u16 ord)
{
	struct ldr_mod *m = find_by_base(module);

	if (!m && module == NTRT_BASE)
		m = ntdll_mod;
	return m ? mod_export(m, name, ord, 0, 0) : 0;
}

static API NTSTATUS w_LdrGetProcedureAddress(u64 module, const ANSI_STRING *name, u32 ord,
					     void **addr)
{
	char n8[256];
	u64 a;

	if (name) {
		size_t n = name->len < sizeof(n8) - 1 ? name->len : sizeof(n8) - 1;

		memcpy(n8, name->buf, n);
		n8[n] = 0;
	}
	if (!find_by_base(module) && module != NTRT_BASE)
		return STATUS_DLL_NOT_FOUND;
	a = ldr_proc(module, name ? n8 : NULL, (u16)ord);
	dbg("getproc", name ? n8 : "#", a);
	*addr = (void *)a;
	return a ? 0 : STATUS_ENTRYPOINT_NOT_FOUND;
}
EXPORT(LdrGetProcedureAddress);

static API NTSTATUS w_LdrGetProcedureAddressForCaller(u64 module, const ANSI_STRING *name, u32 ord,
						      void **addr, u32 flags, void *caller)
{
	(void)flags;
	(void)caller;
	return w_LdrGetProcedureAddress(module, name, ord, addr);
}
EXPORT(LdrGetProcedureAddressForCaller);

static API NTSTATUS w_LdrUnloadDll(u64 module)
{
	struct ldr_mod *m = find_by_base(module);

	if (!m)
		return STATUS_DLL_NOT_FOUND;
	if (m->load_count > 1)
		m->load_count--;
	return 0;               /* images are never unmapped */
}
EXPORT(LdrUnloadDll);

static API NTSTATUS w_LdrAddRefDll(u32 flags, u64 module)
{
	struct ldr_mod *m = find_by_base(module);

	(void)flags;
	if (!m)
		return STATUS_DLL_NOT_FOUND;
	m->load_count++;
	return 0;
}
EXPORT(LdrAddRefDll);

static API NTSTATUS w_LdrDisableThreadCalloutsForDll(u64 module)
{
	struct ldr_mod *m = find_by_base(module);

	if (!m)
		return STATUS_DLL_NOT_FOUND;
	m->flags |= 0x40000;
	return 0;
}
EXPORT(LdrDisableThreadCalloutsForDll);

static API NTSTATUS w_LdrFindEntryForAddress(u64 addr, struct ldr_mod **out)
{
	u64 b = ldr_module_of(addr);
	struct ldr_mod *m = b ? find_by_base(b) : NULL;

	if (!m && b == NTRT_BASE)
		m = ntdll_mod;
	*out = m;
	return m ? 0 : STATUS_NO_MORE_ENTRIES;
}
EXPORT(LdrFindEntryForAddress);

static API NTSTATUS w_LdrGetDllFullName(u64 module, UNICODE_STRING *name)
{
	struct ldr_mod *m = find_by_base(module ? module : rt_params->image_base);

	if (!m)
		return STATUS_DLL_NOT_FOUND;
	if (name->max < m->full.len + 2) {
		name->len = m->full.len;
		return STATUS_BUFFER_TOO_SMALL;
	}
	memcpy(name->buf, m->fullbuf, m->full.len + 2);
	name->len = m->full.len;
	return 0;
}
EXPORT(LdrGetDllFullName);

/* Full path of a module, for GetModuleFileName. Returns characters copied. */
u32 ldr_module_path(u64 module, wchar *buf, u32 cap)
{
	struct ldr_mod *m = find_by_base(module ? module : rt_params->image_base);
	u32 n = 0;

	if (!m || !cap)
		return 0;
	while (m->fullbuf[n] && n + 1 < cap) {
		buf[n] = m->fullbuf[n];
		n++;
	}
	buf[n] = 0;
	return n;
}

static API NTSTATUS w_LdrLockLoaderLock(u32 flags, u32 *result, u64 *magic)
{
	(void)flags;
	if (result)
		*result = 1;
	if (magic)
		*magic = 1;
	return 0;
}
EXPORT(LdrLockLoaderLock);

static API NTSTATUS w_LdrUnlockLoaderLock(u32 flags, u64 magic)
{
	(void)flags;
	(void)magic;
	return 0;
}
EXPORT(LdrUnlockLoaderLock);

static API NTSTATUS w_LdrAddDllDirectory(const UNICODE_STRING *dir, void **cookie)
{
	(void)dir;
	*cookie = (void *)1;
	return 0;
}
EXPORT(LdrAddDllDirectory);

static API NTSTATUS w_LdrRemoveDllDirectory(void *cookie)
{
	(void)cookie;
	return 0;
}
EXPORT(LdrRemoveDllDirectory);

static API NTSTATUS w_LdrSetDefaultDllDirectories(u32 flags)
{
	(void)flags;
	return 0;
}
EXPORT(LdrSetDefaultDllDirectories);

static API NTSTATUS w_LdrSetDllDirectory(const UNICODE_STRING *dir)
{
	(void)dir;
	return 0;
}
EXPORT(LdrSetDllDirectory);

static API NTSTATUS w_LdrGetDllDirectory(UNICODE_STRING *dir)
{
	dir->len = 0;
	return 0;
}
EXPORT(LdrGetDllDirectory);

static API NTSTATUS w_LdrGetDllPath(const wchar *module, u32 flags, wchar **path, wchar **unknown)
{
	static const wchar sys[] = L"C:\\windows\\system32;C:\\windows;.";
	wchar *p = nt_alloc(sizeof(sys));

	(void)module;
	(void)flags;
	if (!p)
		return STATUS_NO_MEMORY;
	memcpy(p, sys, sizeof(sys));
	*path = p;
	*unknown = NULL;
	return 0;
}
EXPORT(LdrGetDllPath);

/* ---- delay-load ---------------------------------------------------------------- */

/*
 * IMAGE_DELAYLOAD_DESCRIPTOR: Attributes, DllNameRVA, ModuleHandleRVA,
 * ImportAddressTableRVA, ImportNameTableRVA, BoundImportAddressTableRVA,
 * UnloadInformationTableRVA, TimeDateStamp (all RVAs, Attributes bit 0 set).
 */
static API void *w_LdrResolveDelayLoadedAPI(u64 base, const u32 *desc, void *dll_hook,
					    void *sys_hook, u64 *thunk, u32 flags)
{
	const char *dll = (const char *)base + desc[1];
	u64 *hmod = (u64 *)(base + desc[2]);
	u64 *iat = (u64 *)(base + desc[3]);
	const u64 *names = (const u64 *)(base + desc[4]);
	size_t i = (size_t)(thunk - iat);
	u64 addr = 0;
	NTSTATUS st = 0;

	(void)dll_hook;
	(void)sys_hook;
	(void)flags;
	if (!*hmod)
		*hmod = ldr_load(dll, &st);
	if (*hmod) {
		if (names[i] >> 63) {
			addr = ldr_proc(*hmod, NULL, (u16)names[i]);
		} else {
			const char *fn = (const char *)base + (u32)names[i] + 2;

			addr = ldr_proc(*hmod, fn, 0);
			if (!addr)
				addr = make_stub(dll, fn, 0);
		}
	}
	if (!addr)
		addr = make_stub(dll, (names[i] >> 63) ? NULL : (const char *)base + (u32)names[i] + 2,
				 (u16)names[i]);
	*thunk = addr;
	return (void *)addr;
}
EXPORT(LdrResolveDelayLoadedAPI);

/* ---- resources ----------------------------------------------------------------- */

static u8 *res_find_entry(u8 *root, u8 *dir, u64 id, bool want_dir, bool any)
{
	u16 named = *(u16 *)(dir + 12), ids = *(u16 *)(dir + 14);
	u32 *e = (u32 *)(dir + 16);

	for (u32 i = 0; i < (u32)named + ids; i++, e += 2) {
		bool match;

		if (any) {
			match = true;
		} else if (id >> 16) {          /* a name: compare case-insensitively */
			const wchar *want = (const wchar *)id;
			u16 *s;
			u16 len;

			if (!(e[0] >> 31))
				continue;
			s = (u16 *)(root + (e[0] & 0x7FFFFFFF));
			len = s[0];
			match = true;
			for (u16 k = 0; k < len; k++)
				if (nt_upcase(s[1 + k]) != nt_upcase(want[k])) {
					match = false;
					break;
				}
			match = match && !want[len];
		} else {
			match = !(e[0] >> 31) && (u16)e[0] == (u16)id;
		}
		if (!match)
			continue;
		if (want_dir != !!(e[1] >> 31))
			return NULL;
		return root + (e[1] & 0x7FFFFFFF);
	}
	return NULL;
}

/* level 0..2: type, name, language directory; 3: data entry. */
NTSTATUS ldr_find_resource(u64 base, const u64 *info, u32 level, u8 **out)
{
	u32 size, rva = pe_dir(base, DIR_RESOURCE, &size);
	u8 *root, *dir;

	if (!rva)
		return STATUS_RESOURCE_DATA_NOT_FOUND;
	root = dir = (u8 *)base + rva;
	for (u32 l = 0; l < level && l < 3; l++) {
		u8 *next;

		if (l < 2) {
			next = res_find_entry(root, dir, info[l], l + 1 < 3, false);
			if (!next) {
				*out = NULL;
				return l == 0 ? STATUS_RESOURCE_TYPE_NOT_FOUND : STATUS_RESOURCE_NAME_NOT_FOUND;
			}
		} else {
			u64 lang = info[2];

			next = res_find_entry(root, dir, lang, false, false);
			if (!next && (lang & 0x3FF))
				next = res_find_entry(root, dir, lang & 0x3FF, false, false);
			if (!next)
				next = res_find_entry(root, dir, 0x409, false, false);
			if (!next)
				next = res_find_entry(root, dir, 0, false, false);
			if (!next)
				next = res_find_entry(root, dir, 0, false, true);
			if (!next) {
				*out = NULL;
				return STATUS_RESOURCE_LANG_NOT_FOUND;
			}
		}
		dir = next;
	}
	*out = dir;
	return 0;
}

static API NTSTATUS w_LdrFindResource_U(u64 base, const u64 *info, u32 level, void **entry)
{
	u8 *p;
	NTSTATUS st = ldr_find_resource(base, info, level, &p);

	*entry = st ? NULL : p;
	return st;
}
EXPORT(LdrFindResource_U);

static API NTSTATUS w_LdrFindResourceDirectory_U(u64 base, const u64 *info, u32 level, void **dir)
{
	u8 *p;
	NTSTATUS st = ldr_find_resource(base, info, level > 2 ? 2 : level, &p);

	*dir = st ? NULL : p;
	return st;
}
EXPORT(LdrFindResourceDirectory_U);

static API NTSTATUS w_LdrAccessResource(u64 base, const u32 *entry, void **ptr, u32 *size)
{
	if (!entry)
		return STATUS_RESOURCE_DATA_NOT_FOUND;
	if (ptr)
		*ptr = (void *)(base + entry[0]);
	if (size)
		*size = entry[1];
	return 0;
}
EXPORT(LdrAccessResource);

/* ---- image helpers ------------------------------------------------------------- */

static API void *w_RtlImageNtHeader(u64 base)
{
	return pe_nt(base);
}
EXPORT(RtlImageNtHeader);

static API void *w_RtlImageDirectoryEntryToData(u64 base, u8 image, u16 dir, u32 *size)
{
	u32 rva = pe_dir(base & ~3ull, dir, size);

	(void)image;
	return rva ? (void *)((base & ~3ull) + rva) : NULL;
}
EXPORT(RtlImageDirectoryEntryToData);

static API void *w_RtlPcToFileHeader(u64 pc, void **base)
{
	u64 b = ldr_module_of(pc);

	*base = (void *)b;
	return (void *)b;
}
EXPORT(RtlPcToFileHeader);

static API void *w_RtlImageRvaToVa(void *nt, u64 base, u32 rva, void **last)
{
	(void)nt;
	(void)last;
	return (void *)(base + rva);
}
EXPORT(RtlImageRvaToVa);
