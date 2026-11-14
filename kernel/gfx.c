/* SPDX-License-Identifier: BSD-2-Clause */
/* Framebuffer mode setup, drawing primitives and the pointer sprite. */
#include <mergedos/kernel.h>
#include <mergedos/gfx.h>
#include <mergedos/io.h> 
#include <mergedos/mm.h>

#define VBE_INDEX        0x1CE
#define VBE_DATA         0x1CF
#define VBE_ID           0
#define VBE_XRES         1
#define VBE_YRES         2
#define VBE_BPP          3
#define VBE_ENABLE       4
#define VBE_ENABLED      0x01
#define VBE_LFB_ENABLED  0x40

static struct gfx_info info;
static u8 *fb;
static u32 bytespp;

const struct gfx_info *gfx_get(void)
{
	return &info;
}

static bool adopt(u64 phys, u32 w, u32 h, u32 pitch, u32 bpp, const char *source)
{
	if ((bpp != 32 && bpp != 24) || !w || !h || pitch < w * (bpp / 8))
		return false;
	if (phys == 0 || phys >= DMAP_LIMIT || (u64)pitch * h > DMAP_LIMIT - phys)
		return false;           /* outside the boot direct map */
	fb = phys_to_virt(phys);
	bytespp = bpp / 8;
	info = (struct gfx_info){ true, w, h, pitch, bpp, source };
	return true;
}

bool gfx_init_bootfb(const struct boot_info *bi)
{
	return bi->has_fb && bi->fb_type == 1 &&
	       adopt(bi->fb_addr, bi->fb_width, bi->fb_height, bi->fb_pitch, bi->fb_bpp,
		     "boot loader");
}

static u16 vbe_read(u16 index)
{
	outw(VBE_INDEX, index);
	return inw(VBE_DATA);
}

static void vbe_write(u16 index, u16 value)
{
	outw(VBE_INDEX, index);
	outw(VBE_DATA, value);
}

bool gfx_init_bochs(const struct hw_profile *hw)
{
	for (u32 i = 0; i < hw->ngpu; i++) {
		const struct hw_gpu *g = &hw->gpu[i];

		/* Only poke the dispi ports on adapters known to implement them. */
		if (g->vendor != 0x1234 && g->vendor != 0x1AF4 && g->vendor != 0x1B36)
			continue;
		if ((vbe_read(VBE_ID) & 0xFFF0) != 0xB0C0 || !g->mem_bar_phys)
			continue;
		vbe_write(VBE_ENABLE, 0);
		vbe_write(VBE_XRES, GFX_DEFAULT_WIDTH);
		vbe_write(VBE_YRES, GFX_DEFAULT_HEIGHT);
		vbe_write(VBE_BPP, 32);
		vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
		if (vbe_read(VBE_XRES) == GFX_DEFAULT_WIDTH &&
		    adopt(g->mem_bar_phys, GFX_DEFAULT_WIDTH, GFX_DEFAULT_HEIGHT,
			  GFX_DEFAULT_WIDTH * 4, 32, "Bochs VBE"))
			return true;
		vbe_write(VBE_ENABLE, 0);       /* back to VGA text */
	}
	return false;
}

/* ---- pixels ---------------------------------------------------------------- */

static inline void put(int x, int y, u32 rgb)
{
	u8 *p = fb + (size_t)y * info.pitch + (size_t)x * bytespp;

	if (bytespp == 4) {
		*(u32 *)p = rgb;
	} else {
		p[0] = (u8)rgb;
		p[1] = (u8)(rgb >> 8);
		p[2] = (u8)(rgb >> 16);
	}
}

u32 gfx_read(int x, int y)
{
	const u8 *p;

	if (!info.active || x < 0 || y < 0 || x >= (int)info.width || y >= (int)info.height)
		return 0;
	p = fb + (size_t)y * info.pitch + (size_t)x * bytespp;
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
}

static void raw_fill(int x, int y, int w, int h, u32 rgb)
{
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > (int)info.width)
		w = (int)info.width - x;
	if (y + h > (int)info.height)
		h = (int)info.height - y;
	for (int row = 0; row < h; row++)
		for (int col = 0; col < w; col++)
			put(x + col, y + row, rgb);
}

/* ---- pointer sprite -------------------------------------------------------- */

#define CUR_W 12
#define CUR_H 18

/* Drawn for MergedOS: 'X' outline, '.' fill, ' ' transparent. */
static const char cursor_shape[CUR_H][CUR_W + 1] = {
	"X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
	"X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
	"X.........X ", "X......XXXXX", "X...X..X    ", "X..X X..X   ", "X.X  X..X   ",
	"XX    X..X  ", "X     X..X  ", "       XX   ",
};

static u32 cur_under[CUR_W * CUR_H];
static int cur_x, cur_y;
static bool cur_wanted, cur_drawn;

static void cursor_erase(void)
{
	if (!cur_drawn)
		return;
	for (int r = 0; r < CUR_H; r++)
		for (int c = 0; c < CUR_W; c++)
			if (cur_x + c < (int)info.width && cur_y + r < (int)info.height)
				put(cur_x + c, cur_y + r, cur_under[r * CUR_W + c]);
	cur_drawn = false;
}

static void cursor_paint(void)
{
	for (int r = 0; r < CUR_H; r++) {
		for (int c = 0; c < CUR_W; c++) {
			int x = cur_x + c, y = cur_y + r;

			if (x >= (int)info.width || y >= (int)info.height)
				continue;
			cur_under[r * CUR_W + c] = gfx_read(x, y);
			if (cursor_shape[r][c] != ' ')
				put(x, y, cursor_shape[r][c] == 'X' ? 0x000000 : 0xFFFFFF);
		}
	}
	cur_drawn = true;
}

