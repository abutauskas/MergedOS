/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Built-in Win32 runtime, part 1: process start-up, TEB/PEB, the heap and
 * a subset of kernel32 / ntdll / advapi32 / secur32.
 *
 * Written from the public Win32 documentation. One process, one thread:
 * critical sections and TLS need no locking, and there are no real DLL
 * files - GetModuleHandle / LoadLibrary hand out a pseudo handle for any
 * module name the runtime stands in for.
 */
#include "src/ntrt/nt.h"

#define CURRENT_PROCESS (~0ull)
#define STD_INPUT_HANDLE  ((u32)-10)
#define STD_OUTPUT_HANDLE ((u32)-11)
#define STD_ERROR_HANDLE  ((u32)-12)
#define ERROR_INVALID_HANDLE        6
#define ERROR_NOT_ENOUGH_MEMORY     8
#define ERROR_INVALID_PARAMETER     87
#define ERROR_INSUFFICIENT_BUFFER   122
#define ERROR_MOD_NOT_FOUND         126
#define ERROR_PROC_NOT_FOUND        127
#define ERROR_ENVVAR_NOT_FOUND      203
#define ERROR_NOT_SUPPORTED         50
#define ERROR_BUFFER_OVERFLOW       111
#define ERROR_MORE_DATA             234
#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000
#define PAGE_EXECUTE_READWRITE 0x40
#define FILETIME_UNIX_EPOCH 116444736000000000ull     /* 1970-01-01 in 100 ns units */

struct ntrt_params *rt_params;

/* TEB and PEB of the only thread. Offsets are the documented x64 ones. */
static u8 teb[0x1840] __attribute__((aligned(4096)));
static u8 peb[0x400] __attribute__((aligned(16)));
static u8 proc_params[0x400] __attribute__((aligned(16)));
static void *tls_array[1];
#define TEB_STACK_BASE   0x08
#define TEB_STACK_LIMIT  0x10
#define TEB_SELF         0x30
#define TEB_PID          0x40
#define TEB_TID          0x48
#define TEB_TLS_POINTER  0x58
#define TEB_PEB          0x60
#define TEB_LAST_ERROR   0x68
#define TEB_TLS_SLOTS    0x1480
#define TLS_SLOTS        64
#define PEB_IMAGE_BASE   0x10
#define PEB_PARAMS       0x20
#define PEB_HEAP         0x30
#define PEB_OS_MAJOR     0x118
#define PEB_OS_MINOR     0x11C
#define PEB_OS_BUILD     0x120
#define PEB_OS_PLATFORM  0x124

#define FIELD(base, off, type) (*(type *)((base) + (off)))

static char cmdline_a[256];
static wchar cmdline_w[256];
static wchar image_path_w[256];
static u64 tls_used;            /* bitmap of TlsAlloc slots */

/* ---- helpers --------------------------------------------------------------- */

u64 rt_teb(void)
{
	return (u64)teb;
}

void rt_set_last_error(u32 e)
{
	FIELD(teb, TEB_LAST_ERROR, u32) = e;
}

const char *rt_cmdline(void)
{
	return cmdline_a;
}

const wchar *rt_wcmdline(void)
{
	return cmdline_w;
}

static u32 kuser32(u32 off)
{
	return *(volatile u32 *)(NTRT_KUSER_VA + off);
}

/* Console handles: the three pseudo handles GetStdHandle returns. */
static int handle_fd(u64 h)
{
	return (h == 4 || h == 8 || h == 0xC) ? (int)(h >> 2) - 1 : -1;
}

static long raw_write(int fd, const void *buf, size_t n)
{
	u64 iosb[2] = { 0, 0 };

	if (NtWriteFile((u64)(fd + 1) << 2, 0, NULL, NULL, iosb, buf, (u32)n, NULL, NULL))
		return -1;
	return (long)iosb[1];
}

/*
 * Single characters (fputc, which MinGW's own printf uses for every byte)
 * are collected per line; any other write flushes them first, so output
 * order is kept.
 */
static char line_buf[128];
static size_t line_len;
static int line_fd;

void rt_flush(void)
{
	size_t n = line_len;

	line_len = 0;
	if (n)
		raw_write(line_fd, line_buf, n);
}

void rt_putc(int fd, char c)
{
	if (line_len && line_fd != fd)
		rt_flush();
	line_fd = fd;
	line_buf[line_len++] = c;
	if (c == '\n' || line_len == sizeof(line_buf))
		rt_flush();
}

long rt_write(int fd, const void *buf, size_t n)
{
	rt_flush();
	return raw_write(fd, buf, n);
}

void rt_exit(u32 code)
{
	rt_flush();
	for (;;)
		NtTerminateProcess(CURRENT_PROCESS, code);
}

