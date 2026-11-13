/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS ntdll, part 1: process environment (PEB, process parameters,
 * environment block), the handle table, files, virtual memory, system and
 * process information, NLS tables, Wine's debug-channel hooks.
 * Spec: docs/NT_USERSPACE.md, part 2. Written from the documented NT API.
 */
#include "src/ntrt/nt.h"

#define CURRENT_PROCESS ((u64)-1)
#define CURRENT_THREAD  ((u64)-2)
#define MAX_HANDLES     1024

u8 *nt_peb;

u8 *nt_teb(void)
{
	u8 *t;

	__asm__ volatile("movq %%gs:0x30, %0" : "=r"(t));
	return t;
}

/* ---- heap glue ------------------------------------------------------------------ */

static volatile u32 heap_lock;

void *nt_alloc(size_t n)
{
	void *p;

	nt_lock(&heap_lock);
	p = rt_alloc(n ? n : 1);
	nt_unlock(&heap_lock);
	return p;
}

void nt_free(void *p)
{
	nt_lock(&heap_lock);
	rt_free(p);
	nt_unlock(&heap_lock);
}

static void *nt_realloc(void *p, size_t n)
{
	nt_lock(&heap_lock);
	p = rt_realloc(p, n);
	nt_unlock(&heap_lock);
	return p;
}

NTSTATUS nt_vm_alloc(void **base, size_t size)
{
	*base = NULL;
	return NtAllocateVirtualMemory(CURRENT_PROCESS, base, 0, &size, 0x3000, 4);
}

/* ---- handles -------------------------------------------------------------------- */

static struct nt_obj *htab[MAX_HANDLES];
static volatile u32 htab_lock;

struct nt_obj *nt_obj_new(enum nt_kind kind)
{
	struct nt_obj *o = nt_alloc(sizeof(*o));

	if (o) {
		o->kind = kind;
		o->refs = 1;
		o->fd = -1;
	}
	return o;
}

u64 nt_handle_new(struct nt_obj *o)
{
	nt_lock(&htab_lock);
	for (u32 i = 3; i < MAX_HANDLES; i++)
		if (!htab[i]) {
			htab[i] = o;
			nt_unlock(&htab_lock);
			return (u64)(i + 1) * 4;
		}
	nt_unlock(&htab_lock);
	return 0;
}

struct nt_obj *nt_handle_get(u64 h)
{
	u64 i = h / 4 - 1;

	if (!h || (h & 3) || i >= MAX_HANDLES)
		return NULL;
	return htab[i];
}

void nt_obj_put(struct nt_obj *o)
{
	if (--o->refs)
		return;
	if (o->kind == NTH_KEY)
		nt_reg_close(o);
	if (o->fd >= 0)
		mos_close(o->fd);
	if (o->delete_on_close && o->path)
		mos_unlink(o->path, o->kind == NTH_DIR);
	nt_free(o->path);
	nt_free(o->ntname);
	nt_free(o->mask);
	nt_free(o);
}

static void std_handles_init(void)
{
	for (int i = 0; i < 3; i++) {
		struct nt_obj *o = nt_obj_new(NTH_FILE);

		if (o) {
			o->fd = i;
			o->access = 0x12019F;
		}
		htab[i] = o;
	}
}

static API NTSTATUS w_NtClose(u64 h)
{
	struct nt_obj *o;

	nt_lock(&htab_lock);
	o = nt_handle_get(h);
	if (!o) {
		nt_unlock(&htab_lock);
		return (h >= (u64)-6 || h == 0x40000 || h > 0x40000) ? 0 : STATUS_INVALID_HANDLE;
	}
	htab[h / 4 - 1] = NULL;
	nt_unlock(&htab_lock);
	nt_obj_put(o);
	return 0;
}
EXPORT(NtClose);

static API NTSTATUS w_NtDuplicateObject(u64 sp, u64 src, u64 tp, u64 *dst, u32 access, u32 attr,
					u32 opts)
{
	struct nt_obj *o = nt_handle_get(src);

	(void)sp;
	(void)tp;
	(void)access;
	(void)attr;
	if (src == CURRENT_PROCESS || src == CURRENT_THREAD) {
		struct nt_obj *n = nt_obj_new(src == CURRENT_PROCESS ? NTH_OTHER : NTH_THREAD);

		if (!n)
			return STATUS_NO_MEMORY;
		n->tid = src == CURRENT_THREAD ? (u64)mos_gettid() : 0;
		if (dst)
			*dst = nt_handle_new(n);
		return 0;
	}
	if (!o)
		return STATUS_INVALID_HANDLE;
	if (dst) {
		o->refs++;
		*dst = nt_handle_new(o);
	}
	if (opts & 1)                   /* DUPLICATE_CLOSE_SOURCE */
		w_NtClose(src);
	return 0;
}
EXPORT(NtDuplicateObject);

static API NTSTATUS w_NtCompareObjects(u64 a, u64 b)
{
	return nt_handle_get(a) == nt_handle_get(b) ? 0 : 0xC00001D2u;  /* STATUS_NOT_SAME_OBJECT */
}
EXPORT(NtCompareObjects);

static API NTSTATUS w_NtQueryObject(u64 h, u32 cls, void *buf, u32 len, u32 *ret)
{
	struct nt_obj *o = nt_handle_get(h);

	if (!o && h > 0xC)
		return STATUS_INVALID_HANDLE;
	if (cls == 4 && len >= 2) {             /* ObjectDataInformation */
		((u8 *)buf)[0] = 0;                 /* InheritHandle */
		((u8 *)buf)[1] = 0;
		if (ret)
			*ret = 2;
		return 0;
	}
	if (cls == 0 && len >= 56) {            /* ObjectBasicInformation */
		memset(buf, 0, 56);
		((u32 *)buf)[1] = o ? o->access : 0x1F01FF;
		((u32 *)buf)[2] = 1;
		if (ret)
			*ret = 56;
		return 0;
	}
	return STATUS_INVALID_INFO_CLASS;
}
EXPORT(NtQueryObject);

static API NTSTATUS w_NtSetInformationObject(u64 h, u32 cls, void *buf, u32 len)
{
	(void)h;
	(void)cls;
	(void)buf;
	(void)len;
	return 0;
}
EXPORT(NtSetInformationObject);

static API NTSTATUS w_NtMakeTemporaryObject(u64 h)
{
	(void)h;
	return 0;
}
EXPORT(NtMakeTemporaryObject);

/* ---- files ---------------------------------------------------------------------- */

#define FILE_DIRECTORY_FILE      0x1
#define FILE_NON_DIRECTORY_FILE  0x40
#define FILE_DELETE_ON_CLOSE     0x1000
#define FILE_ATTRIBUTE_READONLY  0x1
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE   0x20
#define FILE_ATTRIBUTE_NORMAL    0x80
#define BOOT_TIME                133500000000000000ll  /* 2024-01-21, a fixed file time */

static bool special_name(const UNICODE_STRING *u, const char *what)
{
	size_t n = u->len / 2, k = rt_strlen(what);

	if (n < k)
		return false;
	for (size_t i = 0; i < k; i++)
		if (nt_upcase(u->buf[n - k + i]) != (wchar)what[i])
			return false;
	return n == k || u->buf[n - k - 1] == '\\';
}

static u32 file_attrs(u64 kind)
{
	if (kind == MOS_ST_DIR)
		return FILE_ATTRIBUTE_DIRECTORY;
	if (kind == MOS_ST_FILE)
		return FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE;
	return FILE_ATTRIBUTE_ARCHIVE;
}

static char *str_dup(const char *s)
{
	size_t n = rt_strlen(s) + 1;
	char *d = nt_alloc(n);

	if (d)
		memcpy(d, s, n);
	return d;
}

static wchar *ustr_dup(const UNICODE_STRING *u)
{
	wchar *d = nt_alloc(u->len + 2);

	if (d)
		memcpy(d, u->buf, u->len);
	return d;
}

static void parent_of(const char *path, char *out, size_t cap)
{
	size_t n = nt_strlcpy(out, path, cap);

	while (n > 1 && out[n - 1] != '/')
		n--;
	out[n > 1 ? n - 1 : 1] = 0;
}

