/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Early console: VGA text mode mirrored to COM1, with polled input from the
 * PS/2 keyboard and the serial line. Everything is polled so the console
 * works before (and without) interrupt delivery.
 */
#include <mergedos/kernel.h>
#include <mergedos/console.h>
#include <mergedos/gfx.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include <mergedos/sched.h>

#define COM1      0x3F8
#define VGA_PHYS  0xB8000

static volatile u16 *const vga = (volatile u16 *)(DMAP_BASE + VGA_PHYS);
static int cur_row, cur_col;
static int region_top, region_bottom = VGA_ROWS - 1;
static u8 cur_attr = VGA_ATTR(VGA_LGRAY, VGA_BLACK);
static bool serial_present;

/*
 * The character grid is kept in `shadow` and rendered by one of two back
 * ends: VGA text memory, or (after console_use_framebuffer) glyphs drawn
 * into the linear framebuffer. Everything above this layer is unchanged.
 */
static u16 shadow[VGA_ROWS * VGA_COLS];
static bool fb_mode, fb_muted, fb_custom;   /* custom: placed inside a desktop window */
static int fb_scale, fb_cell_w, fb_cell_h, fb_org_x, fb_org_y, fb_cursor_idx = -1;
/* Only cells wholly inside this rectangle are drawn (a desktop tile may be small). */
static int fb_clip_x0, fb_clip_y0, fb_clip_x1 = 1 << 30, fb_clip_y1 = 1 << 30;