/* UTF-16 -> UTF-8. Returns the number of bytes the full conversion needs. */
size_t rt_utf8_from_wide(char *out, size_t cap, const wchar *in, size_t n)
{
	size_t len = 0;

	for (size_t i = 0; i < n; i++) {
		u32 c = in[i];
		u8 tmp[4];
		size_t k = 0;

		if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000)
			c = 0x10000 + ((c - 0xD800) << 10) + (in[++i] - 0xDC00);
		if (c < 0x80) {
			tmp[k++] = (u8)c;
		} else if (c < 0x800) {
			tmp[k++] = (u8)(0xC0 | c >> 6);
			tmp[k++] = (u8)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			tmp[k++] = (u8)(0xE0 | c >> 12);
			tmp[k++] = (u8)(0x80 | ((c >> 6) & 0x3F));
			tmp[k++] = (u8)(0x80 | (c & 0x3F));
		} else {
			tmp[k++] = (u8)(0xF0 | c >> 18);
			tmp[k++] = (u8)(0x80 | ((c >> 12) & 0x3F));
			tmp[k++] = (u8)(0x80 | ((c >> 6) & 0x3F));
			tmp[k++] = (u8)(0x80 | (c & 0x3F));
		}
		for (size_t j = 0; j < k; j++, len++)
			if (out && len < cap)
				out[len] = (char)tmp[j];
	}
	return len;
}

/* UTF-8 (or plain ASCII) -> UTF-16. Returns the number of units needed. */
static size_t wide_from_utf8(wchar *out, size_t cap, const u8 *in, size_t n)
{
	size_t len = 0;

	for (size_t i = 0; i < n;) {
		u32 c = in[i++];
		int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;

		if (more) {
			c &= 0x3F >> more;
			while (more-- && i < n && (in[i] & 0xC0) == 0x80)
				c = c << 6 | (in[i++] & 0x3F);
		}
		if (c >= 0x10000) {
			if (out && len + 1 < cap) {
				out[len] = (wchar)(0xD800 + ((c - 0x10000) >> 10));
				out[len + 1] = (wchar)(0xDC00 + ((c - 0x10000) & 0x3FF));
			}
			len += 2;
		} else {
			if (out && len < cap)
				out[len] = (wchar)c;
			len++;
		}
	}
	return len;
}

static void ascii_to_wide(wchar *out, const char *in, size_t cap)
{
	size_t n = rt_strlen(in), w = wide_from_utf8(out, cap - 1, (const u8 *)in, n);

	out[w < cap - 1 ? w : cap - 1] = 0;
}

/* ---- heap: power-of-two size classes on top of NtAllocateVirtualMemory ---- */

#define HEAP_MIN_SHIFT 4
#define HEAP_CLASSES   17               /* 16 bytes .. 1 MiB; larger: own mapping */
#define HEAP_CHUNK     (256ull << 10)
#define HEAP_MAGIC     0x4E545248u

struct block {
	u32 magic, klass;
	u64 size;               /* usable bytes */
};
static void *free_list[HEAP_CLASSES];
static u8 *arena_cur, *arena_end;

static void *vm_alloc(size_t n)
{
	void *base = NULL;
	size_t size = n;

	if (NtAllocateVirtualMemory(CURRENT_PROCESS, &base, 0, &size, MEM_COMMIT | MEM_RESERVE, 4))
		return NULL;
	return base;
}

void *rt_alloc(size_t n)
{
	u32 k = 0;
	struct block *b;

	if (n > (1ull << 40))
		return NULL;
	while (k < HEAP_CLASSES && (1ull << (k + HEAP_MIN_SHIFT)) < n)
		k++;
	if (k == HEAP_CLASSES) {
		b = vm_alloc(n + sizeof(*b));
		if (!b)
			return NULL;
		*b = (struct block){ HEAP_MAGIC, k, n };
		return b + 1;
	}
	if (free_list[k]) {
		b = (struct block *)free_list[k] - 1;
		free_list[k] = *(void **)free_list[k];
		memset(b + 1, 0, b->size);
		return b + 1;
	}
	{
		size_t need = sizeof(*b) + (1ull << (k + HEAP_MIN_SHIFT));

		if (!arena_cur || (size_t)(arena_end - arena_cur) < need) {
			arena_cur = vm_alloc(need > HEAP_CHUNK ? need : HEAP_CHUNK);
			if (!arena_cur)
				return NULL;
			arena_end = arena_cur + (need > HEAP_CHUNK ? need : HEAP_CHUNK);
		}
		b = (struct block *)arena_cur;
		arena_cur += need;
		*b = (struct block){ HEAP_MAGIC, k, 1ull << (k + HEAP_MIN_SHIFT) };
		return b + 1;   /* fresh pages are already zero */
	}
}

