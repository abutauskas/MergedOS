/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_FILETYPE_H
#define MERGEDOS_FILETYPE_H

/*
 * File identification and viewers: "every file opens in something".
 *
 * ft_identify() names a file by its content first and its file name second
 * (executables are delegated to binfmt_probe(), packages to pkg_probe()).
 * The viewers turn a file into what a window shows: UTF-8 text lines, a hex
 * dump, an info card, an archive listing, or RGB pixels for BMP and PNG.
 *
 * Portable and allocation-free: everything works on caller-supplied
 * buffers, and every offset read from a file is bounds-checked, so the same
 * code runs in the kernel, in host tests and in tools. Formats that are only
 * identified (video, compressed audio, JPEG/GIF/WebP, office documents...)
 * say so on their info card instead of pretending to render.
 */
#include <mergedos/compat.h>

enum ft_kind {
	FT_UNKNOWN = 0,
	FT_EMPTY,
	/* text */
	FT_TEXT, FT_SCRIPT, FT_JSON, FT_XML, FT_HTML, FT_SVG, FT_MARKDOWN, FT_CSV,
	/* images */
	FT_PNG, FT_BMP, FT_JPEG, FT_GIF, FT_WEBP, FT_ICO,
	/* audio and video */
	FT_WAV, FT_MP3, FT_FLAC, FT_OGG, FT_MP4, FT_MOV, FT_MKV, FT_WEBM, FT_AVI,
	/* documents */
	FT_PDF, FT_DOCX, FT_XLSX, FT_PPTX, FT_EPUB, FT_ODF,
	/* archives and compressed files */
	FT_ZIP, FT_JAR, FT_APK_ANDROID, FT_TAR, FT_TARGZ, FT_GZIP, FT_XZ, FT_ZSTD, FT_BZIP2,
	FT_7Z, FT_RAR,
	/* programs and packages */
	FT_EXEC, FT_PACKAGE,
	/* fonts, data, disks */
	FT_TTF, FT_OTF, FT_WOFF, FT_WOFF2, FT_SQLITE, FT_PLIST_BIN, FT_ISO9660, FT_DISK_MBR,
	FT_DISK_GPT,
	FT_BINARY,
	FT_KIND_COUNT
};

enum ft_class {
	FT_C_NONE = 0, FT_C_TEXT, FT_C_IMAGE, FT_C_AUDIO, FT_C_VIDEO, FT_C_DOCUMENT,
	FT_C_ARCHIVE, FT_C_PROGRAM, FT_C_PACKAGE, FT_C_FONT, FT_C_DATA, FT_C_DISK,
	FT_C_COUNT
};

struct ft_type {
	enum ft_kind kind;
	enum ft_class cls;
	const char *name;               /* "PNG image" (static string)            */
	const char *mime;               /* "image/png" (static string)            */
	bool by_name;                   /* content not recognised: file name only */
	char detail[72];                /* "ELF64, x86-64, Linux ABI", "version 1.4" */
};

/* Identify a file. `name_hint` (file name or path, may be NULL) breaks ties. */
enum ft_kind ft_identify(const u8 *buf, size_t len, const char *name_hint, struct ft_type *t);
const char *ft_class_name(enum ft_class c);
/* True for kinds the viewers render (text, BMP, PNG, archive listings). */
bool ft_rendered(enum ft_kind k);

/* ---- facts read cheaply from headers (0 / "" = unknown) ---------------------- */
struct ft_meta {
	u32 width, height;              /* pixels                               */
	u32 depth;                      /* bits per pixel or per sample          */
	u32 channels, rate;             /* audio                                 */
	u64 duration_ms;
	u32 pages;                      /* PDF page objects (heuristic)          */
	u32 frames;
	char extra[56];                 /* "RGBA", "progressive", "volume FOO"   */
};
void ft_meta_read(const u8 *buf, size_t len, const struct ft_type *t, struct ft_meta *m);