static const u32 vga_palette[16] = {
	0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static bool cell_visible(int x, int y)
{
	return x >= fb_clip_x0 && y >= fb_clip_y0 && x + fb_cell_w <= fb_clip_x1 &&
	       y + fb_cell_h <= fb_clip_y1;
}

static void cell_draw(int idx)
{
	int x = fb_org_x + (idx % VGA_COLS) * fb_cell_w;
	int y = fb_org_y + (idx / VGA_COLS) * fb_cell_h;
	u16 v = shadow[idx];

	if (!cell_visible(x, y))
		return;
	gfx_fill(x, y, fb_cell_w, fb_cell_h, vga_palette[(v >> 12) & 0xF]);
	gfx_glyph(x, y + 2 * fb_scale, (char)(v & 0xFF), vga_palette[(v >> 8) & 0xF], fb_scale);
}

static void cell_set(int idx, u16 v)
{
	if (!fb_mode) {
		shadow[idx] = v;
		vga[idx] = v;
	} else if (shadow[idx] != v) {          /* redraw only what changed */
		shadow[idx] = v;
		if (!fb_muted)
			cell_draw(idx);
	}
}

void console_use_framebuffer(void)
{
	const struct gfx_info *g = gfx_get();

	if (!g->active)
		return;
	fb_scale = (g->width >= 960 && g->height >= 600) ? 2 : 1;
	fb_cell_w = (FONT_W + 1) * fb_scale;
	fb_cell_h = 12 * fb_scale;
	fb_org_x = ((int)g->width - VGA_COLS * fb_cell_w) / 2;
	fb_org_y = ((int)g->height - VGA_ROWS * fb_cell_h) / 2;
	fb_mode = true;
	fb_muted = false;
	fb_custom = false;
	fb_cursor_idx = -1;
	fb_clip_x0 = fb_clip_y0 = 0;
	fb_clip_x1 = fb_clip_y1 = 1 << 30;
	gfx_fill(0, 0, (int)g->width, (int)g->height, 0x101418);
	for (int idx = 0; idx < VGA_ROWS * VGA_COLS; idx++)
		cell_draw(idx);
}

void console_fb_mute(bool mute)
{
	if (!fb_mode || mute == fb_muted)
		return;
	if (mute)
		fb_muted = true;
	else if (fb_custom)
		console_fb_place(fb_org_x, fb_org_y);
	else
		console_use_framebuffer();      /* repaint the whole grid */
}

void console_fb_size(int *w, int *h)
{
	*w = VGA_COLS * fb_cell_w;
	*h = VGA_ROWS * fb_cell_h;
}

void console_fb_place(int x, int y)
{
	u64 flags = irq_save();

	if (fb_mode) {
		fb_org_x = x;
		fb_org_y = y;
		fb_custom = true;
		fb_muted = false;
		fb_cursor_idx = -1;
		for (int idx = 0; idx < VGA_ROWS * VGA_COLS; idx++)
			cell_draw(idx);
	}
	irq_restore(flags);
}

void console_fb_frame(int org_x, int org_y, int clip_x, int clip_y, int clip_w, int clip_h)
{
	u64 flags = irq_save();

	if (fb_mode) {
		if (org_x != fb_org_x || org_y != fb_org_y)
			fb_cursor_idx = -1;     /* the old underline is not ours to erase */
		fb_org_x = org_x;
		fb_org_y = org_y;
		fb_clip_x0 = clip_x;
		fb_clip_y0 = clip_y;
		fb_clip_x1 = clip_x + clip_w;
		fb_clip_y1 = clip_y + clip_h;
		fb_custom = true;
	}
	irq_restore(flags);
}

void console_fb_repaint(int *x, int *y, int *w, int *h)
{
	u64 flags = irq_save();
	int c0, c1, r0, r1;

	if (!fb_mode || *w <= 0 || *h <= 0) {
		*w = *h = 0;
		irq_restore(flags);
		return;
	}
	c0 = MOS_MAX(0, (*x - fb_org_x) / fb_cell_w - 1);
	r0 = MOS_MAX(0, (*y - fb_org_y) / fb_cell_h - 1);
	c1 = MOS_MIN(VGA_COLS, (*x + *w - fb_org_x) / fb_cell_w + 1);
	r1 = MOS_MIN(VGA_ROWS, (*y + *h - fb_org_y) / fb_cell_h + 1);
	/* Only cells that touch the rectangle; report the area they cover. */
	while (c0 < c1 && fb_org_x + (c0 + 1) * fb_cell_w <= *x)
		c0++;
	while (r0 < r1 && fb_org_y + (r0 + 1) * fb_cell_h <= *y)
		r0++;
	while (c1 > c0 && fb_org_x + (c1 - 1) * fb_cell_w >= *x + *w)
		c1--;
	while (r1 > r0 && fb_org_y + (r1 - 1) * fb_cell_h >= *y + *h)
		r1--;
	for (int r = r0; r < r1; r++)
		for (int c = c0; c < c1; c++)
			cell_draw(r * VGA_COLS + c);
	if (c0 >= c1 || r0 >= r1) {
		*w = *h = 0;
	} else {
		*x = fb_org_x + c0 * fb_cell_w;
		*y = fb_org_y + r0 * fb_cell_h;
		*w = (c1 - c0) * fb_cell_w;
		*h = (r1 - r0) * fb_cell_h;
	}
	if (fb_cursor_idx >= r0 * VGA_COLS && fb_cursor_idx < r1 * VGA_COLS &&
	    fb_cursor_idx % VGA_COLS >= c0 && fb_cursor_idx % VGA_COLS < c1)
		fb_cursor_idx = -1;             /* the repaint erased the underline */
	irq_restore(flags);
}

/* Typed-ahead input from the desktop (launcher clicks), read before real keys. */
static char inject_buf[128];
static unsigned inject_head, inject_tail;

void console_inject(const char *s)
{
	u64 flags = irq_save();

	for (; *s && inject_head - inject_tail < sizeof(inject_buf); s++)
		inject_buf[inject_head++ % sizeof(inject_buf)] = *s;
	irq_restore(flags);
}

/* ---- serial --------------------------------------------------------------- */

static void serial_init(void)
{
	outb(COM1 + 1, 0x00);           /* no interrupts            */
	outb(COM1 + 3, 0x80);           /* DLAB                     */
	outb(COM1 + 0, 0x01);           /* 115200 baud              */
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);           /* 8N1                      */
	outb(COM1 + 2, 0xC7);           /* FIFO on, clear, 14 bytes */
	outb(COM1 + 4, 0x1E);           /* loopback self-test       */
	outb(COM1 + 0, 0xAE);
	serial_present = inb(COM1 + 0) == 0xAE;
	outb(COM1 + 4, 0x0F);           /* normal operation         */
}

