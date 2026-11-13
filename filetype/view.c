/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Viewers: text lines, hex dump, archive listing, header facts and the info
 * card. Each produces one line at a time into a caller buffer, so a window
 * can ask for exactly the lines it shows.
 */
#include "filetype/ft_internal.h"
#include "pkg_translate/pkg_internal.h"

/* ---- text ------------------------------------------------------------------------- */

static size_t line_start(const u8 *buf, size_t len, int i)
{
	size_t p = 0;

	for (int k = 0; k < i; k++) {
		const u8 *nl = memchr(buf + p, '\n', len - p);

		if (!nl)
			return (size_t)-1;
		p = (size_t)(nl - buf) + 1;
	}
	return p;
}

int ft_text_line(const u8 *buf, size_t len, int i, char *out, size_t cap)
{
	size_t p, e, o = 0;
	int col = 0;

	if (!cap)
		return 0;
	out[0] = '\0';
	if (!buf || i < 0 || cap < 16)
		return 0;
	p = line_start(buf, len, i);
	if (p == (size_t)-1 || p >= len)
		return 0;
	e = p;
	while (e < len && buf[e] != '\n')
		e++;
	if (e > p && buf[e - 1] == '\r')
		e--;
	while (p < e) {
		const u8 c = buf[p];
		int k;

		if (col >= FT_COLS || o + 8 >= cap) {
			memcpy(out + o, "...", 3);
			o += 3;
			break;
		}
		if (c == '\t') {
			do
				out[o++] = ' ';
			while (++col % 4 && col < FT_COLS && o + 8 < cap);
			p++;
			continue;
		}
		k = c < 0x20 || c == 0x7F ? 0 : ft_utf8_len(buf + p, e - p, false);
		if (k <= 1) {
			out[o++] = k ? (char)c : c < 0x80 ? '.' : '?';
			p++;
		} else {
			memcpy(out + o, buf + p, (size_t)k);
			o += (size_t)k;
			p += (size_t)k;
		}
		col++;
	}
	out[o] = '\0';
	return 1;
}

/* ---- hex ---------------------------------------------------------------------------- */

int ft_hex_line(const u8 *buf, size_t len, int i, char *out, size_t cap)
{
	static const char hex[] = "0123456789abcdef";
	char line[FT_HEX_LINE];
	size_t off, o = 0;

	if (cap)
		out[0] = '\0';
	if (!buf || i < 0)
		return 0;
	off = (size_t)i * 16;
	if (off >= len || off / 16 != (size_t)i)
		return 0;
	for (int s = 28; s >= 0; s -= 4)
		line[o++] = hex[(off >> s) & 0xF];
	line[o++] = ' ';
	for (size_t k = 0; k < 16; k++) {
		if (k == 8)
			line[o++] = ' ';
		line[o++] = ' ';
		if (off + k < len) {
			line[o++] = hex[buf[off + k] >> 4];
			line[o++] = hex[buf[off + k] & 0xF];
		} else {
			line[o++] = ' ';
			line[o++] = ' ';
		}
	}
	line[o++] = ' ';
	line[o++] = ' ';
	line[o++] = '|';
	for (size_t k = 0; k < 16 && off + k < len; k++) {
		const u8 c = buf[off + k];

		line[o++] = c >= 0x20 && c < 0x7F ? (char)c : '.';
	}
	line[o++] = '|';
	line[o] = '\0';
	mos_strlcpy(out, line, cap);
	return 1;
}

/* ---- archives ------------------------------------------------------------------------- */

