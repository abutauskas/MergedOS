/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_UI_H
#define MERGEDOS_UI_H

/*
 * Minimal widget toolkit: label, button, text box, checkbox, list.
 *
 * Portable and allocation-free: a screen is a fixed array of widgets, input
 * arrives as events, and drawing goes through two functions the host of the
 * toolkit provides (the kernel maps them to the framebuffer; unit tests
 * record them). Only widgets marked dirty are redrawn.
 */
#include <mergedos/compat.h>

/* ---- drawing back end: implemented by whoever links the toolkit ----------- */
void ui_draw_fill(int x, int y, int w, int h, u32 rgb);
void ui_draw_text(int x, int y, const char *s, u32 rgb, int scale);
void ui_draw_blend(int x, int y, u32 rgb, u8 alpha);     /* one pixel, alpha 0..255 */

#define UI_GLYPH_W 6            /* advance of one character at scale 1 */
#define UI_GLYPH_H 7

/* ---- text: UTF-8 in, glyphs out (text.c) ----------------------------------- */
#define FONT_W 5
#define FONT_H 7
extern const u8 font5x7[95][FONT_W];    /* ASCII 32..126, column-major, bit 0 = top */

enum ui_mark {
	UI_MARK_NONE = 0, UI_MARK_GRAVE, UI_MARK_ACUTE, UI_MARK_CIRCUMFLEX, UI_MARK_TILDE,
	UI_MARK_DIAERESIS, UI_MARK_RING, UI_MARK_CEDILLA
};

enum ui_special {
	UI_SP_BOX = 0,          /* drawn for code points without a glyph */
	UI_SP_AE, UI_SP_OSLASH, UI_SP_SHARP_S, UI_SP_ae, UI_SP_oslash, UI_SP_IQUEST, UI_SP_IEXCL,
	UI_SPECIAL_COUNT
};
extern const u8 ui_special_glyphs[UI_SPECIAL_COUNT][FONT_W];

/* How to draw one code point: an ASCII base (or a special shape) plus a diacritic. */
struct ui_glyph {
	u8 base;                /* ASCII 32..126, or 0: use `special` */
	u8 special;
	enum ui_mark mark;
	bool tall;              /* capital base: the mark goes above the cell */
};

u32  utf8_next(const char **sp);        /* decode one code point; U+FFFD if malformed */
int  utf8_encode(u32 cp, char out[4]);  /* returns the byte count */
int  ui_text_cells(const char *s);      /* characters, i.e. cells on screen */
bool ui_glyph_lookup(u32 cp, struct ui_glyph *g);       /* false: no glyph, box used */

/* ---- anti-aliasing (aa.c) --------------------------------------------------- */
#define UI_RASTER_ROWS 10       /* dot rows per character: 2 above, 7 letter, 1 below */
#define UI_MAX_SCALE   4
/*
 * Anti-aliased coverage (0..255) of one character at `scale`, into a buffer
 * of (UI_GLYPH_W * scale) x (UI_RASTER_ROWS * scale) bytes. The letter's
 * top row starts 2 * scale pixels down.
 */
void ui_glyph_coverage(const struct ui_glyph *g, int scale, u8 *out);
void ui_fill_round(int x, int y, int w, int h, int r, u32 rgb);

/* Keys are Unicode code points, or these (same values as console.h KEY_*). */
#define UI_KEY_UP    0x110001   /* above U+10FFFF: cannot collide with a character */
#define UI_KEY_DOWN  0x110002
#define UI_KEY_LEFT  0x110003
#define UI_KEY_RIGHT 0x110004

/* Theme: one palette for the installer and, later, the desktop shells. */
#define UI_COL_DESKTOP  0x1B2430
#define UI_COL_PANEL    0xF2F4F7
#define UI_COL_TEXT     0x1C2733
#define UI_COL_MUTED    0x6B7785
#define UI_COL_ACCENT   0x148F8F
#define UI_COL_BORDER   0x9AA5B1
#define UI_COL_DISABLED 0xD3D8DE
#define UI_COL_FIELD    0xFFFFFF
#define UI_COL_ERROR    0xC0392B

enum ui_kind { UI_LABEL, UI_BUTTON, UI_TEXTBOX, UI_CHECKBOX, UI_LIST };

#define UI_TEXT_MAX    40
#define UI_LIST_MAX    8
#define UI_LIST_ROW_H  28
#define UI_MAX_WIDGETS 16

struct ui_widget {
	enum ui_kind kind;
	int id;
	int x, y, w, h;
	const char *label;
	bool disabled, hidden, dirty;
	bool pressed;                           /* button: mouse is held on it */
	bool checked;                           /* checkbox                    */
	bool masked;                            /* text box: show '*'          */
	char text[UI_TEXT_MAX + 1];             /* text box contents, UTF-8    */
	int len, max_len;                       /* bytes used; character limit */
	const char *items[UI_LIST_MAX];         /* list                        */
	int nitems, selected;
	u32 color;                              /* label                       */
	int scale;
};

struct ui_screen {
	struct ui_widget w[UI_MAX_WIDGETS];
	int count;
	int focus;                              /* widget index, or -1                 */
	int default_id;                         /* button that Enter activates, or -1  */
};

enum ui_event_type { UI_EV_MOUSE_DOWN, UI_EV_MOUSE_UP, UI_EV_KEY };

struct ui_event {
	enum ui_event_type type;
	int x, y;                               /* mouse events */
	int key;                                /* ASCII or UI_KEY_* */
};

void ui_screen_init(struct ui_screen *s);
struct ui_widget *ui_add(struct ui_screen *s, enum ui_kind kind, int id, int x, int y, int w,
			 int h, const char *label);
struct ui_widget *ui_find(struct ui_screen *s, int id);

/* Topmost enabled, visible, interactive widget at a point; index or -1. */
int ui_hit(const struct ui_screen *s, int x, int y);

void ui_set_focus(struct ui_screen *s, int index);
void ui_focus_next(struct ui_screen *s);        /* Tab order = order of ui_add() */

/*
 * Feed one event. Returns the id of the widget the user acted on (button
 * activated, checkbox toggled, list selection changed), or -1.
 */
int ui_dispatch(struct ui_screen *s, const struct ui_event *ev);

/* Draw every widget (all = true) or only those whose state changed. */
void ui_draw(struct ui_screen *s, bool all);

/* The MergedOS mark: gradient hexagon with four unequal squares. 16 x 16 units. */
void ui_logo(int x, int y, int unit);

#endif /* MERGEDOS_UI_H */
