/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_GFX_H
#define MERGEDOS_GFX_H

/*
 * Linear-framebuffer graphics and pointer input.
 *
 * Deliberately small: no back buffer, no allocation. Drawing goes straight
 * to the framebuffer; the only saved pixels are the few under the pointer.
 * Colours are 0xRRGGBB.
 */
#include <mergedos/types.h>
#include <mergedos/boot.h>
#include "hardware_probe/hwprobe.h"
#include "ui/ui.h"

#define GFX_DEFAULT_WIDTH  1024
#define GFX_DEFAULT_HEIGHT 768

struct gfx_info {
	bool active;
	u32 width, height, pitch, bpp;
	const char *source;             /* who set the mode */
};

/* Mode set by the boot loader (GRUB honours the request in the Multiboot header). */
bool gfx_init_bootfb(const struct boot_info *bi);
/* Bochs/QEMU "dispi" interface: used when the loader set no mode (QEMU -kernel). */
bool gfx_init_bochs(const struct hw_profile *hw);
const struct gfx_info *gfx_get(void);

void gfx_fill(int x, int y, int w, int h, u32 rgb);
void gfx_blend(int x, int y, u32 rgb, u8 alpha);         /* one pixel, alpha 0..255 */
void gfx_glyph(int x, int y, char c, u32 rgb, int scale);              /* ASCII only */
void gfx_text(int x, int y, const char *s, u32 rgb, int scale);        /* UTF-8 */
u32  gfx_read(int x, int y);
int  gfx_selftest(void);                /* 0 = framebuffer reads back what is written */

/* Pointer sprite. Drawing primitives lift it automatically; refresh puts it back. */
void gfx_cursor_set(int x, int y);
void gfx_cursor_refresh(void);

/* Font metrics, UTF-8 decoding and glyph lookup are portable: see ui/ui.h. */

/* ---- PS/2 mouse (mouse.c), polled through console_poll() ------------------ */
#define MOUSE_LEFT   0x1
#define MOUSE_RIGHT  0x2
#define MOUSE_MIDDLE 0x4

struct mouse_state {
	bool present;
	int x, y;
	u8 buttons;
	u32 packets;
	/* Left-button edges, counted so a click between two polls is never lost. */
	u32 presses, releases;
	int press_x, press_y, release_x, release_y;
};

void mouse_init(void);
void mouse_feed(u8 byte);               /* one byte from the controller's aux port */
const struct mouse_state *mouse_get(void);

/* console.c: render the text console into the framebuffer from now on. */
void console_use_framebuffer(void);

#endif /* MERGEDOS_GFX_H */