int ft_archive_open(struct ft_archive *a, const u8 *buf, size_t len, enum ft_kind kind,
		    u8 *scratch, size_t cap)
{
	memset(a, 0, sizeof(*a));
	if (!buf)
		return -MERR_INVAL;
	switch (kind) {
	case FT_ZIP: case FT_JAR: case FT_APK_ANDROID: case FT_DOCX: case FT_XLSX: case FT_PPTX:
	case FT_EPUB: case FT_ODF: {
		const size_t lo = len > 65557 ? len - 65557 : 0;

		a->zip = true;
		a->data = buf;
		a->len = len;
		/* End of central directory: last 22..65557 bytes (it may carry a comment). */
		for (size_t i = len >= 22 ? len - 22 : 0; len >= 22 && i + 1 > lo; i--) {
			if (!memcmp(buf + i, "PK\5\6", 4)) {
				const u32 size = rd32le(buf + i + 12), off = rd32le(buf + i + 16);

				if (mos_in_range(i, off, size)) {
					a->central = true;
					a->cd_off = off;
					a->count = rd16le(buf + i + 10);
				}
				break;
			}
			if (!i)
				break;
		}
		if (!a->central && !(len >= 4 && !memcmp(buf, "PK\3\4", 4)))
			return -MERR_INVAL;
		a->kind = kind;
		return 0;
	}
	case FT_TAR:
		if (!tar_probe(buf, len))
			return -MERR_INVAL;
		a->data = buf;
		a->len = len;
		a->kind = kind;
		return 0;
	case FT_TARGZ: {
		size_t used, got = 0;
		int rc;

		if (!scratch || !cap)
			return -MERR_RANGE;
		rc = gz_decode(buf, len, scratch, cap, &used, &got);
		if (rc < 0 || !tar_probe(scratch, got))
			return -MERR_INVAL;
		a->data = scratch;
		a->len = got;
		a->partial = rc == 1;
		a->kind = kind;
		return 0;
	}
	default:
		return -MERR_INVAL;
	}
}

static void entry_name(struct ft_entry *e, const u8 *p, size_t n)
{
	mos_fieldcpy(e->name, sizeof(e->name), (const char *)p, n);
	for (char *c = e->name; *c; c++)
		if ((u8)*c < 0x20 || (u8)*c == 0x7F)
			*c = '?';
	e->dir = n && p[n - 1] == '/';
}

static int zip_entry(const struct ft_archive *a, int i, struct ft_entry *e)
{
	const u8 *buf = a->data;
	size_t pos = a->central ? a->cd_off : 0;

	if (a->central && (u32)i >= a->count)
		return 0;
	for (int k = 0;; k++) {
		size_t n, x, c, comp;

		if (a->central) {
			if (!mos_in_range(a->len, pos, 46) || memcmp(buf + pos, "PK\1\2", 4))
				return -MERR_INVAL;
			n = rd16le(buf + pos + 28);
			x = rd16le(buf + pos + 30);
			c = rd16le(buf + pos + 32);
			if (!mos_in_range(a->len, pos + 46, n))
				return -MERR_INVAL;
			if (k == i) {
				e->method = rd16le(buf + pos + 10);
				e->packed = rd32le(buf + pos + 20);
				e->size = rd32le(buf + pos + 24);
				entry_name(e, buf + pos + 46, n);
				return 1;
			}
			pos += 46 + n + x + c;
			continue;
		}
		/* No central directory (truncated file): walk the local headers. */
		if (!mos_in_range(a->len, pos, 30) || memcmp(buf + pos, "PK\3\4", 4))
			return 0;
		n = rd16le(buf + pos + 26);
		x = rd16le(buf + pos + 28);
		comp = rd32le(buf + pos + 18);
		if (!mos_in_range(a->len, pos + 30, n))
			return 0;
		if (k == i) {
			e->method = rd16le(buf + pos + 8);
			e->packed = comp;
			e->size = rd32le(buf + pos + 22);
			entry_name(e, buf + pos + 30, n);
			return 1;
		}
		if ((rd16le(buf + pos + 6) & 0x08) && !comp)
			return 0;               /* sizes only in a trailing descriptor */
		if (!mos_in_range(a->len, pos + 30 + n + x, comp))
			return 0;
		pos += 30 + n + x + comp;
	}
}