static void serial_putc(char c)
{
	if (!serial_present)
		return;
	for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
		cpu_relax();
	outb(COM1, (u8)c);
}

/* ---- VGA text ------------------------------------------------------------- */

static void vga_cursor_sync(void)
{
	u16 pos = (u16)(cur_row * VGA_COLS + cur_col);

	if (fb_mode && fb_muted)
		return;
	if (fb_mode) {
		/* Text cursor: an underline, moved by redrawing the cell it left. */
		if (fb_cursor_idx >= 0 && fb_cursor_idx != pos)
			cell_draw(fb_cursor_idx);
		fb_cursor_idx = pos;
		if (cell_visible(fb_org_x + cur_col * fb_cell_w, fb_org_y + cur_row * fb_cell_h))
			gfx_fill(fb_org_x + cur_col * fb_cell_w,
				 fb_org_y + (cur_row + 1) * fb_cell_h - fb_scale, fb_cell_w, fb_scale,
				 vga_palette[cur_attr & 0xF]);
		gfx_cursor_refresh();
		return;
	}
	outb(0x3D4, 0x0F);
	outb(0x3D5, (u8)pos);
	outb(0x3D4, 0x0E);
	outb(0x3D5, (u8)(pos >> 8));
}

void vga_fill_row(int row, char c, u8 attr)
{
	if (row < 0 || row >= VGA_ROWS)
		return;
	for (int col = 0; col < VGA_COLS; col++)
		cell_set(row * VGA_COLS + col, (u16)((attr << 8) | (u8)c));
}

void vga_write_at(int row, int col, const char *s, u8 attr)
{
	if (row < 0 || row >= VGA_ROWS)
		return;
	for (; *s && col < VGA_COLS; s++, col++)
		if (col >= 0)
			cell_set(row * VGA_COLS + col, (u16)((attr << 8) | (u8)*s));
}

static void vga_scroll(void)
{
	for (int row = region_top; row < region_bottom; row++)
		for (int col = 0; col < VGA_COLS; col++)
			cell_set(row * VGA_COLS + col, shadow[(row + 1) * VGA_COLS + col]);
	vga_fill_row(region_bottom, ' ', cur_attr);
}

static void vga_putc(char c)
{
	if (c == '\n') {
		cur_col = 0;
		cur_row++;
	} else if (c == '\r') {
		cur_col = 0;
	} else if (c == '\b') {
		if (cur_col > 0)
			cur_col--;
	} else if (c == '\t') {
		cur_col = (cur_col + 8) & ~7;
	} else {
		cell_set(cur_row * VGA_COLS + cur_col, (u16)((cur_attr << 8) | (u8)c));
		cur_col++;
	}
	if (cur_col >= VGA_COLS) {
		cur_col = 0;
		cur_row++;
	}
	if (cur_row > region_bottom) {
		vga_scroll();
		cur_row = region_bottom;
	}
}

void console_set_attr(u8 attr)
{
	cur_attr = attr;
}

void console_set_region(int top_row, int bottom_row)
{
	if (top_row < 0 || bottom_row >= VGA_ROWS || top_row > bottom_row)
		return;
	region_top = top_row;
	region_bottom = bottom_row;
	cur_row = top_row;
	cur_col = 0;
	vga_cursor_sync();
}

void console_clear(void)
{
	for (int row = region_top; row <= region_bottom; row++)
		vga_fill_row(row, ' ', cur_attr);
	cur_row = region_top;
	cur_col = 0;
	vga_cursor_sync();
}