static API NTSTATUS w_NtCreateFile(u64 *handle, u32 access, OBJECT_ATTRIBUTES *oa,
				   IO_STATUS_BLOCK *iosb, s64 *alloc, u32 attrs, u32 share,
				   u32 disp, u32 options, void *ea, u32 ealen)
{
	char path[512];
	u64 st[2];
	bool exists, isdir;
	u32 flags = 0, info;
	long fd;
	struct nt_obj *o;
	NTSTATUS s;
	bool want_write = access & (0x40000000 | 0x10000000 | 0x2 | 0x4 | 0x100 | 0x10000);

	(void)alloc;
	(void)attrs;
	(void)share;
	(void)ea;
	(void)ealen;
	*handle = 0;
	if (!oa || !oa->name)
		return STATUS_INVALID_PARAMETER;
	/* Devices first: the console and the null device. */
	if (special_name(oa->name, "CONIN$") || special_name(oa->name, "CONOUT$") ||
	    special_name(oa->name, "CON") || special_name(oa->name, "NUL") ||
	    special_name(oa->name, "NULL")) {
		bool in = special_name(oa->name, "CONIN$") ||
			  (special_name(oa->name, "CON") && !want_write);
		bool null = special_name(oa->name, "NUL") || special_name(oa->name, "NULL");

		o = nt_obj_new(NTH_FILE);
		if (!o)
			return STATUS_NO_MEMORY;
		o->fd = null ? -1 : mos_dup(in ? 0 : 1);
		o->options = null ? 0x80000000u : 0;
		o->access = access;
		*handle = nt_handle_new(o);
		if (iosb) {
			iosb->status = 0;
			iosb->info = 1;
		}
		return 0;
	}
	s = nt_unix_path(oa->name, oa->root, path, sizeof(path));
	if (s)
		return s;
	exists = mos_stat(path, st) >= 0;
	isdir = exists && st[1] == MOS_ST_DIR;
	if (!exists) {
		char parent[512];
		u64 pst[2];

		parent_of(path, parent, sizeof(parent));
		if (mos_stat(parent, pst) < 0 || pst[1] != MOS_ST_DIR)
			return STATUS_OBJECT_PATH_NOT_FOUND;
	}
	switch (disp) {
	case 0: case 5:                         /* SUPERSEDE, OVERWRITE_IF */
		info = exists ? (disp ? 3 : 0) : 2;
		flags = MOS_O_CREAT | MOS_O_TRUNC;
		break;
	case 1:                                 /* OPEN */
		if (!exists)
			return STATUS_OBJECT_NAME_NOT_FOUND;
		info = 1;
		break;
	case 2:                                 /* CREATE */
		if (exists)
			return STATUS_OBJECT_NAME_COLLISION;
		info = 2;
		flags = MOS_O_CREAT | MOS_O_EXCL;
		break;
	case 3:                                 /* OPEN_IF */
		info = exists ? 1 : 2;
		flags = exists ? 0 : MOS_O_CREAT;
		break;
	case 4:                                 /* OVERWRITE */
		if (!exists)
			return STATUS_OBJECT_NAME_NOT_FOUND;
		info = 3;
		flags = MOS_O_TRUNC;
		break;
	default:
		return STATUS_INVALID_PARAMETER;
	}
	if (isdir && (options & FILE_NON_DIRECTORY_FILE))
		return STATUS_FILE_IS_A_DIRECTORY;
	if (exists && !isdir && (options & FILE_DIRECTORY_FILE))
		return STATUS_NOT_A_DIRECTORY;
	if (isdir || (options & FILE_DIRECTORY_FILE)) {
		if (!exists) {
			long r = mos_mkdir(path);

			if (r < 0)
				return (NTSTATUS)r;
		}
		fd = mos_open(path, MOS_O_RDONLY | MOS_O_DIRECTORY);
		if (fd < 0)
			return (NTSTATUS)fd;
		o = nt_obj_new(NTH_DIR);
	} else {
		if (want_write || (flags & (MOS_O_CREAT | MOS_O_TRUNC)))
			flags |= MOS_O_RDWR;
		fd = mos_open(path, flags);
		if (fd < 0 && exists && !(flags & (MOS_O_CREAT | MOS_O_TRUNC)) &&
		    !(access & (0x40000000 | 0x2 | 0x4)))
			fd = mos_open(path, MOS_O_RDONLY);   /* read-only medium */
		if (fd < 0)
			return (NTSTATUS)fd;
		if (access & 0x4 && !(access & 0x2))
			mos_seek(fd, 0, 2);             /* append-only data access */
		o = nt_obj_new(NTH_FILE);
	}
	if (!o) {
		mos_close(fd);
		return STATUS_NO_MEMORY;
	}
	o->fd = fd;
	o->path = str_dup(path);
	o->ntname = ustr_dup(oa->name);
	o->access = access;
	o->options = options;
	o->delete_on_close = !!(options & FILE_DELETE_ON_CLOSE);
	*handle = nt_handle_new(o);
	if (iosb) {
		iosb->status = 0;
		iosb->info = info;
	}
	return 0;
}
EXPORT(NtCreateFile);

static API NTSTATUS w_NtOpenFile(u64 *handle, u32 access, OBJECT_ATTRIBUTES *oa,
				 IO_STATUS_BLOCK *iosb, u32 share, u32 options)
{
	return w_NtCreateFile(handle, access, oa, iosb, NULL, 0, share, 1, options, NULL, 0);
}
EXPORT(NtOpenFile);

static long obj_fd(u64 h, struct nt_obj **out)
{
	struct nt_obj *o = nt_handle_get(h);

	if (out)
		*out = o;
	if (!o)
		return -1;
	return o->fd;
}

static API NTSTATUS w_NtReadFile(u64 h, u64 ev, void *apc, void *ctx, IO_STATUS_BLOCK *iosb,
				 void *buf, u32 len, s64 *off, u32 *key)
{
	struct nt_obj *o;
	long fd = obj_fd(h, &o), n;

	(void)ev;
	(void)apc;
	(void)ctx;
	(void)key;
	if (!o)
		return STATUS_INVALID_HANDLE;
	if (o->kind != NTH_FILE)
		return STATUS_INVALID_DEVICE_REQUEST;
	if (fd < 0) {                           /* NUL */
		iosb->status = STATUS_END_OF_FILE;
		iosb->info = 0;
		return STATUS_END_OF_FILE;
	}
	if (off && *off >= 0)
		mos_seek(fd, *off, 0);
	n = mos_read(fd, buf, len);
	if (n < 0)
		return (NTSTATUS)n;
	iosb->status = 0;
	iosb->info = (u64)n;
	if (!n && len) {
		iosb->status = STATUS_END_OF_FILE;
		return STATUS_END_OF_FILE;
	}
	return 0;
}
EXPORT(NtReadFile);

static API NTSTATUS w_NtWriteFile(u64 h, u64 ev, void *apc, void *ctx, IO_STATUS_BLOCK *iosb,
				  const void *buf, u32 len, s64 *off, u32 *key)
{
	struct nt_obj *o;
	long fd = obj_fd(h, &o), n;

	(void)ev;
	(void)apc;
	(void)ctx;
	(void)key;
	if (!o)
		return STATUS_INVALID_HANDLE;
	if (o->kind != NTH_FILE)
		return STATUS_INVALID_DEVICE_REQUEST;
	if (fd < 0) {
		iosb->status = 0;
		iosb->info = len;
		return 0;
	}
	if (fd <= 2)
		rt_flush();
	if (off && *off == -1)
		mos_seek(fd, 0, 2);             /* FILE_WRITE_TO_END_OF_FILE */
	else if (off && *off >= 0)
		mos_seek(fd, *off, 0);
	n = mos_write(fd, buf, len);
	if (n < 0)
		return (NTSTATUS)n;
	iosb->status = 0;
	iosb->info = (u64)n;
	return 0;
}
EXPORT(NtWriteFile);

static API NTSTATUS w_NtFlushBuffersFile(u64 h, IO_STATUS_BLOCK *iosb)
{
	(void)h;
	if (iosb) {
		iosb->status = 0;
		iosb->info = 0;
	}
	return 0;
}
EXPORT(NtFlushBuffersFile);

static API NTSTATUS w_NtCancelIoFile(u64 h, IO_STATUS_BLOCK *iosb)
{
	return w_NtFlushBuffersFile(h, iosb);
}
EXPORT(NtCancelIoFile);

static API NTSTATUS w_NtCancelIoFileEx(u64 h, void *io, IO_STATUS_BLOCK *iosb)
{
	(void)io;
	return w_NtFlushBuffersFile(h, iosb);
}
EXPORT(NtCancelIoFileEx);

/* Kind and size of an open object: from its path when it has one. */
static void obj_stat(struct nt_obj *o, u64 st[2])
{
	st[0] = 0;
	st[1] = o->kind == NTH_DIR ? MOS_ST_DIR : MOS_ST_TTY;
	if (o->fd >= 0)
		mos_fstat(o->fd, st);
	if (o->kind == NTH_DIR)
		st[1] = MOS_ST_DIR;
}

static void put_times(u8 *p)
{
	for (int i = 0; i < 4; i++)
		*(s64 *)(p + i * 8) = BOOT_TIME;
}

