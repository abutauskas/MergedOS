/* SPDX-License-Identifier: BSD-2-Clause */
/* File identification, viewers and image decoders (filetype/). */
#include "tests/test.h"
#include "filetype/filetype.h"
#include "pkg_translate/pkg_internal.h"
#include "desktop/desktop.h"

static u8 rgb[256 * 256 * 3];
static u8 scratch[300 * 1024];

static enum ft_kind kind_of(const u8 *b, size_t n, const char *name)
{
	struct ft_type t;

	return ft_identify(b, n, name, &t);
}

static void check_sample(const char *rel, enum ft_kind want, const char *mime)
{
	size_t n;
	u8 *b = load_fixture(rel, &n);
	struct ft_type t;

	CHECK_EQ(ft_identify(b, n, rel, &t), want);
	CHECK_STR(t.mime, mime);
	CHECK(!t.by_name);
	/* Content first: the same answer without a file name (Markdown and CSV
	 * aside, which have no magic and are recognised by their shape). */
	CHECK_EQ(ft_identify(b, n, NULL, &t), want);
	free(b);
}

/* ---- builders for minimal files ------------------------------------------------- */

static void be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static void le16(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static void le32(u8 *p, u32 v)
{
	le16(p, v);
	le16(p + 2, v >> 16);
}

/* A zip of empty stored members, with a central directory. */
static size_t mkzip(u8 *out, const char *const *names, int n)
{
	size_t pos = 0, cd, offs[16];

	for (int i = 0; i < n; i++) {
		const size_t nl = strlen(names[i]);

		offs[i] = pos;
		memset(out + pos, 0, 30);
		memcpy(out + pos, "PK\3\4", 4);
		le16(out + pos + 4, 20);
		le16(out + pos + 26, (u32)nl);
		memcpy(out + pos + 30, names[i], nl);
		pos += 30 + nl;
	}
	cd = pos;
	for (int i = 0; i < n; i++) {
		const size_t nl = strlen(names[i]);

		memset(out + pos, 0, 46);
		memcpy(out + pos, "PK\1\2", 4);
		le16(out + pos + 28, (u32)nl);
		le32(out + pos + 42, (u32)offs[i]);
		memcpy(out + pos + 46, names[i], nl);
		pos += 46 + nl;
	}
	memset(out + pos, 0, 22);
	memcpy(out + pos, "PK\5\6", 4);
	le16(out + pos + 8, (u32)n);
	le16(out + pos + 10, (u32)n);
	le32(out + pos + 12, (u32)(pos - cd));
	le32(out + pos + 16, (u32)cd);
	return pos + 22;
}

static size_t chunk(u8 *out, const char *type, const u8 *data, size_t n)
{
	be32(out, (u32)n);
	memcpy(out + 4, type, 4);
	if (n)
		memcpy(out + 8, data, n);
	be32(out + 8 + n, pkg_crc32(0, out + 4, n + 4));
	return 12 + n;
}

/* zlib stream of stored DEFLATE blocks. */
static size_t zstored(u8 *out, const u8 *raw, size_t n)
{
	size_t pos = 2, done = 0;
	u32 a = 1, b = 0;

	out[0] = 0x78;
	out[1] = 0x01;
	do {
		const size_t k = n - done > 65535 ? 65535 : n - done;

		out[pos] = done + k == n;
		le16(out + pos + 1, (u32)k);
		le16(out + pos + 3, (u32)(~k & 0xFFFF));
		memcpy(out + pos + 5, raw + done, k);
		pos += 5 + k;
		done += k;
	} while (done < n);
	for (size_t i = 0; i < n; i++) {
		a = (a + raw[i]) % 65521;
		b = (b + a) % 65521;
	}
	be32(out + pos, b << 16 | a);
	return pos + 4;
}

struct pngspec {
	u32 w, h;
	u8 depth, ctype, interlace;
	const u8 *plte, *trns;
	size_t nplte, ntrns;
	int split;                      /* IDAT chunks */
};

static size_t mkpng(u8 *out, const struct pngspec *s, const u8 *raw, size_t rawlen)
{
	u8 ihdr[13], z[8192];
	size_t pos = 8, zn = zstored(z, raw, rawlen), part = (zn + s->split - 1) / s->split;

	memcpy(out, "\x89PNG\r\n\x1a\n", 8);
	be32(ihdr, s->w);
	be32(ihdr + 4, s->h);
	ihdr[8] = s->depth;
	ihdr[9] = s->ctype;
	ihdr[10] = ihdr[11] = 0;
	ihdr[12] = s->interlace;
	pos += chunk(out + pos, "IHDR", ihdr, 13);
	if (s->plte)
		pos += chunk(out + pos, "PLTE", s->plte, s->nplte);
	if (s->trns)
		pos += chunk(out + pos, "tRNS", s->trns, s->ntrns);
	for (size_t off = 0; off < zn; off += part)
		pos += chunk(out + pos, "IDAT", z + off, off + part > zn ? zn - off : part);
	pos += chunk(out + pos, "IEND", NULL, 0);
	return pos;
}

static const u8 *px(const struct ft_image *im, u32 x, u32 y)
{
	return im->rgb + ((size_t)y * im->w + x) * 3;
}

#define CHECK_PX(im, x, y, r, g, b)                                                  \
	do {                                                                          \
		const u8 *p_ = px(im, x, y);                                          \
		CHECK(p_[0] == (r) && p_[1] == (g) && p_[2] == (b));                  \
	} while (0)

/* ---- identification ------------------------------------------------------------------ */

static void test_identify_samples(void)
{
	check_sample("home/samples/sample.png", FT_PNG, "image/png");
	check_sample("home/samples/sample.bmp", FT_BMP, "image/bmp");
	check_sample("home/samples/sample.wav", FT_WAV, "audio/wav");
	check_sample("home/samples/sample.zip", FT_ZIP, "application/zip");
	check_sample("home/samples/sample.tar.gz", FT_TARGZ, "application/gzip");
	check_sample("home/samples/sample.json", FT_JSON, "application/json");
	check_sample("home/samples/sample.csv", FT_CSV, "text/csv");
	check_sample("home/samples/README.md", FT_MARKDOWN, "text/markdown");
	check_sample("home/samples/sample.pdf", FT_PDF, "application/pdf");
	check_sample("home/samples/random.bin", FT_BINARY, "application/octet-stream");
	check_sample("etc/motd", FT_TEXT, "text/plain");
	check_sample("bin/hello.linux", FT_EXEC, "application/x-executable");
	check_sample("bin/hello.exe", FT_EXEC, "application/vnd.microsoft.portable-executable");
	check_sample("bin/hello.macho", FT_EXEC, "application/x-mach-binary");
	check_sample("pkg/hello_1.0-1_amd64.deb", FT_PACKAGE, "application/vnd.debian.binary-package");
	check_sample("pkg/hello-1.0-1.x86_64.rpm", FT_PACKAGE, "application/x-rpm");
	check_sample("etc/mergedfence/default.merged", FT_PACKAGE, "application/x-merged");
	check_sample("neg/Hello-x86_64.AppImage", FT_PACKAGE, "application/vnd.appimage");
	check_sample("Applications/Hello.app/Contents/Info.plist", FT_XML, "application/xml");
	{
		size_t n;
		u8 *b = load_fixture("bin/hello.linux", &n);
		struct ft_type t;

		ft_identify(b, n, "hello.linux", &t);
		CHECK(strstr(t.detail, "ELF64") && strstr(t.detail, "inux"));
		CHECK_EQ(t.cls, FT_C_PROGRAM);
		free(b);
		b = load_fixture("pkg/hello-1.0-r0.apk", &n);
		CHECK_EQ(ft_identify(b, n, "hello-1.0-r0.apk", &t), FT_PACKAGE);
		CHECK(strstr(t.detail, "Alpine") != NULL);
		free(b);
	}
}

static void test_identify_magic(void)
{
	static u8 b[0x8100];
	struct ft_type t;
	struct ft_meta m;

#define ID(bytes, n, name, want)                                       \
	do {                                                            \
		memset(b, 0, sizeof(b));                                \
		memcpy(b, bytes, sizeof(bytes) - 1);                    \
		CHECK_EQ(kind_of(b, n, name), want);                    \
	} while (0)

	/* JPEG with a baseline SOF0: 320 x 200, 3 components. */
	ID("\xFF\xD8\xFF\xE0\x00\x04JF\xFF\xC0\x00\x11\x08\x00\xC8\x01\x40\x03", 64, NULL, FT_JPEG);
	ft_identify(b, 64, NULL, &t);
	ft_meta_read(b, 64, &t, &m);
	CHECK(m.width == 320 && m.height == 200 && m.channels == 3);
	CHECK_STR(m.extra, "baseline");
	ID("GIF89a\x40\x01\xC8\x00", 32, NULL, FT_GIF);
	ft_identify(b, 32, NULL, &t);
	ft_meta_read(b, 32, &t, &m);
	CHECK(m.width == 320 && m.height == 200);
	/* WebP lossless: width-1 = 99, height-1 = 49 packed in 14-bit fields. */
	ID("RIFF\x20\0\0\0WEBPVP8L\x10\0\0\0\x2F\x63\x40\x0C\x00", 40, NULL, FT_WEBP);
	ft_identify(b, 40, NULL, &t);
	ft_meta_read(b, 40, &t, &m);
	CHECK(m.width == 100 && m.height == 50);
	ID("\0\0\1\0\1\0\x20\x20\0\0\1\0\x20\0", 40, NULL, FT_ICO);
	ID("ID3\3\0\0\0\0\0\0", 64, NULL, FT_MP3);
	ID("\xFF\xFB\x90\x64", 64, NULL, FT_MP3);
	ft_identify(b, 64, NULL, &t);
	ft_meta_read(b, 64, &t, &m);
	CHECK(m.rate == 44100 && m.channels == 2);
	/* FLAC STREAMINFO: 44100 Hz, 2 channels, 16 bit, 88200 samples = 2 s. */
	ID("fLaC\x80\0\0\x22\x10\0\x10\0\0\0\0\0\0\0\x0A\xC4\x42\xF0\x00\x01\x58\x88", 64, NULL,
	   FT_FLAC);
	ft_identify(b, 64, NULL, &t);
	ft_meta_read(b, 64, &t, &m);
	CHECK(m.rate == 44100 && m.channels == 2 && m.depth == 16 && m.duration_ms == 2000);
	ID("OggS\0\2\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1\x1E\x01vorbis", 64, NULL, FT_OGG);
	ft_identify(b, 64, NULL, &t);
	CHECK_STR(t.name, "Ogg Vorbis audio");
	ID("\0\0\0\x18" "ftypisom", 32, NULL, FT_MP4);
	ID("\0\0\0\x14" "ftypqt  ", 32, NULL, FT_MOV);
	ID("\0\0\0\x18" "ftypM4A ", 32, NULL, FT_MP4);
	ft_identify(b, 32, NULL, &t);
	CHECK_EQ(t.cls, FT_C_AUDIO);
	ID("\x1A\x45\xDF\xA3\x9F\x42\x86\x81\x01\x42\x82\x88matroska", 64, NULL, FT_MKV);
	ID("\x1A\x45\xDF\xA3\x9F\x42\x86\x81\x01\x42\x82\x84webm", 64, NULL, FT_WEBM);
	ID("RIFF\0\0\0\0AVI LIST", 64, NULL, FT_AVI);
	ID("\3757zXZ\0", 32, NULL, FT_XZ);
	ID("\x28\xB5\x2F\xFD", 32, NULL, FT_ZSTD);
	ID("BZh9\x31\x41\x59\x26", 32, NULL, FT_BZIP2);
	ID("7z\xBC\xAF\x27\x1C", 32, NULL, FT_7Z);
	ID("Rar!\x1A\x07\x01\x00", 32, NULL, FT_RAR);
	ID("\0\1\0\0\0\x0C\0\x80", 64, NULL, FT_TTF);
	ID("OTTO\0\x0A", 64, NULL, FT_OTF);
	ID("wOFF\0\1\0\0", 64, NULL, FT_WOFF);
	ID("wOF2\0\1\0\0", 64, NULL, FT_WOFF2);
	ID("SQLite format 3\0\x10\0", 100, NULL, FT_SQLITE);
	ID("%PDF-1.7\n", 64, NULL, FT_PDF);
	ID("\x1F\x8B\x08\x08\0\0\0\0\0\3x.bin\0\x03\0\0\0\0\0\0\0\0\0", 30, NULL, FT_GZIP);
	ft_identify(b, 30, NULL, &t);
	CHECK(strstr(t.detail, "x.bin") != NULL);
	/* ISO 9660: primary volume descriptor at sector 16. */
	memset(b, 0, sizeof(b));
	memcpy(b + 0x8000, "\1CD001\1", 7);
	memcpy(b + 0x8028, "MERGEDOS                        ", 32);
	CHECK_EQ(kind_of(b, sizeof(b), NULL), FT_ISO9660);
	ft_identify(b, sizeof(b), NULL, &t);
	ft_meta_read(b, sizeof(b), &t, &m);
	CHECK(strstr(m.extra, "MERGEDOS") != NULL);
	/* MBR with one partition, then GPT behind a protective MBR. */
	memset(b, 0, sizeof(b));
	b[446 + 4] = 0x83;
	b[510] = 0x55;
	b[511] = 0xAA;
	CHECK_EQ(kind_of(b, 4096, NULL), FT_DISK_MBR);
	b[446 + 4] = 0xEE;
	memcpy(b + 512, "EFI PART", 8);
	CHECK_EQ(kind_of(b, 4096, NULL), FT_DISK_GPT);
	b[446] = 0x42;                          /* not a partition table: no MBR claim */
	memset(b + 512, 0, 8);
	CHECK(kind_of(b, 4096, NULL) != FT_DISK_MBR);

	/* Zip containers named by their members. */
	{
		static const char *const docx[] = { "[Content_Types].xml", "word/document.xml" };
		static const char *const xlsx[] = { "[Content_Types].xml", "xl/workbook.xml" };
		static const char *const pptx[] = { "[Content_Types].xml", "ppt/presentation.xml" };
		static const char *const jar[] = { "META-INF/MANIFEST.MF", "Main.class" };
		static const char *const apk[] = { "AndroidManifest.xml", "classes.dex", "res/" };
		static const char *const plain[] = { "a.txt", "b/" };
		size_t n;

		memset(b, 0, sizeof(b));
		n = mkzip(b, docx, 2);
		CHECK_EQ(kind_of(b, n, NULL), FT_DOCX);
		n = mkzip(b, xlsx, 2);
		CHECK_EQ(kind_of(b, n, NULL), FT_XLSX);
		n = mkzip(b, pptx, 2);
		CHECK_EQ(kind_of(b, n, NULL), FT_PPTX);
		n = mkzip(b, jar, 2);
		CHECK_EQ(kind_of(b, n, NULL), FT_JAR);
		n = mkzip(b, apk, 3);
		CHECK_EQ(kind_of(b, n, "x.apk"), FT_APK_ANDROID);
		n = mkzip(b, plain, 2);
		CHECK_EQ(kind_of(b, n, NULL), FT_ZIP);
		/* EPUB: stored "mimetype" member first. */
		memset(b, 0, 128);
		memcpy(b, "PK\3\4\x0A\0\0\0\0\0", 10);
		le16(b + 26, 8);
		memcpy(b + 30, "mimetypeapplication/epub+zip", 28);
		CHECK_EQ(kind_of(b, 100, NULL), FT_EPUB);
	}

	/* Text family. */
#define TXT(s, name, want) CHECK_EQ(kind_of((const u8 *)(s), sizeof(s) - 1, name), want)
	TXT("#!/bin/sh\necho hi\n", NULL, FT_SCRIPT);
	TXT("#!/usr/bin/env python3\nprint(1)\n", NULL, FT_SCRIPT);
	ft_identify((const u8 *)"#!/usr/bin/env python3\n", 23, NULL, &t);
	CHECK_STR(t.mime, "text/x-python");
	CHECK_STR(t.detail, "interpreter python3");
	TXT("  {\"a\": [1, 2]}\n", NULL, FT_JSON);
	TXT("[1, 2, 3]", NULL, FT_JSON);
	TXT("<?xml version=\"1.0\"?>\n<root/>\n", NULL, FT_XML);
	TXT("<!DOCTYPE html>\n<html><body>hi</body></html>\n", NULL, FT_HTML);
	TXT("<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n", NULL, FT_SVG);
	TXT("# Title\n\nSome **bold** text.\n", NULL, FT_MARKDOWN);
	TXT("plain words\n", "notes.md", FT_MARKDOWN);
	TXT("# a comment\nkey=value\n", NULL, FT_TEXT);    /* config, not Markdown */
	TXT("a,b,c\n1,2,3\n4,5,6\n", NULL, FT_CSV);
	TXT("x;y\n", "t.csv", FT_CSV);
	TXT("Hello, world.\nPlain prose, with commas, here.\n", NULL, FT_TEXT);
	TXT("caf\xC3\xA9 au lait\n", NULL, FT_TEXT);
	ft_identify((const u8 *)"caf\xC3\xA9\n", 6, NULL, &t);
	CHECK_STR(t.detail, "UTF-8");
	TXT("int main(void) { return 0; }\n", "main.c", FT_TEXT);
	ft_identify((const u8 *)"int x;\n", 7, "main.c", &t);
	CHECK_STR(t.mime, "text/x-c");
	TXT("bad \xC3\x28 utf8\n", NULL, FT_BINARY);
	CHECK_EQ(kind_of(NULL, 0, NULL), FT_EMPTY);
	CHECK_EQ(kind_of((const u8 *)"x", 0, "x.png"), FT_EMPTY);
	/* Name only when the content says nothing. */
	ft_identify((const u8 *)"\x01\x02\x03\x04", 4, "song.mp3", &t);
	CHECK(t.kind == FT_MP3 && t.by_name);
	CHECK_EQ(kind_of((const u8 *)"\x01\x02\x03\x04", 4, "x.unknownext"), FT_BINARY);
	CHECK_EQ(kind_of((const u8 *)"hello\n", 6, "fake.png"), FT_TEXT);  /* content wins */
	CHECK(ft_rendered(FT_PNG) && ft_rendered(FT_TARGZ) && !ft_rendered(FT_JPEG) &&
	      !ft_rendered(FT_MP4) && !ft_rendered(FT_DOCX));
#undef ID
#undef TXT
}

/* ---- images -------------------------------------------------------------------------- */

static void test_images(void)
{
	struct ft_image im;
	size_t n;
	u8 *b = load_fixture("home/samples/sample.png", &n);

	CHECK_EQ(ft_image_decode(b, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im), 0);
	CHECK(im.w == 64 && im.h == 48);
	CHECK_PX(&im, 0, 0, 40, 60, 160);
	CHECK_PX(&im, 20, 24, 255, 200, 40);            /* sun disc */
	CHECK_PX(&im, 46, 20, 30, 160, 120);            /* green block */
	CHECK_PX(&im, 63, 47, 229, 201, 160);
	CHECK_PX(&im, 5, 40, 55, 180, 160);
	/* Size limits: too few pixels, too little scratch. */
	CHECK_EQ(ft_image_decode(b, n, rgb, 64 * 48 * 3 - 1, scratch, sizeof(scratch), &im),
		 -MERR_RANGE);
	CHECK_EQ(ft_image_decode(b, n, rgb, sizeof(rgb), scratch, 4096, &im), -MERR_RANGE);
	CHECK_EQ(ft_image_decode(b, n, rgb, sizeof(rgb), NULL, 0, &im), -MERR_RANGE);
	/* A corrupted CRC is caught. */
	b[40] ^= 1;
	CHECK_EQ(ft_image_decode(b, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im),
		 -MERR_INVAL);
	free(b);

	b = load_fixture("home/samples/sample.bmp", &n);
	CHECK_EQ(ft_image_decode(b, n, rgb, sizeof(rgb), NULL, 0, &im), 0);
	CHECK(im.w == 50 && im.h == 30);
	CHECK_PX(&im, 0, 0, 0, 0, 128);
	CHECK_PX(&im, 15, 10, 255, 0, 0);
	CHECK_PX(&im, 49, 29, 245, 232, 128);
	CHECK_PX(&im, 20, 5, 100, 40, 128);
	CHECK_EQ(ft_image_decode(b, n, rgb, 50 * 30 * 3 - 3, NULL, 0, &im), -MERR_RANGE);
	b[28] = 8;                                      /* 8-bit palette: not decoded */
	CHECK_EQ(ft_image_decode(b, n, rgb, sizeof(rgb), NULL, 0, &im), -MERR_NOTSUP);
	b[28] = 24;
	b[30] = 1;                                      /* RLE8 */
	CHECK_EQ(ft_image_decode(b, n, rgb, sizeof(rgb), NULL, 0, &im), -MERR_NOTSUP);
	b[30] = 0;
	CHECK_EQ(ft_image_decode(b, n - 1, rgb, sizeof(rgb), NULL, 0, &im), -MERR_INVAL);
	/* Top-down (negative height) BMP flips the row order. */
	{
		u8 *td = malloc(n);
		const size_t stride = 152;

		memcpy(td, b, 54);
		le32(td + 22, (u32)-30);
		for (int y = 0; y < 30; y++)
			memcpy(td + 54 + y * stride, b + 54 + (29 - y) * stride, stride);
		CHECK_EQ(ft_image_decode(td, n, rgb, sizeof(rgb), NULL, 0, &im), 0);
		CHECK_PX(&im, 15, 10, 255, 0, 0);
		CHECK_PX(&im, 49, 29, 245, 232, 128);
		free(td);
	}
	/* 32-bit BMP, 2 x 2, bottom-up. */
	{
		u8 bm[54 + 16];

		memset(bm, 0, sizeof(bm));
		memcpy(bm, "BM", 2);
		le32(bm + 10, 54);
		le32(bm + 14, 40);
		le32(bm + 18, 2);
		le32(bm + 22, 2);
		le16(bm + 26, 1);
		le16(bm + 28, 32);
		memcpy(bm + 54, "\x01\x02\x03\xFF\x04\x05\x06\xFF\x07\x08\x09\xFF\x0A\x0B\x0C\xFF", 16);
		CHECK_EQ(ft_image_decode(bm, sizeof(bm), rgb, sizeof(rgb), NULL, 0, &im), 0);
		CHECK_PX(&im, 0, 1, 3, 2, 1);
		CHECK_PX(&im, 1, 0, 12, 11, 10);
	}
	free(b);

	/* PNG variants built here with stored DEFLATE blocks. */
	{
		static u8 png[16384];
		u8 raw[256];
		const u8 plte[] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 10, 20, 30 };
		const u8 trns[] = { 255, 0 };
		struct pngspec s = { 4, 2, 2, 3, 0, plte, trns, 12, 2, 1 };

		/* 2-bit palette, 4 x 2: indices 0 1 2 3 / 3 2 1 0; entry 1 transparent. */
		raw[0] = 0;
		raw[1] = 0x1B;
		raw[2] = 1;                             /* Sub filter on a sub-byte row */
		raw[3] = 0xE4;
		/* Sub with bpp 1 adds the previous byte: store the difference. */
		n = mkpng(png, &s, raw, 4);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im), 0);
		CHECK_PX(&im, 0, 0, 255, 0, 0);
		CHECK_PX(&im, 1, 0, 255, 255, 255);     /* alpha 0 over white */
		CHECK_PX(&im, 3, 0, 10, 20, 30);
		/* row 1: 0xE4 alone (first byte of Sub has no left neighbour) = 3 2 1 0 */
		CHECK_PX(&im, 0, 1, 10, 20, 30);
		CHECK_PX(&im, 3, 1, 255, 0, 0);
		/* No PLTE for a palette image: corrupt. */
		s.plte = NULL;
		s.trns = NULL;
		n = mkpng(png, &s, raw, 4);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im),
			 -MERR_INVAL);

		/* 16-bit greyscale, 2 x 1, split over three IDAT chunks. */
		s = (struct pngspec){ 2, 1, 16, 0, 0, NULL, NULL, 0, 0, 3 };
		memcpy(raw, "\0\x80\x01\xFF\xFF", 5);
		n = mkpng(png, &s, raw, 5);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im), 0);
		CHECK_PX(&im, 0, 0, 128, 128, 128);
		CHECK_PX(&im, 1, 0, 255, 255, 255);

		/* RGBA 8-bit, 2 x 2, Up filter on row 1; half-transparent black. */
		s = (struct pngspec){ 2, 2, 8, 6, 0, NULL, NULL, 0, 0, 1 };
		memcpy(raw, "\0\x00\x00\x00\x80\xFF\x00\x00\xFF" "\2\x00\x00\x00\x00\x00\xFF\x00\x00", 18);
		n = mkpng(png, &s, raw, 18);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im), 0);
		CHECK_PX(&im, 0, 0, 127, 127, 127);
		CHECK_PX(&im, 1, 0, 255, 0, 0);
		CHECK_PX(&im, 0, 1, 127, 127, 127);
		CHECK_PX(&im, 1, 1, 255, 255, 0);       /* Up: (255,0,0)+(0,255,0) */

		/* Grey + alpha, Average and Paeth filters. */
		s = (struct pngspec){ 2, 2, 8, 4, 0, NULL, NULL, 0, 0, 1 };
		memcpy(raw, "\3\x40\xFF\x20\x80" "\4\x10\x00\x10\x00", 10);
		n = mkpng(png, &s, raw, 10);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im), 0);
		CHECK_PX(&im, 0, 0, 0x40, 0x40, 0x40);  /* avg(0,0)=0 -> 0x40 */
		CHECK_PX(&im, 1, 0, 0x40, 0x40, 0x40);  /* 0x20 + avg(0x40,0)=0x20 */
		CHECK_PX(&im, 0, 1, 0x50, 0x50, 0x50);  /* Paeth picks up: 0x40 + 0x10 */

		/* Interlaced: identified, not decoded. Bad filter type: corrupt. */
		s = (struct pngspec){ 2, 1, 8, 0, 1, NULL, NULL, 0, 0, 1 };
		memcpy(raw, "\0\x10\x20", 3);
		n = mkpng(png, &s, raw, 3);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im),
			 -MERR_NOTSUP);
		s.interlace = 0;
		raw[0] = 7;
		n = mkpng(png, &s, raw, 3);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im),
			 -MERR_INVAL);
		/* Too much or too little scanline data. */
		raw[0] = 0;
		n = mkpng(png, &s, raw, 2);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im),
			 -MERR_INVAL);
		/* Oversized dimensions are refused before any decoding. */
		s = (struct pngspec){ 100000, 100000, 8, 2, 0, NULL, NULL, 0, 0, 1 };
		n = mkpng(png, &s, raw, 3);
		CHECK_EQ(ft_image_decode(png, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im),
			 -MERR_RANGE);
	}
	CHECK_EQ(ft_image_decode((const u8 *)"GIF89a", 6, rgb, sizeof(rgb), scratch,
				 sizeof(scratch), &im), -MERR_NOTSUP);
	CHECK_STR(ft_image_error(-MERR_RANGE), "too large for the viewer's image buffer");
}

