/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Self-contained Linux bundles:
 *
 *   AppImage  ELF runtime + squashfs; AppRun and a .desktop entry at the root
 *   Snap      squashfs; identity in meta/snap.yaml
 *   Flatpak   single-file bundle (an OSTree static delta). The payload is not
 *             decoded; identity comes from the [Application] key file and the
 *             "app/<id>/<arch>/<branch>" ref the bundle carries in clear.
 */
#include "pkg_translate/pkg_internal.h"
#include "pkg_translate/squashfs.h"

/* "key<sep> value" at the start of a line (after indentation if `indent`). */
static bool kv(const char *t, size_t n, const char *key, char sep, bool indent, char *out,
	       size_t cap)
{
	size_t kl = strlen(key);

	for (size_t i = 0; i + kl < n; i++) {
		size_t j = i, s, e;

		if (i && t[i - 1] != '\n')
			continue;
		while (indent && j < n && t[j] == ' ')
			j++;
		if (n - j <= kl || memcmp(t + j, key, kl) || t[j + kl] != sep)
			continue;
		for (s = j + kl + 1; s < n && (t[s] == ' ' || t[s] == '\t'); s++)
			;
		for (e = s; e < n && t[e] != '\n' && t[e] != '\r' && t[e]; e++)
			;
		if (e - s >= 2 && (t[s] == '"' || t[s] == '\'') && t[e - 1] == t[s]) {
			s++;
			e--;
		}
		mos_fieldcpy(out, cap, t + s, e - s);
		return true;
	}
	return false;
}

static enum pkg_codec sq_codec(u16 comp)
{
	return comp == 1 ? PKG_CODEC_GZIP : comp == 4 ? PKG_CODEC_XZ : comp == 5 ? PKG_CODEC_LZ4
	     : comp == 6 ? PKG_CODEC_ZSTD : PKG_CODEC_UNKNOWN;
}

/* Squashfs work area at the tail of the scratch buffer, output space in front. */
static int bundle_open(const u8 *img, size_t len, u64 off, struct pkg_scratch *s,
		       struct sqfs *fs, u8 **buf, size_t *cap)
{
	size_t wk;
	u8 *work;

	if (off >= len)
		return -MERR_NOEXEC;
	wk = sqfs_work_size(img + off, len - off);
	if (!s || !s->buf || s->cap < wk + 4096 + 8)
		return -MERR_NOMEM;
	work = (u8 *)ALIGN_DOWN((u64)s->buf + s->cap - wk, 8);
	*buf = s->buf;
	*cap = (size_t)(work - s->buf);
	return sqfs_open(fs, img + off, len - off, work, wk);
}

/* First `cap` bytes of a small text file, NUL-terminated. */
static int read_text(struct sqfs *fs, const struct sqfs_inode *ino, u8 *buf, size_t cap,
		     size_t *n)
{
	int rc = sqfs_read(fs, ino, buf, cap - 1, n);

	if (rc < 0)
		return rc;
	buf[*n] = '\0';
	return 0;
}

static int appimage_meta(struct sqfs *fs, u8 *buf, size_t cap, struct pkg_manifest *m)
{
	struct sqfs_inode root, ino;
	struct sqfs_dir d;
	char name[128];
	size_t n;
	u64 ref;
	int rc;

	if ((rc = sqfs_lookup(fs, "AppRun", &ino)))
		return rc == -MERR_NOENT ? -MERR_NOEXEC : rc;
	mos_strlcpy(m->executable, "AppRun", sizeof(m->executable));
	if ((rc = sqfs_inode(fs, fs->root, &root)) || (rc = sqfs_dir_open(fs, &root, &d)))
		return rc;
	while ((rc = sqfs_dir_next(fs, &d, name, sizeof(name), &ref)) == 1) {
		if (!mos_has_suffix(name, ".desktop") || sqfs_inode(fs, ref, &ino) ||
		    ino.kind != SQFS_FILE)
			continue;
		if ((rc = read_text(fs, &ino, buf, cap < 4096 ? cap : 4096, &n)))
			return rc;
		if (!kv((char *)buf, n, "Name", '=', false, m->name, sizeof(m->name)))
			mos_fieldcpy(m->name, sizeof(m->name), name, strlen(name) - 8);
		kv((char *)buf, n, "X-AppImage-Version", '=', false, m->version, sizeof(m->version));
		kv((char *)buf, n, "Comment", '=', false, m->summary, sizeof(m->summary));
		break;
	}
	return rc < 0 ? rc : 0;
}

static int snap_meta(struct sqfs *fs, u8 *buf, size_t cap, struct pkg_manifest *m)
{
	struct sqfs_inode ino;
	char tmp[64], *t = (char *)buf;
	const char *a;
	size_t n;
	int rc;

	if ((rc = sqfs_lookup(fs, "meta/snap.yaml", &ino)))
		return rc == -MERR_NOENT ? -MERR_NOEXEC : rc;
	if ((rc = read_text(fs, &ino, buf, cap < 8192 ? cap : 8192, &n)))
		return rc;
	kv(t, n, "name", ':', false, m->name, sizeof(m->name));
	kv(t, n, "version", ':', false, m->version, sizeof(m->version));
	kv(t, n, "summary", ':', false, m->summary, sizeof(m->summary));
	if (kv(t, n, "command", ':', true, m->executable, sizeof(m->executable)) &&
	    mos_has_prefix(m->executable, "$SNAP/"))
		memmove(m->executable, m->executable + 6, strlen(m->executable + 6) + 1);
	a = mos_memstr(t, n, "\narchitectures:");
	if (a && (a = mos_memstr(a, n - (size_t)(a - t), "- "))) {
		size_t k = 0;

		for (a += 2; a[k] && a[k] != '\n' && a[k] != '\r' && a[k] != ' '; k++)
			;
		mos_fieldcpy(tmp, sizeof(tmp), a, k);
		pkg_set_arch(m, tmp);
	} else {
		pkg_set_arch(m, "all");
	}
	if (!kv(t, n, "base", ':', false, tmp, sizeof(tmp)))
		mos_strlcpy(tmp, "core", sizeof(tmp));
	pkg_add_dep(m, tmp, "");
	return 0;
}