void rt_free(void *p)
{
	struct block *b = (struct block *)p - 1;

	if (!p || b->magic != HEAP_MAGIC)
		return;
	if (b->klass < HEAP_CLASSES) {
		*(void **)p = free_list[b->klass];
		free_list[b->klass] = p;
	}
	/* Large blocks keep their mapping: NtFreeVirtualMemory does not unmap yet. */
}

size_t rt_size(void *p)
{
	return p ? ((struct block *)p - 1)->size : 0;
}

void *rt_realloc(void *p, size_t n)
{
	void *q;

	if (!p)
		return rt_alloc(n);
	if (n <= rt_size(p))
		return p;
	q = rt_alloc(n);
	if (q) {
		memcpy(q, p, rt_size(p));
		rt_free(p);
	}
	return q;
}

/* ---- start-up ---------------------------------------------------------------- */

typedef API void (*tls_callback)(void *base, u32 reason, void *reserved);

/* Static TLS: the image's template, and its callbacks, as the Windows loader does. */
static void tls_init(u64 base)
{
	u32 lfanew = *(u32 *)(base + 0x3C);
	u8 *opt = (u8 *)base + lfanew + 24;
	u32 rva = *(u32 *)(opt + 112 + 9 * 8), size = *(u32 *)(opt + 112 + 9 * 8 + 4);
	u64 *dir;

	if (*(u32 *)(opt + 108) <= 9 || !rva || size < 40)
		return;
	dir = (u64 *)(base + rva);
	if (dir[1] >= dir[0]) {
		size_t raw = dir[1] - dir[0], zero = *(u32 *)&dir[4];
		u8 *blk = rt_alloc(raw + zero + 16);

		if (blk)
			memcpy(blk, (void *)dir[0], raw);
		tls_array[0] = blk;
		FIELD(teb, TEB_TLS_POINTER, void *) = tls_array;
		if (dir[2])
			*(u32 *)dir[2] = 0;
	}
	for (tls_callback *cb = (tls_callback *)dir[3]; cb && *cb; cb++)
		(*cb)((void *)base, 1 /* DLL_PROCESS_ATTACH */, NULL);
}

static void set_ustr(u8 *at, const wchar *s)
{
	u16 n = (u16)(rt_wcslen(s) * 2);

	FIELD(at, 0, u16) = n;
	FIELD(at, 2, u16) = (u16)(n + 2);
	FIELD(at, 8, const wchar *) = s;
}

void ntrt_start(struct ntrt_params *p) __attribute__((noreturn, used));
void ntrt_start(struct ntrt_params *p)
{

	rt_params = p;
	FIELD(teb, TEB_STACK_BASE, u64) = p->stack_base;
	FIELD(teb, TEB_STACK_LIMIT, u64) = p->stack_limit;
	FIELD(teb, TEB_SELF, u64) = (u64)teb;
	FIELD(teb, TEB_PID, u64) = p->pid;
	FIELD(teb, TEB_TID, u64) = p->tid;
	FIELD(teb, TEB_PEB, u64) = (u64)peb;
	FIELD(peb, PEB_IMAGE_BASE, u64) = p->image_base;
	FIELD(peb, PEB_PARAMS, u64) = (u64)proc_params;
	FIELD(peb, PEB_HEAP, u64) = (u64)free_list;
	FIELD(peb, PEB_OS_MAJOR, u32) = kuser32(KUSER_NT_MAJOR_VERSION);
	FIELD(peb, PEB_OS_MINOR, u32) = kuser32(KUSER_NT_MINOR_VERSION);
	FIELD(peb, PEB_OS_BUILD, u16) = (u16)kuser32(KUSER_NT_BUILD_NUMBER);
	FIELD(peb, PEB_OS_PLATFORM, u32) = 2;   /* VER_PLATFORM_WIN32_NT */

	memcpy(cmdline_a, p->command_line, sizeof(cmdline_a) - 1);
	ascii_to_wide(cmdline_w, cmdline_a, 256);
	{
		/* The image path is the first word of the command line. */
		size_t i = 0;

		while (cmdline_w[i] && cmdline_w[i] != ' ' && i < 255) {
			image_path_w[i] = cmdline_w[i];
			i++;
		}
	}
	FIELD(proc_params, 0x20, u64) = 4;      /* StandardInput / Output / Error */
	FIELD(proc_params, 0x28, u64) = 8;
	FIELD(proc_params, 0x30, u64) = 0xC;
	set_ustr(proc_params + 0x60, image_path_w);
	set_ustr(proc_params + 0x70, cmdline_w);

	/* TEB.StaticUnicodeString over StaticUnicodeBuffer: Wine's ANSI wrappers convert in it. */
	FIELD(teb, 0x1258 + 2, u16) = 522;
	FIELD(teb, 0x1258 + 8, u64) = (u64)(teb + 0x268);
	nt_peb = peb;
	ldr_init();
	nt_init_process();
	nt_sync_init();
	ldr_process_start();   /* binds imports, runs DllMains, calls the entry point */
}

