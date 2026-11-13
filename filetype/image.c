/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Image decoders for the viewer: BMP (24/32-bit, uncompressed) and PNG
 * (non-interlaced, every colour type, 1-16 bit; zlib through the shared
 * inflate code, every chunk CRC checked). Output is packed RGB in a buffer
 * the caller owns, so the size limit is simply its capacity.
 */
#include "filetype/filetype.h"
#include "pkg_translate/pkg_internal.h"

#define IMG_MAX_SIDE 16384u

const char *ft_image_error(int rc)
{
	switch (rc) {
	case 0:            return "ok";
	case -MERR_NOTSUP: return "no decoder for this format or variant";
	case -MERR_RANGE:  return "too large for the viewer's image buffer";
	default:           return "corrupt or truncated image";
	}
}

static bool fits(u32 w, u32 h, size_t rgb_cap)
{
	return w && h && w <= IMG_MAX_SIDE && h <= IMG_MAX_SIDE && (u64)w * h * 3 <= rgb_cap;
}

/* ---- BMP --------------------------------------------------------------------------- */

static int bmp_decode(const u8 *buf, size_t len, u8 *rgb, size_t cap, struct ft_image *img)
{
	u32 off, dib, bpp, comp, w, h;
	s32 sh;
	u64 stride;

	if (len < 54 || buf[0] != 'B' || buf[1] != 'M')
		return -MERR_INVAL;
	off = rd32le(buf + 10);
	dib = rd32le(buf + 14);
	if (dib < 40)
		return -MERR_NOTSUP;            /* OS/2 core header */
	if (dib > len - 14)
		return -MERR_INVAL;
	sh = (s32)rd32le(buf + 22);
	bpp = rd16le(buf + 28);
	comp = rd32le(buf + 30);
	if ((s32)rd32le(buf + 18) <= 0 || sh == 0 || sh == (s32)0x80000000 ||
	    rd16le(buf + 26) != 1)
		return -MERR_INVAL;
	w = rd32le(buf + 18);
	h = sh < 0 ? (u32)-sh : (u32)sh;
	if (bpp != 24 && bpp != 32)
		return -MERR_NOTSUP;
	if (comp == 3 && bpp == 32) {           /* BI_BITFIELDS: only the usual layout */
		const size_t mo = dib >= 52 ? 14 + 40 : 14 + dib;

		if (!mos_in_range(len, mo, 12) || rd32le(buf + mo) != 0xFF0000u ||
		    rd32le(buf + mo + 4) != 0xFF00u || rd32le(buf + mo + 8) != 0xFFu)
			return -MERR_NOTSUP;
	} else if (comp != 0) {
		return -MERR_NOTSUP;            /* RLE, JPEG/PNG inside BMP */
	}
	if (!fits(w, h, cap))
		return w && h ? -MERR_RANGE : -MERR_INVAL;
	stride = ((u64)w * bpp + 31) / 32 * 4;
	if (!mos_in_range(len, off, stride * h))
		return -MERR_INVAL;
	for (u32 y = 0; y < h; y++) {
		const u8 *src = buf + off + stride * (sh < 0 ? y : h - 1 - y);
		u8 *dst = rgb + (size_t)y * w * 3;

		for (u32 x = 0; x < w; x++, src += bpp / 8, dst += 3) {
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
		}
	}
	img->w = w;
	img->h = h;
	img->rgb = rgb;
	return 0;
}

/* ---- PNG --------------------------------------------------------------------------- */

struct png {
	u32 w, h;
	u8 depth, ctype;
	u8 (*pal)[4];                   /* 256 entries, kept in the scratch tail */
	u32 npal;
	const u8 *idat;                 /* first IDAT's data                    */
	size_t idat_len;                /* total over all IDAT chunks           */
	int nidat;
};

