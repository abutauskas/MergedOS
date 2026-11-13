/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * File type identification: magic numbers and structure first, the file
 * name only when the content says nothing. Executables and packages are
 * handed to the probes that already understand them.
 */
#include "filetype/ft_internal.h"
#include "abi_dispatch/binfmt.h"
#include "pkg_translate/pkg_internal.h"

struct kind_info {
	enum ft_class cls;
	const char *name, *mime;
};

static const struct kind_info kinds[FT_KIND_COUNT] = {
	[FT_UNKNOWN]     = { FT_C_NONE, "unknown", "application/octet-stream" },
	[FT_EMPTY]       = { FT_C_NONE, "empty file", "inode/x-empty" },
	[FT_TEXT]        = { FT_C_TEXT, "plain text", "text/plain" },
	[FT_SCRIPT]      = { FT_C_TEXT, "script", "text/plain" },
	[FT_JSON]        = { FT_C_TEXT, "JSON data", "application/json" },
	[FT_XML]         = { FT_C_TEXT, "XML document", "application/xml" },
	[FT_HTML]        = { FT_C_TEXT, "HTML document", "text/html" },
	[FT_SVG]         = { FT_C_IMAGE, "SVG image (shown as text)", "image/svg+xml" },
	[FT_MARKDOWN]    = { FT_C_TEXT, "Markdown text", "text/markdown" },
	[FT_CSV]         = { FT_C_TEXT, "CSV table", "text/csv" },
	[FT_PNG]         = { FT_C_IMAGE, "PNG image", "image/png" },
	[FT_BMP]         = { FT_C_IMAGE, "BMP image", "image/bmp" },
	[FT_JPEG]        = { FT_C_IMAGE, "JPEG image", "image/jpeg" },
	[FT_GIF]         = { FT_C_IMAGE, "GIF image", "image/gif" },
	[FT_WEBP]        = { FT_C_IMAGE, "WebP image", "image/webp" },
	[FT_ICO]         = { FT_C_IMAGE, "Windows icon", "image/vnd.microsoft.icon" },
	[FT_WAV]         = { FT_C_AUDIO, "WAV audio", "audio/wav" },
	[FT_MP3]         = { FT_C_AUDIO, "MP3 audio", "audio/mpeg" },
	[FT_FLAC]        = { FT_C_AUDIO, "FLAC audio", "audio/flac" },
	[FT_OGG]         = { FT_C_AUDIO, "Ogg media", "audio/ogg" },
	[FT_MP4]         = { FT_C_VIDEO, "MPEG-4 media", "video/mp4" },
	[FT_MOV]         = { FT_C_VIDEO, "QuickTime movie", "video/quicktime" },
	[FT_MKV]         = { FT_C_VIDEO, "Matroska video", "video/x-matroska" },
	[FT_WEBM]        = { FT_C_VIDEO, "WebM video", "video/webm" },
	[FT_AVI]         = { FT_C_VIDEO, "AVI video", "video/x-msvideo" },
	[FT_PDF]         = { FT_C_DOCUMENT, "PDF document", "application/pdf" },
	[FT_DOCX]        = { FT_C_DOCUMENT, "Word document (DOCX)",
			     "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
	[FT_XLSX]        = { FT_C_DOCUMENT, "Excel workbook (XLSX)",
			     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
	[FT_PPTX]        = { FT_C_DOCUMENT, "PowerPoint presentation (PPTX)",
			     "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
	[FT_EPUB]        = { FT_C_DOCUMENT, "EPUB e-book", "application/epub+zip" },
	[FT_ODF]         = { FT_C_DOCUMENT, "OpenDocument file", "application/vnd.oasis.opendocument" },
	[FT_ZIP]         = { FT_C_ARCHIVE, "ZIP archive", "application/zip" },
	[FT_JAR]         = { FT_C_ARCHIVE, "Java archive (JAR)", "application/java-archive" },
	[FT_APK_ANDROID] = { FT_C_PACKAGE, "Android package (APK)",
			     "application/vnd.android.package-archive" },
	[FT_TAR]         = { FT_C_ARCHIVE, "tar archive", "application/x-tar" },
	[FT_TARGZ]       = { FT_C_ARCHIVE, "gzip-compressed tar archive", "application/gzip" },
	[FT_GZIP]        = { FT_C_ARCHIVE, "gzip-compressed data", "application/gzip" },
	[FT_XZ]          = { FT_C_ARCHIVE, "xz-compressed data", "application/x-xz" },
	[FT_ZSTD]        = { FT_C_ARCHIVE, "Zstandard-compressed data", "application/zstd" },
	[FT_BZIP2]       = { FT_C_ARCHIVE, "bzip2-compressed data", "application/x-bzip2" },
	[FT_7Z]          = { FT_C_ARCHIVE, "7-Zip archive", "application/x-7z-compressed" },
	[FT_RAR]         = { FT_C_ARCHIVE, "RAR archive", "application/vnd.rar" },
	[FT_EXEC]        = { FT_C_PROGRAM, "program", "application/octet-stream" },
	[FT_PACKAGE]     = { FT_C_PACKAGE, "software package", "application/octet-stream" },
	[FT_TTF]         = { FT_C_FONT, "TrueType font", "font/ttf" },
	[FT_OTF]         = { FT_C_FONT, "OpenType font", "font/otf" },
	[FT_WOFF]        = { FT_C_FONT, "WOFF web font", "font/woff" },
	[FT_WOFF2]       = { FT_C_FONT, "WOFF2 web font", "font/woff2" },
	[FT_SQLITE]      = { FT_C_DATA, "SQLite 3 database", "application/vnd.sqlite3" },
	[FT_PLIST_BIN]   = { FT_C_DATA, "binary property list", "application/x-bplist" },
	[FT_ISO9660]     = { FT_C_DISK, "ISO 9660 CD/DVD image", "application/x-iso9660-image" },
	[FT_DISK_MBR]    = { FT_C_DISK, "disk image or boot sector (MBR)",
			     "application/x-raw-disk-image" },
	[FT_DISK_GPT]    = { FT_C_DISK, "disk image (GPT)", "application/x-raw-disk-image" },
	[FT_BINARY]      = { FT_C_DATA, "data", "application/octet-stream" },
};

const char *ft_class_name(enum ft_class c)
{
	static const char *const names[FT_C_COUNT] = {
		"other", "text", "image", "audio", "video", "document", "archive", "program",
		"package", "font", "data", "disk image",
	};

	return (unsigned)c < FT_C_COUNT ? names[c] : names[0];
}

bool ft_rendered(enum ft_kind k)
{
	switch (k) {
	case FT_TEXT: case FT_SCRIPT: case FT_JSON: case FT_XML: case FT_HTML: case FT_SVG:
	case FT_MARKDOWN: case FT_CSV: case FT_PNG: case FT_BMP: case FT_ZIP: case FT_JAR:
	case FT_TAR: case FT_TARGZ:
		return true;
	default:
		return false;
	}
}

static enum ft_kind set(struct ft_type *t, enum ft_kind k)
{
	t->kind = k;
	t->cls = kinds[k].cls;
	t->name = kinds[k].name;
	t->mime = kinds[k].mime;
	return k;
}

static bool at(const u8 *buf, size_t len, size_t off, const char *magic, size_t n)
{
	return mos_in_range(len, off, n) && !memcmp(buf + off, magic, n);
}

const char *ft_base_name(const char *path)
{
	const char *b = path;

	for (const char *p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			b = p + 1;
	return b;
}

/* ---- text ------------------------------------------------------------------------ */

int ft_utf8_len(const u8 *p, size_t n, bool tail_ok)
{
	int need;

	if (p[0] < 0x80)
		return 1;
	if (p[0] >= 0xC2 && p[0] <= 0xDF)
		need = 2;
	else if (p[0] >= 0xE0 && p[0] <= 0xEF)
		need = 3;
	else if (p[0] >= 0xF0 && p[0] <= 0xF4)
		need = 4;
	else
		return 0;
	for (int i = 1; i < need; i++) {
		if ((size_t)i >= n)
			return tail_ok ? (int)n : 0;
		if ((p[i] & 0xC0) != 0x80)
			return 0;
	}
	return need;
}

bool ft_is_text(const u8 *buf, size_t len)
{
	const size_t n = MOS_MIN(len, (size_t)8192);

	for (size_t i = 0; i < n;) {
		const u8 c = buf[i];
		int k;

		if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f' && c != 0x1B &&
		    c != '\b')
			return false;
		if (c == 0x7F)
			return false;
		k = ft_utf8_len(buf + i, n - i, true);
		if (!k)
			return false;
		i += (size_t)k;
	}
	return len > 0;
}

static size_t skip_ws(const u8 *buf, size_t len, size_t i)
{
	while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n'))
		i++;
	return i;
}

static bool has_ci(const u8 *buf, size_t len, const char *needle)
{
	const size_t n = strlen(needle);

	for (size_t i = 0; i + n <= len; i++) {
		size_t k = 0;

		while (k < n && mos_tolower(buf[i + k]) == needle[k])
			k++;
		if (k == n)
			return true;
	}
	return false;
}

static int count_line_sep(const u8 *p, size_t n, char sep)
{
	int c = 0;
	bool q = false;

	for (size_t i = 0; i < n; i++) {
		if (p[i] == '"')
			q = !q;
		else if (p[i] == (u8)sep && !q)
			c++;
	}
	return c;
}

/* Three or more lines with the same, non-zero number of separators. */
static bool looks_csv(const u8 *buf, size_t len, char sep)
{
	size_t i = 0;
	int lines = 0, want = -1;

	while (i < len && lines < 8) {
		size_t e = i;
		int c;

		while (e < len && buf[e] != '\n')
			e++;
		if (e > i) {
			c = count_line_sep(buf + i, e - i, sep);
			if (!c || (want >= 0 && c != want))
				return false;
			want = c;
			lines++;
		}
		i = e + 1;
	}
	return lines >= 3;
}

static bool looks_markdown(const u8 *buf, size_t len)
{
	const size_t n = MOS_MIN(len, (size_t)4096);
	const char *s = (const char *)buf;

	if (!(n > 2 && buf[0] == '#' && (buf[1] == ' ' || buf[1] == '#')))
		return false;
	return mos_memstr(s, n, "\n## ") || mos_memstr(s, n, "\n- ") ||
	       mos_memstr(s, n, "\n* ") || mos_memstr(s, n, "](") || mos_memstr(s, n, "**") ||
	       mos_memstr(s, n, "```");
}

static void script_detail(const u8 *buf, size_t len, struct ft_type *t)
{
	char line[48], *prog, *p;
	size_t n = 0;

	for (size_t i = 2; i < len && buf[i] != '\n' && n + 1 < sizeof(line); i++)
		line[n++] = (char)buf[i];
	line[n] = '\0';
	p = line;
	while (*p == ' ')
		p++;
	prog = p;
	while (*p && *p != ' ')
		p++;
	if (*p)
		*p++ = '\0';
	prog = (char *)ft_base_name(prog);
	if (!strcmp(prog, "env")) {             /* #!/usr/bin/env python3 */
		while (*p == ' ')
			p++;
		prog = p;
		while (*p && *p != ' ')
			p++;
		*p = '\0';
	}
	if (!*prog)
		prog = "unknown";
	mos_snprintf(t->detail, sizeof(t->detail), "interpreter %s", prog);
	if (mos_has_prefix(prog, "python"))
		t->mime = "text/x-python";
	else if (mos_has_prefix(prog, "perl"))
		t->mime = "text/x-perl";
	else if (!strcmp(prog, "sh") || !strcmp(prog, "bash") || !strcmp(prog, "dash") ||
		 !strcmp(prog, "zsh") || !strcmp(prog, "ksh") || !strcmp(prog, "msh"))
		t->mime = "text/x-shellscript";
}

static enum ft_kind identify_text(const u8 *buf, size_t len, const char *base, struct ft_type *t)
{
	const size_t head = MOS_MIN(len, (size_t)4096);
	size_t i = 0;

	if (len >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
		i = 3;                          /* UTF-8 byte-order mark */
	if (len - i >= 2 && buf[i] == '#' && buf[i + 1] == '!') {
		set(t, FT_SCRIPT);
		script_detail(buf + i, len - i, t);
		return t->kind;
	}
	i = skip_ws(buf, len, i);
	if (i < len && (buf[i] == '{' || buf[i] == '[')) {
		size_t e = len;

		while (e > i && (buf[e - 1] == ' ' || buf[e - 1] == '\n' || buf[e - 1] == '\r' ||
				 buf[e - 1] == '\t'))
			e--;
		if (e > i + 1 && buf[e - 1] == (buf[i] == '{' ? '}' : ']'))
			return set(t, FT_JSON);
	}
	if (i < len && buf[i] == '<') {
		if (has_ci(buf + i, head - MOS_MIN(head, i), "<svg"))
			return set(t, FT_SVG);
		if (has_ci(buf + i, MOS_MIN((size_t)256, len - i), "<!doctype html") ||
		    has_ci(buf + i, MOS_MIN((size_t)512, len - i), "<html"))
			return set(t, FT_HTML);
		if (at(buf, len, i, "<?xml", 5) || (base && mos_has_suffix(base, ".xml"))) {
			set(t, FT_XML);
			if (has_ci(buf, head, "<plist") || has_ci(buf, head, "<!doctype plist"))
				mos_strlcpy(t->detail, "Apple property list", sizeof(t->detail));
			return t->kind;
		}
	}
	if (base && (mos_has_suffix(base, ".md") || mos_has_suffix(base, ".markdown")))
		return set(t, FT_MARKDOWN);
	if (base && (mos_has_suffix(base, ".csv") || mos_has_suffix(base, ".tsv")))
		return set(t, FT_CSV);
	if (base && mos_has_suffix(base, ".json"))
		return set(t, FT_JSON);
	if (looks_markdown(buf, len))
		return set(t, FT_MARKDOWN);
	if (looks_csv(buf, head, ',') || looks_csv(buf, head, '\t'))
		return set(t, FT_CSV);
	set(t, FT_TEXT);
	if (base) {
		static const struct { const char *ext, *what, *mime; } src[] = {
			{ ".c", "C source", "text/x-c" }, { ".h", "C header", "text/x-c" },
			{ ".py", "Python source", "text/x-python" },
			{ ".sh", "shell script", "text/x-shellscript" },
			{ ".conf", "configuration", "text/plain" }, { ".ini", "configuration", "text/plain" },
			{ ".log", "log", "text/plain" },
		};

		for (size_t k = 0; k < ARRAY_SIZE(src); k++)
			if (mos_has_suffix(base, src[k].ext)) {
				mos_strlcpy(t->detail, src[k].what, sizeof(t->detail));
				t->mime = src[k].mime;
				break;
			}
	}
	if (!t->detail[0]) {
		bool ascii = true;

		for (size_t k = 0; k < head && ascii; k++)
			ascii = buf[k] < 0x80;
		mos_strlcpy(t->detail, ascii ? "ASCII" : "UTF-8", sizeof(t->detail));
	}
	return t->kind;
}

/* ---- zip --------------------------------------------------------------------------- */

static enum ft_kind identify_zip(const u8 *buf, size_t len, struct ft_type *t)
{
	struct ft_archive a;
	struct ft_entry e;
	bool manifest = false, android = false, dex = false;

	set(t, FT_ZIP);
	/* EPUB and OpenDocument: a stored "mimetype" member comes first. */
	if (len >= 38 && rd16le(buf + 26) == 8 && !memcmp(buf + 30, "mimetype", 8) &&
	    rd16le(buf + 8) == 0) {
		const size_t off = 30 + 8 + rd16le(buf + 28);

		if (at(buf, len, off, "application/epub+zip", 20))
			return set(t, FT_EPUB);
		if (at(buf, len, off, "application/vnd.oasis.opendocument.", 35)) {
			size_t n = 0;

			set(t, FT_ODF);
			while (off + 35 + n < len && n < 24 && buf[off + 35 + n] >= 'a' &&
			       buf[off + 35 + n] <= 'z')
				n++;
			mos_fieldcpy(t->detail, sizeof(t->detail), (const char *)buf + off + 35, n);
			return t->kind;
		}
	}
	if (ft_archive_open(&a, buf, len, FT_ZIP, NULL, 0))
		return t->kind;
	for (int i = 0; i < 256 && ft_archive_entry(&a, i, &e) == 1; i++) {
		if (mos_has_prefix(e.name, "word/"))
			return set(t, FT_DOCX);
		if (mos_has_prefix(e.name, "xl/"))
			return set(t, FT_XLSX);
		if (mos_has_prefix(e.name, "ppt/"))
			return set(t, FT_PPTX);
		manifest |= !strcmp(e.name, "META-INF/MANIFEST.MF");
		android |= !strcmp(e.name, "AndroidManifest.xml");
		dex |= !strcmp(e.name, "classes.dex");
	}
	if (android && dex)
		return set(t, FT_APK_ANDROID);
	if (manifest)
		return set(t, FT_JAR);
	return t->kind;
}

/* ---- delegation to the program and package probes ---------------------------- */

static bool identify_exec(const u8 *buf, size_t len, struct ft_type *t)
{
	struct binfmt_info bi;
	const char *what;

	if (binfmt_probe(buf, len, &bi) || bi.format == BINFMT_SCRIPT) {
		/* Headers the loader rejects are still worth naming. */
		if (at(buf, len, 0, "\177ELF", 4)) {
			set(t, FT_EXEC);
			t->name = "ELF file";
			t->mime = "application/x-elf";
			mos_strlcpy(t->detail, "not loadable here (other CPU or object file)",
				    sizeof(t->detail));
			return true;
		}
		if (at(buf, len, 0, "MZ", 2) && len >= 64) {
			set(t, FT_EXEC);
			t->name = "DOS/Windows executable";
			t->mime = "application/vnd.microsoft.portable-executable";
			mos_strlcpy(t->detail, "not loadable here", sizeof(t->detail));
			return true;
		}
		return false;
	}
	set(t, FT_EXEC);
	switch (bi.format) {
	case BINFMT_ELF32: case BINFMT_ELF64:
		t->name = bi.flags & BINFMT_F_LIBRARY ? "ELF shared library" : "ELF program";
		t->mime = bi.flags & BINFMT_F_LIBRARY ? "application/x-sharedlib" :
			  "application/x-executable";
		break;
	case BINFMT_PE32: case BINFMT_PE32PLUS:
		t->name = bi.flags & BINFMT_F_LIBRARY ? "Windows DLL" : "Windows program (PE)";
		t->mime = "application/vnd.microsoft.portable-executable";
		break;
	default:
		t->name = bi.flags & BINFMT_F_LIBRARY ? "Mach-O library" : "Mach-O program";
		t->mime = "application/x-mach-binary";
		break;
	}
	what = bi.flags & BINFMT_F_GUI ? ", GUI" : bi.flags & BINFMT_F_DYNAMIC ? ", dynamic" : "";
	mos_snprintf(t->detail, sizeof(t->detail), "%s, %s, %s ABI%s",
		     binfmt_format_name(bi.format), binfmt_arch_name(bi.arch),
		     abi_personality_name(bi.personality), what);
	return true;
}

static bool identify_package(const u8 *buf, size_t len, const char *hint, struct ft_type *t)
{
	static const char *const mime[PKG_FORMAT_COUNT] = {
		[PKG_DEB] = "application/vnd.debian.binary-package",
		[PKG_RPM] = "application/x-rpm",
		[PKG_APPIMAGE] = "application/vnd.appimage",
		[PKG_SNAP] = "application/vnd.snap",
		[PKG_FLATPAK] = "application/vnd.flatpak",
		[PKG_NIX_NAR] = "application/x-nix-archive",
		[PKG_MACOS_DMG] = "application/x-apple-diskimage",
		[PKG_MACOS_XAR] = "application/x-xar",
		[PKG_MERGED] = "application/x-merged",
	};
	const enum pkg_format f = pkg_probe(buf, len, hint);

	if (f == PKG_UNKNOWN)
		return false;
	if (f == PKG_MACOS_APP) {               /* an Info.plist, not a bundle */
		if (at(buf, len, 0, "bplist00", 8)) {
			set(t, FT_PLIST_BIN);
			return true;
		}
		return false;                   /* XML: the text rules name it */
	}
	set(t, FT_PACKAGE);
	if (mime[f])
		t->mime = mime[f];
	else if (pkg_codec_sniff(buf, len) == PKG_CODEC_GZIP)
		t->mime = "application/gzip";
	else if (pkg_codec_sniff(buf, len) == PKG_CODEC_ZSTD)
		t->mime = "application/zstd";
	else if (pkg_codec_sniff(buf, len) == PKG_CODEC_XZ)
		t->mime = "application/x-xz";
	mos_strlcpy(t->detail, pkg_format_name(f), sizeof(t->detail));
	return true;
}

/* ---- magic numbers ----------------------------------------------------------------- */

static bool mp3_frame(const u8 *p, size_t n)
{
	return n >= 4 && p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 && ((p[1] >> 1) & 3) != 0 &&
	       ((p[1] >> 3) & 3) != 1 && (p[2] >> 4) != 15 && (p[2] >> 4) != 0 &&
	       ((p[2] >> 2) & 3) != 3;
}

static bool identify_magic(const u8 *buf, size_t len, struct ft_type *t)
{
	if (at(buf, len, 0, "\x89PNG\r\n\x1a\n", 8))
		return set(t, FT_PNG);
	if (at(buf, len, 0, "BM", 2) && len >= 18) {
		const u32 dib = rd32le(buf + 14);

		if (dib == 12 || dib == 40 || dib == 52 || dib == 56 || dib == 64 || dib == 108 ||
		    dib == 124)
			return set(t, FT_BMP);
	}
	if (at(buf, len, 0, "\xFF\xD8\xFF", 3))
		return set(t, FT_JPEG);
	if (at(buf, len, 0, "GIF87a", 6) || at(buf, len, 0, "GIF89a", 6))
		return set(t, FT_GIF);
	if (at(buf, len, 0, "RIFF", 4) && len >= 12) {
		if (at(buf, len, 8, "WEBP", 4))
			return set(t, FT_WEBP);
		if (at(buf, len, 8, "WAVE", 4))
			return set(t, FT_WAV);
		if (at(buf, len, 8, "AVI ", 4))
			return set(t, FT_AVI);
	}
	if (len >= 22 && rd32le(buf) == 0x00010000u && rd16le(buf + 4) >= 1 &&
	    rd16le(buf + 4) <= 64 && buf[6 + 3] == 0 && rd16le(buf + 6 + 4) <= 1)
		return set(t, FT_ICO);
	if (at(buf, len, 0, "fLaC", 4))
		return set(t, FT_FLAC);
	if (at(buf, len, 0, "OggS", 4)) {
		set(t, FT_OGG);
		if (at(buf, len, 28, "\x01vorbis", 7)) {
			t->name = "Ogg Vorbis audio";
		} else if (at(buf, len, 28, "OpusHead", 8)) {
			t->name = "Ogg Opus audio";
			t->mime = "audio/opus";
		} else if (at(buf, len, 28, "\x80theora", 7)) {
			t->name = "Ogg Theora video";
			t->mime = "video/ogg";
			t->cls = FT_C_VIDEO;
		} else if (at(buf, len, 28, "\x7f" "FLAC", 5)) {
			t->name = "Ogg FLAC audio";
		}
		return true;
	}
	if (at(buf, len, 0, "ID3", 3) || mp3_frame(buf, len))
		return set(t, FT_MP3);
	if (at(buf, len, 4, "ftyp", 4) && len >= 12) {
		set(t, at(buf, len, 8, "qt  ", 4) ? FT_MOV : FT_MP4);
		if (at(buf, len, 8, "M4A ", 4) || at(buf, len, 8, "M4B ", 4)) {
			t->name = "MPEG-4 audio";
			t->mime = "audio/mp4";
			t->cls = FT_C_AUDIO;
		} else if (at(buf, len, 8, "heic", 4) || at(buf, len, 8, "mif1", 4) ||
			   at(buf, len, 8, "avif", 4)) {
			t->name = at(buf, len, 8, "avif", 4) ? "AVIF image" : "HEIF image";
			t->mime = at(buf, len, 8, "avif", 4) ? "image/avif" : "image/heif";
			t->cls = FT_C_IMAGE;
		}
		mos_strlcpy(t->detail, "brand ", sizeof(t->detail));
		mos_fieldcpy(t->detail + 6, sizeof(t->detail) - 6, (const char *)buf + 8, 4);
		return true;
	}
	if (at(buf, len, 0, "\x1A\x45\xDF\xA3", 4)) {
		const size_t n = MOS_MIN(len, (size_t)64);

		return set(t, mos_memstr((const char *)buf, n, "webm") ? FT_WEBM : FT_MKV);
	}
	if (len >= 5) {
		const size_t n = MOS_MIN(len, (size_t)1024);       /* may follow junk bytes */
		const char *p = mos_memstr((const char *)buf, n, "%PDF-");

		if (p) {
			set(t, FT_PDF);
			mos_strlcpy(t->detail, "version ", sizeof(t->detail));
			if (p + 8 <= (const char *)buf + len)
				mos_fieldcpy(t->detail + 8, sizeof(t->detail) - 8, p + 5, 3);
			return true;
		}
	}
	if (at(buf, len, 0, "PK\3\4", 4) || at(buf, len, 0, "PK\5\6", 4)) {
		identify_zip(buf, len, t);
		return true;
	}
	if (at(buf, len, 0, "\x1F\x8B\x08", 3)) {
		u8 head[1024];
		size_t used, got = 0;

		if (gz_decode(buf, len, head, sizeof(head), &used, &got) >= 0 && tar_probe(head, got))
			return set(t, FT_TARGZ);
		set(t, FT_GZIP);
		if (len > 10 && (buf[3] & 0x08) && !(buf[3] & 0x04)) {   /* FNAME, no FEXTRA */
			size_t n = 0;

			while (10 + n < len && buf[10 + n] && n < 48)
				n++;
			mos_strlcpy(t->detail, "original name ", sizeof(t->detail));
			mos_fieldcpy(t->detail + 14, sizeof(t->detail) - 14, (const char *)buf + 10, n);
		}
		return true;
	}
	if (at(buf, len, 0, "\3757zXZ\0", 6))
		return set(t, FT_XZ);
	if (len >= 4 && rd32le(buf) == 0xFD2FB528u)
		return set(t, FT_ZSTD);
	if (at(buf, len, 0, "BZh", 3) && len >= 4 && buf[3] >= '1' && buf[3] <= '9')
		return set(t, FT_BZIP2);
	if (at(buf, len, 0, "7z\xBC\xAF\x27\x1C", 6))
		return set(t, FT_7Z);
	if (at(buf, len, 0, "Rar!\x1A\x07", 6))
		return set(t, FT_RAR);
	if (tar_probe(buf, len))
		return set(t, FT_TAR);
	if ((len >= 12 && rd32be(buf) == 0x00010000u && (buf[4] << 8 | buf[5]) >= 1 && (buf[4] << 8 | buf[5]) <= 64) ||
	    at(buf, len, 0, "true", 4) || at(buf, len, 0, "ttcf", 4)) {
		set(t, FT_TTF);
		if (at(buf, len, 0, "ttcf", 4))
			t->name = "TrueType font collection";
		return true;
	}
	if (at(buf, len, 0, "OTTO", 4))
		return set(t, FT_OTF);
	if (at(buf, len, 0, "wOFF", 4))
		return set(t, FT_WOFF);
	if (at(buf, len, 0, "wOF2", 4))
		return set(t, FT_WOFF2);
	if (at(buf, len, 0, "SQLite format 3\0", 16))
		return set(t, FT_SQLITE);
	if (at(buf, len, 0x8001, "CD001", 5))
		return set(t, FT_ISO9660);
	if (len >= 1024 && buf[510] == 0x55 && buf[511] == 0xAA && at(buf, len, 512, "EFI PART", 8))
		return set(t, FT_DISK_GPT);
	if (len >= 512 && buf[510] == 0x55 && buf[511] == 0xAA) {
		bool ok = true, any = false;

		for (int i = 0; i < 4; i++) {
			const u8 *e = buf + 446 + i * 16;

			ok &= e[0] == 0 || e[0] == 0x80;
			any |= e[4] != 0;
		}
		if (ok && any)
			return set(t, FT_DISK_MBR);
	}
	return false;
}

/* ---- file-name fallback ------------------------------------------------------------- */

static bool identify_name(const char *base, struct ft_type *t)
{
	static const struct { const char *ext; enum ft_kind k; } ext[] = {
		{ ".png", FT_PNG }, { ".bmp", FT_BMP }, { ".jpg", FT_JPEG }, { ".jpeg", FT_JPEG },
		{ ".gif", FT_GIF }, { ".webp", FT_WEBP }, { ".ico", FT_ICO }, { ".wav", FT_WAV },
		{ ".mp3", FT_MP3 }, { ".flac", FT_FLAC }, { ".ogg", FT_OGG }, { ".mp4", FT_MP4 },
		{ ".mov", FT_MOV }, { ".mkv", FT_MKV }, { ".webm", FT_WEBM }, { ".avi", FT_AVI },
		{ ".pdf", FT_PDF }, { ".docx", FT_DOCX }, { ".xlsx", FT_XLSX }, { ".pptx", FT_PPTX },
		{ ".epub", FT_EPUB }, { ".zip", FT_ZIP }, { ".jar", FT_JAR }, { ".tar", FT_TAR },
		{ ".gz", FT_GZIP }, { ".xz", FT_XZ }, { ".zst", FT_ZSTD }, { ".bz2", FT_BZIP2 },
		{ ".7z", FT_7Z }, { ".rar", FT_RAR }, { ".ttf", FT_TTF }, { ".otf", FT_OTF },
		{ ".woff", FT_WOFF }, { ".woff2", FT_WOFF2 }, { ".sqlite", FT_SQLITE },
		{ ".db", FT_SQLITE }, { ".iso", FT_ISO9660 }, { ".img", FT_DISK_MBR },
	};

	for (size_t i = 0; base && i < ARRAY_SIZE(ext); i++)
		if (mos_has_suffix(base, ext[i].ext)) {
			set(t, ext[i].k);
			t->by_name = true;
			mos_strlcpy(t->detail, "by file name only: content not recognised",
				    sizeof(t->detail));
			return true;
		}
	return false;
}

enum ft_kind ft_identify(const u8 *buf, size_t len, const char *name_hint, struct ft_type *t)
{
	const char *base = name_hint ? ft_base_name(name_hint) : NULL;

	memset(t, 0, sizeof(*t));
	if (!buf || !len)
		return set(t, FT_EMPTY);
	if (identify_package(buf, len, name_hint, t) || identify_exec(buf, len, t) ||
	    identify_magic(buf, len, t))
		return t->kind;
	if (ft_is_text(buf, len))
		return identify_text(buf, len, base, t);
	if (identify_name(base, t))
		return t->kind;
	return set(t, FT_BINARY);
}