/* Bound by the loader to every import nobody implements. */
static API void ntrt_missing(const char *what) __attribute__((used, noreturn));
static API void ntrt_missing(const char *what)
{
	static const char pre[] = "missing import ";

	rt_write(2, pre, sizeof(pre) - 1);
	rt_write(2, what, rt_strlen(what));
	rt_write(2, "\n", 1);
	rt_exit(NTRT_STATUS_MISSING_IMPORT);
}

extern const struct ntrt_export __exports_start[], __exports_end[];
extern const u8 __text_end[], __image_end[], __file_end[];

__attribute__((section(".ntrt_header"), used))
static const struct ntrt_header header = {
	.magic = NTRT_MAGIC,
	.text_end = (unsigned long long)__text_end,
	.image_end = (unsigned long long)__image_end,
	.file_end = (unsigned long long)__file_end,
	.start = (unsigned long long)ntrt_start,
	.missing = (unsigned long long)ntrt_missing,
	.teb = (unsigned long long)teb,
	.exports = (unsigned long long)__exports_start,
	.exports_end = (unsigned long long)__exports_end,
};

u64 rt_lookup(const char *name)
{
	for (const struct ntrt_export *e = __exports_start; e < __exports_end; e++) {
		const char *n = (const char *)e->name;
		size_t i = 0;

		while (n[i] && n[i] == name[i])
			i++;
		if (n[i] == name[i])
			return e->addr;
	}
	return 0;
}

/* ---- kernel32: process, handles, console I/O ------------------------------- */

static API void w_ExitProcess(u32 code)
{
	rt_exit(code);
}
EXPORT(ExitProcess);

static API u64 w_GetStdHandle(u32 which)
{
	switch (which) {
	case STD_INPUT_HANDLE:  return 4;
	case STD_OUTPUT_HANDLE: return 8;
	case STD_ERROR_HANDLE:  return 0xC;
	default:
		rt_set_last_error(ERROR_INVALID_HANDLE);
		return ~0ull;
	}
}
EXPORT(GetStdHandle);

static API BOOL w_WriteFile(u64 h, const void *buf, u32 n, u32 *written, void *ov)
{
	long r = handle_fd(h) > 0 ? rt_write(handle_fd(h), buf, n) : -1;

	(void)ov;
	if (written)
		*written = r < 0 ? 0 : (u32)r;
	if (r < 0)
		rt_set_last_error(ERROR_INVALID_HANDLE);
	return r >= 0;
}
EXPORT(WriteFile);

static API BOOL w_WriteConsoleA(u64 h, const void *buf, u32 n, u32 *written, void *rsv)
{
	return w_WriteFile(h, buf, n, written, rsv);
}
EXPORT(WriteConsoleA);

static API BOOL w_WriteConsoleW(u64 h, const wchar *buf, u32 n, u32 *written, void *rsv)
{
	char tmp[256];
	u32 done = 0;

	(void)rsv;
	while (done < n) {
		u32 chunk = n - done > 64 ? 64 : n - done;
		size_t len;

		/* Keep surrogate pairs together. */
		if (chunk == 64 && buf[done + 63] >= 0xD800 && buf[done + 63] < 0xDC00)
			chunk--;
		len = rt_utf8_from_wide(tmp, sizeof(tmp), buf + done, chunk);
		if (handle_fd(h) <= 0 || rt_write(handle_fd(h), tmp, len) < 0) {
			rt_set_last_error(ERROR_INVALID_HANDLE);
			return 0;
		}
		done += chunk;
	}
	if (written)
		*written = n;
	return 1;
}
EXPORT(WriteConsoleW);

static API BOOL w_ReadFile(u64 h, void *buf, u32 n, u32 *got, void *ov)
{
	u64 iosb[2] = { 0, 0 };
	NTSTATUS st;

	(void)ov;
	st = NtReadFile(h, 0, NULL, NULL, iosb, buf, n, NULL, NULL);
	if (got)
		*got = st ? 0 : (u32)iosb[1];
	if (st)
		rt_set_last_error(ERROR_INVALID_HANDLE);
	return !st;
}
EXPORT(ReadFile);