static u8 paeth(u8 a, u8 b, u8 c)
{
	const int p = a + b - c;
	const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;

	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static int unfilter(u8 *raw, u32 h, size_t rowbytes, size_t bpp)
{
	for (u32 y = 0; y < h; y++) {
		u8 *row = raw + (size_t)y * (rowbytes + 1);
		u8 *cur = row + 1;
		const u8 *prev = y ? row - rowbytes : NULL;     /* previous row's pixels */

		switch (row[0]) {
		case 0:
			break;
		case 1:
			for (size_t i = bpp; i < rowbytes; i++)
				cur[i] = (u8)(cur[i] + cur[i - bpp]);
			break;
		case 2:
			for (size_t i = 0; prev && i < rowbytes; i++)
				cur[i] = (u8)(cur[i] + prev[i]);
			break;
		case 3:
			for (size_t i = 0; i < rowbytes; i++)
				cur[i] = (u8)(cur[i] + (((i >= bpp ? cur[i - bpp] : 0) +
							 (prev ? prev[i] : 0)) >> 1));
			break;
		case 4:
			for (size_t i = 0; i < rowbytes; i++)
				cur[i] = (u8)(cur[i] + paeth(i >= bpp ? cur[i - bpp] : 0,
							      prev ? prev[i] : 0,
							      i >= bpp && prev ? prev[i - bpp] : 0));
			break;
		default:
			return -MERR_INVAL;
		}
	}
	return 0;
}

/* Sample `c` of pixel x in a row, scaled to 0..255. */
static u8 sample(const u8 *row, u32 x, u32 c, u32 ch, u32 depth, bool scale)
{
	const u32 idx = x * ch + c;

	if (depth == 8)
		return row[idx];
	if (depth == 16)
		return row[idx * 2];            /* high byte */
	{
		const u32 bit = idx * depth, max = (1u << depth) - 1;
		const u32 v = (u32)(row[bit / 8] >> (8 - depth - bit % 8)) & max;

		return (u8)(scale ? v * 255 / max : v);
	}
}

static u8 over_white(u8 c, u8 a)
{
	return (u8)((c * a + 255 * (255 - a) + 127) / 255);
}

static int png_chunks(const u8 *buf, size_t len, struct png *p)
{
	size_t pos = 8;
	bool ihdr = false, plte = false;

	for (;;) {
		u32 n;
		const u8 *type, *data;

		if (!mos_in_range(len, pos, 12))
			return -MERR_INVAL;             /* truncated before IEND */
		n = rd32be(buf + pos);
		if (n > 0x7FFFFFFFu || !mos_in_range(len, pos + 12, n))
			return -MERR_INVAL;
		type = buf + pos + 4;
		data = buf + pos + 8;
		if (rd32be(data + n) != pkg_crc32(0, type, (size_t)n + 4))
			return -MERR_INVAL;
		if (!ihdr && memcmp(type, "IHDR", 4))
			return -MERR_INVAL;
		if (!memcmp(type, "IHDR", 4)) {
			if (ihdr || n != 13 || data[10] || data[11])
				return -MERR_INVAL;
			p->w = rd32be(data);
			p->h = rd32be(data + 4);
			p->depth = data[8];
			p->ctype = data[9];
			if (data[12])
				return -MERR_NOTSUP;    /* Adam7 interlacing */
			ihdr = true;
		} else if (!memcmp(type, "PLTE", 4)) {
			if (plte || n % 3 || n / 3 > 256 || !n)
				return -MERR_INVAL;
			p->npal = n / 3;
			for (u32 i = 0; i < p->npal; i++) {
				p->pal[i][0] = data[i * 3];
				p->pal[i][1] = data[i * 3 + 1];
				p->pal[i][2] = data[i * 3 + 2];
				p->pal[i][3] = 255;
			}
			plte = true;
		} else if (!memcmp(type, "tRNS", 4)) {
			for (u32 i = 0; p->ctype == 3 && i < n && i < p->npal; i++)
				p->pal[i][3] = data[i];
		} else if (!memcmp(type, "IDAT", 4)) {
			if (!p->nidat++)
				p->idat = data;
			p->idat_len += n;
		} else if (!memcmp(type, "IEND", 4)) {
			return 0;
		} else if (!(type[0] & 0x20)) {
			return -MERR_NOTSUP;            /* unknown critical chunk */
		}
		pos += 12 + (size_t)n;
	}
}

static int png_decode(const u8 *buf, size_t len, u8 *rgb, size_t cap, u8 *scratch,
		      size_t scap, struct ft_image *img)
{
	static const u8 chans[7] = { 1, 0, 3, 1, 2, 0, 4 };
	struct png p;
	u32 ch;
	size_t rowbytes, bpp, used, got;
	u64 raw;
	const u8 *z;
	int rc;

	memset(&p, 0, sizeof(p));
	if (len < 8 || memcmp(buf, "\x89PNG\r\n\x1a\n", 8))
		return -MERR_INVAL;
	if (!scratch || scap < 1024 + 64)
		return -MERR_RANGE;
	scap -= 1024;                   /* the palette lives in the last KiB */
	p.pal = (u8 (*)[4])(scratch + scap);
	rc = png_chunks(buf, len, &p);
	if (rc)
		return rc;
	ch = p.ctype < 7 ? chans[p.ctype] : 0;
	if (!ch)
		return -MERR_INVAL;
	switch (p.depth) {
	case 1: case 2: case 4:
		if (p.ctype != 0 && p.ctype != 3)
			return -MERR_INVAL;
		break;
	case 8:
		break;
	case 16:
		if (p.ctype == 3)
			return -MERR_INVAL;
		break;
	default:
		return -MERR_INVAL;
	}
	if ((p.ctype == 3 && !p.npal) || !p.nidat)
		return -MERR_INVAL;
	if (!p.w || !p.h)
		return -MERR_INVAL;
	if (!fits(p.w, p.h, cap))
		return -MERR_RANGE;
	rowbytes = ((size_t)p.w * ch * p.depth + 7) / 8;
	bpp = MOS_MAX((size_t)1, (size_t)ch * p.depth / 8);
	raw = (u64)p.h * (rowbytes + 1);
	if (raw > scap)
		return -MERR_RANGE;
	z = p.idat;
	if (p.nidat > 1) {
		/* Several IDAT chunks: join their data behind the scanline area. */
		size_t pos = 8, at = (size_t)raw;

		if (p.idat_len > scap - (size_t)raw)
			return -MERR_RANGE;
		while (mos_in_range(len, pos, 12)) {
			const u32 n = rd32be(buf + pos);

			if (!mos_in_range(len, pos + 12, n))
				break;
			if (!memcmp(buf + pos + 4, "IDAT", 4)) {
				memcpy(scratch + at, buf + pos + 8, n);
				at += n;
			}
			if (!memcmp(buf + pos + 4, "IEND", 4))
				break;
			pos += 12 + (size_t)n;
		}
		z = scratch + (size_t)raw;
	}
	got = 0;
	rc = zlib_decode(z, p.idat_len, scratch, (size_t)raw, &used, &got);
	if (rc < 0 || got != raw)
		return -MERR_INVAL;
	if (unfilter(scratch, p.h, rowbytes, bpp))
		return -MERR_INVAL;
	for (u32 y = 0; y < p.h; y++) {
		const u8 *row = scratch + (size_t)y * (rowbytes + 1) + 1;
		u8 *dst = rgb + (size_t)y * p.w * 3;

		for (u32 x = 0; x < p.w; x++, dst += 3) {
			u8 r, g, b, a = 255;

			switch (p.ctype) {
			case 0: case 4:
				r = g = b = sample(row, x, 0, ch, p.depth, true);
				if (p.ctype == 4)
					a = sample(row, x, 1, ch, p.depth, true);
				break;
			case 3: {
				const u8 i = sample(row, x, 0, 1, p.depth, false);

				if (i < p.npal) {
					r = p.pal[i][0];
					g = p.pal[i][1];
					b = p.pal[i][2];
					a = p.pal[i][3];
				} else {
					r = g = b = 0;
				}
				break;
			}
			default:
				r = sample(row, x, 0, ch, p.depth, true);
				g = sample(row, x, 1, ch, p.depth, true);
				b = sample(row, x, 2, ch, p.depth, true);
				if (p.ctype == 6)
					a = sample(row, x, 3, ch, p.depth, true);
				break;
			}
			dst[0] = over_white(r, a);
			dst[1] = over_white(g, a);
			dst[2] = over_white(b, a);
		}
	}
	img->w = p.w;
	img->h = p.h;
	img->rgb = rgb;
	return 0;
}

int ft_image_decode(const u8 *buf, size_t len, u8 *rgb, size_t rgb_cap, u8 *scratch,
		    size_t scratch_cap, struct ft_image *img)
{
	memset(img, 0, sizeof(*img));
	if (!buf || !rgb)
		return -MERR_INVAL;
	if (len >= 8 && !memcmp(buf, "\x89PNG\r\n\x1a\n", 8))
		return png_decode(buf, len, rgb, rgb_cap, scratch, scratch_cap, img);
	if (len >= 2 && buf[0] == 'B' && buf[1] == 'M')
		return bmp_decode(buf, len, rgb, rgb_cap, img);
	return -MERR_NOTSUP;
}