int ft_archive_entry(const struct ft_archive *a, int i, struct ft_entry *e)
{
	struct tar_entry te;
	size_t off = 0;
	int rc;

	memset(e, 0, sizeof(*e));
	if (!a->kind || i < 0)
		return 0;
	if (a->zip)
		return zip_entry(a, i, e);
	for (int k = 0; (rc = tar_next(a->data, a->len, &off, &te)) == 1; k++)
		if (k == i) {
			mos_strlcpy(e->name, te.name, sizeof(e->name));
			e->size = e->packed = te.type == '0' || te.type == '\0' ? te.size : 0;
			e->dir = te.type == '5';
			if (e->dir && !mos_has_suffix(e->name, "/"))
				mos_strlcat(e->name, "/", sizeof(e->name));
			return 1;
		}
	return rc < 0 ? rc : 0;
}

void ft_entry_format(const struct ft_archive *a, const struct ft_entry *e, char *out, size_t cap)
{
	char method[16];

	if (!a->zip) {
		mos_snprintf(out, cap, "%10llu  %s", (unsigned long long)e->size, e->name);
		return;
	}
	if (e->method == 0)
		mos_strlcpy(method, "stored", sizeof(method));
	else if (e->method == 8)
		mos_strlcpy(method, "deflated", sizeof(method));
	else
		mos_snprintf(method, sizeof(method), "method %u", (unsigned)e->method);
	mos_snprintf(out, cap, "%10llu %10llu  %-9s %s", (unsigned long long)e->size,
		     (unsigned long long)e->packed, method, e->name);
}

/* ---- header facts ------------------------------------------------------------------------ */

static u16 rd16be(const u8 *p)
{
	return (u16)(p[0] << 8 | p[1]);
}

static void png_meta(const u8 *b, size_t len, struct ft_meta *m)
{
	static const char *const ct[7] = { "greyscale", "", "RGB", "palette", "grey + alpha", "",
					   "RGBA" };

	if (len < 33 || memcmp(b + 12, "IHDR", 4))
		return;
	m->width = rd32be(b + 16);
	m->height = rd32be(b + 20);
	m->depth = b[24];
	mos_snprintf(m->extra, sizeof(m->extra), "%s%s", b[25] < 7 ? ct[b[25]] : "?",
		     b[28] ? ", interlaced" : "");
}

static void jpeg_meta(const u8 *b, size_t len, struct ft_meta *m)
{
	size_t p = 2;

	while (p + 4 <= len) {
		u8 mk;

		if (b[p] != 0xFF)
			return;
		mk = b[p + 1];
		if (mk == 0xFF) {
			p++;
			continue;
		}
		if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7)) {
			p += 2;
			continue;
		}
		if (mk == 0xDA || mk == 0xD9)
			return;
		if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
			if (p + 10 > len)
				return;
			m->depth = b[p + 4];
			m->height = rd16be(b + p + 5);
			m->width = rd16be(b + p + 7);
			m->channels = b[p + 9];
			mos_strlcpy(m->extra, mk == 0xC0 || mk == 0xC1 ? "baseline" :
				    mk == 0xC2 ? "progressive" : "lossless/arithmetic", sizeof(m->extra));
			return;
		}
		p += 2 + rd16be(b + p + 2);
	}
}

static void webp_meta(const u8 *b, size_t len, struct ft_meta *m)
{
	if (len >= 30 && !memcmp(b + 12, "VP8 ", 4) && b[23] == 0x9D && b[24] == 0x01 &&
	    b[25] == 0x2A) {
		m->width = rd16le(b + 26) & 0x3FFF;
		m->height = rd16le(b + 28) & 0x3FFF;
		mos_strlcpy(m->extra, "lossy", sizeof(m->extra));
	} else if (len >= 25 && !memcmp(b + 12, "VP8L", 4) && b[20] == 0x2F) {
		const u32 v = rd32le(b + 21);

		m->width = (v & 0x3FFF) + 1;
		m->height = ((v >> 14) & 0x3FFF) + 1;
		mos_strlcpy(m->extra, "lossless", sizeof(m->extra));
	} else if (len >= 30 && !memcmp(b + 12, "VP8X", 4)) {
		m->width = (rd32le(b + 24) & 0xFFFFFF) + 1;
		m->height = (rd32le(b + 27) & 0xFFFFFF) + 1;
		mos_strlcpy(m->extra, "extended", sizeof(m->extra));
	}
}