static API BOOL w_GetConsoleMode(u64 h, u32 *mode)
{
	if (handle_fd(h) < 0)
		return 0;
	if (mode)
		*mode = 3;
	return 1;
}
EXPORT(GetConsoleMode);

static API u32 w_GetFileType(u64 h)
{
	return handle_fd(h) >= 0 ? 2 /* FILE_TYPE_CHAR */ : 0;
}
EXPORT(GetFileType);

static API BOOL w_CloseHandle(u64 h)
{
	(void)h;
	return 1;
}
EXPORT(CloseHandle);

static API u64 w_GetCurrentProcess(void) { return CURRENT_PROCESS; }
EXPORT(GetCurrentProcess);
static API u64 w_GetCurrentThread(void) { return ~1ull; }
EXPORT(GetCurrentThread);
static API u32 w_GetCurrentProcessId(void) { return rt_params->pid; }
EXPORT(GetCurrentProcessId);
static API u32 w_GetCurrentThreadId(void) { return rt_params->tid; }
EXPORT(GetCurrentThreadId);
static API u32 w_GetLastError(void) { return FIELD(teb, TEB_LAST_ERROR, u32); }
EXPORT(GetLastError);
static API void w_SetLastError(u32 e) { rt_set_last_error(e); }
EXPORT(SetLastError);
static API BOOL w_IsDebuggerPresent(void) { return 0; }
EXPORT(IsDebuggerPresent);
static API const char *w_GetCommandLineA(void) { return cmdline_a; }
EXPORT(GetCommandLineA);
static API const wchar *w_GetCommandLineW(void) { return cmdline_w; }
EXPORT(GetCommandLineW);
static API u32 w_GetACP(void) { return 65001; }        /* CP_UTF8 */
EXPORT(GetACP);
static API u32 w_GetOEMCP(void) { return 65001; }
EXPORT(GetOEMCP);
static API u32 w_GetConsoleOutputCP(void) { return 65001; }
EXPORT(GetConsoleOutputCP);
static API BOOL w_IsDBCSLeadByteEx(u32 cp, u8 c) { (void)cp; (void)c; return 0; }
EXPORT(IsDBCSLeadByteEx);

/* No exceptions are dispatched to user mode yet: the filter is only stored. */
static void *unhandled_filter;
static API void *w_SetUnhandledExceptionFilter(void *f)
{
	void *old = unhandled_filter;

	unhandled_filter = f;
	return old;
}
EXPORT(SetUnhandledExceptionFilter);

/* Single thread: there is nobody to wait for. */
static API void w_Sleep(u32 ms) { (void)ms; }
EXPORT(Sleep);

static API void w_GetStartupInfoA(u8 *si)
{
	memset(si, 0, 104);
	FIELD(si, 0, u32) = 104;
	FIELD(si, 0x50, u64) = 4;
	FIELD(si, 0x58, u64) = 8;
	FIELD(si, 0x60, u64) = 0xC;
}
EXPORT(GetStartupInfoA);
EXPORT_AS("GetStartupInfoW", w_GetStartupInfoA);      /* same layout, no strings set */

/* ---- modules ------------------------------------------------------------------ */

/* Modules the runtime stands in for. Their "handle" is the runtime's base. */
static bool known_module(const wchar *w, const char *a)
{
	static const char *const names[] = {
		"kernel32", "kernelbase", "ntdll", "msvcrt", "ucrtbase", "advapi32", "secur32",
	};
	char buf[32];
	size_t n = 0;

	for (; n < sizeof(buf) - 1 && (w ? w[n] : a[n]) && (w ? w[n] : a[n]) != '.'; n++)
		buf[n] = (char)rt_lower(w ? (int)w[n] : a[n]);
	buf[n] = 0;
	if (!memcmp(buf, "api-ms-win-", 11))
		return true;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (rt_strlen(names[i]) == n && !memcmp(buf, names[i], n))
			return true;
	return false;
}

static API u64 w_GetModuleHandleW(const wchar *name)
{
	if (!name)
		return rt_params->image_base;
	if (known_module(name, NULL))
		return NTRT_BASE;
	rt_set_last_error(ERROR_MOD_NOT_FOUND);
	return 0;
}
EXPORT(GetModuleHandleW);
EXPORT_AS("LoadLibraryW", w_GetModuleHandleW);

static API u64 w_GetModuleHandleA(const char *name)
{
	if (!name)
		return rt_params->image_base;
	if (known_module(NULL, name))
		return NTRT_BASE;
	rt_set_last_error(ERROR_MOD_NOT_FOUND);
	return 0;
}
EXPORT(GetModuleHandleA);
EXPORT_AS("LoadLibraryA", w_GetModuleHandleA);

