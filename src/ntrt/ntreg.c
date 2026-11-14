/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS ntdll, registry (spec: docs/NT_USERSPACE.md part 3).
 *
 * The hive is a directory tree in the writable ramfs under /tmp/.reg, so
 * every process sees the same keys:
 *   <root>/<lower-case key name>/...      one directory per key
 *   <key>/.name                           original spelling of the key name
 *   <key>/v_<escaped lower-case name>     one file per value:
 *                                         u32 type, u32 name chars, u32 data
 *                                         bytes, UTF-16 name, data
 * Roots: m = \Registry\Machine, u = \Registry\User\<sid>, uc = its classes.
 */
#include "src/ntrt/nt.h"

#define STATUS_CANNOT_DELETE   0xC0000121u
#define STATUS_KEY_HAS_CHILDREN 0xC0000428u
#define REG_ROOT "/tmp/.reg"
#define PATH_CAP 256

#define KEY_BASIC   0
#define KEY_NODE    1
#define KEY_FULL    2
#define KEY_NAME    3
#define KEY_CACHED  4
#define KEY_FLAGS   5

static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }

static size_t cstr_len(const char *s) { return rt_strlen(s); }

static bool cat(char *buf, size_t cap, const char *s)
{
	size_t n = cstr_len(buf), m = cstr_len(s);

	if (n + m + 1 > cap)
		return false;
	memcpy(buf + n, s, m + 1);
	return true;
}

void nt_reg_init(void)
{
	static const char *const dirs[] = { REG_ROOT, REG_ROOT "/m", REG_ROOT "/u", REG_ROOT "/uc" };

	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		mos_mkdir(dirs[i]);
}

NTSTATUS nt_reg_close(struct nt_obj *o)
{
	(void)o;
	return STATUS_SUCCESS;
}

/* ---- name resolution ------------------------------------------------------------ */

static bool is_dir(const char *path)
{
	u64 st[2];

	return mos_stat(path, st) >= 0 && st[1] == MOS_ST_DIR;
}

static bool wmatch(const wchar *s, size_t n, const char *lit)
{
	size_t i = 0;

	for (; i < n && lit[i]; i++)
		if (nt_downcase(s[i]) != (wchar)(u8)lit[i])
			return false;
	return i == n && !lit[i];
}

static void write_file(const char *path, const void *data, size_t len)
{
	long fd = mos_open(path, MOS_O_WRONLY | MOS_O_CREAT | MOS_O_TRUNC);

	if (fd < 0)
		return;
	if (len)
		mos_write(fd, data, len);
	mos_close(fd);
}

static void *read_file(const char *path, u64 *len)
{
	u64 st[2];
	long fd = mos_open(path, MOS_O_RDONLY);
	u8 *buf;
	long n;

	if (fd < 0)
		return NULL;
	if (mos_fstat(fd, st) < 0) {
		mos_close(fd);
		return NULL;
	}
	buf = nt_alloc(st[0] + 1);
	if (!buf) {
		mos_close(fd);
		return NULL;
	}
	n = st[0] ? mos_read(fd, buf, st[0]) : 0;
	mos_close(fd);
	if (n < 0 || (u64)n != st[0]) {
		nt_free(buf);
		return NULL;
	}
	*len = st[0];
	return buf;
}

/*
 * Resolve an object name to the key's directory. With `create`, missing
 * components are created. `ntname` receives the absolute NT name with the
 * caller's spelling.
 */
