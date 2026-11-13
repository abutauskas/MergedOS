/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Package translation front end: container walk -> metadata reader ->
 * native manifest, plus payload access and the store path view.
 */
#include "pkg_translate/pkg_internal.h"

#define GZIP_MEMBERS_MAX 4

static bool codec_supported(enum pkg_codec c)
{
	return c == PKG_CODEC_NONE || c == PKG_CODEC_GZIP;
}

/* "control.tar.xz" -> xz. Falls back to sniffing the content. */
static enum pkg_codec member_codec(const char *name, const u8 *data, size_t len)
{
	if (mos_has_suffix(name, ".gz"))
		return PKG_CODEC_GZIP;
	if (mos_has_suffix(name, ".xz") || mos_has_suffix(name, ".lzma"))
		return PKG_CODEC_XZ;
	if (mos_has_suffix(name, ".zst"))
		return PKG_CODEC_ZSTD;
	if (mos_has_suffix(name, ".bz2"))
		return PKG_CODEC_BZIP2;
	if (mos_has_suffix(name, ".tar"))
		return PKG_CODEC_NONE;
	return pkg_codec_sniff(data, len);
}

/* ---- .deb ------------------------------------------------------------------ */

static int deb_translate(const u8 *img, size_t len, struct pkg_scratch *s, struct pkg_manifest *m)
{
	struct ar_member mem;
	size_t off = 0;
	bool have_control = false, have_data = false;
	int rc;

	while ((rc = ar_next(img, len, &off, &mem)) == 1) {
		if (mos_has_prefix(mem.name, "control.tar")) {
			struct tar_entry e;
			const u8 *tar;
			size_t tar_len, toff = 0;

			m->meta_codec = member_codec(mem.name, mem.data, (size_t)mem.size);
			rc = pkg_unpack(m->meta_codec, mem.data, (size_t)mem.size, s, &tar, &tar_len);
			if (rc < 0)
				return rc;
			while (tar_next(tar, tar_len, &toff, &e) > 0) {
				if (e.type != '0')
					continue;
				if (!strcmp(e.name, "control"))
					have_control = !deb_control_parse((const char *)e.data,
									  (size_t)e.size, m);
				else if (!strcmp(e.name, "preinst") || !strcmp(e.name, "postinst") ||
					 !strcmp(e.name, "prerm") || !strcmp(e.name, "postrm"))
					m->flags |= PKG_F_SCRIPTS;
			}
		} else if (mos_has_prefix(mem.name, "data.tar")) {
			have_data = true;
			m->payload_codec = member_codec(mem.name, mem.data, (size_t)mem.size);
			m->payload_off = (u64)(mem.data - img);
			m->payload_size = mem.size;
		}
	}
	if (rc < 0 || !have_data)
		return -MERR_NOEXEC;
	return have_control ? 0 : -MERR_NOEXEC;
}

/* ---- .apk / pacman --------------------------------------------------------- */

static int tarpkg_translate(const u8 *img, size_t len, struct pkg_scratch *s,
			    struct pkg_manifest *m)
{
	enum pkg_codec codec = pkg_codec_sniff(img, len);
	size_t member_off = 0;
	bool is_alpine = false;

	m->meta_codec = m->payload_codec = codec;
	if (!codec_supported(codec))
		return -MERR_NOTSUP;

	/*
	 * pacman: one stream, .PKGINFO near the front, payload in the same tar.
	 * apk: concatenated gzip members (signature, control, data); the
	 * member after the one holding .PKGINFO is the payload.
	 */
	for (int i = 0; i < GZIP_MEMBERS_MAX && member_off < len; i++) {
		struct tar_entry e;
		const u8 *tar = img + member_off;
		size_t tar_len = len - member_off, consumed = len - member_off;
		int rc = 0;

		if (codec == PKG_CODEC_GZIP) {
			if (!s || !s->buf)
				return -MERR_NOMEM;
			rc = gz_decode(img + member_off, len - member_off, s->buf, s->cap,
				       &consumed, &tar_len);
			if (rc < 0)
				return -MERR_NOEXEC;
			tar = s->buf;
		}
		if (!tar_find(tar, tar_len, ".PKGINFO", &e)) {
			int prc = pkginfo_parse((const char *)e.data, (size_t)e.size, m, &is_alpine);

			if (prc)
				return prc;
			if (i > 0 || is_alpine)
				m->format = PKG_APK;
			if (m->format == PKG_APK && rc == 0 && member_off + consumed < len)
				member_off += consumed;         /* payload is the next member */
			m->payload_off = member_off;
			m->payload_size = len - member_off;
			return 0;
		}
		if (rc == 1 || codec != PKG_CODEC_GZIP)
			break;                          /* single stream and it is not in there */
		member_off += consumed;
	}
	return -MERR_NOEXEC;
}