static u32 name_info(struct nt_obj *o, u8 *buf, u32 len, u32 *need)
{
	/* FILE_NAME_INFORMATION: the path without the drive letter. */
	const char *p = o->path ? o->path : "\\";
	u32 n = 0;
	wchar *out = (wchar *)(buf + 4);

	for (; p[n]; n++)
		;
	*need = 4 + n * 2;
	*(u32 *)buf = n * 2;
	for (u32 i = 0; i < n && 4 + (i + 1) * 2 <= len; i++)
		out[i] = p[i] == '/' ? '\\' : (wchar)(u8)p[i];
	return *need <= len ? 0 : STATUS_BUFFER_OVERFLOW;
}

static API NTSTATUS w_NtQueryInformationFile(u64 h, IO_STATUS_BLOCK *iosb, void *buf, u32 len,
					     u32 cls)
{
	struct nt_obj *o = nt_handle_get(h);
	u8 *b = buf;
	u64 st[2];
	u32 need = 0;
	NTSTATUS s = 0;

	if (!o)
		return STATUS_INVALID_HANDLE;
	obj_stat(o, st);
	switch (cls) {
	case 4:         /* FileBasicInformation */
		need = 40;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		put_times(b);
		*(u32 *)(b + 32) = file_attrs(st[1]);
		break;
	case 5:         /* FileStandardInformation */
		need = 24;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 24);
		*(u64 *)b = (st[0] + 4095) & ~4095ull;
		*(u64 *)(b + 8) = st[0];
		*(u32 *)(b + 16) = 1;
		b[20] = o->delete_on_close;
		b[21] = st[1] == MOS_ST_DIR;
		break;
	case 6:         /* FileInternalInformation */
		need = 8;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u64 *)b = (u64)o;
		break;
	case 7:         /* FileEaInformation */
	case 8:         /* FileAccessInformation */
	case 16:        /* FileModeInformation */
	case 17:        /* FileAlignmentInformation */
		need = 4;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u32 *)b = cls == 8 ? o->access : cls == 16 ? 0x20 : 0;
		break;
	case 9:         /* FileNameInformation */
	case 48:        /* FileNormalizedNameInformation */
		if (len < 4)
			return STATUS_INFO_LENGTH_MISMATCH;
		s = name_info(o, b, len, &need);
		break;
	case 14:        /* FilePositionInformation */
		need = 8;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(s64 *)b = o->fd >= 0 ? mos_seek(o->fd, 0, 1) : 0;
		if (*(s64 *)b < 0)
			*(s64 *)b = 0;
		break;
	case 18:        /* FileAllInformation */
		if (len < 100)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 100);
		put_times(b);
		*(u32 *)(b + 32) = file_attrs(st[1]);
		*(u64 *)(b + 40) = (st[0] + 4095) & ~4095ull;
		*(u64 *)(b + 48) = st[0];
		*(u32 *)(b + 56) = 1;
		b[60] = o->delete_on_close;
		b[61] = st[1] == MOS_ST_DIR;
		*(u64 *)(b + 64) = (u64)o;
		*(u32 *)(b + 76) = o->access;
		*(s64 *)(b + 80) = o->fd >= 0 && st[1] != MOS_ST_DIR ? mos_seek(o->fd, 0, 1) : 0;
		s = name_info(o, b + 96, len - 96, &need);
		need += 96;
		break;
	case 34:        /* FileNetworkOpenInformation */
		need = 56;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		put_times(b);
		*(u64 *)(b + 32) = (st[0] + 4095) & ~4095ull;
		*(u64 *)(b + 40) = st[0];
		*(u32 *)(b + 48) = file_attrs(st[1]);
		*(u32 *)(b + 52) = 0;
		break;
	case 35:        /* FileAttributeTagInformation */
		need = 8;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u32 *)b = file_attrs(st[1]);
		*(u32 *)(b + 4) = 0;
		break;
	case 59:        /* FileIdInformation */
		need = 24;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 24);
		*(u64 *)(b + 8) = (u64)o;
		break;
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
	if (iosb) {
		iosb->status = s;
		iosb->info = need < len ? need : len;
	}
	return s;
}
EXPORT(NtQueryInformationFile);

