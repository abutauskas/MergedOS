/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * macOS .app bundles.
 *
 * A bundle is a directory, not an archive: Contents/Info.plist names the
 * main executable (Contents/MacOS/<CFBundleExecutable>) and the bundle's
 * identity. Nothing has to be unpacked or installed - "translation" is
 * reading the property list, inspecting the executable's load commands for
 * what it links against, and launching it with the XNU personality.
 *
 * Only XML property lists are parsed; binary plists ("bplist00") are
 * detected and reported as unsupported.
 */
#include "abi_dispatch/binfmt.h"
#include "pkg_translate/pkg_internal.h"

/* Value of <key>KEY</key><string>VALUE</string>; false if absent or not a string. */
static bool plist_string(const char *xml, size_t len, const char *key, char *out, size_t cap)
{
	char pattern[80];
	const char *p, *end = xml + len, *close;

	mos_snprintf(pattern, sizeof(pattern), "<key>%s</key>", key);
	p = mos_memstr(xml, len, pattern);
	if (!p)
		return false;
	p += strlen(pattern);
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if ((size_t)(end - p) < 8 || memcmp(p, "<string>", 8))
		return false;
	p += 8;
	close = mos_memstr(p, (size_t)(end - p), "</string>");
	if (!close)
		return false;
	mos_fieldcpy(out, cap, p, (size_t)(close - p));
	return true;
}

static void add_dylib(const char *install_name, void *arg)
{
	pkg_add_dylib_dep(arg, install_name);
}

/* Attribute each imported symbol to the library it comes from, when that library
 * is one MergedOS does not provide: that is the "what is missing" list. */
static void add_import(const char *lib, const char *sym, bool weak, void *arg)
{
	struct pkg_manifest *m = arg;
	size_t sl = strlen(sym), n;

	(void)weak;
	if (!lib)
		return;
	for (u32 i = 0; i < m->ndeps; i++) {
		struct pkg_dep *d = &m->deps[i];

		if (d->base || strncmp(d->name, lib, sizeof(d->name) - 1))
			continue;
		for (const char *p = d->imports; *p;) {         /* already listed? */
			size_t tl = 0;

			while (p[tl] && p[tl] != ' ')
				tl++;

			if (tl == sl && !memcmp(p, sym, sl))
				return;
			p += tl + (p[tl] ? 1 : 0);
		}
		d->nimports++;
		n = strlen(d->imports);
		if (n + sl + 2 < sizeof(d->imports)) {
			if (n)
				d->imports[n++] = ' ';
			memcpy(d->imports + n, sym, sl + 1);
		}
		return;
	}
}

int pkg_translate_app(const u8 *plist, size_t plist_len, const u8 *exe, size_t exe_len,
		      struct pkg_manifest *m)
{
	const char *xml = (const char *)plist;
	char exe_name[96];

	memset(m, 0, sizeof(*m));
	m->format = PKG_MACOS_APP;
	m->personality = ABI_XNU;
	mos_strlcpy(m->libc, "libSystem", sizeof(m->libc));
	mos_strlcpy(m->arch, "unknown", sizeof(m->arch));
	/* A bundle is used in place: no payload to decode, no install scripts. */
	m->flags = PKG_F_RELOCATABLE | PKG_F_PAYLOAD_OK;

	if (plist_len >= 8 && !memcmp(plist, "bplist00", 8))
		return -MERR_NOTSUP;
	if (!plist_string(xml, plist_len, "CFBundleExecutable", exe_name, sizeof(exe_name)) ||
	    !exe_name[0] || strchr(exe_name, '/'))
		return -MERR_NOEXEC;
	mos_snprintf(m->executable, sizeof(m->executable), "Contents/MacOS/%s", exe_name);

	if (!plist_string(xml, plist_len, "CFBundleName", m->name, sizeof(m->name)))
		mos_strlcpy(m->name, exe_name, sizeof(m->name));
	if (!plist_string(xml, plist_len, "CFBundleShortVersionString", m->version,
			  sizeof(m->version)) &&
	    !plist_string(xml, plist_len, "CFBundleVersion", m->version, sizeof(m->version)))
		mos_strlcpy(m->version, "0", sizeof(m->version));
	plist_string(xml, plist_len, "CFBundleIdentifier", m->bundle_id, sizeof(m->bundle_id));
	plist_string(xml, plist_len, "LSMinimumSystemVersion", m->min_os, sizeof(m->min_os));

	if (exe && exe_len) {
		struct binfmt_info info;

		if (binfmt_probe(exe, exe_len, &info) || info.personality != ABI_XNU)
			return -MERR_NOEXEC;    /* the "executable" is not a Mach-O */
		mos_strlcpy(m->foreign_arch, binfmt_arch_name(info.arch), sizeof(m->foreign_arch));
		mos_strlcpy(m->arch, binfmt_arch_name(info.arch), sizeof(m->arch));
		(void)binfmt_macho_dylibs(exe, exe_len, add_dylib, m);
		(void)binfmt_macho_imports(exe, exe_len, add_import, m);
	}

	mos_snprintf(m->store_prefix, sizeof(m->store_prefix), "/pkg/app/%s/%s",
		     m->bundle_id[0] ? m->bundle_id : m->name, m->version);
	return 0;
}