static NTSTATUS resolve(const OBJECT_ATTRIBUTES *oa, bool create, char *path, wchar *ntname,
			u32 *disp)
{
	const wchar *s = oa->name ? oa->name->buf : NULL;
	size_t n = oa->name ? oa->name->len / 2 : 0, i = 0, nn = 0;
	bool absolute;

	path[0] = 0;
	ntname[0] = 0;
	if (disp)
		*disp = 2;
	if (oa->root) {
		struct nt_obj *r = nt_handle_get(oa->root);

		if (!r || r->kind != NTH_KEY || !r->path)
			return STATUS_INVALID_HANDLE;
		if (!cat(path, PATH_CAP, r->path))
			return STATUS_OBJECT_NAME_INVALID;
		for (const wchar *q = r->ntname; q && *q && nn < 1000; q++)
			ntname[nn++] = *q;
		absolute = false;
	} else {
		absolute = true;
		cat(path, PATH_CAP, REG_ROOT);
	}
	while (i < n && s[i] == '\\')
		i++;
	if (absolute) {
		size_t a = i, b;
		const char *root = NULL;

		while (i < n && s[i] != '\\')
			i++;
		if (!wmatch(s + a, i - a, "registry"))
			return STATUS_OBJECT_NAME_NOT_FOUND;
		memcpy(ntname, L"\\Registry", 18);
		nn = 9;
		while (i < n && s[i] == '\\')
			i++;
		a = i;
		while (i < n && s[i] != '\\')
			i++;
		if (i == a)
			goto done;
		if (wmatch(s + a, i - a, "machine"))
			root = "/m";
		else if (wmatch(s + a, i - a, "user"))
			root = "/u";
		else
			return STATUS_OBJECT_NAME_NOT_FOUND;
		ntname[nn++] = '\\';
		for (b = a; b < i; b++)
			ntname[nn++] = s[b];
		if (root[1] == 'u') {
			/* \Registry\User\<sid>[_Classes] */
			while (i < n && s[i] == '\\')
				i++;
			a = i;
			while (i < n && s[i] != '\\')
				i++;
			if (i == a)
				goto done;
			if (i - a > 8 && wmatch(s + i - 8, 8, "_classes"))
				root = "/uc";
			ntname[nn++] = '\\';
			for (b = a; b < i; b++)
				ntname[nn++] = s[b];
		}
		if (!cat(path, PATH_CAP, root))
			return STATUS_OBJECT_NAME_INVALID;
	}
	while (i < n) {
		char comp[96], orig[96];
		size_t a, cl = 0, ol;
		wchar low[48];

		a = i;
		while (i < n && s[i] != '\\')
			i++;
		if (i > a) {
			size_t len = i - a;

			if (len > 40 || nn + len + 2 >= 1000)
				return STATUS_OBJECT_NAME_INVALID;
			for (size_t k = 0; k < len; k++)
				low[k] = nt_downcase(s[a + k]);
			cl = nt_wtou(comp, sizeof(comp) - 1, low, len);
			ol = nt_wtou(orig, sizeof(orig) - 1, s + a, len);
			if (cl >= sizeof(comp) - 1 || ol >= sizeof(orig) - 1)
				return STATUS_OBJECT_NAME_INVALID;
			comp[cl] = 0;
			orig[ol] = 0;
			if (comp[0] == '.' && (cl == 1 || (cl == 2 && comp[1] == '.')))
				return STATUS_OBJECT_NAME_INVALID;
			for (size_t k = 0; k < cl; k++)
				if (comp[k] == '/')
					return STATUS_OBJECT_NAME_INVALID;
			if (!cat(path, PATH_CAP, "/") || !cat(path, PATH_CAP, comp))
				return STATUS_OBJECT_NAME_INVALID;
			ntname[nn++] = '\\';
			for (size_t k = 0; k < len; k++)
				ntname[nn++] = s[a + k];
			if (!is_dir(path)) {
				char nf[PATH_CAP + 8];

				if (!create)
					return STATUS_OBJECT_NAME_NOT_FOUND;
				if (mos_mkdir(path) < 0)
					return STATUS_OBJECT_NAME_NOT_FOUND;
				nt_strlcpy(nf, path, sizeof(nf));
				cat(nf, sizeof(nf), "/.name");
				write_file(nf, orig, ol);
				if (disp)
					*disp = 1;
			}
		}
		while (i < n && s[i] == '\\')
			i++;
	}
done:
	ntname[nn] = 0;
	if (!is_dir(path))
		return STATUS_OBJECT_NAME_NOT_FOUND;
	return STATUS_SUCCESS;
}