/* ---- output --------------------------------------------------------------- */

void console_putc(char c)
{
	u64 flags = irq_save();         /* one writer at a time */

	if (c == '\n')
		serial_putc('\r');
	serial_putc(c);
	vga_putc(c);
	vga_cursor_sync();
	irq_restore(flags);
}

/* Serial line only: for notes that must not disturb what is on screen. */
void console_trace(const char *s)
{
	u64 flags = irq_save();

	for (; *s; s++) {
		if (*s == '\n')
			serial_putc('\r');
		serial_putc(*s);
	}
	irq_restore(flags);
}

void console_write(const char *s, size_t n)
{
	u64 flags = irq_save();

	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\n')
			serial_putc('\r');
		serial_putc(s[i]);
		vga_putc(s[i]);
	}
	vga_cursor_sync();
	irq_restore(flags);
}

/* ---- input ---------------------------------------------------------------- */

/* PS/2 scancode set 1, US layout. */
static const char keymap[2][0x3A] = {
	{ 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
	  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
	  0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
	  0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ' },
	{ 0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
	  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
	  0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
	  0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ' },
};

static int keyboard_poll(void)
{
	static bool shift, extended, ctrl;
	u8 status, sc;

	/* Mouse bytes share the port: drain them to the mouse driver first. */
	for (int i = 0;; i++) {
		status = inb(0x64);
		if (!(status & 0x01) || i == 64)
			return -1;
		sc = inb(0x60);
		if (!(status & 0x20))
			break;
		mouse_feed(sc);
	}
	if (sc == 0xE0) {
		extended = true;
		return -1;
	}
	if (extended) {                                 /* arrow keys; others ignored */
		extended = false;
		switch (sc) {
		case 0x48: return KEY_UP;
		case 0x50: return KEY_DOWN;
		case 0x4B: return KEY_LEFT;
		case 0x4D: return KEY_RIGHT;
		default:   return -1;
		}
	}
	if (sc == 0x1D || sc == 0x9D) {                /* left Ctrl: Ctrl-C, Ctrl-D for programs */
		ctrl = sc == 0x1D;
		return -1;
	}
	if (sc == 0x2A || sc == 0x36) {
		shift = true;
		return -1;
	}
	if (sc == 0xAA || sc == 0xB6) {
		shift = false;
		return -1;
	}
	if (sc & 0x80 || sc >= 0x3A)                    /* key release / unmapped */
		return -1;
	if (ctrl && keymap[0][sc] >= 'a' && keymap[0][sc] <= 'z')
		return keymap[0][sc] & 0x1F;
	return keymap[shift][sc] ? keymap[shift][sc] : -1;
}

static int serial_poll(void)
{
	static int esc;
	int c;

	if (!serial_present || !(inb(COM1 + 5) & 0x01))
		return -1;
	c = inb(COM1);
	/* ANSI cursor keys arrive as ESC [ A/B/C/D. */
	if (esc == 0 && c == 0x1B) {
		esc = 1;
		return -1;
	}
	if (esc == 1) {
		esc = c == '[' ? 2 : 0;
		if (esc)
			return -1;
	} else if (esc == 2) {
		esc = 0;
		if (c >= 'A' && c <= 'D')
			return c == 'A' ? KEY_UP : c == 'B' ? KEY_DOWN : c == 'C' ? KEY_RIGHT : KEY_LEFT;
	}
	if (c == '\r')
		return '\n';
	if (c == 0x7F)
		return '\b';
	return c;
}

int console_poll(void)
{
	int c;

	if (inject_head != inject_tail)
		return inject_buf[inject_tail++ % sizeof(inject_buf)];
	c = serial_poll();

	return c >= 0 ? c : keyboard_poll();
}

int console_getc(void)
{
	int c;

	while ((c = console_poll()) < 0)
		thread_sleep_ms(10);            /* let other threads (and the CPU) rest */
	return c;
}

void console_init(void)
{
	serial_init();
	console_clear();
}
