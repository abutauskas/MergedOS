/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Internal declarations of the built-in Win32 runtime ("ntrt").
 *
 * Everything an application can import is a static function with the
 * Microsoft x64 calling convention, registered under its Win32 name with
 * EXPORT(). Helpers shared between the files use the normal SysV ABI.
 */
#ifndef NTRT_RT_H
#define NTRT_RT_H

#include "src/ntrt/ntrt_abi.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned long long size_t;
typedef unsigned short wchar;
typedef int BOOL;
typedef _Bool bool;
#define true 1
#define false 0
typedef u32 NTSTATUS;

#define API __attribute__((ms_abi))
#define NULL ((void *)0)

/* EXPORT(Foo) publishes w_Foo as "Foo"; EXPORT_AS for names C cannot spell. */
#define EXPORT_AS(name, sym) EXPORT_AS_(name, sym, __COUNTER__)
#define EXPORT_AS_(name, sym, n) EXPORT_AS__(name, sym, n)
#define EXPORT_AS__(name, sym, n)                                                \
	static const struct ntrt_export exp_##n                                  \
	__attribute__((used, section(".ntrt_exports"), aligned(8))) = {          \
		(unsigned long long)(name), (unsigned long long)(sym) }
#define EXPORT(sym) EXPORT_AS(#sym, w_##sym)

/* ms_abi variadic arguments are an array of 8-byte slots. */
typedef __builtin_ms_va_list ms_va_list;
#define ms_va_start(ap, last) __builtin_ms_va_start(ap, last)
#define ms_va_end(ap)         __builtin_ms_va_end(ap)

/* ntdll system-call stubs (start.S). Numbers: abi_dispatch/personality_nt.c. */
API NTSTATUS NtReadFile(u64 h, u64 ev, void *apc, void *ctx, u64 *iosb, void *buf, u32 len,
			s64 *off, u32 *key);
API NTSTATUS NtWriteFile(u64 h, u64 ev, void *apc, void *ctx, u64 *iosb, const void *buf,
			 u32 len, s64 *off, u32 *key);
API NTSTATUS NtTerminateProcess(u64 h, NTSTATUS status);
API NTSTATUS NtAllocateVirtualMemory(u64 h, void **base, u64 zero, size_t *size, u32 type,
				     u32 prot);
API NTSTATUS NtFreeVirtualMemory(u64 h, void **base, size_t *size, u32 type);
API NTSTATUS NtProtectVirtualMemory(u64 h, void **base, size_t *size, u32 prot, u32 *old);
API NTSTATUS NtQueryPerformanceCounter(s64 *counter, s64 *freq);
API NTSTATUS NtQuerySystemTime(s64 *time);

/* Shared state and helpers. */
extern struct ntrt_params *rt_params;
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t rt_strlen(const char *s);
size_t rt_wcslen(const wchar *s);
int rt_lower(int c);
void *rt_alloc(size_t n);
void rt_free(void *p);
void *rt_realloc(void *p, size_t n);
size_t rt_size(void *p);
void rt_set_last_error(u32 e);
long rt_write(int fd, const void *buf, size_t n);
void rt_putc(int fd, char c);
void rt_flush(void);
void rt_exit(u32 code) __attribute__((noreturn));
void rt_run_atexit(void);
u64 rt_lookup(const char *name);
u64 rt_teb(void);
const char *rt_cmdline(void);
const wchar *rt_wcmdline(void);
size_t rt_utf8_from_wide(char *out, size_t cap, const wchar *in, size_t n);

/*
 * The printf engine (crt.c). Output goes to a sink: a narrow or wide
 * buffer, or (both NULL) a file descriptor. len counts every unit, even
 * those past cap. Variadic arguments are 8-byte slots (ms_abi va_list).
 */
struct rt_sink {
	char *buf;
	wchar *wbuf;
	size_t cap, len;
	int fd;
	size_t tlen;
	char tmp[128];
};
void rt_format(struct rt_sink *s, const void *fmt, bool wide_fmt, const u64 *args);
void rt_sink_flush(struct rt_sink *s);

#endif /* NTRT_RT_H */