static void wav_meta(const u8 *b, size_t len, struct ft_meta *m)
{
	size_t p = 12;
	u32 bytes_per_s = 0;

	while (mos_in_range(len, p, 8)) {
		const u32 n = rd32le(b + p + 4);

		if (!memcmp(b + p, "fmt ", 4) && n >= 16 && mos_in_range(len, p + 8, 16)) {
			const u16 fmt = rd16le(b + p + 8);

			m->channels = rd16le(b + p + 10);
			m->rate = rd32le(b + p + 12);
			bytes_per_s = rd32le(b + p + 16);
			m->depth = rd16le(b + p + 22);
			mos_strlcpy(m->extra, fmt == 1 ? "PCM" : fmt == 3 ? "float" :
				    fmt == 0xFFFE ? "extensible" : "compressed", sizeof(m->extra));
		} else if (!memcmp(b + p, "data", 4)) {
			if (bytes_per_s)
				m->duration_ms = (u64)n * 1000 / bytes_per_s;
			return;
		}
		if (n > len - p - 8)
			return;
		p += 8 + (size_t)n + (n & 1);
	}
}

static void flac_meta(const u8 *b, size_t len, struct ft_meta *m)
{
	u64 total;

	if (len < 26 || (b[4] & 0x7F) != 0)
		return;
	m->rate = (u32)b[18] << 12 | (u32)b[19] << 4 | b[20] >> 4;
	m->channels = ((b[20] >> 1) & 7) + 1;
	m->depth = (((b[20] & 1) << 4) | (b[21] >> 4)) + 1;
	total = (u64)(b[21] & 0xF) << 32 | rd32be(b + 22);
	if (m->rate)
		m->duration_ms = total * 1000 / m->rate;
}

static void mp3_meta(const u8 *b, size_t len, struct ft_meta *m)
{
	static const u16 br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256,
				     320, 0 };
	static const u32 rates[3] = { 44100, 48000, 32000 };
	size_t p = 0;
	u32 kbps;

	if (len >= 10 && !memcmp(b, "ID3", 3))
		p = 10 + ((size_t)(b[6] & 0x7F) << 21 | (size_t)(b[7] & 0x7F) << 14 |
			  (size_t)(b[8] & 0x7F) << 7 | (b[9] & 0x7F));
	if (!mos_in_range(len, p, 4) || b[p] != 0xFF || (b[p + 1] & 0xE0) != 0xE0)
		return;
	if (((b[p + 1] >> 3) & 3) != 3 || ((b[p + 1] >> 1) & 3) != 1 || ((b[p + 2] >> 2) & 3) == 3)
		return;                         /* only MPEG-1 Layer III is summarised */
	kbps = br1[b[p + 2] >> 4];
	m->rate = rates[(b[p + 2] >> 2) & 3];
	m->channels = (b[p + 3] >> 6) == 3 ? 1 : 2;
	if (kbps) {
		m->duration_ms = (u64)(len - p) * 8 / kbps;
		mos_snprintf(m->extra, sizeof(m->extra), "MPEG-1 Layer III, %u kbit/s (CBR estimate)",
			     (unsigned)kbps);
	}
}

static void pdf_meta(const u8 *b, size_t len, struct ft_meta *m)
{
	/* Page objects: "/Type /Page" (any spacing) not followed by "s". */
	for (size_t i = 0; i + 10 <= len; i++) {
		size_t k;

		if (b[i] != '/' || memcmp(b + i, "/Type", 5))
			continue;
		k = i + 5;
		while (k < len && (b[k] == ' ' || b[k] == '\r' || b[k] == '\n'))
			k++;
		if (k + 5 <= len && !memcmp(b + k, "/Page", 5) && (k + 5 == len || b[k + 5] != 's'))
			m->pages++;
	}
}