static API BOOL w_FreeLibrary(u64 h) { (void)h; return 1; }
EXPORT(FreeLibrary);

static API u64 w_GetProcAddress(u64 module, const char *name)
{
	u64 a = 0;

	if (module == NTRT_BASE && (u64)name > 0xFFFF)
		a = rt_lookup(name);
	if (!a)
		rt_set_last_error(ERROR_PROC_NOT_FOUND);
	return a;
}
EXPORT(GetProcAddress);

static API u32 w_GetModuleFileNameW(u64 module, wchar *buf, u32 cap)
{
	u32 n = 0;

	(void)module;
	if (!cap)
		return 0;
	while (image_path_w[n] && n + 1 < cap) {
		buf[n] = image_path_w[n];
		n++;
	}
	buf[n] = 0;
	return n;
}
EXPORT(GetModuleFileNameW);

/* ---- memory ------------------------------------------------------------------- */

static API u64 w_GetProcessHeap(void) { return (u64)free_list; }
EXPORT(GetProcessHeap);

static API void *w_HeapAlloc(u64 heap, u32 flags, size_t n)
{
	void *p = rt_alloc(n);

	(void)heap;
	(void)flags;            /* blocks are always zeroed */
	if (!p)
		rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
	return p;
}
EXPORT(HeapAlloc);

static API BOOL w_HeapFree(u64 heap, u32 flags, void *p)
{
	(void)heap;
	(void)flags;
	rt_free(p);
	return 1;
}
EXPORT(HeapFree);

static API void *w_HeapReAlloc(u64 heap, u32 flags, void *p, size_t n)
{
	(void)heap;
	(void)flags;
	return rt_realloc(p, n);
}
EXPORT(HeapReAlloc);

static API size_t w_HeapSize(u64 heap, u32 flags, void *p)
{
	(void)heap;
	(void)flags;
	return rt_size(p);
}
EXPORT(HeapSize);

static API void *w_LocalAlloc(u32 flags, size_t n) { (void)flags; return rt_alloc(n); }
EXPORT(LocalAlloc);
static API void *w_LocalFree(void *p) { rt_free(p); return NULL; }
EXPORT(LocalFree);

