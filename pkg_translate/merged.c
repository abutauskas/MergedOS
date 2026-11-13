/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Native .merged packages: a fixed header, a text manifest, an opaque
 * payload and a SHA-256 trailer. The whole image is untrusted: every length
 * is checked against the buffer before it is used, and nothing is accepted
 * until the hash matches.
 */
#include "pkg_translate/pkg_internal.h"
#include "installer/sha256.h"

bool merged_probe(const u8 *img, size_t len)
{
	return img && len >= 4 && !memcmp(img, MERGED_MAGIC, 4);
}

const char *merged_type_name(enum merged_type t)
{
	switch (t) {
	case MERGED_TYPE_APP:       return "app";
	case MERGED_TYPE_BLOCKLIST: return "blocklist";
	default:                    return "unknown";
	}
}

/* Names and versions end up in store paths: keep them to a safe alphabet. */
static bool ident_ok(const char *s)
{
	if (!s[0] || s[0] == '.')
		return false;
	for (; *s; s++) {
		char c = *s;

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '.' || c == '-' || c == '_' || c == '+'))
			return false;
	}
	return true;
}

#define SEEN_NAME    0x1
#define SEEN_VERSION 0x2
#define SEEN_TYPE    0x4
#define SEEN_SUMMARY 0x8

static int parse_line(const char *line, size_t n, struct merged_pkg *p, u32 *seen)
{
	char type[16];
	size_t k = 0, v;
	u32 bit;

	while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t'))
		n--;
	if (!n || line[0] == '#')
		return 0;
	while (k < n && line[k] != ':')
		k++;
	if (k == n || k == 0)
		return -MERR_NOEXEC;
	v = k + 1;
	while (v < n && (line[v] == ' ' || line[v] == '\t'))
		v++;
	if (v == n)
		return -MERR_NOEXEC;            /* empty value */

#define KEY_IS(s) (k == sizeof(s) - 1 && !memcmp(line, s, k))
	if (KEY_IS("name")) {
		bit = SEEN_NAME;
		if (n - v >= sizeof(p->name))
			return -MERR_NOEXEC;
		mos_fieldcpy(p->name, sizeof(p->name), line + v, n - v);
		if (!ident_ok(p->name))
			return -MERR_NOEXEC;
	} else if (KEY_IS("version")) {
		bit = SEEN_VERSION;
		if (n - v >= sizeof(p->version))
			return -MERR_NOEXEC;
		mos_fieldcpy(p->version, sizeof(p->version), line + v, n - v);
		if (!ident_ok(p->version))
			return -MERR_NOEXEC;
	} else if (KEY_IS("type")) {
		bit = SEEN_TYPE;
		if (n - v >= sizeof(type))
			return -MERR_NOEXEC;
		mos_fieldcpy(type, sizeof(type), line + v, n - v);
		if (!strcmp(type, "app"))
			p->type = MERGED_TYPE_APP;
		else if (!strcmp(type, "blocklist"))
			p->type = MERGED_TYPE_BLOCKLIST;
		else
			return -MERR_NOEXEC;
	} else if (KEY_IS("summary")) {
		bit = SEEN_SUMMARY;
		mos_fieldcpy(p->summary, sizeof(p->summary), line + v, n - v); /* truncates */
	} else {
		return 0;                       /* unknown keys are ignored */
	}
#undef KEY_IS
	if (*seen & bit)
		return -MERR_NOEXEC;            /* duplicate key */
	*seen |= bit;
	return 0;
}

static int parse_manifest(const char *txt, size_t len, struct merged_pkg *p)
{
	u32 seen = 0;
	size_t i = 0;

	while (i < len) {
		size_t start = i;
		int rc;

		while (i < len && txt[i] != '\n') {
			unsigned char c = (unsigned char)txt[i];

			if (c < 0x20 && c != '\t' && c != '\r')
				return -MERR_NOEXEC;    /* NUL and other control bytes */
			i++;
		}
		rc = parse_line(txt + start, i - start, p, &seen);
		if (rc)
			return rc;
		i++;                            /* skip '\n' */
	}
	if ((seen & (SEEN_NAME | SEEN_VERSION | SEEN_TYPE)) != (SEEN_NAME | SEEN_VERSION | SEEN_TYPE))
		return -MERR_NOEXEC;
	return 0;
}

int merged_parse(const u8 *img, size_t len, struct merged_pkg *out)
{
	u8 digest[SHA256_LEN];
	struct sha256 h;
	u32 mlen, plen;
	size_t body;
	int rc;

	memset(out, 0, sizeof(*out));
	if (!merged_probe(img, len) || len < MERGED_HDR_LEN + MERGED_HASH_LEN)
		return -MERR_NOEXEC;
	if (rd32le(img + 4) != MERGED_VERSION)
		return -MERR_NOTSUP;
	mlen = rd32le(img + 8);
	plen = rd32le(img + 12);
	if (mlen == 0 || mlen > MERGED_MANIFEST_MAX || plen > MERGED_PAYLOAD_MAX)
		return -MERR_RANGE;
	/* Both lengths are capped above, so the sum cannot overflow. */
	body = MERGED_HDR_LEN + (size_t)mlen + (size_t)plen;
	if (!mos_in_range(len, body, MERGED_HASH_LEN) || body + MERGED_HASH_LEN != len)
		return -MERR_NOEXEC;            /* truncated or trailing garbage */

	sha256_init(&h);
	sha256_update(&h, img, body);
	sha256_final(&h, digest);
	if (memcmp(digest, img + body, SHA256_LEN))
		return -MERR_NOEXEC;

	rc = parse_manifest((const char *)img + MERGED_HDR_LEN, mlen, out);
	if (rc) {
		memset(out, 0, sizeof(*out));
		return rc;
	}
	out->payload = img + MERGED_HDR_LEN + mlen;
	out->payload_len = plen;
	return 0;
}

int merged_translate(const u8 *img, size_t len, struct pkg_manifest *m)
{
	struct merged_pkg p;
	int rc = merged_parse(img, len, &p);

	if (rc)
		return rc == -MERR_NOTSUP ? rc : -MERR_NOEXEC;
	mos_strlcpy(m->name, p.name, sizeof(m->name));
	mos_strlcpy(m->version, p.version, sizeof(m->version));
	mos_snprintf(m->summary, sizeof(m->summary), "[%s]", merged_type_name(p.type));
	if (p.summary[0]) {
		mos_strlcat(m->summary, " ", sizeof(m->summary));
		mos_strlcat(m->summary, p.summary, sizeof(m->summary));
	}
	mos_strlcpy(m->arch, "noarch", sizeof(m->arch));
	mos_strlcpy(m->foreign_arch, "any", sizeof(m->foreign_arch));
	mos_strlcpy(m->libc, "MergedOS", sizeof(m->libc));
	m->personality = ABI_NONE;
	m->meta_codec = m->payload_codec = PKG_CODEC_NONE;
	m->payload_off = (u64)(p.payload - img);
	m->payload_size = p.payload_len;
	m->flags = PKG_F_PAYLOAD_OK | PKG_F_RELOCATABLE;
	mos_snprintf(m->store_prefix, sizeof(m->store_prefix), "/pkg/merged/%s/%s", p.name,
		     p.version);
	return 0;
}