/* Lift the pointer if a draw is about to touch the pixels saved beneath it. */
static void guard(int x, int y, int w, int h)
{
	if (cur_drawn && x < cur_x + CUR_W && x + w > cur_x && y < cur_y + CUR_H && y + h > cur_y)
		cursor_erase();
}

/*
 * Several threads draw (shell, desktop): each public primitive runs with
 * interrupts off so that the pointer bookkeeping is never seen half-done.
 */
void gfx_cursor_set(int x, int y)
{
	u64 flags = irq_save();

	if (info.active) {
		cursor_erase();
		cur_x = x;
		cur_y = y;
		cur_wanted = true;
		cursor_paint();
	}
	irq_restore(flags);
}

void gfx_cursor_refresh(void)
{
	u64 flags = irq_save();

	if (info.active && cur_wanted && !cur_drawn)
		cursor_paint();
	irq_restore(flags);
}

/* ---- public drawing -------------------------------------------------------- */

void gfx_fill(int x, int y, int w, int h, u32 rgb)
{
	u64 flags = irq_save();

	if (info.active) {
		guard(x, y, w, h);
		raw_fill(x, y, w, h, rgb);
	}
	irq_restore(flags);
}

void gfx_blit(int x, int y, int w, int h, const u32 *src, size_t stride)
{
	int x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0;

	if (!info.active)
		return;
	if (x + w > (int)info.width)
		w = (int)info.width - x;
	if (y + h > (int)info.height)
		h = (int)info.height - y;
	for (int row = y0; row < h; row++) {
		u64 flags = irq_save();             /* per row: keeps interrupt latency short */

		guard(x + x0, y + row, w - x0, 1);
		for (int col = x0; col < w; col++)
			put(x + col, y + row, src[(size_t)row * stride + (size_t)col] & 0xFFFFFF);
		irq_restore(flags);
	}
}

void gfx_blend(int x, int y, u32 rgb, u8 alpha)
{
	u32 bg, out = 0;
	u64 flags;

	if (!info.active || !alpha || x < 0 || y < 0 || x >= (int)info.width || y >= (int)info.height)
		return;
	flags = irq_save();
	guard(x, y, 1, 1);
	if (alpha != 255) {
		bg = gfx_read(x, y);
		for (int ch = 0; ch < 24; ch += 8)
			out |= ((((rgb >> ch) & 0xFF) * alpha + ((bg >> ch) & 0xFF) * (255u - alpha)) / 255)
			       << ch;
		rgb = out;
	}
	put(x, y, rgb);
	irq_restore(flags);
}

/*
 * Glyph coverage at the console's size is computed once per character and
 * kept (95 x 240 bytes); other sizes are rare (titles) and computed per draw.
 */
#define CACHE_SCALE 2
#define CACHE_BYTES (UI_GLYPH_W * CACHE_SCALE * UI_RASTER_ROWS * CACHE_SCALE)

static u8 glyph_cache[95][CACHE_BYTES];
static bool glyph_cached[95];

static void draw_glyph(int x, int y, const struct ui_glyph *g, u32 rgb, int scale)
{
	u8 tmp[UI_GLYPH_W * UI_MAX_SCALE * UI_RASTER_ROWS * UI_MAX_SCALE];
	const u8 *cov = tmp;
	int w, h;

	scale = MOS_MAX(1, MOS_MIN(scale, UI_MAX_SCALE));
	w = UI_GLYPH_W * scale;
	h = UI_RASTER_ROWS * scale;
	if (scale == CACHE_SCALE && g->base && g->mark == UI_MARK_NONE) {
		int i = g->base - 32;

		if (!glyph_cached[i]) {
			ui_glyph_coverage(g, scale, glyph_cache[i]);
			glyph_cached[i] = true;
		}
		cov = glyph_cache[i];
	} else {
		ui_glyph_coverage(g, scale, tmp);
	}
	y -= 2 * scale;                         /* the raster starts two dot rows above */
	guard(x, y, w, h);
	for (int row = 0; row < h; row++)
		for (int col = 0; col < w; col++)
			if (cov[row * w + col])
				gfx_blend(x + col, y + row, rgb, cov[row * w + col]);
}

void gfx_glyph(int x, int y, char c, u32 rgb, int scale)
{
	struct ui_glyph g = { .base = (u8)c };

	if (info.active && c > 32 && c <= 126)
		draw_glyph(x, y, &g, rgb, scale);
}

void gfx_text(int x, int y, const char *s, u32 rgb, int scale)
{
	if (!info.active)
		return;
	for (; *s; x += UI_GLYPH_W * MOS_MAX(1, MOS_MIN(scale, UI_MAX_SCALE))) {
		struct ui_glyph g;

		(void)ui_glyph_lookup(utf8_next(&s), &g);
		if (g.base != ' ')
			draw_glyph(x, y, &g, rgb, scale);
	}
}

int gfx_selftest(void)
{
	static const u32 probe[] = { 0x123456, 0xABCDEF, 0x000000, 0xFFFFFF };
	u32 saved;
	int bad = 0;

	if (!info.active)
		return 1;
	guard(0, 0, 1, 1);
	saved = gfx_read(0, 0);
	for (unsigned i = 0; i < ARRAY_SIZE(probe); i++) {
		put(0, 0, probe[i]);
		bad += gfx_read(0, 0) != probe[i];
	}
	put(0, 0, saved);
	return bad;
}