static API NTSTATUS w_NtSetInformationFile(u64 h, IO_STATUS_BLOCK *iosb, void *buf, u32 len,
					   u32 cls)
{
	struct nt_obj *o = nt_handle_get(h);
	u8 *b = buf;

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (iosb) {
		iosb->status = 0;
		iosb->info = 0;
	}
	switch (cls) {
	case 14:        /* FilePositionInformation */
		if (len < 8)
			return STATUS_INFO_LENGTH_MISMATCH;
		if (o->fd >= 0 && mos_seek(o->fd, *(s64 *)b, 0) < 0)
			return STATUS_INVALID_PARAMETER;
		return 0;
	case 13:        /* FileDispositionInformation */
		o->delete_on_close = len && b[0];
		return 0;
	case 64:        /* FileDispositionInformationEx */
		o->delete_on_close = len >= 4 && (*(u32 *)b & 1);
		return 0;
	case 10:        /* FileRenameInformation */
	case 65: {      /* FileRenameInformationEx */
		UNICODE_STRING u;
		char to[512];
		NTSTATUS s;
		long r;
		u64 st[2];

		if (len < 20 || !o->path)
			return STATUS_INVALID_PARAMETER;
		u.len = u.max = (u16)*(u32 *)(b + 16);
		u.buf = (wchar *)(b + 20);
		s = nt_unix_path(&u, *(u64 *)(b + 8), to, sizeof(to));
		if (s)
			return s;
		if (mos_stat(to, st) >= 0) {
			if (!(cls == 10 ? b[0] : (*(u32 *)b & 1)))
				return STATUS_OBJECT_NAME_COLLISION;
			mos_unlink(to, 0);
		}
		r = mos_rename(o->path, to);
		if (r < 0)
			return (NTSTATUS)r;
		nt_free(o->path);
		o->path = str_dup(to);
		return 0;
	}
	case 4:         /* FileBasicInformation: times and attributes are not kept */
	case 19:        /* FileAllocationInformation */
	case 20:        /* FileEndOfFileInformation */
	case 16:        /* FileModeInformation */
	case 39:        /* FileValidDataLengthInformation */
		return 0;
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
}
EXPORT(NtSetInformationFile);

static NTSTATUS attrs_of(OBJECT_ATTRIBUTES *oa, u64 st[2])
{
	char path[512];
	NTSTATUS s;

	if (!oa || !oa->name)
		return STATUS_INVALID_PARAMETER;
	s = nt_unix_path(oa->name, oa->root, path, sizeof(path));
	if (s)
		return s;
	if (mos_stat(path, st) < 0) {
		char parent[512];
		u64 pst[2];

		parent_of(path, parent, sizeof(parent));
		return mos_stat(parent, pst) < 0 ? STATUS_OBJECT_PATH_NOT_FOUND :
		       STATUS_OBJECT_NAME_NOT_FOUND;
	}
	return 0;
}

static API NTSTATUS w_NtQueryAttributesFile(OBJECT_ATTRIBUTES *oa, u8 *info)
{
	u64 st[2];
	NTSTATUS s = attrs_of(oa, st);

	if (s)
		return s;
	put_times(info);
	*(u32 *)(info + 32) = file_attrs(st[1]);
	return 0;
}
EXPORT(NtQueryAttributesFile);

static API NTSTATUS w_NtQueryFullAttributesFile(OBJECT_ATTRIBUTES *oa, u8 *info)
{
	u64 st[2];
	NTSTATUS s = attrs_of(oa, st);

	if (s)
		return s;
	put_times(info);
	*(u64 *)(info + 32) = (st[0] + 4095) & ~4095ull;
	*(u64 *)(info + 40) = st[0];
	*(u32 *)(info + 48) = file_attrs(st[1]);
	*(u32 *)(info + 52) = 0;
	return 0;
}
EXPORT(NtQueryFullAttributesFile);

static API NTSTATUS w_NtDeleteFile(OBJECT_ATTRIBUTES *oa)
{
	char path[512];
	u64 st[2];
	NTSTATUS s = nt_unix_path(oa->name, oa->root, path, sizeof(path));

	if (s)
		return s;
	if (mos_stat(path, st) < 0)
		return STATUS_OBJECT_NAME_NOT_FOUND;
	return mos_unlink(path, st[1] == MOS_ST_DIR) < 0 ? STATUS_ACCESS_DENIED : 0;
}
EXPORT(NtDeleteFile);

/* DOS wildcard match, case-insensitive: '*' any run, '?' one character. */
static bool wild_match(const wchar *m, size_t mn, const wchar *s, size_t sn)
{
	size_t i = 0, j = 0, star = (size_t)-1, mark = 0;

	while (j < sn) {
		if (i < mn && (m[i] == '?' || m[i] == '>' ||
			       nt_upcase(m[i]) == nt_upcase(s[j]))) {
			i++;
			j++;
		} else if (i < mn && (m[i] == '*' || m[i] == '<')) {
			star = i++;
			mark = j;
		} else if (star != (size_t)-1) {
			i = star + 1;
			j = ++mark;
		} else {
			return false;
		}
	}
	while (i < mn && (m[i] == '*' || m[i] == '<' || m[i] == '"' || m[i] == '.' ||
			  m[i] == '>'))
		i++;
	return i == mn;
}

/* Fill one directory entry of class `cls`; returns its size or 0 if it does not fit. */
static u32 dir_entry(u8 *b, u32 room, u32 cls, const wchar *name, u32 nlen, u64 size, u32 attrs)
{
	u32 off;

	switch (cls) {
	case 1: off = 64; break;        /* FileDirectoryInformation */
	case 2: off = 68; break;        /* FileFullDirectoryInformation */
	case 3: off = 94; break;        /* FileBothDirectoryInformation */
	case 12: off = 12; break;       /* FileNamesInformation */
	case 37: off = 104; break;      /* FileIdBothDirectoryInformation */
	case 38: off = 80; break;       /* FileIdFullDirectoryInformation */
	default: return 0;
	}
	if (off + nlen * 2 > room)
		return 0;
	memset(b, 0, off);
	if (cls == 12) {
		*(u32 *)(b + 8) = nlen * 2;
	} else {
		put_times(b + 8);
		*(u64 *)(b + 40) = size;
		*(u64 *)(b + 48) = (size + 4095) & ~4095ull;
		*(u32 *)(b + 56) = attrs;
		*(u32 *)(b + 60) = nlen * 2;
	}
	memcpy(b + off, name, nlen * 2);
	return (off + nlen * 2 + 7) & ~7u;
}

static API NTSTATUS w_NtQueryDirectoryFile(u64 h, u64 ev, void *apc, void *ctx,
					   IO_STATUS_BLOCK *iosb, void *buf, u32 len, u32 cls,
					   u8 single, UNICODE_STRING *mask, u8 restart)
{
	struct nt_obj *o = nt_handle_get(h);
	u8 *b = buf, *last = NULL;
	u32 used = 0;
	char name[256];
	long r;

	(void)ev;
	(void)apc;
	(void)ctx;
	if (!o)
		return STATUS_INVALID_HANDLE;
	if (o->kind != NTH_DIR)
		return STATUS_INVALID_PARAMETER;
	if (mask && mask->len && !o->mask) {
		o->mask = ustr_dup(mask);
		o->max = mask->len / 2;
	}
	if (restart || o->dirpos == 0) {
		mos_close(o->fd);
		o->fd = mos_open(o->path, MOS_O_RDONLY | MOS_O_DIRECTORY);
		o->dirpos = 0;
	}
	for (;;) {
		wchar wname[256];
		u32 nlen, attrs = FILE_ATTRIBUTE_DIRECTORY, sz;
		u64 size = 0;

		if (o->dirpos < 2) {
			name[0] = '.';
			name[1] = o->dirpos ? '.' : 0;
			name[2] = 0;
			r = 2;
		} else {
			r = mos_readdir(o->fd, name, sizeof(name));
			if (r <= 0)
				break;
		}
		nlen = (u32)nt_utow(wname, 256, name, rt_strlen(name));
		if (o->mask && !wild_match(o->mask, o->max, wname, nlen)) {
			o->dirpos++;
			continue;
		}
		if (r != 2 || o->dirpos >= 2) {
			char full[512];
			size_t pn = nt_strlcpy(full, o->path, sizeof(full));
			u64 st[2];

			if (pn > 1)
				full[pn++] = '/';
			nt_strlcpy(full + pn, name, sizeof(full) - pn);
			if (mos_stat(full, st) >= 0) {
				size = st[0];
				attrs = file_attrs(st[1]);
			}
		}
		sz = dir_entry(b + used, len - used, cls, wname, nlen, size, attrs);
		if (!sz) {
			if (!used)
				return cls == 1 || cls == 2 || cls == 3 || cls == 12 || cls == 37 ||
				       cls == 38 ? STATUS_BUFFER_OVERFLOW : STATUS_INVALID_INFO_CLASS;
			/* Does not fit: re-read it next time. */
			mos_close(o->fd);
			o->fd = mos_open(o->path, MOS_O_RDONLY | MOS_O_DIRECTORY);
			for (long k = 2; k < o->dirpos; k++)
				mos_readdir(o->fd, name, sizeof(name));
			break;
		}
		o->dirpos++;
		if (last)
			*(u32 *)last = (u32)(b + used - last);
		last = b + used;
		used += sz;
		if (single)
			break;
	}
	if (!used) {
		iosb->status = STATUS_NO_MORE_FILES;
		iosb->info = 0;
		return STATUS_NO_MORE_FILES;
	}
	iosb->status = 0;
	iosb->info = used;
	return 0;
}
EXPORT(NtQueryDirectoryFile);

static API NTSTATUS w_NtQueryVolumeInformationFile(u64 h, IO_STATUS_BLOCK *iosb, void *buf,
						   u32 len, u32 cls)
{
	struct nt_obj *o = nt_handle_get(h);
	u8 *b = buf;
	u32 need;
	u64 st[2];

	if (!o)
		return STATUS_INVALID_HANDLE;
	obj_stat(o, st);
	switch (cls) {
	case 4:         /* FileFsDeviceInformation */
		need = 8;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u32 *)b = o->options == 0x80000000u ? 0x15 :          /* NULL        */
			    st[1] == MOS_ST_TTY ? 0x22 :                /* UNKNOWN: char */
			    st[1] == MOS_ST_FIFO ? 0x11 : 0x7;          /* pipe, disk  */
		*(u32 *)(b + 4) = 0;
		break;
	case 5: {       /* FileFsAttributeInformation */
		static const wchar fs[] = L"NTFS";

		need = 12 + 8;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u32 *)b = 0x3;                /* case sensitive + case preserved */
		*(u32 *)(b + 4) = 255;
		*(u32 *)(b + 8) = 8;
		memcpy(b + 12, fs, 8);
		break;
	}
	case 1: {       /* FileFsVolumeInformation */
		static const wchar label[] = L"MERGEDOS";

		need = 18 + 16;
		if (len < 18)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 18);
		*(s64 *)b = BOOT_TIME;
		*(u32 *)(b + 8) = 0x4D4F5321;   /* serial number */
		*(u32 *)(b + 12) = 16;
		memcpy(b + 18, label, len >= need ? 16 : len - 18);
		break;
	}
	case 3:         /* FileFsSizeInformation */
	case 7:         /* FileFsFullSizeInformation */
		need = cls == 3 ? 24 : 32;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u64 *)b = 65536;
		*(u64 *)(b + 8) = 32768;
		if (cls == 7)
			*(u64 *)(b + 16) = 32768;
		*(u32 *)(b + need - 8) = 8;
		*(u32 *)(b + need - 4) = 512;
		break;
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
	iosb->status = 0;
	iosb->info = need;
	return 0;
}
EXPORT(NtQueryVolumeInformationFile);

/* Console and other device requests: nothing here is a condrv console. */
static API NTSTATUS w_NtDeviceIoControlFile(u64 h, u64 ev, void *apc, void *ctx,
					    IO_STATUS_BLOCK *iosb, u32 code, void *in, u32 inlen,
					    void *out, u32 outlen)
{
	(void)ev;
	(void)apc;
	(void)ctx;
	(void)code;
	(void)in;
	(void)inlen;
	(void)out;
	(void)outlen;
	(void)iosb;
	return nt_handle_get(h) ? STATUS_INVALID_DEVICE_REQUEST : STATUS_INVALID_HANDLE;
}
EXPORT(NtDeviceIoControlFile);
EXPORT_AS("NtFsControlFile", w_NtDeviceIoControlFile);

/* ---- virtual memory ------------------------------------------------------------- */

static API NTSTATUS w_NtAllocateVirtualMemory(u64 p, void **base, u64 zero, size_t *size, u32 type,
					      u32 prot)
{
	NTSTATUS s = NtAllocateVirtualMemory(p, base, zero, size, type, prot);

	if (!s && (prot & 0xF0)) {
		u32 old;
		void *b = *base;
		size_t sz = *size;

		NtProtectVirtualMemory(p, &b, &sz, prot, &old);
	}
	return s;
}
EXPORT(NtAllocateVirtualMemory);

static API NTSTATUS w_NtAllocateVirtualMemoryEx(u64 p, void **base, size_t *size, u32 type,
						u32 prot, void *params, u32 count)
{
	(void)params;
	(void)count;
	return w_NtAllocateVirtualMemory(p, base, 0, size, type, prot);
}
EXPORT(NtAllocateVirtualMemoryEx);