/* ---- front end --------------------------------------------------------------- */

int pkg_translate(const u8 *img, size_t len, const char *name_hint, struct pkg_scratch *scratch,
		  struct pkg_manifest *m)
{
	int rc;

	memset(m, 0, sizeof(*m));
	m->format = pkg_probe(img, len, name_hint);
	m->meta_codec = m->payload_codec = PKG_CODEC_UNKNOWN;

	switch (m->format) {
	case PKG_DEB:
		rc = deb_translate(img, len, scratch, m);
		break;
	case PKG_RPM:
		m->meta_codec = PKG_CODEC_NONE; /* RPM headers are never compressed */
		rc = rpm_parse(img, len, m);
		break;
	case PKG_APK:
	case PKG_PACMAN:
		rc = tarpkg_translate(img, len, scratch, m);
		break;
	case PKG_MACOS_APP:
		return pkg_translate_app(img, len, NULL, 0, m);
	case PKG_MERGED:
		return merged_translate(img, len, m);
	case PKG_UNKNOWN:
		return -MERR_NOEXEC;
	default:
		return -MERR_NOTSUP;            /* identified; no translator backend yet */
	}
	if (rc)
		return rc;

	m->personality = ABI_LINUX;
	if (!m->libc[0])
		mos_strlcpy(m->libc, m->format == PKG_APK ? "musl" : "glibc", sizeof(m->libc));
	if (codec_supported(m->payload_codec))
		m->flags |= PKG_F_PAYLOAD_OK;
	mos_snprintf(m->store_prefix, sizeof(m->store_prefix), "/pkg/%s/%s/%s",
		     pkg_family(m->format), m->name, m->version);
	return 0;
}

static bool is_program_path(const char *name)
{
	while (name[0] == '.' && name[1] == '/')
		name += 2;
	while (*name == '/')
		name++;
	return mos_has_prefix(name, "usr/bin/") || mos_has_prefix(name, "bin/") ||
	       mos_has_prefix(name, "usr/local/bin/") || mos_has_prefix(name, "usr/sbin/") ||
	       mos_has_prefix(name, "sbin/");
}

int pkg_payload_exec(const u8 *img, size_t len, const struct pkg_manifest *m,
		     struct pkg_scratch *scratch, const u8 **exe, size_t *exe_len, char *path,
		     size_t path_cap)
{
	const u8 *payload;
	size_t payload_len, off = 0;
	int rc;

	if (m->format != PKG_DEB && m->format != PKG_RPM && m->format != PKG_APK &&
	    m->format != PKG_PACMAN)
		return -MERR_NOTSUP;
	if (!mos_in_range(len, m->payload_off, m->payload_size))
		return -MERR_NOEXEC;
	rc = pkg_unpack(m->payload_codec, img + m->payload_off, (size_t)m->payload_size, scratch,
			&payload, &payload_len);
	if (rc < 0)
		return rc;

	if (m->format == PKG_RPM) {
		struct cpio_entry e;

		while (cpio_next(payload, payload_len, &off, &e) > 0) {
			if ((e.mode & 0170000) == 0100000 && (e.mode & 0111) &&
			    is_program_path(e.name)) {
				*exe = e.data;
				*exe_len = (size_t)e.size;
				mos_strlcpy(path, e.name, path_cap);
				return 0;
			}
		}
	} else {
		struct tar_entry e;

		while (tar_next(payload, payload_len, &off, &e) > 0) {
			if (e.type == '0' && is_program_path(e.name)) {
				*exe = e.data;
				*exe_len = (size_t)e.size;
				mos_strlcpy(path, e.name, path_cap);
				return 0;
			}
		}
	}
	/* rc == 1: the scratch buffer held only a prefix of the payload. */
	return rc == 1 ? -MERR_RANGE : -MERR_NOENT;
}