static void avi_meta(const u8 *b, size_t len, struct ft_meta *m)
{
	if (len < 72 || memcmp(b + 24, "avih", 4))
		return;
	m->frames = rd32le(b + 48);
	m->width = rd32le(b + 64);
	m->height = rd32le(b + 68);
	if (rd32le(b + 32))
		m->duration_ms = (u64)m->frames * rd32le(b + 32) / 1000;
}

static void trimmed(char *out, size_t cap, const u8 *p, size_t n)
{
	while (n && (p[n - 1] == ' ' || !p[n - 1]))
		n--;
	mos_fieldcpy(out, cap, (const char *)p, n);
	for (char *c = out; *c; c++)
		if ((u8)*c < 0x20 || (u8)*c >= 0x7F)
			*c = '?';
}

void ft_meta_read(const u8 *b, size_t len, const struct ft_type *t, struct ft_meta *m)
{
	memset(m, 0, sizeof(*m));
	if (!b)
		return;
	switch (t->by_name ? FT_UNKNOWN : t->kind) {
	case FT_PNG:
		png_meta(b, len, m);
		break;
	case FT_BMP:
		if (len >= 34 && rd32le(b + 14) >= 40) {
			const u32 h = rd32le(b + 22);

			m->width = rd32le(b + 18);
			m->height = (s32)h < 0 ? 0u - h : h;    /* negative: top-down rows */
			m->depth = rd16le(b + 28);
			mos_strlcpy(m->extra, rd32le(b + 30) == 0 ? "uncompressed" :
				    rd32le(b + 30) == 3 ? "bitfields" : "compressed", sizeof(m->extra));
		} else if (len >= 26) {
			m->width = rd16le(b + 18);
			m->height = rd16le(b + 20);
		}
		break;
	case FT_JPEG:
		jpeg_meta(b, len, m);
		break;
	case FT_GIF:
		if (len >= 10) {
			m->width = rd16le(b + 6);
			m->height = rd16le(b + 8);
			mos_fieldcpy(m->extra, sizeof(m->extra), (const char *)b, 6);
		}
		break;
	case FT_WEBP:
		webp_meta(b, len, m);
		break;
	case FT_ICO:
		if (len >= 22) {
			m->frames = rd16le(b + 4);
			m->width = b[6] ? b[6] : 256;
			m->height = b[7] ? b[7] : 256;
		}
		break;
	case FT_WAV:
		wav_meta(b, len, m);
		break;
	case FT_FLAC:
		flac_meta(b, len, m);
		break;
	case FT_MP3:
		mp3_meta(b, len, m);
		break;
	case FT_AVI:
		avi_meta(b, len, m);
		break;
	case FT_PDF:
		pdf_meta(b, len, m);
		break;
	case FT_SQLITE:
		if (len >= 32)
			mos_snprintf(m->extra, sizeof(m->extra), "%u pages of %u bytes",
				     (unsigned)rd32be(b + 28),
				     rd16be(b + 16) == 1 ? 65536u : (unsigned)rd16be(b + 16));
		break;
	case FT_ISO9660:
		if (len >= 0x8054) {
			char vol[40];

			trimmed(vol, sizeof(vol), b + 0x8028, 32);
			mos_snprintf(m->extra, sizeof(m->extra), "volume \"%s\", %u KiB", vol,
				     (unsigned)(rd32le(b + 0x8050) * 2u));
		}
		break;
	case FT_DISK_MBR: {
		unsigned n = 0;

		for (int i = 0; i < 4; i++)
			n += b[446 + i * 16 + 4] != 0;
		mos_snprintf(m->extra, sizeof(m->extra), "%u primary partition%s", n,
			     n == 1 ? "" : "s");
		break;
	}
	case FT_DISK_GPT:
		if (len >= 512 + 84)
			mos_snprintf(m->extra, sizeof(m->extra), "%u partition entry slots",
				     (unsigned)rd32le(b + 512 + 80));
		break;
	case FT_TTF: case FT_OTF:
		if (len >= 6 && memcmp(b, "ttcf", 4))
			mos_snprintf(m->extra, sizeof(m->extra), "%u tables", (unsigned)rd16be(b + 4));
		break;
	default:
		break;
	}
}