static int flatpak_meta(const u8 *img, size_t len, struct pkg_manifest *m)
{
	const char *t = (const char *)img, *app = mos_memstr(t, len, "[Application]\n"), *ref;
	char key[112], dep[64];
	size_t n;

	if (!app)
		return -MERR_NOEXEC;
	n = len - (size_t)(app - t) < 4096 ? len - (size_t)(app - t) : 4096;
	if (!kv(app, n, "name", '=', false, m->bundle_id, sizeof(m->bundle_id)) || !m->bundle_id[0])
		return -MERR_NOEXEC;
	mos_strlcpy(m->name, m->bundle_id, sizeof(m->name));
	kv(app, n, "command", '=', false, m->executable, sizeof(m->executable));
	if (kv(app, n, "runtime", '=', false, dep, sizeof(dep)))
		pkg_add_dep(m, dep, "");
	/* The ref: app/<id>/<arch>/<branch>, NUL-terminated (a GVariant string). */
	mos_snprintf(key, sizeof(key), "app/%s/", m->bundle_id);
	ref = mos_memstr(t, len, key);
	if (ref) {
		const char *arch = ref + strlen(key), *end = t + len, *br;

		for (br = arch; br < end && *br && *br != '/'; br++)
			;
		mos_fieldcpy(dep, sizeof(dep), arch, (size_t)(br - arch));
		pkg_set_arch(m, dep);
		if (br < end && *br == '/') {
			const char *e = ++br;

			while (e < end && *e && *e != '\n')
				e++;
			mos_fieldcpy(m->version, sizeof(m->version), br, (size_t)(e - br));
		}
	}
	m->payload_codec = PKG_CODEC_UNKNOWN;
	m->payload_size = len;
	return 0;
}

int bundle_translate(const u8 *img, size_t len, struct pkg_scratch *s, struct pkg_manifest *m)
{
	struct sqfs fs = { 0 };
	long off;
	u8 *buf;
	size_t cap;
	int rc;

	m->meta_codec = PKG_CODEC_NONE;
	m->flags |= PKG_F_RELOCATABLE;
	if (m->format == PKG_FLATPAK)
		rc = flatpak_meta(img, len, m);
	else {
		off = sqfs_find(img, len);
		if (off < 0 || (m->format == PKG_APPIMAGE) != (off > 0))
			return -MERR_NOEXEC;
		rc = bundle_open(img, len, (u64)off, s, &fs, &buf, &cap);
		m->payload_off = (u64)off;
		m->payload_size = fs.bytes_used;
		m->payload_codec = m->meta_codec = sq_codec(fs.comp);
		if (rc)
			return rc;
		if (m->format == PKG_APPIMAGE) {
			u16 mach = rd16le(img + 18);

			pkg_set_arch(m, mach == 0x3E ? "x86_64" : mach == 0xB7 ? "aarch64"
				      : mach == 3 ? "i386" : "unknown");
			rc = appimage_meta(&fs, buf, cap, m);
		} else {
			rc = snap_meta(&fs, buf, cap, m);
		}
		m->flags |= PKG_F_PAYLOAD_OK;
	}
	if (rc)
		return rc;
	if (!m->name[0])
		mos_strlcpy(m->name, pkg_family(m->format), sizeof(m->name));
	if (!m->version[0])
		mos_strlcpy(m->version, "0", sizeof(m->version));
	return 0;
}

int bundle_exec(const u8 *img, size_t len, const struct pkg_manifest *m, struct pkg_scratch *s,
		const u8 **exe, size_t *exe_len, char *path, size_t path_cap)
{
	struct sqfs fs;
	struct sqfs_inode ino;
	char cur[128], link[128];
	u8 *buf;
	size_t cap, got, slash;
	int rc;

	if (m->format == PKG_FLATPAK)
		return -MERR_NOTSUP;
	if ((rc = bundle_open(img, len, m->payload_off, s, &fs, &buf, &cap)))
		return rc;
	mos_strlcpy(cur, m->executable, sizeof(cur));
	for (int hop = 0;; hop++) {                     /* follow up to 4 symlinks */
		if ((rc = sqfs_lookup(&fs, cur, &ino)))
			return rc;
		if (ino.kind != SQFS_SYMLINK)
			break;
		if (hop == 4 || (rc = sqfs_readlink(&fs, &ino, link, sizeof(link))) < 0)
			return rc < 0 ? rc : -MERR_NOENT;
		slash = 0;
		for (size_t k = 0; cur[k]; k++)
			if (cur[k] == '/')
				slash = k + 1;
		if (link[0] == '/')
			slash = 0;
		cur[slash] = '\0';
		mos_strlcat(cur, link, sizeof(cur));
	}
	if (ino.kind != SQFS_FILE)
		return -MERR_NOENT;
	rc = sqfs_read(&fs, &ino, buf, cap, &got);
	if (rc)
		return rc == 1 ? -MERR_RANGE : rc;
	*exe = buf;
	*exe_len = got;
	mos_strlcpy(path, cur[0] == '/' ? cur + 1 : cur, path_cap);
	return 0;
}
