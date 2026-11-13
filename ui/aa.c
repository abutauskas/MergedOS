/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Anti-aliased rendering, integer only (the kernel has no floating point).
 *
 * Text: the 5x7 dot patterns are treated as skeletons, not bitmaps. Lit
 * dots that touch are joined by round-ended strokes, and the result is
 * sampled 3x3 per pixel. Diagonals come out as smooth lines instead of
 * staircases at every size, from the same 475 bytes of font data.
 *
 * Shapes: rounded rectangles and the logo's hexagon use 4x4 samples on
 * their edges; interiors are plain fills.
 */
#include "ui/ui.h"

#define GLYPH_SS 3

/* Diacritics above a letter: two rows of five dots (bit n = column n). */
static const u8 mark_rows[][2] = {
	[UI_MARK_GRAVE]      = { 0x02, 0x04 },
	[UI_MARK_ACUTE]      = { 0x08, 0x04 },
	[UI_MARK_CIRCUMFLEX] = { 0x04, 0x0A },
	[UI_MARK_TILDE]      = { 0x14, 0x0A },
	[UI_MARK_DIAERESIS]  = { 0x00, 0x0A },
	[UI_MARK_RING]       = { 0x04, 0x04 },
	[UI_MARK_CEDILLA]    = { 0x00, 0x00 },
};

static bool lit(const u16 *cols, int c, int r)
{
	return c >= 0 && c < FONT_W && r >= 0 && r < UI_RASTER_ROWS && ((cols[c] >> r) & 1);
}

/* Is point p within distance sqrt(r2) of segment a-b? */
static bool near_seg(s64 px, s64 py, s64 ax, s64 ay, s64 bx, s64 by, s64 r2)
{
	s64 dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
	s64 t = (px - ax) * dx + (py - ay) * dy, cross;

	if (t <= 0)
		return (px - ax) * (px - ax) + (py - ay) * (py - ay) <= r2;
	if (t >= len2)
		return (px - bx) * (px - bx) + (py - by) * (py - by) <= r2;
	cross = (px - ax) * dy - (py - ay) * dx;
	return cross * cross <= r2 * len2;
}

/*
 * Dot grid for one character, 5 columns x 10 rows: rows 0-1 hold a mark
 * above a capital, rows 2-8 the letter, row 9 a cedilla. A small letter is
 * five rows tall, so its mark replaces the letter's own top two rows.
 */
static void dot_grid(const struct ui_glyph *g, u16 cols[FONT_W])
{
	const u8 *base = g->base ? font5x7[g->base - 32] : ui_special_glyphs[g->special];
	const bool above = g->mark != UI_MARK_NONE && g->mark != UI_MARK_CEDILLA;
	const int mark_row = g->tall ? 0 : 2;

	for (int c = 0; c < FONT_W; c++) {
		cols[c] = (u16)((base[c] & ((above && !g->tall) ? 0x7C : 0x7F)) << 2);
		for (int r = 0; above && r < 2; r++)
			if (mark_rows[g->mark][r] & (1u << c))
				cols[c] |= (u16)(1u << (mark_row + r));
	}
	if (g->mark == UI_MARK_CEDILLA)
		cols[2] |= 1u << 9;
}

void ui_glyph_coverage(const struct ui_glyph *g, int scale, u8 *out)
{
	const int F = GLYPH_SS * scale;                 /* samples per dot pitch */
	const s64 radius = 84 * F / 100;                /* in half-samples: stroke = 0.84 pitch */
	u16 cols[FONT_W];

	dot_grid(g, cols);
	for (int py = 0; py < UI_RASTER_ROWS * scale; py++) {
		for (int px = 0; px < UI_GLYPH_W * scale; px++) {
			int hits = 0;

			for (int s = 0; s < GLYPH_SS * GLYPH_SS; s++) {
				int sx = px * GLYPH_SS + s % GLYPH_SS, sy = py * GLYPH_SS + s / GLYPH_SS;
				s64 X = 2 * sx + 1, Y = 2 * sy + 1;     /* half-sample units */
				bool hit = false;

				/* Only strokes starting at one of the nine nearest dots can reach. */
				for (int n = 0; n < 9 && !hit; n++) {
					int c = sx / F + n % 3 - 1, r = sy / F + n / 3 - 1;
					s64 ax = (2 * c + 1) * F, ay = (2 * r + 1) * F, d = 2 * F;

					if (!lit(cols, c, r))
						continue;
					hit = near_seg(X, Y, ax, ay, ax, ay, radius * radius) ||
					      (lit(cols, c + 1, r) &&
					       near_seg(X, Y, ax, ay, ax + d, ay, radius * radius)) ||
					      (lit(cols, c, r + 1) &&
					       near_seg(X, Y, ax, ay, ax, ay + d, radius * radius)) ||
					      /* Diagonals only where no orthogonal path joins the dots. */
					      (lit(cols, c + 1, r + 1) && !lit(cols, c + 1, r) &&
					       !lit(cols, c, r + 1) &&
					       near_seg(X, Y, ax, ay, ax + d, ay + d, radius * radius)) ||
					      (lit(cols, c - 1, r + 1) && !lit(cols, c - 1, r) &&
					       !lit(cols, c, r + 1) &&
					       near_seg(X, Y, ax, ay, ax - d, ay + d, radius * radius));
				}
				hits += hit;
			}
			*out++ = (u8)(hits * 255 / (GLYPH_SS * GLYPH_SS));
		}
	}
}

