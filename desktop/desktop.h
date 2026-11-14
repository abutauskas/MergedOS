/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_DESKTOP_H
#define MERGEDOS_DESKTOP_H

/*
 * Graphical desktop shell: a small window manager for the four layouts.
 * Portable: geometry, z-order, hit-testing and clipped drawing through the
 * ui/ back end; it knows nothing about the kernel and never allocates. The
 * host (kernel/deskrun.c) owns the mouse, the console that fills the
 * terminal window, and the text shown in the other windows.
 *
 * Up to DESK_MAX_WINDOWS windows. z[] lists window slots back to front; the
 * focused window is the topmost visible one. Every change records the
 * screen areas it spoiled in `damage` (and `bar_dirty` for the running-
 * window strip); the host repaints that with desktop_paint() and clears it.
 */
#include "de_selector/de_selector.h"
#include "ui/ui.h"

enum desk_action {
	DESK_NONE = 0,
	DESK_TERMINAL, DESK_FILES, DESK_PACKAGES, DESK_SYSTEM, DESK_HELP, DESK_POWER,
	DESK_FENCE,                     /* Mergedfence status */
};

enum desk_kind { DESK_WIN_TERMINAL, DESK_WIN_FILES, DESK_WIN_SYSTEM, DESK_WIN_FENCE,
		 DESK_WIN_VIEWER,              /* file viewer: tab strip + text or image */
		 DESK_WIN_APP,                 /* client pixels (host app_paint): /dev/mwm */
		 DESK_KINDS, DESK_WIN_NONE = -1 };
enum desk_state { DESK_NORMAL, DESK_MINIMISED, DESK_MAXIMISED };
enum desk_part {
	DESK_PART_NONE = 0,
	DESK_PART_LAUNCHER,             /* hit.action says which              */
	DESK_PART_TASK,                 /* running-window strip: hit.win      */
	DESK_PART_TITLE, DESK_PART_CLOSE, DESK_PART_MINIMISE, DESK_PART_MAXIMISE,
	DESK_PART_CONTENT,              /* hit.row: text line under the point */
	DESK_PART_BUTTON,               /* the window's push button           */
	DESK_PART_TAB,                  /* viewer tab strip: hit.row = slot   */
};

struct desk_rect {
	int x, y, w, h;
};

#define DESK_ITEMS       7
#define DESK_MAX_WINDOWS 6
#define DESK_DAMAGE_MAX  4
#define DESK_LINE_BUTTON (-1)       /* line(): label of the window's button */
#define DESK_LINE_TITLE  (-2)       /* line(): title override (NULL: default) */
#define DESK_TABS        6          /* viewer tab-strip slots */

struct desk_item {
	struct desk_rect r;
	enum desk_action action;
};

struct desk_window {
	int id;                         /* 0: free slot */
	enum desk_kind kind;
	enum desk_state state;
	enum desk_state prev;           /* state to restore from minimised */
	struct desk_rect frame;         /* border included              */
	struct desk_rect saved;         /* frame to restore after maximise */
};

struct desktop {
	enum de_id layout;
	int sw, sh;
	int term_w, term_h;             /* size of the text console          */
	int scale;                      /* text scale inside windows         */
	struct desk_rect work;          /* where windows may be              */
	struct desk_rect bar;           /* taskbar / menu bar / top bar      */
	struct desk_rect dock;          /* dock layout only, else empty      */
	struct desk_rect status;        /* text refreshed by the host        */
	struct desk_rect tasks;         /* running-window strip, in the bar  */
	struct desk_item item[DESK_ITEMS];
	bool movable, tiled;
	struct desk_window win[DESK_MAX_WINDOWS];
	int z[DESK_MAX_WINDOWS];        /* slot indices, back to front */
	int nz;
	int next_id;
	struct desk_rect damage[DESK_DAMAGE_MAX];       /* to repaint */
	int ndamage;
	bool bar_dirty;
	char status_text[48];
};

/*
 * What the host supplies for painting. line(): text line i of a window of
 * that kind (NULL ends the list), or with DESK_LINE_BUTTON the button label
 * (NULL: no button). term_paint(): repaint the console inside `clip`; it
 * returns the area it actually painted (whole cells), which may be larger.
 * Either may be NULL (self-checks, tests).
 */
/* An RGB image (3 bytes per pixel, rows top to bottom) a viewer window shows. */
struct desk_image {
	const u8 *rgb;
	int w, h;
};

/*
 * Viewer windows: tab() labels tab-strip slot i (NULL: slot unused) and says
 * whether it is the active one; image() supplies the picture to draw scaled
 * to fit instead of text lines (false: show line() text). line(ctx, kind, 0)
 * is the caption under an image.
 */