static API NTSTATUS w_NtFreeVirtualMemory(u64 p, void **base, size_t *size, u32 type)
{
	return NtFreeVirtualMemory(p, base, size, type);
}
EXPORT(NtFreeVirtualMemory);

static API NTSTATUS w_NtProtectVirtualMemory(u64 p, void **base, size_t *size, u32 prot, u32 *old)
{
	NTSTATUS s = NtProtectVirtualMemory(p, base, size, prot, old);

	if (old)
		*old = 0x40;    /* rights are only ever added: everything is RWX at most */
	return s;
}
EXPORT(NtProtectVirtualMemory);

static API NTSTATUS w_NtQueryVirtualMemory(u64 p, u64 addr, u32 cls, void *buf, size_t len,
					   size_t *ret)
{
	u8 *b = buf;
	u64 mod = ldr_module_of(addr), page = addr & ~0xFFFull;
	u64 top = *(u64 *)(nt_teb() + 8), bottom = *(u64 *)(nt_teb() + 16);

	(void)p;
	if (cls != 0)                   /* MemoryWineUnixFuncs etc.: no unix side here */
		return STATUS_INVALID_INFO_CLASS;
	if (len < 48)
		return STATUS_INFO_LENGTH_MISMATCH;
	memset(b, 0, 48);
	*(u64 *)b = page;
	*(u32 *)(b + 0x20) = 0x1000;            /* MEM_COMMIT */
	*(u32 *)(b + 0x24) = 0x04;              /* PAGE_READWRITE */
	*(u32 *)(b + 0x28) = 0x20000;           /* MEM_PRIVATE */
	*(u64 *)(b + 8) = page;
	*(u32 *)(b + 0x10) = 0x04;
	*(u64 *)(b + 0x18) = 0x1000;
	if (mod && mod != NTRT_BASE) {
		u8 *nt = (u8 *)mod + *(u32 *)(mod + 0x3C);
		u32 size = *(u32 *)(nt + 24 + 56);

		*(u64 *)(b + 8) = mod;
		*(u64 *)(b + 0x18) = mod + size - page;
		*(u32 *)(b + 0x24) = 0x40;
		*(u32 *)(b + 0x10) = 0x80;      /* PAGE_EXECUTE_WRITECOPY */
		*(u32 *)(b + 0x28) = 0x1000000; /* MEM_IMAGE */
	} else if (addr >= bottom - 0x100000 && addr < top) {
		*(u64 *)(b + 8) = bottom;
		*(u64 *)(b + 0x18) = top - page;
	}
	if (ret)
		*ret = 48;
	return 0;
}
EXPORT(NtQueryVirtualMemory);

static API NTSTATUS w_NtFlushInstructionCache(u64 p, void *a, size_t n)
{
	(void)p;
	(void)a;
	(void)n;
	return 0;
}
EXPORT(NtFlushInstructionCache);

static API NTSTATUS w_NtLockVirtualMemory(u64 p, void **a, size_t *n, u32 t)
{
	(void)p;
	(void)a;
	(void)n;
	(void)t;
	return 0;
}
EXPORT(NtLockVirtualMemory);
EXPORT_AS("NtUnlockVirtualMemory", w_NtLockVirtualMemory);

static API NTSTATUS w_NtReadVirtualMemory(u64 p, const void *a, void *buf, size_t n, size_t *got)
{
	if (p != CURRENT_PROCESS)
		return STATUS_ACCESS_DENIED;
	memcpy(buf, a, n);
	if (got)
		*got = n;
	return 0;
}
EXPORT(NtReadVirtualMemory);

static API NTSTATUS w_NtWriteVirtualMemory(u64 p, void *a, const void *buf, size_t n, size_t *put)
{
	if (p != CURRENT_PROCESS)
		return STATUS_ACCESS_DENIED;
	memcpy(a, buf, n);
	if (put)
		*put = n;
	return 0;
}
EXPORT(NtWriteVirtualMemory);

/* ---- processes and threads ------------------------------------------------------ */

static API NTSTATUS w_NtTerminateProcess(u64 h, NTSTATUS code)
{
	if (h && h != CURRENT_PROCESS)
		return STATUS_ACCESS_DENIED;
	rt_exit(code);
}
EXPORT(NtTerminateProcess);

static API void w_RtlExitUserProcess(NTSTATUS code)
{
	rt_exit(code);
}
EXPORT(RtlExitUserProcess);

static u32 put_ustr_info(u8 *b, u32 len, const wchar *s)
{
	u32 n = (u32)rt_wcslen(s) * 2, need = 16 + n + 2;

	if (len >= need) {
		UNICODE_STRING *u = (UNICODE_STRING *)b;

		u->len = (u16)n;
		u->max = (u16)(n + 2);
		u->buf = (wchar *)(b + 16);
		memcpy(b + 16, s, n + 2);
	}
	return need;
}

static API NTSTATUS w_NtQueryInformationProcess(u64 h, u32 cls, void *buf, u32 len, u32 *ret)
{
	u8 *b = buf;
	u32 need;

	(void)h;
	switch (cls) {
	case 0:         /* ProcessBasicInformation */
		need = 48;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 48);
		*(u32 *)b = STATUS_PENDING;
		*(u8 **)(b + 8) = nt_peb;
		*(u64 *)(b + 16) = 1;
		*(u32 *)(b + 24) = 8;
		*(u64 *)(b + 32) = rt_params->pid;
		break;
	case 7:         /* ProcessDebugPort */
	case 26:        /* ProcessWow64Information */
		need = 8;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u64 *)b = 0;
		break;
	case 30:        /* ProcessDebugObjectHandle */
		return 0xC0000353u;             /* STATUS_PORT_NOT_SET */
	case 12:        /* ProcessDefaultHardErrorMode */
	case 24:        /* ProcessSessionInformation */
	case 31:        /* ProcessDebugFlags */
	case 34:        /* ProcessExecuteFlags */
	case 36:        /* ProcessCookie */
	case 20:        /* ProcessHandleCount */
	case 29:        /* ProcessBreakOnTermination */
		need = 4;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u32 *)b = cls == 31 ? 1 : cls == 36 ? 0x4D4F5321 : cls == 24 ? 1 : 0;
		break;
	case 18:        /* ProcessPriorityClass */
		need = 2;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		b[0] = 0;
		b[1] = 2;                       /* NORMAL */
		break;
	case 27:        /* ProcessImageFileName (NT path) */
	case 43: {      /* ProcessImageFileNameWin32 */
		wchar nt[200];
		const wchar *dos = ldr_image_path();
		size_t n = 0;

		if (cls == 27) {
			static const wchar pre[] = L"\\??\\";

			for (; pre[n]; n++)
				nt[n] = pre[n];
		}
		for (size_t i = 0; dos[i] && n < 199; i++)
			nt[n++] = dos[i];
		nt[n] = 0;
		need = put_ustr_info(b, len, nt);
		if (len < need) {
			if (ret)
				*ret = need;
			return STATUS_INFO_LENGTH_MISMATCH;
		}
		break;
	}
	case 4:         /* ProcessTimes */
		need = 32;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 32);
		*(s64 *)b = BOOT_TIME;
		break;
	case 3:         /* ProcessVmCounters */
	case 2:         /* ProcessIoCounters */
		need = cls == 3 ? 80 : 48;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, need);
		break;
	case 37:        /* ProcessImageInformation */
		need = 64;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 64);
		*(u64 *)b = rt_params->entry;
		*(u16 *)(b + 0x24) = 3;         /* SubSystemType: console */
		*(u16 *)(b + 0x30) = 0x8664;    /* Machine */
		break;
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
	if (ret)
		*ret = need;
	return 0;
}
EXPORT(NtQueryInformationProcess);

static API NTSTATUS w_NtSetInformationProcess(u64 h, u32 cls, void *buf, u32 len)
{
	(void)h;
	(void)cls;
	(void)buf;
	(void)len;
	return 0;
}
EXPORT(NtSetInformationProcess);

static API NTSTATUS w_NtQueryInformationThread(u64 h, u32 cls, void *buf, u32 len, u32 *ret)
{
	u8 *b = buf;
	u32 need;
	struct nt_obj *o = nt_handle_get(h);

	switch (cls) {
	case 0:         /* ThreadBasicInformation */
		need = 48;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 48);
		*(u32 *)b = STATUS_PENDING;
		*(u8 **)(b + 8) = nt_teb();
		*(u64 *)(b + 16) = rt_params->pid;
		*(u64 *)(b + 24) = o && o->kind == NTH_THREAD ? o->tid * 4 :
				   *(u64 *)(nt_teb() + 0x48);
		*(u64 *)(b + 32) = 1;
		*(u32 *)(b + 40) = 8;
		*(u32 *)(b + 44) = 8;
		break;
	case 12:        /* ThreadAmILastThread */
	case 20:        /* ThreadIsTerminated */
	case 18:        /* ThreadHideFromDebugger */
		need = 4;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u32 *)b = 0;
		break;
	case 9:         /* ThreadQuerySetWin32StartAddress */
		need = 8;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		*(u64 *)b = rt_params->entry;
		break;
	case 1:         /* ThreadTimes */
		need = 32;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 32);
		break;
	case 38:        /* ThreadDescription */
		need = 16;
		if (len < need)
			return STATUS_INFO_LENGTH_MISMATCH;
		memset(b, 0, 16);
		break;
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
	if (ret)
		*ret = need;
	return 0;
}
EXPORT(NtQueryInformationThread);