/* ---- rounded rectangles ------------------------------------------------------ */

/* Coverage of the pixel dx,dy away from the centre of a quarter circle of radius r. */
static u8 corner_alpha(int dx, int dy, int r)
{
	int hits = 0;

	for (int s = 0; s < 16; s++) {
		int X = 8 * dx + 2 * (s % 4) + 1, Y = 8 * dy + 2 * (s / 4) + 1;

		hits += X * X + Y * Y <= 64 * r * r;
	}
	return (u8)(hits * 255 / 16);
}

void ui_fill_round(int x, int y, int w, int h, int r, u32 rgb)
{
	r = MOS_MAX(0, MOS_MIN(r, MOS_MIN(w, h) / 2));
	ui_draw_fill(x + r, y, w - 2 * r, h, rgb);
	ui_draw_fill(x, y + r, r, h - 2 * r, rgb);
	ui_draw_fill(x + w - r, y + r, r, h - 2 * r, rgb);
	for (int dy = 0; dy < r; dy++) {
		for (int dx = 0; dx < r; dx++) {
			u8 a = corner_alpha(dx, dy, r);

			if (!a)
				continue;
			ui_draw_blend(x + r - 1 - dx, y + r - 1 - dy, rgb, a);
			ui_draw_blend(x + w - r + dx, y + r - 1 - dy, rgb, a);
			ui_draw_blend(x + r - 1 - dx, y + h - r + dy, rgb, a);
			ui_draw_blend(x + w - r + dx, y + h - r + dy, rgb, a);
		}
	}
}

/* ---- logo --------------------------------------------------------------------- */

#define LOGO_TOP    0xE5484D            /* red  */
#define LOGO_BOTTOM 0x2F6FED            /* blue */

/*
 * The MergedOS mark: a pointy-top hexagon shaded red to blue, holding four
 * squares of four different sizes - four systems, one shell. Drawn here
 * from numbers; docs/logo.svg has the same geometry. Coordinates are
 * quarter units of a 16 x 16 unit box.
 */
void ui_logo(int x, int y, int unit)
{
	static const s8 hex[6][2] = { { 32, 2 }, { 58, 17 }, { 58, 47 }, { 32, 62 }, { 6, 47 }, { 6, 17 } };
	static const u8 squares[4][3] = {       /* x, y, size */
		{ 15, 15, 16 }, { 33, 19, 12 }, { 33, 33, 9 }, { 25, 33, 6 },
	};
	const int size = 16 * unit;

	for (int py = 0; py < size; py++) {
		const int t = py * 256 / size;
		u32 rgb = 0;

		for (int ch = 0; ch < 3; ch++) {        /* vertical gradient */
			int a = (LOGO_TOP >> (8 * ch)) & 0xFF, b = (LOGO_BOTTOM >> (8 * ch)) & 0xFF;

			rgb |= (u32)((a * (256 - t) + b * t) >> 8) << (8 * ch);
		}
		for (int px = 0; px < size; px++) {
			int hits = 0;

			for (int s = 0; s < 16; s++) {  /* 4x4 samples, in 1/16 pixel */
				s64 X = 16 * px + 4 * (s % 4) + 2, Y = 16 * py + 4 * (s / 4) + 2;
				bool in = true;

				for (int e = 0; e < 6 && in; e++) {
					s64 ax = hex[e][0] * unit * 4, ay = hex[e][1] * unit * 4;
					s64 bx = hex[(e + 1) % 6][0] * unit * 4, by = hex[(e + 1) % 6][1] * unit * 4;

					in = (bx - ax) * (Y - ay) - (by - ay) * (X - ax) >= 0;
				}
				hits += in;
			}
			if (hits)
				ui_draw_blend(x + px, y + py, rgb, (u8)(hits * 255 / 16));
		}
	}
	for (int i = 0; i < 4; i++)
		ui_draw_fill(x + squares[i][0] * unit / 4, y + squares[i][1] * unit / 4,
			     squares[i][2] * unit / 4, squares[i][2] * unit / 4, 0xF4F6FA);
}
