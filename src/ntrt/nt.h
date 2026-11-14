/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * NT types and internal interfaces of MergedOS's ntdll (the user-space half
 * of the NT personality): loader, handles, files, registry, sync objects.
 * Layouts are the documented x64 ones.
 */
#ifndef NTRT_NT_H
#define NTRT_NT_H

#include "src/ntrt/rt.h"

typedef struct list_entry {
	struct list_entry *next, *prev;
} LIST_ENTRY;

typedef struct {
	u16 len, max;                   /* bytes, without / with room for NUL */
	wchar *buf;
} UNICODE_STRING;

typedef struct {
	u16 len, max;
	char *buf;
} ANSI_STRING;

typedef struct {
	u32 len;
	u64 root;
	UNICODE_STRING *name;
	u32 attrs;
	void *sd, *sqos;
} OBJECT_ATTRIBUTES;

typedef struct {
	union {
		NTSTATUS status;
		void *ptr;
	};
	u64 info;
} IO_STATUS_BLOCK;

/* NTSTATUS values used by more than one file. */
#define STATUS_SUCCESS                0x00000000u
#define STATUS_TIMEOUT                0x00000102u
#define STATUS_PENDING                0x00000103u
#define STATUS_MORE_ENTRIES           0x00000105u
#define STATUS_OBJECT_NAME_EXISTS     0x40000000u
#define STATUS_BUFFER_OVERFLOW        0x80000005u
#define STATUS_NO_MORE_FILES          0x80000006u
#define STATUS_NO_MORE_ENTRIES        0x8000001Au
#define STATUS_UNSUCCESSFUL           0xC0000001u
#define STATUS_NOT_IMPLEMENTED        0xC0000002u
#define STATUS_INVALID_INFO_CLASS     0xC0000003u
#define STATUS_INFO_LENGTH_MISMATCH   0xC0000004u
#define STATUS_ACCESS_VIOLATION       0xC0000005u
#define STATUS_INVALID_HANDLE         0xC0000008u
#define STATUS_INVALID_PARAMETER      0xC000000Du
#define STATUS_NO_SUCH_FILE           0xC000000Fu
#define STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define STATUS_END_OF_FILE            0xC0000011u
#define STATUS_NO_MEMORY              0xC0000017u
#define STATUS_ACCESS_DENIED          0xC0000022u
#define STATUS_BUFFER_TOO_SMALL       0xC0000023u
#define STATUS_OBJECT_TYPE_MISMATCH   0xC0000024u
#define STATUS_OBJECT_NAME_INVALID    0xC0000033u
#define STATUS_OBJECT_NAME_NOT_FOUND  0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION  0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND  0xC000003Au
#define STATUS_MUTANT_NOT_OWNED       0xC0000046u
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047u
#define STATUS_INVALID_IMAGE_FORMAT   0xC000007Bu
#define STATUS_NO_TOKEN               0xC000007Cu
#define STATUS_FILE_IS_A_DIRECTORY    0xC00000BAu
#define STATUS_NOT_SUPPORTED          0xC00000BBu
#define STATUS_INTERNAL_ERROR         0xC00000E5u
#define STATUS_DIRECTORY_NOT_EMPTY    0xC0000101u
#define STATUS_NOT_A_DIRECTORY        0xC0000103u
#define STATUS_DLL_NOT_FOUND          0xC0000135u
#define STATUS_ENTRYPOINT_NOT_FOUND   0xC0000139u
#define STATUS_DLL_INIT_FAILED        0xC0000142u
#define STATUS_KEY_DELETED            0xC000017Cu
#define STATUS_MESSAGE_NOT_FOUND      0xC0000109u
#define STATUS_NOT_FOUND              0xC0000225u
#define STATUS_RESOURCE_DATA_NOT_FOUND 0xC0000089u
#define STATUS_RESOURCE_TYPE_NOT_FOUND 0xC000008Au
#define STATUS_RESOURCE_NAME_NOT_FOUND 0xC000008Bu
#define STATUS_RESOURCE_LANG_NOT_FOUND 0xC0000204u

#define NT_ERROR(s) ((s) >= 0xC0000000u)

/* MergedOS kernel services (start.S): a value >= 0 or an NTSTATUS error. */
API long mos_open(const char *path, u32 flags);
API long mos_close(long fd);
API long mos_read(long fd, void *buf, u64 len);
API long mos_write(long fd, const void *buf, u64 len);
API long mos_seek(long fd, s64 off, int whence);
API long mos_fstat(long fd, u64 out[2]);
API long mos_stat(const char *path, u64 out[2]);
API long mos_readdir(long fd, char *name, u64 cap);
API long mos_mkdir(const char *path);
API long mos_unlink(const char *path, long dir);
API long mos_rename(const char *from, const char *to);
API long mos_futex(void *addr, long op, u32 val, void *timespec);
API long mos_sleep(u64 ns);
API long mos_dup(long fd);
API long mos_gettid(void);
API NTSTATUS NtCreateThreadEx(u64 *h, u32 access, void *oa, u64 process, void *start, void *arg,
			      u32 flags, u64 zero, u64 stack, u64 maxstack, void *attrs);