static NTSTATUS open_key(u64 *handle, const OBJECT_ATTRIBUTES *oa, bool create, u32 *disp,
			 u32 access)
{
	char path[PATH_CAP + 8];
	wchar ntn[1024];
	struct nt_obj *o;
	NTSTATUS s;
	size_t n;

	*handle = 0;
	if (!oa || !oa->name)
		return STATUS_INVALID_PARAMETER;
	s = resolve(oa, create, path, ntn, disp);
	nt_trace_line(create ? "NtCreateKey" : "NtOpenKey", oa->name, s);
	if (s)
		return s;
	o = nt_obj_new(NTH_KEY);
	if (!o)
		return STATUS_NO_MEMORY;
	n = cstr_len(path);
	o->path = nt_alloc(n + 1);
	memcpy(o->path, path, n + 1);
	for (n = 0; ntn[n]; n++)
		;
	o->ntname = nt_alloc((n + 1) * 2);
	memcpy(o->ntname, ntn, (n + 1) * 2);
	o->access = access;
	*handle = nt_handle_new(o);
	if (!*handle) {
		nt_obj_put(o);
		return STATUS_NO_MEMORY;
	}
	return STATUS_SUCCESS;
}

static struct nt_obj *key_of(u64 h)
{
	struct nt_obj *o = nt_handle_get(h);

	return o && o->kind == NTH_KEY ? o : NULL;
}

static API NTSTATUS w_NtCreateKey(u64 *h, u32 access, OBJECT_ATTRIBUTES *oa, u32 title,
				  void *cls, u32 options, u32 *disp)
{
	u32 d = 2;
	NTSTATUS s;

	(void)title;
	(void)cls;
	(void)options;
	s = open_key(h, oa, true, &d, access);
	if (!s && disp)
		*disp = d;
	return s;
}
EXPORT(NtCreateKey);

static API NTSTATUS w_NtOpenKey(u64 *h, u32 access, OBJECT_ATTRIBUTES *oa)
{
	return open_key(h, oa, false, NULL, access);
}
EXPORT(NtOpenKey);

static API NTSTATUS w_NtOpenKeyEx(u64 *h, u32 access, OBJECT_ATTRIBUTES *oa, u32 options)
{
	(void)options;
	return open_key(h, oa, false, NULL, access);
}
EXPORT(NtOpenKeyEx);

static API NTSTATUS w_NtFlushKey(u64 h)
{
	return key_of(h) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}
EXPORT(NtFlushKey);

static API NTSTATUS w_NtNotifyChangeKey(u64 h, u64 ev, void *apc, void *ctx, void *iosb,
					u32 filter, u8 subtree, void *buf, u32 len, u8 async)
{
	(void)h; (void)ev; (void)apc; (void)ctx; (void)iosb; (void)filter; (void)subtree;
	(void)buf; (void)len; (void)async;
	return STATUS_NOT_IMPLEMENTED;
}
EXPORT(NtNotifyChangeKey);

/* ---- directory scans -------------------------------------------------------------- */

struct reg_ent {
	bool is_key;
	char name[300];
};

/* The idx-th entry of the wanted kind (keys or values), or false. */
static bool nth_entry(const char *dir, bool want_key, u32 idx, struct reg_ent *out)
{
	long fd = mos_open(dir, MOS_O_RDONLY | MOS_O_DIRECTORY);
	char name[300];
	long r;
	u32 seen = 0;

	if (fd < 0)
		return false;
	while ((r = mos_readdir(fd, name, sizeof(name))) > 0) {
		bool key = r == 2;

		if (!nt_strcmp(name, ".") || !nt_strcmp(name, "..") || !nt_strcmp(name, ".name"))
			continue;
		if (key != want_key || (!key && !(name[0] == 'v' && name[1] == '_')))
			continue;
		if (seen++ == idx) {
			out->is_key = key;
			nt_strlcpy(out->name, name, sizeof(out->name));
			mos_close(fd);
			return true;
		}
	}
	mos_close(fd);
	return false;
}

