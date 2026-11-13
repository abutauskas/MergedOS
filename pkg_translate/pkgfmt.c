/* SPDX-License-Identifier: BSD-2-Clause */
/* Package format identification by content, with the file name as a tie-breaker. */
#include "pkg_translate/pkg_internal.h"

static bool hint_has(const char *hint, const char *needle)
{
	return hint && mos_memstr(hint, strlen(hint), needle);
}

/* .apk and pacman packages are both (compressed) tar files: look at the first entry. */
static enum pkg_format probe_tar_family(const u8 *img, size_t len, const char *hint)
{
	u8 head[1024];
	const u8 *tar = img;
	size_t tar_len = len, consumed;

	if (pkg_codec_sniff(img, len) == PKG_CODEC_GZIP) {
		/* Peek at the first kilobyte only: enough for one tar header. */
		if (gz_decode(img, len, head, sizeof(head), &consumed, &tar_len) < 0)
			return PKG_UNKNOWN;
		tar = head;
	}
	if (!tar_probe(tar, tar_len))
		return PKG_UNKNOWN;
	if (hint && mos_has_suffix(hint, ".apk"))
		return PKG_APK;
	if (hint_has(hint, ".pkg.tar"))
		return PKG_PACMAN;
	/* No usable file name: the first member's name tells the two apart. */
	if (!memcmp(tar, ".SIGN.", 6) || !memcmp(tar, ".PKGINFO", 9))
		return PKG_APK;
	if (!memcmp(tar, ".BUILDINFO", 11) || !memcmp(tar, ".MTREE", 7))
		return PKG_PACMAN;
	return PKG_UNKNOWN;
}

enum pkg_format pkg_probe(const u8 *img, size_t len, const char *name_hint)
{
	enum pkg_codec codec;

	if (hint_has(name_hint, ".app") &&
	    (mos_has_suffix(name_hint, ".app") || mos_has_suffix(name_hint, ".app/") ||
	     mos_has_suffix(name_hint, "Info.plist")))
		return PKG_MACOS_APP;
	if (!img || len < 8)
		return PKG_UNKNOWN;

	if (!memcmp(img, "!<arch>\n", 8)) {
		struct ar_member m;
		size_t off = 0;

		if (ar_next(img, len, &off, &m) == 1 && !strcmp(m.name, "debian-binary"))
			return PKG_DEB;
		return PKG_UNKNOWN;             /* a static library, most likely */
	}
	if (merged_probe(img, len))
		return PKG_MERGED;
	if (rd32be(img) == 0xEDABEEDBu)
		return PKG_RPM;
	if (len >= 11 && !memcmp(img, "\177ELF", 4) && img[8] == 'A' && img[9] == 'I' &&
	    (img[10] == 1 || img[10] == 2))
		return PKG_APPIMAGE;
	if (!memcmp(img, "hsqs", 4))
		return PKG_SNAP;
	if (len >= 21 && rd64le(img) == 13 && !memcmp(img + 8, "nix-archive-1", 13))
		return PKG_NIX_NAR;
	if (!memcmp(img, "xar!", 4))
		return PKG_MACOS_XAR;
	if (len >= 512 && !memcmp(img + len - 512, "koly", 4))
		return PKG_MACOS_DMG;
	if (name_hint && mos_has_suffix(name_hint, ".flatpak"))
		return PKG_FLATPAK;
	if ((!memcmp(img, "<?xml", 5) || !memcmp(img, "bplist00", 8)) &&
	    (!memcmp(img, "bplist00", 8) || mos_memstr((const char *)img, len, "CFBundle")))
		return PKG_MACOS_APP;           /* an Info.plist handed in directly */

	codec = pkg_codec_sniff(img, len);
	if (codec == PKG_CODEC_GZIP || codec == PKG_CODEC_NONE)
		return probe_tar_family(img, len, name_hint);
	if (codec == PKG_CODEC_XZ || codec == PKG_CODEC_ZSTD) {
		if (hint_has(name_hint, ".pkg.tar"))
			return PKG_PACMAN;
		if (name_hint && mos_has_suffix(name_hint, ".apk"))
			return PKG_APK;
	}
	return PKG_UNKNOWN;
}

const char *pkg_format_name(enum pkg_format f)
{
	static const char *const names[PKG_FORMAT_COUNT] = {
		[PKG_UNKNOWN]   = "unknown",
		[PKG_DEB]       = "deb (Debian family)",
		[PKG_RPM]       = "rpm (Red Hat / SUSE family)",
		[PKG_APK]       = "apk (Alpine)",
		[PKG_PACMAN]    = "pacman (Arch family)",
		[PKG_APPIMAGE]  = "AppImage",
		[PKG_SNAP]      = "snap (squashfs)",
		[PKG_FLATPAK]   = "flatpak bundle",
		[PKG_NIX_NAR]   = "Nix archive",
		[PKG_MACOS_APP] = "macOS .app bundle",
		[PKG_MACOS_DMG] = "macOS disk image (UDIF)",
		[PKG_MACOS_XAR] = "macOS installer package (xar)",
		[PKG_MERGED]    = "merged (MergedOS native)",
	};

	return (unsigned)f < PKG_FORMAT_COUNT ? names[f] : names[PKG_UNKNOWN];
}

const char *pkg_codec_name(enum pkg_codec c)
{
	switch (c) {
	case PKG_CODEC_NONE:  return "stored";
	case PKG_CODEC_GZIP:  return "gzip";
	case PKG_CODEC_XZ:    return "xz";
	case PKG_CODEC_ZSTD:  return "zstd";
	case PKG_CODEC_BZIP2: return "bzip2";
	case PKG_CODEC_LZ4:   return "lz4";
	default:              return "unknown";
	}
}