/* ---- the info card ---------------------------------------------------------------------- */

static const char *viewer_note(const struct ft_type *t)
{
	if (ft_rendered(t->kind))
		return t->kind == FT_SVG ? "shown as text (no vector renderer)" :
		       t->cls == FT_C_TEXT ? "shown as text" :
		       t->cls == FT_C_IMAGE ? "decoded and drawn" : "entries listed";
	switch (t->cls) {
	case FT_C_AUDIO: return "identified only: no audio decoder or sound driver";
	case FT_C_VIDEO: return "identified only: no video decoder";
	case FT_C_IMAGE: return "identified only: no decoder for this image format";
	case FT_C_DOCUMENT: return "identified only: document not rendered";
	case FT_C_PROGRAM: return "run it from the shell (run <path>)";
	case FT_C_PACKAGE: return t->kind == FT_APK_ANDROID ? "identified only (Android)" :
				  "mpkg inspect|run <path>";
	case FT_C_NONE: return "nothing to show";
	default: return "identified only: shown as hex";
	}
}

static void duration(char *out, size_t cap, u64 ms)
{
	mos_snprintf(out, cap, "%llu.%03llu s", (unsigned long long)(ms / 1000),
		     (unsigned long long)(ms % 1000));
}

/* The card's facts, in order; returns false past the last one. */
static bool info_fact(const struct ft_doc *d, int i, char *out, size_t cap)
{
	const struct ft_type *t = &d->type;
	const struct ft_meta *m = &d->meta;
	char tmp[40];

	switch (i) {
	case 0: mos_snprintf(out, cap, "Name     %s", d->name[0] ? d->name : "-"); return true;
	case 1: mos_snprintf(out, cap, "Type     %s", t->name); return true;
	case 2: mos_snprintf(out, cap, "MIME     %s", t->mime); return true;
	case 3:
		if (d->len >= 10240)
			mos_snprintf(out, cap, "Size     %llu bytes (%llu KiB)",
				     (unsigned long long)d->len, (unsigned long long)(d->len / 1024));
		else
			mos_snprintf(out, cap, "Size     %llu bytes", (unsigned long long)d->len);
		return true;
	case 4: mos_snprintf(out, cap, "Class    %s", ft_class_name(t->cls)); return true;
	case 5:
		if (t->detail[0])
			mos_snprintf(out, cap, "%-8s %s", t->cls == FT_C_PROGRAM ? "Program" :
				     t->cls == FT_C_PACKAGE ? "Package" : "Detail", t->detail);
		return true;
	case 6:
		if (m->width && m->height) {
			mos_snprintf(out, cap, "%-8s %u x %u", t->cls == FT_C_VIDEO ? "Video" : "Image",
				     (unsigned)m->width, (unsigned)m->height);
			if (m->depth && t->cls == FT_C_IMAGE) {
				mos_snprintf(tmp, sizeof(tmp), ", %u-bit", (unsigned)m->depth);
				mos_strlcat(out, tmp, cap);
			}
		}
		return true;
	case 7:
		if (m->rate || m->channels) {
			mos_snprintf(out, cap, "Audio    %u ch, %u Hz", (unsigned)m->channels,
				     (unsigned)m->rate);
			if (m->depth && t->cls == FT_C_AUDIO) {
				mos_snprintf(tmp, sizeof(tmp), ", %u-bit", (unsigned)m->depth);
				mos_strlcat(out, tmp, cap);
			}
		}
		return true;
	case 8:
		if (m->duration_ms) {
			duration(tmp, sizeof(tmp), m->duration_ms);
			mos_snprintf(out, cap, "Length   %s", tmp);
		}
		return true;
	case 9:
		if (t->kind == FT_PDF)
			mos_snprintf(out, cap, "Pages    %u (page objects counted, heuristic)",
				     (unsigned)m->pages);
		else if (m->frames)
			mos_snprintf(out, cap, "%-8s %u", t->kind == FT_ICO ? "Icons" : "Frames",
				     (unsigned)m->frames);
		return true;
	case 10:
		if (m->extra[0])
			mos_snprintf(out, cap, "Format   %s", m->extra);
		return true;
	case 11:
		mos_snprintf(out, cap, "Viewer   %s", viewer_note(t));
		return true;
	case 12:
		if (d->note[0])
			mos_snprintf(out, cap, "Note     %s", d->note);
		return true;
	default:
		return false;
	}
}