/* Original spelling of a subkey name, UTF-16; returns chars. */
static size_t subkey_name(const char *dir, const char *lname, wchar *out, size_t cap)
{
	char p[PATH_CAP + 320];
	u64 len = 0;
	char *b;
	size_t n;

	nt_strlcpy(p, dir, sizeof(p));
	cat(p, sizeof(p), "/");
	cat(p, sizeof(p), lname);
	cat(p, sizeof(p), "/.name");
	b = read_file(p, &len);
	if (!b)
		return nt_utow(out, cap, lname, cstr_len(lname));
	n = nt_utow(out, cap, b, len);
	nt_free(b);
	return n;
}

struct value {
	u32 type, nlen, dlen;
	wchar *name;
	u8 *data;
	u8 *raw;
};

static bool load_value(const char *dir, const char *file, struct value *v)
{
	char p[PATH_CAP + 320];
	u64 len = 0;
	u8 *b;

	nt_strlcpy(p, dir, sizeof(p));
	cat(p, sizeof(p), "/");
	cat(p, sizeof(p), file);
	b = read_file(p, &len);
	if (!b)
		return false;
	if (len < 12) {
		nt_free(b);
		return false;
	}
	memcpy(&v->type, b, 4);
	memcpy(&v->nlen, b + 4, 4);
	memcpy(&v->dlen, b + 8, 4);
	if (12 + (u64)v->nlen * 2 + v->dlen > len) {
		nt_free(b);
		return false;
	}
	v->name = (wchar *)(b + 12);
	v->data = b + 12 + v->nlen * 2;
	v->raw = b;
	return true;
}

/* Value file name for a UTF-16 value name. */
static bool value_file(const wchar *name, size_t n, char *out, size_t cap)
{
	wchar low[260];
	char u[600];
	size_t ul, o = 2;

	if (n > 255)
		return false;
	for (size_t i = 0; i < n; i++)
		low[i] = nt_downcase(name[i]);
	ul = nt_wtou(u, sizeof(u), low, n);
	if (ul >= sizeof(u) || cap < 4)
		return false;
	out[0] = 'v';
	out[1] = '_';
	for (size_t i = 0; i < ul; i++) {
		u8 c = (u8)u[i];

		if (c == '/' || c == '%' || c < 0x20) {
			static const char hex[] = "0123456789abcdef";

			if (o + 3 >= cap)
				return false;
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		} else {
			if (o + 1 >= cap)
				return false;
			out[o++] = (char)c;
		}
	}
	out[o] = 0;
	return true;
}

/* ---- information ---------------------------------------------------------------- */

