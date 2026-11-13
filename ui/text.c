/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * UTF-8 decoding and glyph selection.
 *
 * Coverage: ASCII, plus Latin-1 letters (U+00A0..U+00FF) built by composing
 * a base letter with a diacritic, plus a few standalone shapes. That is
 * Western European text. Other scripts, combining sequences and
 * right-to-left layout are not handled: unknown code points draw as a box.
 */
#include "ui/ui.h"

const u8 ui_special_glyphs[UI_SPECIAL_COUNT][FONT_W] = {
	[UI_SP_BOX]      = { 0x7F, 0x41, 0x41, 0x41, 0x7F },
	[UI_SP_AE]       = { 0x7E, 0x09, 0x7F, 0x49, 0x41 },
	[UI_SP_OSLASH]   = { 0x5E, 0x21, 0x5D, 0x42, 0x3D },
	[UI_SP_SHARP_S]  = { 0x7E, 0x01, 0x49, 0x49, 0x36 },
	[UI_SP_ae]       = { 0x74, 0x54, 0x78, 0x54, 0x58 },
	[UI_SP_oslash]   = { 0x58, 0x24, 0x54, 0x48, 0x34 },
	[UI_SP_IQUEST]   = { 0x20, 0x40, 0x45, 0x48, 0x30 },
	[UI_SP_IEXCL]    = { 0x00, 0x00, 0x7D, 0x00, 0x00 },
};

u32 utf8_next(const char **sp)
{
	const u8 *s = (const u8 *)*sp;
	u32 cp, min;
	int extra;

	if (s[0] < 0x80) {
		*sp += 1;
		return s[0];
	}
	if ((s[0] & 0xE0) == 0xC0) {
		cp = s[0] & 0x1F;
		extra = 1;
		min = 0x80;
	} else if ((s[0] & 0xF0) == 0xE0) {
		cp = s[0] & 0x0F;
		extra = 2;
		min = 0x800;
	} else if ((s[0] & 0xF8) == 0xF0) {
		cp = s[0] & 0x07;
		extra = 3;
		min = 0x10000;
	} else {
		*sp += 1;                       /* stray continuation or invalid lead byte */
		return 0xFFFD;
	}
	for (int i = 1; i <= extra; i++) {
		if ((s[i] & 0xC0) != 0x80) {    /* truncated: resume at the offending byte */
			*sp += i;
			return 0xFFFD;
		}
		cp = (cp << 6) | (s[i] & 0x3F);
	}
	*sp += extra + 1;
	/* Reject overlong forms, surrogates and values beyond Unicode. */
	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return 0xFFFD;
	return cp;
}

int utf8_encode(u32 cp, char out[4])
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

int ui_text_cells(const char *s)
{
	int n = 0;

	while (*s) {
		(void)utf8_next(&s);
		n++;
	}
	return n;
}

bool ui_glyph_lookup(u32 cp, struct ui_glyph *g)
{
	/* U+00C0..U+00DF: base letter and mark; U+00E0..U+00FF is the lower-case mirror. */
	static const char bases[] = "AAAAAA\1CEEEEIIIIDNOOOOOx\2UUUUY?\3";
	static const char marks[] = "gactdr-,gacdgacd-tgactd--gacda--";
	static const char mark_ids[] = "-gactdr,";
	unsigned i;

	memset(g, 0, sizeof(*g));
	if (cp >= 32 && cp <= 126) {
		g->base = (u8)cp;
		return true;
	}
	switch (cp) {
	case 0x00A0: g->base = ' ';  return true;       /* no-break space */
	case 0x00A1: g->special = UI_SP_IEXCL;  return true;
	case 0x00BF: g->special = UI_SP_IQUEST; return true;
	case 0x00AB: g->base = '<';  return true;
	case 0x00BB: g->base = '>';  return true;
	case 0x2013: case 0x2014: g->base = '-'; return true;
	case 0x2018: case 0x2019: g->base = '\''; return true;
	default: break;
	}
	if (cp < 0xC0 || cp > 0xFF) {
		g->special = UI_SP_BOX;
		return false;
	}
	i = (cp - 0xC0) & 0x1F;
	g->base = (u8)bases[i];
	g->mark = (enum ui_mark)(strchr(mark_ids, marks[i]) - mark_ids);
	if (cp >= 0xE0) {
		g->base = (u8)mos_tolower(g->base);
		if (cp == 0xFF) {                       /* y with diaeresis */
			g->base = 'y';
			g->mark = UI_MARK_DIAERESIS;
		}
	}
	if (g->base == 1 || g->base == 2) {
		g->special = (u8)(g->base == 1 ? (cp < 0xE0 ? UI_SP_AE : UI_SP_ae)
					       : (cp < 0xE0 ? UI_SP_OSLASH : UI_SP_oslash));
		g->base = 0;
	} else if (g->base == 3) {
		g->special = cp == 0xDF ? UI_SP_SHARP_S : UI_SP_BOX;
		g->base = 0;
	}
	g->tall = g->base >= 'A' && g->base <= 'Z';
	return g->base != '?';
}
