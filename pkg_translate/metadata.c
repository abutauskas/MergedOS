/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Metadata readers: Debian control files, .PKGINFO (Alpine and Arch share
 * the syntax), and the RPM header structure. Each one fills the same
 * struct pkg_manifest.
 */
#include "pkg_translate/pkg_internal.h"

/* ---- small text helpers --------------------------------------------------- */

static void trim(const char **s, size_t *n)
{
	while (*n && (**s == ' ' || **s == '\t'))
		(*s)++, (*n)--;
	while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' || (*s)[*n - 1] == '\r'))
		(*n)--;
}

/* Next line of a text buffer; returns false at the end. */
static bool next_line(const char **p, size_t *left, const char **line, size_t *n)
{
	const char *nl;

	if (!*left)
		return false;
	*line = *p;
	nl = memchr(*p, '\n', *left);
	*n = nl ? (size_t)(nl - *p) : *left;
	*left -= nl ? *n + 1 : *n;
	*p += nl ? *n + 1 : *n;
	return true;
}

static size_t find_any(const char *s, size_t n, const char *set)
{
	for (size_t i = 0; i < n; i++)
		if (strchr(set, s[i]))
			return i;
	return n;
}

/* ---- Debian control ------------------------------------------------------- */

/* "libc6 (>= 2.34), libgcc-s1 | libgcc1, foo:any" */
static void deb_depends(struct pkg_manifest *m, const char *v, size_t n)
{
	while (n) {
		size_t item = find_any(v, n, ","), len, cut;
		const char *s = v;
		char name[64], constraint[24] = "";

		len = find_any(s, item, "|");           /* alternatives: keep the first */
		trim(&s, &len);
		cut = find_any(s, len, " (");
		mos_fieldcpy(name, sizeof(name), s, find_any(s, cut, ":"));
		if (cut < len) {
			const char *c = s + cut;
			size_t clen = len - cut;

			trim(&c, &clen);
			if (clen >= 2 && c[0] == '(' && c[clen - 1] == ')')
				mos_fieldcpy(constraint, sizeof(constraint), c + 1, clen - 2);
		}
		if (name[0])
			pkg_add_dep(m, name, constraint);
		v += item < n ? item + 1 : item;
		n -= item < n ? item + 1 : item;
	}
}

int deb_control_parse(const char *txt, size_t len, struct pkg_manifest *m)
{
	const char *line;
	size_t n;

	while (next_line(&txt, &len, &line, &n)) {
		size_t colon = find_any(line, n, ":");
		const char *v = line + colon + 1;
		size_t vn;

		if (!n || line[0] == ' ' || line[0] == '\t' || colon == n)
			continue;                       /* blank or continuation line */
		vn = n - colon - 1;
		trim(&v, &vn);
		if (colon == 7 && !memcmp(line, "Package", 7)) {
			mos_fieldcpy(m->name, sizeof(m->name), v, vn);
		} else if (colon == 7 && !memcmp(line, "Version", 7)) {
			mos_fieldcpy(m->version, sizeof(m->version), v, vn);
		} else if (colon == 12 && !memcmp(line, "Architecture", 12)) {
			char arch[16];

			mos_fieldcpy(arch, sizeof(arch), v, vn);
			pkg_set_arch(m, arch);
		} else if (colon == 11 && !memcmp(line, "Description", 11)) {
			mos_fieldcpy(m->summary, sizeof(m->summary), v, vn);
		} else if ((colon == 7 && !memcmp(line, "Depends", 7)) ||
			   (colon == 11 && !memcmp(line, "Pre-Depends", 11))) {
			deb_depends(m, v, vn);
		}
	}
	return m->name[0] && m->version[0] ? 0 : -MERR_NOEXEC;
}

/* ---- .PKGINFO (apk, pacman) ----------------------------------------------- */