static API NTSTATUS w_NtSetInformationThread(u64 h, u32 cls, void *buf, u32 len)
{
	(void)h;
	(void)cls;
	(void)buf;
	(void)len;
	return 0;
}
EXPORT(NtSetInformationThread);

static API NTSTATUS w_NtOpenProcess(u64 *h, u32 access, void *oa, u64 *cid)
{
	struct nt_obj *o;

	(void)access;
	(void)oa;
	if (!cid || cid[0] != rt_params->pid)
		return 0xC000000Bu;             /* STATUS_INVALID_CID */
	o = nt_obj_new(NTH_OTHER);
	if (!o)
		return STATUS_NO_MEMORY;
	*h = nt_handle_new(o);
	return 0;
}
EXPORT(NtOpenProcess);

static API NTSTATUS w_NtOpenThread(u64 *h, u32 access, void *oa, u64 *cid)
{
	struct nt_obj *o = nt_obj_new(NTH_THREAD);

	(void)access;
	(void)oa;
	if (!o)
		return STATUS_NO_MEMORY;
	o->tid = cid ? cid[1] / 4 : 0;
	*h = nt_handle_new(o);
	return 0;
}
EXPORT(NtOpenThread);

/* ---- tokens: one local user ------------------------------------------------------ */

static const u8 sid_user[] = { 1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			       0, 0, 0, 0, 0xE8, 3, 0, 0 };    /* S-1-5-21-0-0-0-1000 */
static const u8 sid_admins[] = { 1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 2, 0, 0 };
static const u8 sid_high[] = { 1, 1, 0, 0, 0, 0, 0, 16, 0, 0x30, 0, 0 };

static API NTSTATUS w_NtOpenProcessToken(u64 p, u32 access, u64 *h)
{
	struct nt_obj *o = nt_obj_new(NTH_OTHER);

	(void)p;
	(void)access;
	if (!o)
		return STATUS_NO_MEMORY;
	*h = nt_handle_new(o);
	return 0;
}
EXPORT(NtOpenProcessToken);

static API NTSTATUS w_NtOpenProcessTokenEx(u64 p, u32 access, u32 attr, u64 *h)
{
	(void)attr;
	return w_NtOpenProcessToken(p, access, h);
}
EXPORT(NtOpenProcessTokenEx);

static API NTSTATUS w_NtOpenThreadToken(u64 t, u32 access, u8 self, u64 *h)
{
	(void)t;
	(void)access;
	(void)self;
	*h = 0;
	return STATUS_NO_TOKEN;
}
EXPORT(NtOpenThreadToken);

static API NTSTATUS w_NtOpenThreadTokenEx(u64 t, u32 access, u8 self, u32 attr, u64 *h)
{
	(void)attr;
	return w_NtOpenThreadToken(t, access, self, h);
}
EXPORT(NtOpenThreadTokenEx);