static NTSTATUS finish(u32 need, u32 have, u32 fixed, u32 *rlen)
{
	if (rlen)
		*rlen = need;
	if (have < fixed)
		return STATUS_BUFFER_TOO_SMALL;
	return have < need ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

static void copy_tail(u8 *buf, u32 have, u32 off, const void *src, u32 n)
{
	if (have > off)
		memcpy(buf + off, src, n < have - off ? n : have - off);
}

struct key_stats {
	u32 subkeys, max_sub, values, max_vname, max_vdata;
};

static void key_stats(const char *dir, struct key_stats *k)
{
	struct reg_ent e;
	wchar w[300];

	memset(k, 0, sizeof(*k));
	for (u32 i = 0; nth_entry(dir, true, i, &e); i++) {
		u32 n = (u32)subkey_name(dir, e.name, w, 300) * 2;

		k->subkeys++;
		if (n > k->max_sub)
			k->max_sub = n;
	}
	for (u32 i = 0; nth_entry(dir, false, i, &e); i++) {
		struct value v;

		if (!load_value(dir, e.name, &v))
			continue;
		k->values++;
		if (v.nlen * 2 > k->max_vname)
			k->max_vname = v.nlen * 2;
		if (v.dlen > k->max_vdata)
			k->max_vdata = v.dlen;
		nt_free(v.raw);
	}
}

static NTSTATUS key_info(const char *dir, const wchar *ntname, const wchar *name, u32 namelen,
			 u32 cls, u8 *buf, u32 len, u32 *rlen)
{
	u32 nbytes = namelen * 2;
	struct key_stats st;

	switch (cls) {
	case KEY_BASIC: {
		u32 need = 16 + nbytes;
		NTSTATUS s = finish(need, len, 16, rlen);

		if (len >= 16) {
			put64(buf, 0);
			put32(buf + 8, 0);
			put32(buf + 12, nbytes);
			copy_tail(buf, len, 16, name, nbytes);
		}
		return s;
	}
	case KEY_NODE: {
		u32 need = 24 + nbytes;
		NTSTATUS s = finish(need, len, 24, rlen);

		if (len >= 24) {
			put64(buf, 0);
			put32(buf + 8, 0);
			put32(buf + 12, need);
			put32(buf + 16, 0);
			put32(buf + 20, nbytes);
			copy_tail(buf, len, 24, name, nbytes);
		}
		return s;
	}
	case KEY_FULL: {
		NTSTATUS s = finish(44, len, 44, rlen);

		if (len >= 44) {
			key_stats(dir, &st);
			put64(buf, 0);
			put32(buf + 8, 0);
			put32(buf + 12, 44);
			put32(buf + 16, 0);
			put32(buf + 20, st.subkeys);
			put32(buf + 24, st.max_sub);
			put32(buf + 28, 0);
			put32(buf + 32, st.values);
			put32(buf + 36, st.max_vname);
			put32(buf + 40, st.max_vdata);
		}
		return s;
	}
	case KEY_NAME: {
		u32 nl = 0;
		u32 need;
		NTSTATUS s;

		while (ntname[nl])
			nl++;
		need = 4 + nl * 2;
		s = finish(need, len, 4, rlen);
		if (len >= 4) {
			put32(buf, nl * 2);
			copy_tail(buf, len, 4, ntname, nl * 2);
		}
		return s;
	}
	case KEY_CACHED: {
		u32 nl = 0;
		NTSTATUS s = finish(36, len, 36, rlen);

		while (ntname[nl])
			nl++;
		if (len >= 36) {
			key_stats(dir, &st);
			put64(buf, 0);
			put32(buf + 8, 0);
			put32(buf + 12, st.subkeys);
			put32(buf + 16, st.max_sub);
			put32(buf + 20, st.values);
			put32(buf + 24, st.max_vname);
			put32(buf + 28, st.max_vdata);
			put32(buf + 32, nl * 2);
		}
		return s;
	}
	case KEY_FLAGS: {
		NTSTATUS s = finish(4, len, 4, rlen);

		if (len >= 4)
			put32(buf, 0);
		return s;
	}
	default:
		return STATUS_INVALID_PARAMETER;
	}
}

static API NTSTATUS w_NtQueryKey(u64 h, u32 cls, void *buf, u32 len, u32 *rlen)
{
	struct nt_obj *o = key_of(h);
	const wchar *last;
	size_t n = 0;

	if (!o)
		return STATUS_INVALID_HANDLE;
	last = o->ntname;
	for (const wchar *q = o->ntname; q && *q; q++, n++)
		if (*q == '\\')
			last = q + 1;
	n = 0;
	while (last && last[n])
		n++;
	return key_info(o->path, o->ntname, last, (u32)n, cls, buf, len, rlen);
}
EXPORT(NtQueryKey);

static API NTSTATUS w_NtEnumerateKey(u64 h, u32 index, u32 cls, void *buf, u32 len, u32 *rlen)
{
	struct nt_obj *o = key_of(h);
	struct reg_ent e;
	wchar name[300];
	size_t n;
	char sub[PATH_CAP + 320];

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (cls > KEY_NODE && cls != KEY_FULL)
		return STATUS_INVALID_INFO_CLASS;
	if (!nth_entry(o->path, true, index, &e))
		return STATUS_NO_MORE_ENTRIES;
	n = subkey_name(o->path, e.name, name, 300);
	nt_strlcpy(sub, o->path, sizeof(sub));
	cat(sub, sizeof(sub), "/");
	cat(sub, sizeof(sub), e.name);
	return key_info(sub, L"", name, (u32)n, cls, buf, len, rlen);
}
EXPORT(NtEnumerateKey);

static NTSTATUS value_info(const struct value *v, u32 cls, u8 *buf, u32 len, u32 *rlen)
{
	u32 nb = v->nlen * 2;

	switch (cls) {
	case 0: {
		u32 need = 12 + nb;
		NTSTATUS s = finish(need, len, 12, rlen);

		if (len >= 12) {
			put32(buf, 0);
			put32(buf + 4, v->type);
			put32(buf + 8, nb);
			copy_tail(buf, len, 12, v->name, nb);
		}
		return s;
	}
	case 1: {
		u32 doff = (20 + nb + 7) & ~7u;
		u32 need = doff + v->dlen;
		NTSTATUS s = finish(need, len, 20, rlen);

		if (len >= 20) {
			put32(buf, 0);
			put32(buf + 4, v->type);
			put32(buf + 8, doff);
			put32(buf + 12, v->dlen);
			put32(buf + 16, nb);
			copy_tail(buf, len, 20, v->name, nb);
			copy_tail(buf, len, doff, v->data, v->dlen);
		}
		return s;
	}
	case 2: {
		u32 need = 12 + v->dlen;
		NTSTATUS s = finish(need, len, 12, rlen);

		if (len >= 12) {
			put32(buf, 0);
			put32(buf + 4, v->type);
			put32(buf + 8, v->dlen);
			copy_tail(buf, len, 12, v->data, v->dlen);
		}
		return s;
	}
	case 3: {
		u32 need = 8 + v->dlen;
		NTSTATUS s = finish(need, len, 8, rlen);

		if (len >= 8) {
			put32(buf, v->type);
			put32(buf + 4, v->dlen);
			copy_tail(buf, len, 8, v->data, v->dlen);
		}
		return s;
	}
	default:
		return STATUS_INVALID_INFO_CLASS;
	}
}

static API NTSTATUS w_NtSetValueKey(u64 h, const UNICODE_STRING *name, u32 title, u32 type,
				    const void *data, u32 size)
{
	struct nt_obj *o = key_of(h);
	char p[PATH_CAP + 320], f[300];
	size_t nch = name ? name->len / 2 : 0;
	u8 *blob;

	(void)title;
	if (!o)
		return STATUS_INVALID_HANDLE;
	if (!value_file(name ? name->buf : NULL, nch, f, sizeof(f)))
		return STATUS_INVALID_PARAMETER;
	blob = nt_alloc(12 + nch * 2 + size + 1);
	if (!blob)
		return STATUS_NO_MEMORY;
	put32(blob, type);
	put32(blob + 4, (u32)nch);
	put32(blob + 8, size);
	if (nch)
		memcpy(blob + 12, name->buf, nch * 2);
	if (size)
		memcpy(blob + 12 + nch * 2, data, size);
	nt_strlcpy(p, o->path, sizeof(p));
	cat(p, sizeof(p), "/");
	cat(p, sizeof(p), f);
	write_file(p, blob, 12 + nch * 2 + size);
	nt_free(blob);
	return STATUS_SUCCESS;
}
EXPORT(NtSetValueKey);

static API NTSTATUS w_NtQueryValueKey(u64 h, const UNICODE_STRING *name, u32 cls, void *buf,
				      u32 len, u32 *rlen)
{
	struct nt_obj *o = key_of(h);
	char f[300];
	struct value v;
	NTSTATUS s;

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (!value_file(name ? name->buf : NULL, name ? name->len / 2 : 0, f, sizeof(f)))
		return STATUS_OBJECT_NAME_NOT_FOUND;
	if (!load_value(o->path, f, &v))
		return STATUS_OBJECT_NAME_NOT_FOUND;
	s = value_info(&v, cls, buf, len, rlen);
	nt_free(v.raw);
	return s;
}
EXPORT(NtQueryValueKey);

static API NTSTATUS w_NtEnumerateValueKey(u64 h, u32 index, u32 cls, void *buf, u32 len,
					  u32 *rlen)
{
	struct nt_obj *o = key_of(h);
	struct reg_ent e;
	struct value v;
	NTSTATUS s;

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (cls > 2)
		return STATUS_INVALID_INFO_CLASS;
	if (!nth_entry(o->path, false, index, &e) || !load_value(o->path, e.name, &v))
		return STATUS_NO_MORE_ENTRIES;
	s = value_info(&v, cls, buf, len, rlen);
	nt_free(v.raw);
	return s;
}
EXPORT(NtEnumerateValueKey);

static API NTSTATUS w_NtDeleteValueKey(u64 h, const UNICODE_STRING *name)
{
	struct nt_obj *o = key_of(h);
	char p[PATH_CAP + 320], f[300];

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (!value_file(name ? name->buf : NULL, name ? name->len / 2 : 0, f, sizeof(f)))
		return STATUS_OBJECT_NAME_NOT_FOUND;
	nt_strlcpy(p, o->path, sizeof(p));
	cat(p, sizeof(p), "/");
	cat(p, sizeof(p), f);
	return mos_unlink(p, 0) < 0 ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_SUCCESS;
}
EXPORT(NtDeleteValueKey);

static API NTSTATUS w_NtDeleteKey(u64 h)
{
	struct nt_obj *o = key_of(h);
	struct reg_ent e;
	char p[PATH_CAP + 320];

	if (!o)
		return STATUS_INVALID_HANDLE;
	if (nth_entry(o->path, true, 0, &e))
		return STATUS_CANNOT_DELETE;
	if (!nt_strcmp(o->path, REG_ROOT "/m") || !nt_strcmp(o->path, REG_ROOT "/u") ||
	    !nt_strcmp(o->path, REG_ROOT "/uc"))
		return STATUS_CANNOT_DELETE;
	for (u32 i = 0; nth_entry(o->path, false, 0, &e) && i < 100000; i++) {
		nt_strlcpy(p, o->path, sizeof(p));
		cat(p, sizeof(p), "/");
		cat(p, sizeof(p), e.name);
		mos_unlink(p, 0);
	}
	nt_strlcpy(p, o->path, sizeof(p));
	cat(p, sizeof(p), "/.name");
	mos_unlink(p, 0);
	return mos_unlink(o->path, 1) < 0 ? STATUS_CANNOT_DELETE : STATUS_SUCCESS;
}
EXPORT(NtDeleteKey);

static API NTSTATUS w_RtlOpenCurrentUser(u32 access, u64 *h)
{
	static wchar name[] = L"\\Registry\\User\\S-1-5-21-0-0-0-1000";
	UNICODE_STRING us = { sizeof(name) - 2, sizeof(name), name };
	OBJECT_ATTRIBUTES oa = { sizeof(oa), 0, &us, 0x40, NULL, NULL };

	return open_key(h, &oa, false, NULL, access);
}
EXPORT(RtlOpenCurrentUser);
