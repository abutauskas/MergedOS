/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_CONSOLE_H
#define MERGEDOS_CONSOLE_H

#include <mergedos/types.h>

#define VGA_COLS 80
#define VGA_ROWS 25

/* VGA text attribute helper: low nibble foreground, high nibble background. */
#define VGA_ATTR(fg, bg) ((u8)(((bg) << 4) | ((fg) & 0x0F)))
enum vga_color {
	VGA_BLACK, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LGRAY,
	VGA_DGRAY, VGA_LBLUE, VGA_LGREEN, VGA_LCYAN, VGA_LRED, VGA_PINK, VGA_YELLOW, VGA_WHITE
};

/* Keys beyond ASCII, as returned by console_poll() (same values as UI_KEY_*). */
#define KEY_UP    0x110001
#define KEY_DOWN  0x110002
#define KEY_LEFT  0x110003
#define KEY_RIGHT 0x110004

void console_init(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_trace(const char *s);      /* serial line only */

/* Input: merged PS/2 keyboard + COM1. poll() returns -1 when nothing is pending. */
int  console_poll(void);
int  console_getc(void);

/* VGA-only presentation controls (no effect on the serial mirror). */
/* While muted the framebuffer belongs to a GUI; text goes to the grid and serial only. */
void console_fb_mute(bool mute);
/* Framebuffer console as a desktop window: pixel size, and where to draw it. */
void console_fb_size(int *w, int *h);
void console_fb_place(int x, int y);
/* Set the grid origin and the clip (only whole cells inside it are drawn); no drawing. */
void console_fb_frame(int org_x, int org_y, int clip_x, int clip_y, int clip_w, int clip_h);
/* Redraw the cells touching a rectangle, even while muted; returns the area they cover. */
void console_fb_repaint(int *x, int *y, int *w, int *h);
/* Queue text as if it had been typed. */
void console_inject(const char *s);
void console_clear(void);
void console_set_attr(u8 attr);
void console_set_region(int top_row, int bottom_row);
void vga_fill_row(int row, char c, u8 attr);
void vga_write_at(int row, int col, const char *s, u8 attr);

#endif /* MERGEDOS_CONSOLE_H */