static API NTSTATUS w_NtQueryInformationToken(u64 h, u32 cls, void *buf, u32 len, u32 *ret)
{
	u8 *b = buf;
	u32 need;

	(void)h;
	switch (cls) {
	case 1:         /* TokenUser: SID_AND_ATTRIBUTES + SID */
	case 25:        /* TokenIntegrityLevel */
	case 4:         /* TokenOwner */
	case 5: {       /* TokenPrimaryGroup */
		const u8 *sid = cls == 25 ? sid_high : sid_user;
		u32 sl = cls == 25 ? sizeof(sid_high) : sizeof(sid_user);
		u32 hdr = (cls == 1 || cls == 25) ? 16 : 8;

		need = hdr + sl;
		if (len < need)
			break;
		memset(b, 0, hdr);
		*(u8 **)b = b + hdr;
		if (cls == 25)
			*(u32 *)(b + 8) = 0x20;         /* SE_GROUP_INTEGRITY */
		memcpy(b + hdr, sid, sl);
		break;
	}
	case 2:         /* TokenGroups */
		need = 8 + 16 + sizeof(sid_admins);
		if (len < need)
			break;
		*(u32 *)b = 1;
		*(u32 *)(b + 4) = 0;
		*(u8 **)(b + 8) = b + 24;
		*(u32 *)(b + 16) = 0xF;         /* enabled, mandatory, owner */
		memcpy(b + 24, sid_admins, sizeof(sid_admins));
		break;
	case 3:         /* TokenPrivileges */
		need = 4;
		if (len < need)
			break;
		*(u32 *)b = 0;
		break;
	case 8:         /* TokenType */
	case 9:         /* TokenImpersonationLevel */
	case 12:        /* TokenSessionId */
	case 18:        /* TokenElevationType */
	case 20:        /* TokenElevation */
	case 29:        /* TokenUIAccess */
		need = 4;
		if (len < need)
			break;
		*(u32 *)b = cls == 8 ? 1 : cls == 18 ? 1 : cls == 20 ? 1 : cls == 12 ? 1 : 0;
		break;
	case 10:        /* TokenStatistics */
		need = 56;
		if (len < need)
			break;
		memset(b, 0, 56);
		*(u32 *)(b + 32) = 1;
		break;
	case 6:         /* TokenDefaultDacl: none */
		need = 8;
		if (len < need)
			break;
		*(u64 *)b = 0;
		break;
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
	if (ret)
		*ret = need;
	return len < need ? STATUS_BUFFER_TOO_SMALL : 0;
}
EXPORT(NtQueryInformationToken);

static API NTSTATUS w_NtSetInformationToken(u64 h, u32 cls, void *buf, u32 len)
{
	(void)h;
	(void)cls;
	(void)buf;
	(void)len;
	return 0;
}
EXPORT(NtSetInformationToken);

static API NTSTATUS w_NtAdjustPrivilegesToken(u64 h, u8 all, void *np, u32 len, void *prev,
					      u32 *ret)
{
	(void)h;
	(void)all;
	(void)np;
	(void)len;
	(void)prev;
	if (ret)
		*ret = 0;
	return 0;
}
EXPORT(NtAdjustPrivilegesToken);

static API NTSTATUS w_NtDuplicateToken(u64 h, u32 access, void *oa, u8 eff, u32 type, u64 *out)
{
	(void)h;
	(void)oa;
	(void)eff;
	(void)type;
	return w_NtOpenProcessToken(CURRENT_PROCESS, access, out);
}
EXPORT(NtDuplicateToken);

static API NTSTATUS w_NtAccessCheck(void *sd, u64 tok, u32 access, void *map, void *priv,
				    u32 *plen, u32 *granted, NTSTATUS *status)
{
	(void)sd;
	(void)tok;
	(void)map;
	(void)priv;
	(void)plen;
	*granted = access;
	*status = 0;
	return 0;
}
EXPORT(NtAccessCheck);

static API NTSTATUS w_NtPrivilegeCheck(u64 tok, void *priv, u8 *result)
{
	(void)tok;
	(void)priv;
	*result = 1;
	return 0;
}
EXPORT(NtPrivilegeCheck);

static API NTSTATUS w_NtQuerySecurityObject(u64 h, u32 info, void *sd, u32 len, u32 *need)
{
	(void)h;
	(void)info;
	*need = 20;
	if (len < 20)
		return STATUS_BUFFER_TOO_SMALL;
	memset(sd, 0, 20);
	((u8 *)sd)[0] = 1;                      /* revision */
	*(u16 *)((u8 *)sd + 2) = 0x8000;        /* self-relative, no DACL = everyone */
	return 0;
}
EXPORT(NtQuerySecurityObject);

static API NTSTATUS w_NtSetSecurityObject(u64 h, u32 info, void *sd)
{
	(void)h;
	(void)info;
	(void)sd;
	return 0;
}
EXPORT(NtSetSecurityObject);

static API NTSTATUS w_NtAllocateLocallyUniqueId(u64 *luid)
{
	static u64 next = 0x1000;

	*luid = __atomic_add_fetch(&next, 1, __ATOMIC_SEQ_CST);
	return 0;
}
EXPORT(NtAllocateLocallyUniqueId);

/* ---- system information ---------------------------------------------------------- */

static API NTSTATUS w_NtQuerySystemInformation(u32 cls, void *buf, u32 len, u32 *ret)
{
	u8 *b = buf;
	u32 need;

	switch (cls) {
	case 0:         /* SystemBasicInformation */
	case 62:        /* SystemEmulationBasicInformation */
	case 114:       /* SystemNativeBasicInformation */
		need = 64;
		if (len < need)
			break;
		memset(b, 0, 64);
		*(u32 *)(b + 4) = 156250;
		*(u32 *)(b + 8) = 4096;
		*(u32 *)(b + 12) = 16384;
		*(u32 *)(b + 16) = 1;
		*(u32 *)(b + 20) = 16384;
		*(u32 *)(b + 24) = 65536;
		*(u64 *)(b + 32) = 0x10000;
		*(u64 *)(b + 40) = 0x7FFFFFFEFFFFull;
		*(u64 *)(b + 48) = 1;
		b[56] = 1;
		break;
	case 1:         /* SystemCpuInformation */
	case 63:        /* SystemEmulationProcessorInformation */
		need = 12;
		if (len < need)
			break;
		*(u16 *)b = 9;                  /* PROCESSOR_ARCHITECTURE_AMD64 */
		*(u16 *)(b + 2) = 6;
		*(u16 *)(b + 4) = 0x0F01;
		*(u16 *)(b + 6) = 1;
		*(u32 *)(b + 8) = 0x0020CDFF;   /* the features every x86-64 has */
		break;
	case 2:         /* SystemPerformanceInformation */
		need = 0x158;
		if (len < 0x138)
			break;
		memset(b, 0, len < need ? len : need);
		need = len < need ? len : need;
		break;
	case 3: {       /* SystemTimeOfDayInformation */
		s64 now;

		need = 48;
		if (len < need)
			break;
		memset(b, 0, 48);
		NtQuerySystemTime(&now);
		*(s64 *)b = BOOT_TIME;
		*(s64 *)(b + 8) = now;
		break;
	}
	case 8:         /* SystemProcessorPerformanceInformation */
		need = 48;
		if (len < need)
			break;
		memset(b, 0, 48);
		break;
	case 9:         /* SystemFlagsInformation */
	case 50:        /* SystemRangeStartInformation */
		need = cls == 9 ? 4 : 8;
		if (len < need)
			break;
		if (cls == 9)
			*(u32 *)b = 0;
		else
			*(u64 *)b = 0xFFFF800000000000ull;
		break;
	case 35:        /* SystemKernelDebuggerInformation */
		need = 2;
		if (len < need)
			break;
		b[0] = 0;
		b[1] = 1;
		break;
	case 105:       /* SystemCodeIntegrityInformation */
		need = 8;
		if (len < need)
			break;
		*(u32 *)(b + 4) = 0;
		break;
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
	if (ret)
		*ret = need;
	return len < need ? STATUS_INFO_LENGTH_MISMATCH : 0;
}
EXPORT(NtQuerySystemInformation);

static API NTSTATUS w_NtQuerySystemInformationEx(u32 cls, void *q, u32 qlen, void *buf, u32 len,
						 u32 *ret)
{
	(void)q;
	(void)qlen;
	return w_NtQuerySystemInformation(cls, buf, len, ret);
}
EXPORT(NtQuerySystemInformationEx);

static API NTSTATUS w_RtlGetNativeSystemInformation(u32 cls, void *buf, u32 len, u32 *ret)
{
	return w_NtQuerySystemInformation(cls, buf, len, ret);
}
EXPORT(RtlGetNativeSystemInformation);

static API NTSTATUS w_NtSetSystemInformation(u32 cls, void *buf, u32 len)
{
	(void)cls;
	(void)buf;
	(void)len;
	return 0;
}
EXPORT(NtSetSystemInformation);

static API NTSTATUS w_NtPowerInformation(u32 lvl, void *in, u32 inlen, void *out, u32 outlen)
{
	(void)lvl;
	(void)in;
	(void)inlen;
	if (out)
		memset(out, 0, outlen);
	return 0;
}
EXPORT(NtPowerInformation);

static API NTSTATUS w_NtQueryPerformanceCounter(s64 *c, s64 *f)
{
	return NtQueryPerformanceCounter(c, f);
}
EXPORT(NtQueryPerformanceCounter);

static API NTSTATUS w_NtQuerySystemTime(s64 *t)
{
	return NtQuerySystemTime(t);
}
EXPORT(NtQuerySystemTime);

static API u32 w_NtGetTickCount(void)
{
	s64 c;

	NtQueryPerformanceCounter(&c, NULL);
	return (u32)(c / 10000);
}
EXPORT(NtGetTickCount);

static API NTSTATUS w_NtSetSystemTime(s64 *t, s64 *old)
{
	(void)t;
	if (old)
		NtQuerySystemTime(old);
	return 0;
}
EXPORT(NtSetSystemTime);

static API u64 w_RtlGetSystemTimePrecise(void)
{
	s64 t;

	NtQuerySystemTime(&t);
	return (u64)t;
}
EXPORT(RtlGetSystemTimePrecise);

static API BOOL w_RtlQueryUnbiasedInterruptTime(u64 *t)
{
	s64 c;

	NtQueryPerformanceCounter(&c, NULL);
	*t = (u64)c;
	return 1;
}
EXPORT(RtlQueryUnbiasedInterruptTime);

static API BOOL w_RtlQueryPerformanceCounter(s64 *c)
{
	NtQueryPerformanceCounter(c, NULL);
	return 1;
}
EXPORT(RtlQueryPerformanceCounter);

static API BOOL w_RtlQueryPerformanceFrequency(s64 *f)
{
	*f = 10000000;
	return 1;
}
EXPORT(RtlQueryPerformanceFrequency);

/* ---- NLS tables ------------------------------------------------------------------- */

static struct {
	u32 type, id;
	void *data;
	u64 size;
} nls_cache[16];

static void *nls_load(u32 type, u32 id, const char *file, u64 *size)
{
	char path[96];
	u64 st[2];
	long fd;
	void *p = NULL;

	for (u32 i = 0; i < 16; i++)
		if (nls_cache[i].data && nls_cache[i].type == type && nls_cache[i].id == id) {
			if (size)
				*size = nls_cache[i].size;
			return nls_cache[i].data;
		}
	memcpy(path, "/windows/system32/", 18);
	nt_strlcpy(path + 18, file, sizeof(path) - 18);
	fd = mos_open(path, MOS_O_RDONLY);
	if (fd < 0)
		return NULL;
	if (mos_fstat(fd, st) >= 0 && st[0] && !nt_vm_alloc(&p, st[0])) {
		u64 got = 0;

		while (got < st[0]) {
			long r = mos_read(fd, (u8 *)p + got, st[0] - got);

			if (r <= 0)
				break;
			got += (u64)r;
		}
		if (got != st[0])
			p = NULL;
	}
	mos_close(fd);
	for (u32 i = 0; p && i < 16; i++)
		if (!nls_cache[i].data) {
			nls_cache[i].type = type;
			nls_cache[i].id = id;
			nls_cache[i].data = p;
			nls_cache[i].size = st[0];
			break;
		}
	if (size)
		*size = st[0];
	return p;
}

static void fmt_u(char *out, const char *pre, u32 v, const char *post)
{
	char d[12];
	int n = 0;

	while (*pre)
		*out++ = *pre++;
	do
		d[n++] = (char)('0' + v % 10);
	while ((v /= 10));
	if (n < 3)                              /* c_037.nls, c_437.nls */
		while (n < 3)
			d[n++] = '0';
	while (n)
		*out++ = d[--n];
	while (*post)
		*out++ = *post++;
	*out = 0;
}

void *nt_nls(u32 type, u32 id, u64 *size)
{
	char file[32];

	switch (type) {
	case 9:                                 /* NLS_SECTION_SORTKEYS */
		nt_strlcpy(file, "sortdefault.nls", sizeof(file));
		break;
	case 10:                                /* NLS_SECTION_CASEMAP */
		nt_strlcpy(file, "l_intl.nls", sizeof(file));
		break;
	case 11:                                /* NLS_SECTION_CODEPAGE */
		fmt_u(file, "c_", id, ".nls");
		break;
	case 12: {                              /* NLS_SECTION_NORMALIZE */
		static const char *const forms[] = { NULL, "normnfc.nls", "normnfd.nls", NULL, NULL,
						     "normnfkc.nls", "normnfkd.nls" };

		if (id >= 7 || !forms[id])
			return NULL;
		nt_strlcpy(file, forms[id], sizeof(file));
		break;
	}
	default:
		return NULL;
	}
	return nls_load(type, id, file, size);
}

static API NTSTATUS w_NtGetNlsSectionPtr(u32 type, u32 id, void *unk, void **ptr, u64 *size)
{
	(void)unk;
	*ptr = nt_nls(type, id, size);
	return *ptr ? 0 : STATUS_OBJECT_NAME_NOT_FOUND;
}
EXPORT(NtGetNlsSectionPtr);

static API NTSTATUS w_NtInitializeNlsFiles(void **ptr, u32 *lcid, s64 *size)
{
	u64 sz = 0;

	*ptr = nls_load(0, 0, "locale.nls", &sz);
	if (!*ptr)
		return STATUS_OBJECT_NAME_NOT_FOUND;
	if (lcid)
		*lcid = 0x409;
	if (size)
		*size = (s64)sz;
	return 0;
}
EXPORT(NtInitializeNlsFiles);

/* Older name of the same service (Wine's kernelbase still imports it). */
static API NTSTATUS w_RtlGetLocaleFileMappingAddress(void **ptr, u32 *lcid, s64 *size)
{
	return w_NtInitializeNlsFiles(ptr, lcid, size);
}
EXPORT(RtlGetLocaleFileMappingAddress);

static API NTSTATUS w_NtQueryDefaultLocale(u8 user, u32 *lcid)
{
	(void)user;
	*lcid = 0x409;
	return 0;
}
EXPORT(NtQueryDefaultLocale);

static API NTSTATUS w_NtQueryDefaultUILanguage(u16 *lang)
{
	*lang = 0x409;
	return 0;
}
EXPORT(NtQueryDefaultUILanguage);
EXPORT_AS("NtQueryInstallUILanguage", w_NtQueryDefaultUILanguage);

static API NTSTATUS w_NtSetDefaultLocale(u8 user, u32 lcid)
{
	(void)user;
	(void)lcid;
	return 0;
}
EXPORT(NtSetDefaultLocale);

/* ---- Wine's debug channels: all off ------------------------------------------------ */

static API u8 w___wine_dbg_get_channel_flags(void *channel)
{
	(void)channel;
	return 0;
}
EXPORT(__wine_dbg_get_channel_flags);

static API int w___wine_dbg_header(int cls, void *channel, const char *function)
{
	(void)cls;
	(void)channel;
	(void)function;
	return -1;
}
EXPORT(__wine_dbg_header);

static API int w___wine_dbg_output(const char *str)
{
	size_t n = rt_strlen(str);

	rt_write(2, str, n);
	return (int)n;
}
EXPORT(__wine_dbg_output);

static API const char *w___wine_dbg_strdup(const char *str)
{
	static char ring[4096];
	static u32 pos;
	size_t n = rt_strlen(str) + 1;
	char *p;

	if (n > 1024)
		n = 1024;
	if (pos + n > sizeof(ring))
		pos = 0;
	p = ring + pos;
	pos += (u32)n;
	memcpy(p, str, n - 1);
	p[n - 1] = 0;
	return p;
}
EXPORT(__wine_dbg_strdup);

/* ---- process start-up -------------------------------------------------------------- */

static void ustr_set(u8 *at, const wchar *s, size_t cap_chars)
{
	UNICODE_STRING *u = (UNICODE_STRING *)at;

	u->len = (u16)(rt_wcslen(s) * 2);
	u->max = (u16)(cap_chars ? cap_chars * 2 : u->len + 2);
	u->buf = (wchar *)s;
}

static wchar *wdup8(const char *s)
{
	size_t n = rt_strlen(s);
	wchar *w = nt_alloc((n + 1) * 2);

	if (w)
		nt_utow(w, n + 1, s, n + 1);
	return w;
}

static u8 *params;
static wchar curdir[262];

/* RTL_USER_PROCESS_PARAMETERS, the environment block and the NLS tables. */
void nt_init_process(void)
{
	static const char *const env[] = {
		"ALLUSERSPROFILE=C:\\ProgramData", "APPDATA=C:\\home\\user\\AppData\\Roaming",
		"COMSPEC=C:\\windows\\system32\\cmd.exe", "HOMEDRIVE=C:", "HOMEPATH=\\home\\user",
		"LOCALAPPDATA=C:\\home\\user\\AppData\\Local", "NUMBER_OF_PROCESSORS=1",
		"OS=Windows_NT", "PATH=C:\\windows\\system32;C:\\windows;C:\\bin",
		"PATHEXT=.COM;.EXE;.BAT;.CMD", "PROCESSOR_ARCHITECTURE=AMD64",
		"ProgramData=C:\\ProgramData", "ProgramFiles=C:\\Program Files", "PROMPT=$P$G",
		"PUBLIC=C:\\users\\Public", "SystemDrive=C:", "SystemRoot=C:\\windows",
		"TEMP=C:\\tmp", "TMP=C:\\tmp", "USERDOMAIN=MERGEDOS", "USERNAME=user",
		"USERPROFILE=C:\\home\\user", "windir=C:\\windows",
	};
	size_t total = 0, pos = 0;
	wchar *block, *img, *cmd;
	char line[48];

	std_handles_init();
	params = nt_alloc(0x1000);
	*(u8 **)(nt_peb + 0x20) = params;
	*(u32 *)params = 0x1000;
	*(u32 *)(params + 4) = 0x1000;
	*(u32 *)(params + 8) = 1;               /* PROCESS_PARAMETERS_NORMALIZED */
	*(u64 *)(params + 0x20) = 4;
	*(u64 *)(params + 0x28) = 8;
	*(u64 *)(params + 0x30) = 0xC;
	nt_utow(curdir, 262, "C:\\", 4);
	ustr_set(params + 0x38, curdir, 262);
	ustr_set(params + 0x50, wdup8("C:\\windows\\system32;C:\\windows"), 0);
	img = (wchar *)ldr_image_path();
	cmd = wdup8(rt_params->command_line);
	ustr_set(params + 0x70, cmd, 0);
	ustr_set(params + 0xB0, cmd, 0);        /* WindowTitle */
	*(u32 *)(params + 0xA4) = 0;
	for (size_t i = 0; i < sizeof(env) / sizeof(env[0]); i++)
		total += rt_strlen(env[i]) + 1;
	fmt_u(line, "COMPUTERNAME=", 0, "");
	total += 14 + rt_strlen(rt_params->computer_name) + 2;
	block = nt_alloc((total + 1) * 2);
	for (size_t i = 0; i < sizeof(env) / sizeof(env[0]); i++) {
		if (i == 3) {                   /* keep the block sorted: COMPUTERNAME */
			char c[40];
			size_t n = nt_strlcpy(c, "COMPUTERNAME=", sizeof(c));

			nt_strlcpy(c + n, rt_params->computer_name, sizeof(c) - n);
			pos += nt_utow(block + pos, total - pos, c, rt_strlen(c) + 1);
		}
		pos += nt_utow(block + pos, total - pos, env[i], rt_strlen(env[i]) + 1);
	}
	block[pos++] = 0;
	*(wchar **)(params + 0x80) = block;
	*(u64 *)(params + 0x3F0) = pos * 2;
	(void)img;

	/* Code-page tables, when Wine's NLS files are installed. */
	*(void **)(nt_peb + 0xA0) = nt_nls(11, 1252, NULL);    /* AnsiCodePageData */
	*(void **)(nt_peb + 0xA8) = nt_nls(11, 437, NULL);     /* OemCodePageData  */
	*(void **)(nt_peb + 0xB0) = nt_nls(10, 0, NULL);       /* UnicodeCaseTableData */
	{
		/* PEB TLS bitmaps (64 + 1024 slots), number of processors. */
		static u64 tls_bits[1];
		static u64 tls_exp_bits[16];
		static struct { u32 size; u32 pad; u64 *buf; } tls_bm, tls_exp_bm;

		tls_bm.size = 64;
		tls_bm.buf = tls_bits;
		tls_exp_bm.size = 1024;
		tls_exp_bm.buf = tls_exp_bits;
		*(void **)(nt_peb + 0x78) = &tls_bm;
		*(void **)(nt_peb + 0x238) = &tls_exp_bm;
		*(u32 *)(nt_peb + 0xB8) = 1;
	}
	nt_reg_init();
}

/* Image path set after the loader registered the program (ldr.c calls back). */
void nt_set_image_path(const wchar *path)
{
	ustr_set(params + 0x60, path, 0);
}

const wchar *nt_curdir(void)
{
	return curdir;
}

void nt_set_curdir(const wchar *dir)
{
	size_t n = 0;

	for (; dir[n] && n < 259; n++)
		curdir[n] = dir[n];
	if (n && curdir[n - 1] != '\\')
		curdir[n++] = '\\';
	curdir[n] = 0;
	ustr_set(params + 0x38, curdir, 262);
}

u8 *nt_params(void)
{
	return params;
}

void *nt_realloc_(void *p, size_t n)
{
	return nt_realloc(p, n);
}