API NTSTATUS NtWaitForSingleObject(u64 h, u8 alertable, s64 *timeout);
API NTSTATUS NtTerminateThread(u64 h, NTSTATUS status);
#define MOS_ERR(r) ((r) < 0 && (r) >= -0x80000000L)
#define MOS_O_RDONLY 0
#define MOS_O_WRONLY 1
#define MOS_O_RDWR   2
#define MOS_O_CREAT  0x40
#define MOS_O_EXCL   0x80
#define MOS_O_TRUNC  0x200
#define MOS_O_APPEND 0x400
#define MOS_O_DIRECTORY 0x10000
#define MOS_ST_FILE 0
#define MOS_ST_DIR  1
#define MOS_ST_TTY  2
#define MOS_ST_FIFO 3
#define MOS_ST_FILE_RW 4

static inline int nt_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (u8)*a - (u8)*b;
}

static inline size_t nt_strlcpy(char *d, const char *s, size_t cap)
{
	size_t n = 0;

	for (; s[n]; n++)
		if (n + 1 < cap)
			d[n] = s[n];
	if (cap)
		d[n < cap ? n : cap - 1] = 0;
	return n;
}

static inline const char *nt_strchr(const char *s, char c)
{
	for (; *s; s++)
		if (*s == c)
			return s;
	return NULL;
}

/* Loader (ldr.c). */
void ldr_set_debug(bool on);
u64 ldr_load(const char *name, NTSTATUS *st);
u64 ldr_handle(const char *name);
u32 ldr_module_path(u64 module, wchar *buf, u32 cap);
NTSTATUS ldr_find_resource(u64 base, const u64 *info, u32 level, u8 **out);
struct ldr_mod;
void ldr_init(void);
void ldr_process_start(void) __attribute__((noreturn));
u64 ldr_proc(u64 module, const char *name, u16 ord);
u64 ldr_module_of(u64 addr);
u64 ldr_image_base(void);
const wchar *ldr_image_path(void);
void ldr_thread_attach(void);

/* Process environment (ntdll.c). */
extern u8 *nt_peb;
u8 *nt_teb(void);
void nt_init_process(void);
void nt_set_image_path(const wchar *path);
const wchar *nt_curdir(void);
void nt_set_curdir(const wchar *dir);
u8 *nt_params(void);
void *nt_nls(u32 type, u32 id, u64 *size);
void *nt_realloc_(void *p, size_t n);
void *nt_alloc(size_t n);               /* zeroed heap memory */
void nt_free(void *p);
NTSTATUS nt_vm_alloc(void **base, size_t size);
const char *nt_status_name(NTSTATUS s);
void nt_set_trace(bool on);
void nt_trace_line(const char *what, const UNICODE_STRING *name, NTSTATUS st);

/* Paths (rtl.c): NT or DOS path -> absolute UTF-8 VFS path. */
NTSTATUS nt_unix_path(const UNICODE_STRING *name, u64 root, char *out, size_t cap);
size_t nt_wtou(char *out, size_t cap, const wchar *in, size_t n);     /* UTF-16 -> UTF-8 */
size_t nt_utow(wchar *out, size_t cap, const char *in, size_t n);     /* UTF-8 -> UTF-16 */
int nt_wcsicmp(const wchar *a, const wchar *b);
int nt_wcsnicmp(const wchar *a, const wchar *b, size_t n);
wchar nt_upcase(wchar c);
wchar nt_downcase(wchar c);
void nt_init_ustr(UNICODE_STRING *u, const wchar *s);
int nt_stricmp(const char *a, const char *b);

/* Handles (ntdll.c). */
enum nt_kind { NTH_FREE, NTH_FILE, NTH_DIR, NTH_KEY, NTH_EVENT, NTH_MUTANT, NTH_SEMAPHORE,
	       NTH_THREAD, NTH_OTHER };
struct nt_obj {
	enum nt_kind kind;
	u32 refs;
	long fd;                        /* files, directories      */
	char *path;                     /* UTF-8 VFS path, or NULL */
	wchar *ntname;                  /* NT name for files/keys  */
	u32 access, options;
	bool delete_on_close;
	long dirpos;                    /* NtQueryDirectoryFile    */
	wchar *mask;
	/* sync objects */
	volatile u32 state;             /* event signalled, semaphore count, mutant recursion */
	u32 max, owner;
	bool manual;
	u64 tid;                        /* threads */
};
u64 nt_handle_new(struct nt_obj *o);
struct nt_obj *nt_handle_get(u64 h);
struct nt_obj *nt_obj_new(enum nt_kind kind);
void nt_obj_put(struct nt_obj *o);

/* Registry (ntreg.c). */
NTSTATUS nt_reg_close(struct nt_obj *o);
void nt_reg_init(void);

/* Sync (ntsync.c). */
void nt_sync_wake_all(void);
NTSTATUS nt_wait_objects(u32 n, const u64 *h, bool all, const s64 *timeout);
void nt_sync_init(void);
void nt_lock(volatile u32 *l);
void nt_unlock(volatile u32 *l);

/* Exceptions (ntexc.c). */
void nt_exc_init(void);

#endif /* NTRT_NT_H */