int pkginfo_parse(const char *txt, size_t len, struct pkg_manifest *m, bool *is_alpine)
{
	const char *line;
	size_t n;

	*is_alpine = false;
	while (next_line(&txt, &len, &line, &n)) {
		size_t eq = find_any(line, n, "=");
		const char *k = line, *v = line + eq + 1;
		size_t kn = eq, vn;

		if (n && line[0] == '#') {
			if (mos_memstr(line, n, "abuild"))
				*is_alpine = true;
			continue;
		}
		if (eq == n)
			continue;
		vn = n - eq - 1;
		trim(&k, &kn);
		trim(&v, &vn);
		if (kn == 7 && !memcmp(k, "pkgname", 7)) {
			mos_fieldcpy(m->name, sizeof(m->name), v, vn);
		} else if (kn == 6 && !memcmp(k, "pkgver", 6)) {
			mos_fieldcpy(m->version, sizeof(m->version), v, vn);
		} else if (kn == 7 && !memcmp(k, "pkgdesc", 7)) {
			mos_fieldcpy(m->summary, sizeof(m->summary), v, vn);
		} else if (kn == 4 && !memcmp(k, "arch", 4)) {
			char arch[16];

			mos_fieldcpy(arch, sizeof(arch), v, vn);
			pkg_set_arch(m, arch);
		} else if (kn == 8 && !memcmp(k, "datahash", 8)) {
			*is_alpine = true;              /* only abuild writes this key */
		} else if (kn == 6 && !memcmp(k, "depend", 6)) {
			/* "gcc-libs>=13", "so:libz.so.1", "musl=1.2.4-r2" */
			size_t op = find_any(v, vn, "<>=");
			char name[64], constraint[24];

			mos_fieldcpy(name, sizeof(name), v, op);
			mos_fieldcpy(constraint, sizeof(constraint), v + op, vn - op);
			if (name[0] && name[0] != '!')  /* '!' marks an apk conflict */
				pkg_add_dep(m, name, constraint);
		}
	}
	return m->name[0] && m->version[0] ? 0 : -MERR_NOEXEC;
}

/* ---- RPM ------------------------------------------------------------------ */

#define RPM_LEAD_SIZE        96
#define RPM_HDR_FIXED        16         /* magic(4) reserved(4) nindex(4) hsize(4) */
#define RPMTAG_NAME          1000
#define RPMTAG_VERSION       1001
#define RPMTAG_RELEASE       1002
#define RPMTAG_SUMMARY       1004
#define RPMTAG_ARCH          1022
#define RPMTAG_PREIN         1023
#define RPMTAG_POSTIN        1024
#define RPMTAG_REQUIREFLAGS  1048
#define RPMTAG_REQUIRENAME   1049
#define RPMTAG_REQUIREVER    1050
#define RPMTAG_COMPRESSOR    1125
#define RPMSENSE_LESS        0x02
#define RPMSENSE_GREATER     0x04
#define RPMSENSE_EQUAL       0x08

struct rpm_hdr {
	const u8 *index, *store;
	u32 nindex, store_len;
};

static bool rpm_hdr_open(const u8 *img, size_t len, u64 off, struct rpm_hdr *h, u64 *end)
{
	static const u8 magic[4] = { 0x8E, 0xAD, 0xE8, 0x01 };

	if (!mos_in_range(len, off, RPM_HDR_FIXED) || memcmp(img + off, magic, 4))
		return false;
	h->nindex = rd32be(img + off + 8);
	h->store_len = rd32be(img + off + 12);
	if (h->nindex > 65536 ||
	    !mos_in_range(len, off + RPM_HDR_FIXED, (u64)h->nindex * 16 + h->store_len))
		return false;
	h->index = img + off + RPM_HDR_FIXED;
	h->store = h->index + (u64)h->nindex * 16;
	*end = off + RPM_HDR_FIXED + (u64)h->nindex * 16 + h->store_len;
	return true;
}

/* Find a tag; returns its data offset in the store and its count. */
static bool rpm_find(const struct rpm_hdr *h, u32 tag, u32 *off, u32 *count)
{
	for (u32 i = 0; i < h->nindex; i++) {
		const u8 *e = h->index + (u64)i * 16;

		if (rd32be(e) == tag && rd32be(e + 8) < h->store_len) {
			*off = rd32be(e + 8);
			*count = rd32be(e + 12);
			return true;
		}
	}
	return false;
}

/* NUL-terminated string at `off`, or NULL if it runs off the store. */
static const char *rpm_str(const struct rpm_hdr *h, u32 off)
{
	if (off >= h->store_len || !memchr(h->store + off, '\0', h->store_len - off))
		return NULL;
	return (const char *)h->store + off;
}