/* ---- text and hex views --------------------------------------------------------- */
#define FT_COLS      96         /* longer text lines are cut ("...")           */
#define FT_LINE_MAX  (FT_COLS * 4 + 8)  /* worst case bytes of one UTF-8 line   */
#define FT_HEX_LINE  80         /* "00000000  89 50 ... 0a  |.PNG....|"        */

/* Line i of the file as sanitised UTF-8 (tabs expanded, controls and bad
 * bytes shown as '.'/'?'). Returns 1, or 0 past the last line. */
int ft_text_line(const u8 *buf, size_t len, int i, char *out, size_t cap);
/* Line i of the hex dump: offset, 16 bytes, ASCII. Returns 1, or 0 past the end. */
int ft_hex_line(const u8 *buf, size_t len, int i, char *out, size_t cap);
bool ft_is_text(const u8 *buf, size_t len);

/* ---- archive listing (zip, tar, tar.gz) ----------------------------------------- */
struct ft_entry {
	char name[128];
	u64 size, packed;               /* packed == size when stored            */
	u16 method;                     /* zip: 0 stored, 8 deflated, other      */
	bool dir;
};

struct ft_archive {
	enum ft_kind kind;              /* FT_UNKNOWN: nothing to list            */
	bool zip;
	const u8 *data;                 /* the zip, or the (decompressed) tar     */
	size_t len;
	bool partial;                   /* tar.gz larger than the scratch buffer  */
	bool central;                   /* zip: central directory found           */
	size_t cd_off;                  /* zip: where it starts                   */
	u32 count;                      /* zip: entries it declares               */
};

/* Prepare a listing; tar.gz is decompressed into `scratch` (a prefix if it
 * does not fit). Returns 0, or <0 when the file is not a listable archive. */
int ft_archive_open(struct ft_archive *a, const u8 *buf, size_t len, enum ft_kind kind,
		    u8 *scratch, size_t cap);
/* Entry i: 1, 0 past the last, <0 corrupt. */
int ft_archive_entry(const struct ft_archive *a, int i, struct ft_entry *e);
void ft_entry_format(const struct ft_archive *a, const struct ft_entry *e, char *out,
		     size_t cap);

/* ---- images ------------------------------------------------------------------------ */
struct ft_image {
	u32 w, h;
	u8 *rgb;                        /* 3 bytes per pixel, top row first       */
};

/*
 * Decode BMP (24/32-bit, uncompressed) or PNG (non-interlaced, any colour
 * type, 1-16 bit; alpha is blended over white) into `rgb`. PNG also needs
 * `scratch` for the inflated scanlines. Returns 0; -MERR_NOTSUP for formats
 * or variants without a decoder; -MERR_RANGE when the picture does not fit
 * the buffers (the size limit); -MERR_INVAL when the file is corrupt.
 */
int ft_image_decode(const u8 *buf, size_t len, u8 *rgb, size_t rgb_cap, u8 *scratch,
		    size_t scratch_cap, struct ft_image *img);
const char *ft_image_error(int rc);

/* ---- a file opened for viewing --------------------------------------------------- */
#define FT_VIEW_TEXT  0x1
#define FT_VIEW_HEX   0x2
#define FT_VIEW_IMAGE 0x4
#define FT_VIEW_INFO  0x8

struct ft_doc {
	const u8 *buf;
	size_t len;
	char name[64];                  /* last path component                    */
	struct ft_type type;
	struct ft_meta meta;
	struct ft_archive arc;
	unsigned views;                 /* FT_VIEW_* that apply                   */
	char note[64];                  /* host remark for the info card          */
};

void ft_doc_open(struct ft_doc *d, const u8 *buf, size_t len, const char *path, u8 *scratch,
		 size_t cap);
/* The view to show first: image, else text, else info, else hex. */
unsigned ft_doc_default_view(const struct ft_doc *d);
/* Line i of a text, hex or info view (info ends with the archive listing).
 * Returns 1, or 0 past the end. */
int ft_doc_line(const struct ft_doc *d, unsigned view, int i, char *out, size_t cap);

#endif /* MERGEDOS_FILETYPE_H */