int pkg_store_path(const struct pkg_manifest *m, const char *foreign_path, char *out, size_t cap)
{
	/* Layout differences between distributions collapse onto one store layout. */
	static const struct {
		const char *prefix, *store_dir;
	} map[] = {
		{ "usr/lib/x86_64-linux-gnu/", "lib/" },        /* Debian multiarch   */
		{ "lib/x86_64-linux-gnu/",     "lib/" },
		{ "usr/lib64/",                "lib/" },        /* Fedora, openSUSE   */
		{ "lib64/",                    "lib/" },
		{ "usr/lib/",                  "lib/" },        /* Arch, Alpine       */
		{ "lib/",                      "lib/" },
		{ "usr/local/bin/",            "bin/" },
		{ "usr/sbin/",                 "bin/" },
		{ "usr/bin/",                  "bin/" },
		{ "sbin/",                     "bin/" },
		{ "bin/",                      "bin/" },
		{ "usr/share/",                "share/" },
		{ "etc/",                      "etc/" },
	};
	const char *p = foreign_path;
	int n;

	while (p[0] == '.' && p[1] == '/')
		p += 2;
	while (*p == '/')
		p++;

	if (m->format == PKG_MACOS_APP) {
		n = mos_snprintf(out, cap, "%s/%s", m->store_prefix, p);
		return (size_t)n < cap ? n : -MERR_RANGE;
	}
	for (size_t i = 0; i < ARRAY_SIZE(map); i++) {
		if (mos_has_prefix(p, map[i].prefix)) {
			n = mos_snprintf(out, cap, "%s/%s%s", m->store_prefix, map[i].store_dir,
					 p + strlen(map[i].prefix));
			return (size_t)n < cap ? n : -MERR_RANGE;
		}
	}
	n = mos_snprintf(out, cap, "%s/root/%s", m->store_prefix, p);
	return (size_t)n < cap ? n : -MERR_RANGE;
}

int pkg_manifest_print(const struct pkg_manifest *m, char *out, size_t cap)
{
	size_t n = 0;

#define EMIT(...)                                                              \
	do {                                                                   \
		int w_ = mos_snprintf(out + n, n < cap ? cap - n : 0, __VA_ARGS__); \
		n = (w_ > 0 && n + (size_t)w_ < cap) ? n + (size_t)w_ : cap - 1; \
	} while (0)

	if (!cap)
		return 0;
	out[0] = '\0';
	EMIT("  format       %s\n", pkg_format_name(m->format));
	EMIT("  package      %s %s\n", m->name, m->version);
	if (m->summary[0])
		EMIT("  summary      %s\n", m->summary);
	if (m->bundle_id[0])
		EMIT("  bundle id    %s (min OS %s)\n", m->bundle_id, m->min_os[0] ? m->min_os : "?");
	EMIT("  arch         %s -> %s\n", m->foreign_arch[0] ? m->foreign_arch : "?", m->arch);
	if (m->format == PKG_MERGED)
		EMIT("  personality  native (%s)\n", m->libc);
	else
		EMIT("  personality  %s (%s)\n", abi_personality_name(m->personality), m->libc);
	if (m->format != PKG_MACOS_APP)
		EMIT("  payload      %s, %llu bytes%s\n", pkg_codec_name(m->payload_codec),
		     (unsigned long long)m->payload_size,
		     (m->flags & PKG_F_PAYLOAD_OK) ? "" : " (codec not built in)");
	else
		EMIT("  executable   %s\n", m->executable);
	if (m->flags & PKG_F_SCRIPTS)
		EMIT("  scripts      maintainer scripts present (run jailed at install)\n");
	EMIT("  store        %s\n", m->store_prefix);
	for (u32 i = 0; i < m->ndeps; i++)
		EMIT("  requires     %s%s%s -> %s%s\n", m->deps[i].name,
		     m->deps[i].constraint[0] ? " " : "", m->deps[i].constraint,
		     m->deps[i].capability, m->deps[i].base ? " [base]" : "");
	if (m->deps_total > m->ndeps)
		EMIT("  requires     ... and %u more\n", m->deps_total - m->ndeps);
#undef EMIT
	return (int)n;
}