static void rpm_copy(const struct rpm_hdr *h, u32 tag, char *dst, size_t cap)
{
	u32 off, count;
	const char *s;

	if (rpm_find(h, tag, &off, &count) && (s = rpm_str(h, off)))
		mos_strlcpy(dst, s, cap);
}

static void rpm_requires(const struct rpm_hdr *h, struct pkg_manifest *m)
{
	u32 noff, nver = 0, nflag = 0, n, voff = 0, foff = 0;

	if (!rpm_find(h, RPMTAG_REQUIRENAME, &noff, &n))
		return;
	if (!rpm_find(h, RPMTAG_REQUIREVER, &voff, &nver) || nver != n)
		nver = 0;
	if (!rpm_find(h, RPMTAG_REQUIREFLAGS, &foff, &nflag) || nflag != n ||
	    (u64)foff + (u64)n * 4 > h->store_len)
		nflag = 0;

	for (u32 i = 0; i < n; i++) {
		const char *name = rpm_str(h, noff);
		const char *ver = nver ? rpm_str(h, voff) : NULL;
		u32 flags = nflag ? rd32be(h->store + foff + i * 4) : 0;
		char constraint[24] = "";

		if (!name)
			break;
		noff += (u32)strlen(name) + 1;
		if (ver)
			voff += (u32)strlen(ver) + 1;
		else
			nver = 0;
		if (mos_has_prefix(name, "rpmlib("))
			continue;               /* a feature of rpm itself, not a dependency */
		if (ver && ver[0] && (flags & (RPMSENSE_LESS | RPMSENSE_GREATER | RPMSENSE_EQUAL)))
			mos_snprintf(constraint, sizeof(constraint), "%s%s%s %s",
				     (flags & RPMSENSE_LESS) ? "<" : "",
				     (flags & RPMSENSE_GREATER) ? ">" : "",
				     (flags & RPMSENSE_EQUAL) ? "=" : "", ver);
		pkg_add_dep(m, name, constraint);
	}
}

int rpm_parse(const u8 *img, size_t len, struct pkg_manifest *m)
{
	struct rpm_hdr sig, hdr;
	char release[24] = "", arch[16] = "", comp[12] = "";
	u64 end;
	u32 off, count;

	/* Lead, then the signature header (padded to 8 bytes), then the main header. */
	if (!rpm_hdr_open(img, len, RPM_LEAD_SIZE, &sig, &end) ||
	    !rpm_hdr_open(img, len, ALIGN_UP(end, 8), &hdr, &end))
		return -MERR_NOEXEC;

	rpm_copy(&hdr, RPMTAG_NAME, m->name, sizeof(m->name));
	rpm_copy(&hdr, RPMTAG_VERSION, m->version, sizeof(m->version));
	rpm_copy(&hdr, RPMTAG_RELEASE, release, sizeof(release));
	rpm_copy(&hdr, RPMTAG_SUMMARY, m->summary, sizeof(m->summary));
	rpm_copy(&hdr, RPMTAG_ARCH, arch, sizeof(arch));
	rpm_copy(&hdr, RPMTAG_COMPRESSOR, comp, sizeof(comp));
	if (!m->name[0] || !m->version[0])
		return -MERR_NOEXEC;
	if (release[0]) {
		mos_strlcat(m->version, "-", sizeof(m->version));
		mos_strlcat(m->version, release, sizeof(m->version));
	}
	pkg_set_arch(m, arch);
	rpm_requires(&hdr, m);
	if (rpm_find(&hdr, RPMTAG_PREIN, &off, &count) || rpm_find(&hdr, RPMTAG_POSTIN, &off, &count))
		m->flags |= PKG_F_SCRIPTS;

	m->payload_off = end;
	m->payload_size = len - end;
	m->payload_codec = !strcmp(comp, "gzip") ? PKG_CODEC_GZIP
			 : !strcmp(comp, "xz") ? PKG_CODEC_XZ
			 : !strcmp(comp, "zstd") ? PKG_CODEC_ZSTD
			 : !strcmp(comp, "bzip2") ? PKG_CODEC_BZIP2
			 : pkg_codec_sniff(img + end, (size_t)m->payload_size);
	return 0;
}