/* ---- archives, text and hex views ------------------------------------------------------- */

static void test_views(void)
{
	struct ft_archive a;
	struct ft_entry e;
	struct ft_doc d;
	char line[FT_LINE_MAX];
	size_t n;
	u8 *b = load_fixture("home/samples/sample.zip", &n);

	CHECK_EQ(ft_archive_open(&a, b, n, FT_ZIP, NULL, 0), 0);
	CHECK(a.central && a.count == 2);
	CHECK_EQ(ft_archive_entry(&a, 0, &e), 1);
	CHECK_STR(e.name, "readme.txt");
	CHECK(e.method == 8 && e.size == 25 * 8 && e.packed < e.size);
	CHECK_EQ(ft_archive_entry(&a, 1, &e), 1);
	CHECK_STR(e.name, "notes/hello.txt");
	CHECK(e.method == 0 && e.size == 24 && e.packed == 24);
	ft_entry_format(&a, &e, line, sizeof(line));
	CHECK(strstr(line, "stored") && strstr(line, "notes/hello.txt"));
	CHECK_EQ(ft_archive_entry(&a, 2, &e), 0);
	/* Truncated: no central directory, local headers still list. */
	CHECK_EQ(ft_archive_open(&a, b, n - 40, FT_ZIP, NULL, 0), 0);
	CHECK(!a.central);
	CHECK_EQ(ft_archive_entry(&a, 0, &e), 1);
	CHECK_STR(e.name, "readme.txt");
	ft_doc_open(&d, b, n, "/home/samples/sample.zip", scratch, sizeof(scratch));
	CHECK(d.views == (FT_VIEW_HEX | FT_VIEW_INFO));
	CHECK_EQ(ft_doc_default_view(&d), FT_VIEW_INFO);
	{
		bool found = false;

		for (int i = 0; ft_doc_line(&d, FT_VIEW_INFO, i, line, sizeof(line)); i++)
			found |= strstr(line, "deflated") && strstr(line, "readme.txt");
		CHECK(found);
	}
	free(b);

	b = load_fixture("home/samples/sample.tar.gz", &n);
	CHECK_EQ(ft_archive_open(&a, b, n, FT_TARGZ, scratch, sizeof(scratch)), 0);
	CHECK(!a.partial);
	CHECK_EQ(ft_archive_entry(&a, 0, &e), 1);
	CHECK(!strcmp(e.name, "a.txt") && e.size == 11);
	CHECK_EQ(ft_archive_entry(&a, 1, &e), 1);
	CHECK(!strcmp(e.name, "docs/") && e.dir);
	CHECK_EQ(ft_archive_entry(&a, 2, &e), 1);
	CHECK_STR(e.name, "docs/b.txt");
	CHECK_EQ(ft_archive_entry(&a, 3, &e), 0);
	/* Scratch smaller than the tar: a prefix is listed. */
	CHECK_EQ(ft_archive_open(&a, b, n, FT_TARGZ, scratch, 1024), 0);
	CHECK(a.partial);
	CHECK_EQ(ft_archive_entry(&a, 0, &e), 1);
	CHECK_EQ(ft_archive_open(&a, b, n, FT_TARGZ, NULL, 0), -MERR_RANGE);
	free(b);
	b = load_fixture("pkg/hello_1.0-1_amd64.deb", &n);
	CHECK(ft_archive_open(&a, b, n, FT_TAR, NULL, 0) < 0);
	free(b);

	/* Hex view. */
	b = load_fixture("home/samples/sample.png", &n);
	CHECK_EQ(ft_hex_line(b, n, 0, line, sizeof(line)), 1);
	CHECK_STR(line, "00000000  89 50 4e 47 0d 0a 1a 0a  00 00 00 0d 49 48 44 52  |.PNG........IHDR|");
	CHECK(strlen(line) < FT_HEX_LINE);
	CHECK_EQ(ft_hex_line(b, n, (int)((n + 15) / 16), line, sizeof(line)), 0);
	CHECK_EQ(ft_hex_line(b, n, (int)((n - 1) / 16), line, sizeof(line)), 1);
	CHECK(line[strlen(line) - 1] == '|');
	ft_doc_open(&d, b, n, "sample.png", scratch, sizeof(scratch));
	CHECK(d.views == (FT_VIEW_HEX | FT_VIEW_INFO | FT_VIEW_IMAGE));
	CHECK_EQ(ft_doc_default_view(&d), FT_VIEW_IMAGE);
	CHECK(d.meta.width == 64 && d.meta.height == 48 && d.meta.depth == 8);
	CHECK_STR(d.meta.extra, "RGB");
	{
		bool dims = false;

		for (int i = 0; ft_doc_line(&d, FT_VIEW_INFO, i, line, sizeof(line)); i++)
			dims |= !strcmp(line, "Image    64 x 48, 8-bit");
		CHECK(dims);
	}
	free(b);

	/* Text view: lines, tabs, control bytes, bad UTF-8, cut-off. */
	{
		static const char txt[] = "first\r\n\tTab\x01\xFFz caf\xC3\xA9\n\nlast";
		char big[300];

		CHECK_EQ(ft_text_line((const u8 *)txt, sizeof(txt) - 1, 0, line, sizeof(line)), 1);
		CHECK_STR(line, "first");
		CHECK_EQ(ft_text_line((const u8 *)txt, sizeof(txt) - 1, 1, line, sizeof(line)), 1);
		CHECK_STR(line, "    Tab.?z caf\xC3\xA9");
		CHECK_EQ(ft_text_line((const u8 *)txt, sizeof(txt) - 1, 2, line, sizeof(line)), 1);
		CHECK_STR(line, "");
		CHECK_EQ(ft_text_line((const u8 *)txt, sizeof(txt) - 1, 3, line, sizeof(line)), 1);
		CHECK_STR(line, "last");
		CHECK_EQ(ft_text_line((const u8 *)txt, sizeof(txt) - 1, 4, line, sizeof(line)), 0);
		CHECK_EQ(ft_text_line((const u8 *)"a\n", 2, 1, line, sizeof(line)), 0);
		memset(big, 'x', sizeof(big));
		CHECK_EQ(ft_text_line((const u8 *)big, sizeof(big), 0, line, sizeof(line)), 1);
		CHECK_EQ(strlen(line), FT_COLS + 3);
		CHECK(!strcmp(line + FT_COLS, "..."));
		CHECK_EQ(ft_text_line((const u8 *)big, sizeof(big), 0, line, 20), 1);
		CHECK(strlen(line) < 20);
	}
	b = load_fixture("home/samples/sample.csv", &n);
	ft_doc_open(&d, b, n, "sample.csv", scratch, sizeof(scratch));
	CHECK_EQ(ft_doc_default_view(&d), FT_VIEW_TEXT);
	CHECK_EQ(ft_doc_line(&d, FT_VIEW_TEXT, 1, line, sizeof(line)), 1);
	CHECK_STR(line, "sample.png,PNG image,yes");
	free(b);

	b = load_fixture("home/samples/sample.wav", &n);
	ft_doc_open(&d, b, n, "sample.wav", scratch, sizeof(scratch));
	CHECK(d.meta.channels == 1 && d.meta.rate == 8000 && d.meta.depth == 8 &&
	      d.meta.duration_ms == 1000);
	{
		bool honest = false;

		for (int i = 0; ft_doc_line(&d, FT_VIEW_INFO, i, line, sizeof(line)); i++)
			honest |= strstr(line, "no audio decoder or sound driver") != NULL;
		CHECK(honest);
	}
	free(b);
	b = load_fixture("home/samples/sample.pdf", &n);
	ft_doc_open(&d, b, n, "sample.pdf", scratch, sizeof(scratch));
	CHECK_EQ(d.meta.pages, 1);
	CHECK_STR(d.type.detail, "version 1.4");
	free(b);
	b = load_fixture("home/samples/random.bin", &n);
	ft_doc_open(&d, b, n, "random.bin", scratch, sizeof(scratch));
	CHECK_EQ(ft_doc_default_view(&d), FT_VIEW_HEX);
	CHECK(!(d.views & FT_VIEW_TEXT));
	free(b);
	ft_doc_open(&d, NULL, 0, "empty", scratch, sizeof(scratch));
	CHECK(d.type.kind == FT_EMPTY && d.views == FT_VIEW_INFO);
	CHECK_EQ(ft_doc_line(&d, FT_VIEW_HEX, 0, line, sizeof(line)), 0);
	CHECK_EQ(ft_doc_line(&d, FT_VIEW_INFO, 0, line, sizeof(line)), 1);
}