static int info_line(const struct ft_doc *d, int i, char *out, size_t cap)
{
	struct ft_entry e;
	int n = 0, f;

	/* Facts that produced nothing are skipped. */
	for (f = 0;; f++) {
		out[0] = '\0';
		if (!info_fact(d, f, out, cap))
			break;
		if (out[0] && n++ == i)
			return 1;
	}
	if (!d->arc.kind)
		return 0;
	i -= n;
	if (i == 0) {
		mos_snprintf(out, cap, "Contents%s:", d->arc.partial ? " (first part only)" : "");
		return 1;
	}
	if (i == 1) {
		mos_strlcpy(out, d->arc.zip ? "      size     packed  method    name" :
			    "      size  name", cap);
		return 1;
	}
	if (ft_archive_entry(&d->arc, i - 2, &e) != 1)
		return 0;
	ft_entry_format(&d->arc, &e, out, cap);
	return 1;
}

/* ---- documents ------------------------------------------------------------------------------ */

void ft_doc_open(struct ft_doc *d, const u8 *buf, size_t len, const char *path, u8 *scratch,
		 size_t cap)
{
	memset(d, 0, sizeof(*d));
	d->buf = buf;
	d->len = buf ? len : 0;
	if (path)
		mos_strlcpy(d->name, ft_base_name(path), sizeof(d->name));
	ft_identify(d->buf, d->len, path, &d->type);
	ft_meta_read(d->buf, d->len, &d->type, &d->meta);
	if (!d->type.by_name)
		(void)ft_archive_open(&d->arc, d->buf, d->len, d->type.kind, scratch, cap);
	d->views = FT_VIEW_INFO;
	if (d->len)
		d->views |= FT_VIEW_HEX;
	if (d->type.cls == FT_C_TEXT || d->type.kind == FT_SVG)
		d->views |= FT_VIEW_TEXT;
	if ((d->type.kind == FT_PNG || d->type.kind == FT_BMP) && !d->type.by_name)
		d->views |= FT_VIEW_IMAGE;
}

unsigned ft_doc_default_view(const struct ft_doc *d)
{
	if (d->views & FT_VIEW_IMAGE)
		return FT_VIEW_IMAGE;
	if (d->views & FT_VIEW_TEXT)
		return FT_VIEW_TEXT;
	if (d->type.kind == FT_BINARY || !d->len)
		return d->len ? FT_VIEW_HEX : FT_VIEW_INFO;
	return FT_VIEW_INFO;
}

int ft_doc_line(const struct ft_doc *d, unsigned view, int i, char *out, size_t cap)
{
	if (!cap)
		return 0;
	out[0] = '\0';
	if (i < 0)
		return 0;
	switch (view) {
	case FT_VIEW_TEXT:
		return ft_text_line(d->buf, d->len, i, out, cap);
	case FT_VIEW_HEX:
		return ft_hex_line(d->buf, d->len, i, out, cap);
	case FT_VIEW_INFO:
		return info_line(d, i, out, cap);
	default:
		return 0;
	}
}