static API void *w_VirtualAlloc(void *addr, size_t n, u32 type, u32 prot)
{
	void *base = addr;
	size_t size = n;

	if (NtAllocateVirtualMemory(CURRENT_PROCESS, &base, 0, &size, type, prot)) {
		rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	return base;
}
EXPORT(VirtualAlloc);

static API BOOL w_VirtualFree(void *addr, size_t n, u32 type)
{
	void *base = addr;
	size_t size = n;

	return !NtFreeVirtualMemory(CURRENT_PROCESS, &base, &size, type);
}
EXPORT(VirtualFree);

static API BOOL w_VirtualProtect(void *addr, size_t n, u32 prot, u32 *old)
{
	void *base = addr;
	size_t size = n;

	return !NtProtectVirtualMemory(CURRENT_PROCESS, &base, &size, prot, old);
}
EXPORT(VirtualProtect);

/* MEMORY_BASIC_INFORMATION for the page holding `addr`. Coarse: no VAD tree. */
static API size_t w_VirtualQuery(const void *addr, u8 *mbi, size_t len)
{
	u64 page = (u64)addr & ~0xFFFull;

	if (len < 48)
		return 0;
	memset(mbi, 0, 48);
	FIELD(mbi, 0, u64) = page;              /* BaseAddress       */
	FIELD(mbi, 8, u64) = page;              /* AllocationBase    */
	FIELD(mbi, 16, u32) = PAGE_EXECUTE_READWRITE;
	FIELD(mbi, 24, u64) = 0x1000;           /* RegionSize        */
	FIELD(mbi, 32, u32) = MEM_COMMIT;       /* State             */
	FIELD(mbi, 36, u32) = PAGE_EXECUTE_READWRITE;
	FIELD(mbi, 40, u32) = 0x20000;          /* MEM_PRIVATE       */
	return 48;
}
EXPORT(VirtualQuery);

/* ---- time ----------------------------------------------------------------------- */

static API void w_GetSystemTimeAsFileTime(u64 *ft)
{
	s64 t = 0;

	NtQuerySystemTime(&t);
	*ft = (u64)t;
}
EXPORT(GetSystemTimeAsFileTime);

static API BOOL w_QueryPerformanceCounter(s64 *c) { return !NtQueryPerformanceCounter(c, NULL); }
EXPORT(QueryPerformanceCounter);

static API BOOL w_QueryPerformanceFrequency(s64 *f)
{
	s64 c;

	return !NtQueryPerformanceCounter(&c, f);
}
EXPORT(QueryPerformanceFrequency);

static API u64 w_GetTickCount64(void)
{
	s64 c = 0, f = 1;

	NtQueryPerformanceCounter(&c, &f);
	return f ? (u64)c / ((u64)f / 1000) : 0;
}
EXPORT(GetTickCount64);
EXPORT_AS("GetTickCount", w_GetTickCount64);  /* the u32 return truncates in eax */

/* ---- synchronisation and TLS: one thread, so no contention ----------------- */

static API void w_InitializeCriticalSection(u8 *cs) { memset(cs, 0, 40); }
EXPORT(InitializeCriticalSection);
static API BOOL w_InitializeCriticalSectionAndSpinCount(u8 *cs, u32 spin)
{
	(void)spin;
	memset(cs, 0, 40);
	return 1;
}
EXPORT(InitializeCriticalSectionAndSpinCount);
static API void w_EnterCriticalSection(u8 *cs) { FIELD(cs, 8, u32)++; }
EXPORT(EnterCriticalSection);
static API void w_LeaveCriticalSection(u8 *cs) { FIELD(cs, 8, u32)--; }
EXPORT(LeaveCriticalSection);
static API BOOL w_TryEnterCriticalSection(u8 *cs) { FIELD(cs, 8, u32)++; return 1; }
EXPORT(TryEnterCriticalSection);
static API void w_DeleteCriticalSection(u8 *cs) { (void)cs; }
EXPORT(DeleteCriticalSection);

static API u32 w_TlsAlloc(void)
{
	for (u32 i = 0; i < TLS_SLOTS; i++)
		if (!(tls_used & (1ull << i))) {
			tls_used |= 1ull << i;
			((void **)(teb + TEB_TLS_SLOTS))[i] = NULL;
			return i;
		}
	return 0xFFFFFFFFu;     /* TLS_OUT_OF_INDEXES */
}
EXPORT(TlsAlloc);

static API BOOL w_TlsFree(u32 i)
{
	if (i >= TLS_SLOTS)
		return 0;
	tls_used &= ~(1ull << i);
	return 1;
}
EXPORT(TlsFree);

static API void *w_TlsGetValue(u32 i)
{
	if (i >= TLS_SLOTS) {
		rt_set_last_error(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	rt_set_last_error(0);
	return ((void **)(teb + TEB_TLS_SLOTS))[i];
}
EXPORT(TlsGetValue);

static API BOOL w_TlsSetValue(u32 i, void *v)
{
	if (i >= TLS_SLOTS)
		return 0;
	((void **)(teb + TEB_TLS_SLOTS))[i] = v;
	return 1;
}
EXPORT(TlsSetValue);

/* ---- version and environment ------------------------------------------------- */

/* RTL_OSVERSIONINFOW / OSVERSIONINFOEXW: size, major, minor, build, platform, CSD. */
static API NTSTATUS w_RtlGetVersion(u32 *v)
{
	if (v[0] < 276)
		return 0xC000000Du;     /* STATUS_INVALID_PARAMETER */
	v[1] = kuser32(KUSER_NT_MAJOR_VERSION);
	v[2] = kuser32(KUSER_NT_MINOR_VERSION);
	v[3] = kuser32(KUSER_NT_BUILD_NUMBER) & 0xFFFF;
	v[4] = 2;                       /* VER_PLATFORM_WIN32_NT */
	memset(v + 5, 0, 256);          /* szCSDVersion: no service pack */
	if (v[0] >= 284) {              /* ...EX: service pack, suite, product type */
		memset((u8 *)v + 276, 0, 8);
		((u8 *)v)[282] = 1;     /* VER_NT_WORKSTATION */
	}
	return 0;
}
EXPORT(RtlGetVersion);

static API BOOL w_GetVersionExW(u32 *v)
{
	if (w_RtlGetVersion(v)) {
		rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	return 1;
}
EXPORT(GetVersionExW);

/* OSVERSIONINFOA has a 128-byte CSD string instead of 128 wide characters. */
static API BOOL w_GetVersionExA(u32 *v)
{
	if (v[0] < 148) {
		rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	v[1] = kuser32(KUSER_NT_MAJOR_VERSION);
	v[2] = kuser32(KUSER_NT_MINOR_VERSION);
	v[3] = kuser32(KUSER_NT_BUILD_NUMBER) & 0xFFFF;
	v[4] = 2;
	memset(v + 5, 0, 128);
	return 1;
}
EXPORT(GetVersionExA);

static API u32 w_GetVersion(void)
{
	return kuser32(KUSER_NT_MAJOR_VERSION) | kuser32(KUSER_NT_MINOR_VERSION) << 8 |
	       (kuser32(KUSER_NT_BUILD_NUMBER) & 0x7FFF) << 16;
}
EXPORT(GetVersion);

/* The environment block is empty. */
static API u32 w_GetEnvironmentVariableW(const wchar *name, wchar *buf, u32 cap)
{
	(void)name;
	if (buf && cap)
		buf[0] = 0;
	rt_set_last_error(ERROR_ENVVAR_NOT_FOUND);
	return 0;
}
EXPORT(GetEnvironmentVariableW);
EXPORT_AS("GetEnvironmentVariableA", w_GetEnvironmentVariableW);

/* Copy an ASCII identity string out, in the GetComputerName convention. */
static BOOL name_out(const char *s, wchar *w, char *a, u32 *cap, u32 overflow_error)
{
	u32 n = (u32)rt_strlen(s);

	if (!cap)
		return 0;
	if (*cap <= n || (!w && !a)) {
		*cap = n + 1;
		rt_set_last_error(overflow_error);
		return 0;
	}
	for (u32 i = 0; i <= n; i++) {
		if (w)
			w[i] = (u8)s[i];
		else
			a[i] = s[i];
	}
	*cap = n;
	return 1;
}

static API BOOL w_GetComputerNameW(wchar *buf, u32 *cap)
{
	return name_out(rt_params->computer_name, buf, NULL, cap, ERROR_BUFFER_OVERFLOW);
}
EXPORT(GetComputerNameW);

static API BOOL w_GetComputerNameA(char *buf, u32 *cap)
{
	return name_out(rt_params->computer_name, NULL, buf, cap, ERROR_BUFFER_OVERFLOW);
}
EXPORT(GetComputerNameA);

/*
 * secur32!GetUserNameExW. There are no user accounts behind the NT
 * personality: every process runs as the one local account, "user".
 */
static API BOOL w_GetUserNameExW(u32 format, wchar *buf, u32 *cap)
{
	char full[48];
	size_t n = rt_strlen(rt_params->computer_name);

	if (format != 2 /* NameSamCompatible */) {
		rt_set_last_error(ERROR_NOT_SUPPORTED);
		return 0;
	}
	memcpy(full, rt_params->computer_name, n);
	memcpy(full + n, "\\user", 6);
	return name_out(full, buf, NULL, cap, ERROR_MORE_DATA);
}
EXPORT(GetUserNameExW);

static API BOOL w_GetUserNameW(wchar *buf, u32 *cap)
{
	if (!name_out("user", buf, NULL, cap, ERROR_INSUFFICIENT_BUFFER))
		return 0;
	(*cap)++;               /* GetUserName counts the terminator */
	return 1;
}
EXPORT(GetUserNameW);

/* ---- code pages: UTF-8 (and ASCII, a subset) only ---------------------------- */

static API int w_MultiByteToWideChar(u32 cp, u32 flags, const char *s, int n, wchar *out, int cap)
{
	size_t len, need;

	(void)cp;
	(void)flags;
	len = n < 0 ? rt_strlen(s) + 1 : (size_t)n;
	need = wide_from_utf8(NULL, 0, (const u8 *)s, len);
	if (!cap)
		return (int)need;
	if (need > (size_t)cap) {
		rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	return (int)wide_from_utf8(out, (size_t)cap, (const u8 *)s, len);
}
EXPORT(MultiByteToWideChar);

static API int w_WideCharToMultiByte(u32 cp, u32 flags, const wchar *s, int n, char *out, int cap,
				     const char *def, BOOL *used_def)
{
	size_t len, need;

	(void)cp;
	(void)flags;
	(void)def;
	if (used_def)
		*used_def = 0;
	len = n < 0 ? rt_wcslen(s) + 1 : (size_t)n;
	need = rt_utf8_from_wide(NULL, 0, s, len);
	if (!cap)
		return (int)need;
	if (need > (size_t)cap) {
		rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	return (int)rt_utf8_from_wide(out, (size_t)cap, s, len);
}
EXPORT(WideCharToMultiByte);

/* ---- ntdll: the system-call stubs are importable as well -------------------- */

EXPORT_AS("NtReadFile", NtReadFile);
EXPORT_AS("NtWriteFile", NtWriteFile);
EXPORT_AS("NtTerminateProcess", NtTerminateProcess);
EXPORT_AS("NtAllocateVirtualMemory", NtAllocateVirtualMemory);
EXPORT_AS("NtFreeVirtualMemory", NtFreeVirtualMemory);
EXPORT_AS("NtProtectVirtualMemory", NtProtectVirtualMemory);
EXPORT_AS("NtQueryPerformanceCounter", NtQueryPerformanceCounter);
EXPORT_AS("NtQuerySystemTime", NtQuerySystemTime);