struct desk_host {
	void *ctx;
	const char *(*line)(void *ctx, enum desk_kind kind, int i);
	struct desk_rect (*term_paint)(void *ctx, struct desk_rect clip);
	const char *(*tab)(void *ctx, int i, bool *active);
	bool (*image)(void *ctx, struct desk_image *img);
	/* DESK_WIN_APP: title of window `win`, and its pixels inside `content` (clip: only draw there). */
	const char *(*title)(void *ctx, int win);
	void (*app_paint)(void *ctx, int win, struct desk_rect content, struct desk_rect clip);
};

struct desk_hit {
	enum desk_part part;
	int win;                        /* window id, 0 if none            */
	enum desk_action action;        /* launcher, DESK_NONE otherwise   */
	int row;                        /* content line, -1 if none        */
};

/* Layout with one terminal window (id 1) whose content is the console size. */
void desktop_layout(struct desktop *d, enum de_id layout, int sw, int sh, int term_w,
		    int term_h);

/* ---- window model ---------------------------------------------------------- */
/* New window on top; returns its id, or 0 when all slots are taken (or a
 * second terminal is asked for). */
int  desktop_open(struct desktop *d, enum desk_kind kind);
/* DESK_WIN_APP window whose content area is cw x ch (clamped to the work area); 0 if full. */
int  desktop_open_app(struct desktop *d, int cw, int ch);
bool desktop_set_content(struct desktop *d, int id, int cw, int ch);
/* Launcher semantics: focus (and restore) the first window of that kind,
 * opening one if there is none. *opened tells which happened. */
int  desktop_launch(struct desktop *d, enum desk_kind kind, bool *opened);
/* Close; the terminal is minimised instead. True if the window went away. */
bool desktop_close(struct desktop *d, int id);
bool desktop_focus(struct desktop *d, int id);          /* raise; restores if minimised */
bool desktop_minimise(struct desktop *d, int id);
bool desktop_toggle_max(struct desktop *d, int id);
/* Move by a delta, keeping the frame inside the work area. True if it moved. */
bool desktop_move(struct desktop *d, int id, int dx, int dy);

struct desk_window *desktop_window(struct desktop *d, int id);
const struct desk_window *desktop_find_kind(const struct desktop *d, enum desk_kind kind);
int  desktop_top(const struct desktop *d);              /* focused window id, or 0 */
bool desktop_visible(const struct desk_window *w);
/* True if a visible window above `id` overlaps it. */
bool desktop_covered(const struct desktop *d, int id);

struct desk_rect desktop_title_rect(const struct desktop *d, const struct desk_window *w);
struct desk_rect desktop_content_rect(const struct desktop *d, const struct desk_window *w);
/* Viewer: tab-strip slot i, and how many text lines fit below the strip. */
struct desk_rect desktop_tab_rect(const struct desktop *d, const struct desk_window *w, int i);
int  desktop_viewer_rows(const struct desktop *d, const struct desk_window *w);
/* Text line `row` of a Files/System/Mergedfence window (what desktop_hit() maps back). */
struct desk_rect desktop_row_rect(const struct desktop *d, const struct desk_window *w, int row);
/* Frame moved by a delta and clamped to the work area (the drag outline). */
struct desk_rect desktop_moved_frame(const struct desktop *d, const struct desk_window *w,
				     int dx, int dy);

struct desk_hit desktop_hit(const struct desktop *d, int x, int y);
enum desk_kind desktop_launcher_kind(enum desk_action a);
const char *desktop_kind_name(enum desk_kind k);

/* ---- drawing ---------------------------------------------------------------- */
/* Repaint everything inside `clip` back to front; draws nothing outside. */
void desktop_paint(struct desktop *d, struct desk_rect clip, const struct desk_host *h);
void desktop_draw(struct desktop *d, const struct desk_host *h);        /* whole screen */
void desktop_draw_status(struct desktop *d, const char *text);
/* The 2-pixel drag outline, and the four strips that erase it again. */
void desktop_draw_outline(const struct desktop *d, struct desk_rect r);
void desktop_outline_strips(struct desk_rect r, struct desk_rect out[4]);

/* Shell command a launcher types, or NULL. */
const char *desktop_command(enum desk_action a);
const char *desktop_action_name(enum desk_action a);
/* VGA attribute (fg | bg << 4) the terminal uses in this layout. */
u8 desktop_console_attr(enum de_id layout);

/* rect helpers */
bool desk_rect_empty(struct desk_rect r);
struct desk_rect desk_rect_and(struct desk_rect a, struct desk_rect b);
struct desk_rect desk_rect_union(struct desk_rect a, struct desk_rect b);       /* bounding box */

/* Geometry, window-model and hit-test checks for all layouts; returns failed checks. */
int desktop_selfcheck(int sw, int sh, int term_w, int term_h, bool draw);

#endif /* MERGEDOS_DESKTOP_H */