/* ---- robustness: truncations and byte flips (ASan watches) ------------------------------ */

static u32 lcg(u32 *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

/* Open, render every view and decode; false if any output broke its contract. */
static bool exercise(const u8 *b, size_t n, const char *name)
{
	struct ft_doc d;
	struct ft_image im;
	char line[FT_LINE_MAX];
	bool ok = true;
	int rc;

	ft_doc_open(&d, b, n, name, scratch, sizeof(scratch));
	for (unsigned v = FT_VIEW_TEXT; v <= FT_VIEW_INFO; v <<= 1)
		for (int i = 0; i < 64 && ft_doc_line(&d, v, i, line, sizeof(line)); i++)
			ok &= strlen(line) < sizeof(line);
	rc = ft_image_decode(b, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im);
	ok &= rc == 0 || rc == -MERR_INVAL || rc == -MERR_NOTSUP || rc == -MERR_RANGE;
	if (!rc)
		ok &= im.w && im.h && (size_t)im.w * im.h * 3 <= sizeof(rgb);
	return ok;
}

static void test_fuzz(void)
{
	static const char *const files[] = { "home/samples/sample.png", "home/samples/sample.bmp",
					     "home/samples/sample.zip", "home/samples/sample.tar.gz",
					     "home/samples/sample.wav", "home/samples/sample.pdf" };
	u32 seed = 12345;

	for (size_t f = 0; f < ARRAY_SIZE(files); f++) {
		size_t n;
		u8 *orig = load_fixture(files[f], &n), *b = malloc(n);
		int bad = 0;

		/* Every truncation, in an exactly-sized heap block. */
		for (size_t len = 0; len <= n; len++) {
			u8 *t = malloc(len ? len : 1);

			memcpy(t, orig, len);
			bad += !exercise(t, len, files[f]);
			free(t);
		}
		/* Many single and multiple byte flips. */
		for (int round = 0; round < 1500; round++) {
			const int flips = 1 + (int)(lcg(&seed) % 4);

			memcpy(b, orig, n);
			for (int k = 0; k < flips; k++) {
				const size_t at = lcg(&seed) % n;

				b[at] = round & 1 ? (u8)lcg(&seed) : (u8)(b[at] ^ (1u << (lcg(&seed) % 8)));
			}
			bad += !exercise(b, n, files[f]);
		}
		CHECK_EQ(bad, 0);               /* one check per file: ASan does the rest */
		free(b);
		free(orig);
	}
}

/* ---- the Viewer window ----------------------------------------------------------------- */

static struct desk_image vimg;
static bool vimg_on;

static const char *v_line(void *ctx, enum desk_kind kind, int i)
{
	(void)ctx;
	if (kind != DESK_WIN_VIEWER)
		return NULL;
	if (i == DESK_LINE_TITLE)
		return "Viewer - sample.png";
	return i >= 0 && i < 3 ? "00000000  89 50 4e 47" : NULL;
}

static const char *v_tab(void *ctx, int i, bool *active)
{
	static const char *const tabs[] = { "Hex", "Image", "Info" };

	(void)ctx;
	*active = i == 1;
	return i >= 0 && i < 3 ? tabs[i] : NULL;
}

static bool v_image(void *ctx, struct desk_image *img)
{
	(void)ctx;
	*img = vimg;
	return vimg_on;
}

static void test_viewer_window(void)
{
	static const struct desk_host host = { NULL, v_line, NULL, v_tab, v_image };
	static struct desktop d;
	struct ft_image im;
	size_t n;
	u8 *b = load_fixture("home/samples/sample.png", &n);
	const struct desk_window *w;
	struct desk_rect c, t0, t2;
	struct desk_hit h;
	int id;

	CHECK_EQ(ft_image_decode(b, n, rgb, sizeof(rgb), scratch, sizeof(scratch), &im), 0);
	vimg = (struct desk_image){ im.rgb, (int)im.w, (int)im.h };
	desktop_layout(&d, DE_DOCK, 1024, 768, 640, 400);
	id = desktop_open(&d, DESK_WIN_VIEWER);
	CHECK(id != 0);
	w = desktop_window(&d, id);
	CHECK_STR(desktop_kind_name(DESK_WIN_VIEWER), "viewer");
	c = desktop_content_rect(&d, w);
	t0 = desktop_tab_rect(&d, w, 0);
	t2 = desktop_tab_rect(&d, w, 2);
	CHECK(t0.w > 0 && t2.x > t0.x && t0.y >= c.y);
	h = desktop_hit(&d, t2.x + 2, t2.y + 2);
	CHECK(h.part == DESK_PART_TAB && h.row == 2 && h.win == id);
	CHECK(desktop_viewer_rows(&d, w) >= 10);
	CHECK(desktop_tab_rect(&d, desktop_window(&d, 1), 0).w == 0);   /* terminal: none */

	/* Image mode: many fills, all inside the content area, the tabs drawn. */
	vimg_on = true;
	mock_bounds_reset();
	mock_fill_count = 0;
	mock_draw_len = 0;
	desktop_paint(&d, c, &host);
	CHECK(mock_fill_count > 64);
	CHECK(mock_x0 >= c.x && mock_y0 >= c.y && mock_x1 <= c.x + c.w && mock_y1 <= c.y + c.h);
	CHECK(strstr(mock_draw_log, "Image") && strstr(mock_draw_log, "Info"));
	/* A partial repaint stays inside its clip. */
	{
		const struct desk_rect part = { c.x + c.w / 3, c.y + c.h / 3, 37, 23 };

		mock_bounds_reset();
		desktop_paint(&d, part, &host);
		CHECK(mock_x0 >= part.x && mock_y0 >= part.y && mock_x1 <= part.x + part.w &&
		      mock_y1 <= part.y + part.h);
	}
	/* Text mode, and the title override. */
	vimg_on = false;
	mock_draw_len = 0;
	desktop_draw(&d, &host);
	CHECK(strstr(mock_draw_log, "00000000  89 50 4e 47") != NULL);
	CHECK(strstr(mock_draw_log, "Viewer - sample.png") != NULL);
	/* A huge image is shrunk into the window, a 1x1 one grown, never outside. */
	{
		static u8 one[3] = { 1, 2, 3 };

		vimg_on = true;
		vimg = (struct desk_image){ rgb, 4000, 10 };
		mock_bounds_reset();
		desktop_paint(&d, c, &host);
		CHECK(mock_x0 >= c.x && mock_x1 <= c.x + c.w && mock_y1 <= c.y + c.h);
		vimg = (struct desk_image){ one, 1, 1 };
		mock_bounds_reset();
		desktop_paint(&d, c, &host);
		CHECK(mock_x0 >= c.x && mock_x1 <= c.x + c.w);
	}
	CHECK(desktop_close(&d, id));
	free(b);
}

void test_filetype(void)
{
	test_identify_samples();
	test_identify_magic();
	test_images();
	test_views();
	test_fuzz();
	test_viewer_window();
}
