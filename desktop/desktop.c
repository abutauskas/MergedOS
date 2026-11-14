/* SPDX-License-Identifier: BSD-2-Clause */
#include "desktop/desktop.h"

struct theme {
	u32 bg_top, bg_bottom;          /* wallpaper: vertical gradient      */
	u32 bar, bar_text;
	u32 title, title_text, border;
	u32 title_idle;                 /* title bar of an unfocused window  */
	u32 panel, panel_text;          /* content of non-terminal windows   */
	u32 term_bg;                    /* terminal area the console leaves  */
	u32 accent;                     /* buttons, drag outline             */
	int bar_h, title_h, icon;
	u8 console_attr;
};

static const struct theme themes[DE_COUNT] = {
	[DE_TASKBAR] = { 0x2A5078, 0x10223A, 0x1B2430, 0xFFFFFF, 0x2D3A4A, 0xFFFFFF, 0x0E141B,
			 0x56616E, 0xF2F4F7, 0x1C2733, 0x000000, 0x3A86D4, 44, 28, 32, 0x07 },
	[DE_DOCK]    = { 0x6A5AA0, 0x1C2942, 0xECEFF4, 0x1C2733, 0xE4E7EC, 0x1C2733, 0x9AA5B1,
			 0xF6F7F9, 0xFFFFFF, 0x1C2733, 0x000000, 0x2F7BF6, 28, 28, 40, 0x0F },
	[DE_TILING]  = { 0x101418, 0x101418, 0x12301F, 0x7CF29C, 0x1F6F4A, 0xFFFFFF, 0x2ECC71,
			 0x1B2A22, 0x0B0F12, 0x7CF29C, 0x000000, 0x2ECC71, 26, 22, 18, 0x0A },
	[DE_RETRO]   = { 0xAAAAAA, 0xAAAAAA, 0x0000AA, 0xFFFFFF, 0x0000AA, 0xFFFFFF, 0x0000AA,
			 0x555555, 0xFFFFFF, 0x000000, 0xAAAAAA, 0xAA0000, 26, 24, 18, 0x71 },
};

static const enum desk_action launchers[DESK_ITEMS] = {
	DESK_TERMINAL, DESK_FILES, DESK_PACKAGES, DESK_SYSTEM, DESK_HELP, DESK_FENCE, DESK_POWER,
};

u8 desktop_console_attr(enum de_id layout)
{
	return themes[layout].console_attr;
}

const char *desktop_command(enum desk_action a)
{
	switch (a) {
	case DESK_FILES:    return "ls /";      /* typed as well as opening the window */
	case DESK_PACKAGES: return "ls /pkg";
	case DESK_HELP:     return "help";
	case DESK_POWER:    return "poweroff";
	default:            return NULL;
	}
}

const char *desktop_action_name(enum desk_action a)
{
	static const char *const names[] = { "none", "terminal", "files", "packages", "system",
					     "help", "power", "fence" };

	return names[a];
}

enum desk_kind desktop_launcher_kind(enum desk_action a)
{
	switch (a) {
	case DESK_TERMINAL: return DESK_WIN_TERMINAL;
	case DESK_FILES:    return DESK_WIN_FILES;
	case DESK_SYSTEM:   return DESK_WIN_SYSTEM;
	case DESK_FENCE:    return DESK_WIN_FENCE;
	default:            return DESK_WIN_NONE;
	}
}

const char *desktop_kind_name(enum desk_kind k)
{
	static const char *const names[DESK_KINDS] = { "terminal", "files", "system", "fence",
						       "viewer", "app" };

	return k >= 0 && k < DESK_KINDS ? names[k] : "none";
}

static const char *kind_title(enum desk_kind k)
{
	static const char *const titles[DESK_KINDS] = { "Terminal - msh", "Files", "System",
							"Mergedfence", "Viewer", "Window" };

	return titles[k];
}

static enum desk_action kind_launcher(enum desk_kind k)
{
	static const enum desk_action a[DESK_KINDS] = { DESK_TERMINAL, DESK_FILES, DESK_SYSTEM,
							 DESK_FENCE, DESK_NONE, DESK_NONE };

	return a[k];
}

/* ---- rectangles ------------------------------------------------------------- */

bool desk_rect_empty(struct desk_rect r)
{
	return r.w <= 0 || r.h <= 0;
}

struct desk_rect desk_rect_and(struct desk_rect a, struct desk_rect b)
{
	int x0 = MOS_MAX(a.x, b.x), y0 = MOS_MAX(a.y, b.y);
	int x1 = MOS_MIN(a.x + a.w, b.x + b.w), y1 = MOS_MIN(a.y + a.h, b.y + b.h);

	if (x1 <= x0 || y1 <= y0)
		return (struct desk_rect){ 0, 0, 0, 0 };
	return (struct desk_rect){ x0, y0, x1 - x0, y1 - y0 };
}

struct desk_rect desk_rect_union(struct desk_rect a, struct desk_rect b)
{
	int x0, y0, x1, y1;

	if (desk_rect_empty(a))
		return b;
	if (desk_rect_empty(b))
		return a;
	x0 = MOS_MIN(a.x, b.x);
	y0 = MOS_MIN(a.y, b.y);
	x1 = MOS_MAX(a.x + a.w, b.x + b.w);
	y1 = MOS_MAX(a.y + a.h, b.y + b.h);
	return (struct desk_rect){ x0, y0, x1 - x0, y1 - y0 };
}

static bool inside(const struct desk_rect *r, int x, int y)
{
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static bool within(const struct desk_rect *in, const struct desk_rect *out)
{
	return in->x >= out->x && in->y >= out->y && in->x + in->w <= out->x + out->w &&
	       in->y + in->h <= out->y + out->h;
}

static bool overlap(struct desk_rect a, struct desk_rect b)
{
	return !desk_rect_empty(desk_rect_and(a, b));
}

/* ---- window geometry ---------------------------------------------------------- */

static int line_h(const struct desktop *d)
{
	return 12 * d->scale;
}

static int pad(const struct desktop *d)
{
	return 4 * d->scale + 4;
}

struct desk_rect desktop_title_rect(const struct desktop *d, const struct desk_window *w)
{
	const struct desk_rect *f = &w->frame;

	return (struct desk_rect){ f->x + 1, f->y + 1, f->w - 2, themes[d->layout].title_h };
}

struct desk_rect desktop_content_rect(const struct desktop *d, const struct desk_window *w)
{
	const struct desk_rect *f = &w->frame;
	const int th = themes[d->layout].title_h;

	return (struct desk_rect){ f->x + 1, f->y + 1 + th, f->w - 2, f->h - th - 2 };
}

/* The push button at the bottom of a Mergedfence window. */
static struct desk_rect button_rect(const struct desktop *d, const struct desk_window *w)
{
	struct desk_rect c = desktop_content_rect(d, w);
	const int bh = 12 * d->scale + 8, bw = 14 * UI_GLYPH_W * d->scale;

	if (w->kind != DESK_WIN_FENCE)
		return (struct desk_rect){ 0, 0, 0, 0 };
	return (struct desk_rect){ c.x + pad(d), c.y + c.h - pad(d) - bh, bw, bh };
}

/*
 * Viewer text scale: the window scale, or 1 when an 80-column hex dump would
 * not fit at that scale.
 */
static int viewer_scale(const struct desktop *d, const struct desk_rect *c)
{
	return c->w - 2 * pad(d) >= 80 * UI_GLYPH_W * d->scale ? d->scale : 1;
}

static int strip_h(const struct desktop *d)
{
	return 12 * d->scale + 6;
}

struct desk_rect desktop_tab_rect(const struct desktop *d, const struct desk_window *w, int i)
{
	const struct desk_rect c = desktop_content_rect(d, w);
	const int tw = 6 * UI_GLYPH_W * d->scale + 8;

	if (w->kind != DESK_WIN_VIEWER || i < 0 || i >= DESK_TABS)
		return (struct desk_rect){ 0, 0, 0, 0 };
	return (struct desk_rect){ c.x + pad(d) + i * (tw + 4), c.y + 3, tw, strip_h(d) - 2 };
}

int desktop_viewer_rows(const struct desktop *d, const struct desk_window *w)
{
	const struct desk_rect c = desktop_content_rect(d, w);
	const int vs = viewer_scale(d, &c);

	return MOS_MAX(1, (c.h - strip_h(d) - 3 - 2 * pad(d)) / (12 * vs));
}

struct desk_rect desktop_row_rect(const struct desktop *d, const struct desk_window *w, int row)
{
	const struct desk_rect c = desktop_content_rect(d, w);

	return (struct desk_rect){ c.x + pad(d), c.y + pad(d) + row * line_h(d), c.w - 2 * pad(d),
				   line_h(d) };
}

/* Close, minimise and maximise boxes; empty where the layout has none. */
static struct desk_rect control_rect(const struct desktop *d, const struct desk_window *w,
				     enum desk_part part)
{
	const struct desk_rect t = desktop_title_rect(d, w);
	const int cy = t.y + t.h / 2;
	const struct desk_rect none = { 0, 0, 0, 0 };

	switch (d->layout) {
	case DE_DOCK: {                         /* three discs on the left */
		int i = part == DESK_PART_CLOSE ? 0 : part == DESK_PART_MINIMISE ? 1 : 2;

		return (struct desk_rect){ t.x + 8 + i * 20, cy - 8, 16, 16 };
	}
	case DE_TASKBAR: {                      /* minimise, maximise, close on the right */
		int x = t.x + t.w - 3 * 26;
		int i = part == DESK_PART_MINIMISE ? 0 : part == DESK_PART_MAXIMISE ? 1 : 2;

		return (struct desk_rect){ x + i * 26, t.y, 26, t.h };
	}
	case DE_RETRO:                          /* one box on the left: close */
		return part == DESK_PART_CLOSE ? (struct desk_rect){ t.x + 4, cy - 8, 16, 16 } : none;
	default:                                /* tiles have no controls */
		return none;
	}
}

static struct desk_rect default_frame(const struct desktop *d, enum desk_kind kind)
{
	static const int chars[DESK_KINDS] = { 0, 42, 48, 38, 80 };
	static const int lines[DESK_KINDS] = { 0, 16, 12, 7, 22 };
	const int th = themes[d->layout].title_h;
	int cw, ch;

	if (kind == DESK_WIN_TERMINAL) {
		cw = d->term_w;
		ch = d->term_h;
	} else {
		cw = chars[kind] * UI_GLYPH_W * d->scale + 2 * pad(d);
		ch = lines[kind] * line_h(d) + 2 * pad(d);
		if (kind == DESK_WIN_FENCE)
			ch += 12 * d->scale + 8 + pad(d);
	}
	return (struct desk_rect){ 0, 0, MOS_MIN(cw + 2, d->work.w), MOS_MIN(ch + th + 2, d->work.h) };
}

static struct desk_rect clamp_frame(const struct desktop *d, struct desk_rect f)
{
	f.x = MOS_MAX(d->work.x, MOS_MIN(f.x, d->work.x + d->work.w - f.w));
	f.y = MOS_MAX(d->work.y, MOS_MIN(f.y, d->work.y + d->work.h - f.h));
	return f;
}

/* ---- window model ------------------------------------------------------------- */

static void damage(struct desktop *d, struct desk_rect r)
{
	r = desk_rect_and(r, (struct desk_rect){ 0, 0, d->sw, d->sh });
	if (desk_rect_empty(r))
		return;
	for (int i = 0; i < d->ndamage; i++)
		if (within(&r, &d->damage[i]))
			return;
	if (d->ndamage < DESK_DAMAGE_MAX)
		d->damage[d->ndamage++] = r;
	else
		d->damage[DESK_DAMAGE_MAX - 1] = desk_rect_union(d->damage[DESK_DAMAGE_MAX - 1], r);
}

bool desktop_visible(const struct desk_window *w)
{
	return w && w->id && w->state != DESK_MINIMISED;
}

static int slot_of(const struct desktop *d, int id)
{
	for (int i = 0; id && i < DESK_MAX_WINDOWS; i++)
		if (d->win[i].id == id)
			return i;
	return -1;
}

struct desk_window *desktop_window(struct desktop *d, int id)
{
	int s = slot_of(d, id);

	return s < 0 ? NULL : &d->win[s];
}

const struct desk_window *desktop_find_kind(const struct desktop *d, enum desk_kind kind)
{
	for (int i = 0; i < DESK_MAX_WINDOWS; i++)
		if (d->win[i].id && d->win[i].kind == kind)
			return &d->win[i];
	return NULL;
}

int desktop_top(const struct desktop *d)
{
	if (d->nz && desktop_visible(&d->win[d->z[d->nz - 1]]))
		return d->win[d->z[d->nz - 1]].id;
	return 0;
}

static int z_index(const struct desktop *d, int slot)
{
	for (int i = 0; i < d->nz; i++)
		if (d->z[i] == slot)
			return i;
	return -1;
}

bool desktop_covered(const struct desktop *d, int id)
{
	int s = slot_of(d, id), zi = s < 0 ? -1 : z_index(d, s);

	if (zi < 0 || !desktop_visible(&d->win[s]))
		return false;
	for (int i = zi + 1; i < d->nz; i++)
		if (desktop_visible(&d->win[d->z[i]]) &&
		    overlap(d->win[d->z[i]].frame, d->win[s].frame))
			return true;
	return false;
}

static void z_remove(struct desktop *d, int slot)
{
	int zi = z_index(d, slot);

	if (zi < 0)
		return;
	for (int i = zi; i + 1 < d->nz; i++)
		d->z[i] = d->z[i + 1];
	d->nz--;
}

/* The focused window changed: its title bar (and the strip) need a repaint. */
static void focus_changed(struct desktop *d, int old_top)
{
	struct desk_window *w = desktop_window(d, old_top);

	if (w && desktop_visible(w))
		damage(d, desktop_title_rect(d, w));
	w = desktop_window(d, desktop_top(d));
	if (w)
		damage(d, desktop_title_rect(d, w));
	d->bar_dirty = true;
}

/* Tiling: 1 window fills the work area, 2 split it, 3+ are a master and a stack. */
static void retile(struct desktop *d)
{
	const struct desk_rect wk = d->work;
	int n = 0, i = 0;

	for (int s = 0; s < DESK_MAX_WINDOWS; s++)
		n += desktop_visible(&d->win[s]);
	for (int s = 0; s < DESK_MAX_WINDOWS; s++) {
		struct desk_window *w = &d->win[s];

		if (!desktop_visible(w))
			continue;
		if (n == 1)
			w->frame = wk;
		else if (i == 0)
			w->frame = (struct desk_rect){ wk.x, wk.y, wk.w / 2, wk.h };
		else if (n == 2)
			w->frame = (struct desk_rect){ wk.x + wk.w / 2, wk.y, wk.w - wk.w / 2, wk.h };
		else
			w->frame = (struct desk_rect){ wk.x + wk.w / 2, wk.y + wk.h * (i - 1) / (n - 1),
						       wk.w - wk.w / 2,
						       wk.h * i / (n - 1) - wk.h * (i - 1) / (n - 1) };
		w->saved = w->frame;
		i++;
	}
	damage(d, wk);
	d->bar_dirty = true;
}

int desktop_open(struct desktop *d, enum desk_kind kind)
{
	int slot = -1, old_top = desktop_top(d), cascade = 0;
	struct desk_window *w;

	if (kind < 0 || kind >= DESK_KINDS ||
	    (kind == DESK_WIN_TERMINAL && desktop_find_kind(d, DESK_WIN_TERMINAL)))
		return 0;
	for (int i = 0; i < DESK_MAX_WINDOWS; i++) {
		if (!d->win[i].id && slot < 0)
			slot = i;
		cascade += d->win[i].id && d->win[i].kind != DESK_WIN_TERMINAL;
	}
	if (slot < 0)
		return 0;
	w = &d->win[slot];
	w->id = d->next_id++;
	w->kind = kind;
	w->state = DESK_NORMAL;
	w->frame = default_frame(d, kind);
	if (kind == DESK_WIN_TERMINAL) {        /* centred */
		w->frame.x = d->work.x + (d->work.w - w->frame.w) / 2;
		w->frame.y = d->work.y + (d->work.h - w->frame.h) / 2;
	} else {                                /* cascade from the top left */
		w->frame.x = d->work.x + 24 + 28 * cascade;
		w->frame.y = d->work.y + 16 + 28 * cascade;
	}
	w->frame = clamp_frame(d, w->frame);
	w->saved = w->frame;
	d->z[d->nz++] = slot;
	if (d->tiled)
		retile(d);
	damage(d, w->frame);
	focus_changed(d, old_top);
	return w->id;
}

bool desktop_focus(struct desktop *d, int id)
{
	int s = slot_of(d, id), old_top = desktop_top(d);
	struct desk_window *w;

	if (s < 0)
		return false;
	w = &d->win[s];
	if (w->state == DESK_MINIMISED) {
		w->state = d->tiled ? DESK_NORMAL : w->prev;
		if (d->tiled)
			retile(d);
	} else if (old_top == id) {
		return false;
	}
	z_remove(d, s);
	d->z[d->nz++] = s;
	damage(d, w->frame);
	focus_changed(d, old_top);
	return true;
}

bool desktop_minimise(struct desktop *d, int id)
{
	int s = slot_of(d, id), old_top = desktop_top(d);
	struct desk_window *w;

	if (s < 0 || d->win[s].state == DESK_MINIMISED)
		return false;
	w = &d->win[s];
	w->prev = w->state;
	w->state = DESK_MINIMISED;
	z_remove(d, s);                         /* to the bottom of the stack */
	for (int i = d->nz; i > 0; i--)
		d->z[i] = d->z[i - 1];
	d->z[0] = s;
	d->nz++;
	damage(d, w->frame);
	if (d->tiled)
		retile(d);
	focus_changed(d, old_top);
	return true;
}

bool desktop_set_content(struct desktop *d, int id, int cw, int ch)
{
	struct desk_window *w = desktop_window(d, id);
	struct desk_rect old, nf;

	if (!w || cw <= 0 || ch <= 0)
		return false;
	old = w->frame;
	nf = old;
	nf.w = MOS_MIN(cw + 2, d->work.w);
	nf.h = MOS_MIN(ch + themes[d->layout].title_h + 2, d->work.h);
	nf = clamp_frame(d, nf);
	w->frame = nf;
	w->saved = nf;
	damage(d, desk_rect_union(old, nf));
	return true;
}

int desktop_open_app(struct desktop *d, int cw, int ch)
{
	const int id = desktop_open(d, DESK_WIN_APP);

	if (id && !desktop_set_content(d, id, cw, ch)) {
		(void)desktop_close(d, id);
		return 0;
	}
	return id;
}

bool desktop_close(struct desktop *d, int id)
{
	int s = slot_of(d, id), old_top = desktop_top(d);
	struct desk_window *w;

	if (s < 0)
		return false;
	w = &d->win[s];
	if (w->kind == DESK_WIN_TERMINAL) {     /* the shell always keeps its window */
		(void)desktop_minimise(d, id);
		return false;
	}
	if (w->state != DESK_MINIMISED)
		damage(d, w->frame);
	z_remove(d, s);
	memset(w, 0, sizeof(*w));
	if (d->tiled)
		retile(d);
	focus_changed(d, old_top);
	return true;
}

bool desktop_toggle_max(struct desktop *d, int id)
{
	struct desk_window *w = desktop_window(d, id);
	struct desk_rect old;

	if (!w || d->tiled || w->state == DESK_MINIMISED)
		return false;
	old = w->frame;
	if (w->state == DESK_MAXIMISED) {
		w->frame = w->saved;
		w->state = DESK_NORMAL;
	} else {
		w->saved = w->frame;
		w->frame = d->work;
		w->state = DESK_MAXIMISED;
	}
	damage(d, old);
	damage(d, w->frame);
	(void)desktop_focus(d, id);
	return true;
}

struct desk_rect desktop_moved_frame(const struct desktop *d, const struct desk_window *w,
				     int dx, int dy)
{
	struct desk_rect f = w->frame;

	f.x += dx;
	f.y += dy;
	return clamp_frame(d, f);
}

bool desktop_move(struct desktop *d, int id, int dx, int dy)
{
	struct desk_window *w = desktop_window(d, id);
	struct desk_rect f;

	if (!w || !d->movable || w->state != DESK_NORMAL)
		return false;
	f = desktop_moved_frame(d, w, dx, dy);
	if (f.x == w->frame.x && f.y == w->frame.y)
		return false;
	damage(d, w->frame);
	w->frame = f;
	w->saved = f;
	damage(d, f);
	return true;
}

int desktop_launch(struct desktop *d, enum desk_kind kind, bool *opened)
{
	const struct desk_window *w = desktop_find_kind(d, kind);

	*opened = false;
	if (w) {
		(void)desktop_focus(d, w->id);
		return w->id;
	}
	*opened = true;
	return desktop_open(d, kind);
}

/* ---- layout ------------------------------------------------------------------- */

/* The running-window strip: one button per open window, in slot order. */
static int open_count(const struct desktop *d)
{
	int n = 0;

	for (int i = 0; i < DESK_MAX_WINDOWS; i++)
		n += d->win[i].id != 0;
	return n;
}

static struct desk_rect task_rect(const struct desktop *d, int nth, int n)
{
	int bw;

	if (desk_rect_empty(d->tasks) || n <= 0)
		return (struct desk_rect){ 0, 0, 0, 0 };
	bw = MOS_MIN(150, d->tasks.w / n);
	return (struct desk_rect){ d->tasks.x + nth * bw, d->tasks.y, bw - 4, d->tasks.h };
}

void desktop_layout(struct desktop *d, enum de_id layout, int sw, int sh, int term_w,
		    int term_h)
{
	const struct theme *t = &themes[layout];
	const int n = DESK_ITEMS, gap = layout == DE_DOCK ? 12 : 10;
	int ix, iy;

	memset(d, 0, sizeof(*d));
	d->layout = layout;
	d->sw = sw;
	d->sh = sh;
	d->term_w = term_w;
	d->term_h = term_h;
	d->scale = sw >= 900 && sh >= 600 ? 2 : 1;
	d->tiled = layout == DE_TILING;
	d->movable = !d->tiled;                 /* tiles are placed, not dragged */
	d->next_id = 1;

	switch (layout) {
	case DE_TASKBAR:                        /* panel along the bottom */
		d->bar = (struct desk_rect){ 0, sh - t->bar_h, sw, t->bar_h };
		d->work = (struct desk_rect){ 0, 0, sw, sh - t->bar_h };
		ix = 60;
		iy = d->bar.y + (t->bar_h - t->icon) / 2;
		d->status = (struct desk_rect){ sw - 260, d->bar.y + 15, 250, 16 };
		d->tasks = (struct desk_rect){ ix + n * (t->icon + gap) + 8, d->bar.y + 6, 0,
					       t->bar_h - 12 };
		d->tasks.w = d->status.x - 10 - d->tasks.x;
		break;
	case DE_DOCK: {                         /* menu bar on top, dock floating below */
		int dw = n * t->icon + (n + 1) * gap;

		d->bar = (struct desk_rect){ 0, 0, sw, t->bar_h };
		d->dock = (struct desk_rect){ (sw - dw) / 2, sh - t->icon - 2 * gap - 6, dw,
					      t->icon + 2 * gap };
		d->work = (struct desk_rect){ 0, t->bar_h, sw, d->dock.y - 4 - t->bar_h };
		ix = d->dock.x + gap;
		iy = d->dock.y + gap;
		d->status = (struct desk_rect){ sw - 260, 7, 250, 16 };
		break;                          /* open windows: dots under the launchers */
	}
	default:                                /* tiling, retro: one bar on top */
		d->bar = (struct desk_rect){ 0, 0, sw, t->bar_h };
		d->work = (struct desk_rect){ 0, t->bar_h, sw, sh - t->bar_h };
		ix = sw - n * (t->icon + gap) - 4;
		iy = (t->bar_h - t->icon) / 2;
		d->status = (struct desk_rect){ 150, 6, 200, 16 };
		d->tasks = (struct desk_rect){ 358, 3, ix - 8 - 358, t->bar_h - 6 };
		break;
	}
	if (d->tasks.w < 0)
		d->tasks.w = 0;
	for (int i = 0; i < n; i++) {
		d->item[i].action = launchers[i];
		d->item[i].r = (struct desk_rect){ ix + i * (t->icon + gap), iy, t->icon, t->icon };
	}
	(void)desktop_open(d, DESK_WIN_TERMINAL);
	d->ndamage = 0;
	d->bar_dirty = false;
}

/* ---- hit-testing -------------------------------------------------------------- */

struct desk_hit desktop_hit(const struct desktop *d, int x, int y)
{
	struct desk_hit h = { DESK_PART_NONE, 0, DESK_NONE, -1 };
	static const enum desk_part controls[3] = { DESK_PART_CLOSE, DESK_PART_MINIMISE,
						    DESK_PART_MAXIMISE };
	int n = open_count(d), k = 0;

	for (int i = 0; i < DESK_ITEMS; i++)
		if (inside(&d->item[i].r, x, y)) {
			h.part = DESK_PART_LAUNCHER;
			h.action = d->item[i].action;
			return h;
		}
	for (int s = 0; s < DESK_MAX_WINDOWS; s++) {
		struct desk_rect r;

		if (!d->win[s].id)
			continue;
		r = task_rect(d, k++, n);
		if (inside(&r, x, y)) {
			h.part = DESK_PART_TASK;
			h.win = d->win[s].id;
			return h;
		}
	}
	for (int i = d->nz - 1; i >= 0; i--) {
		const struct desk_window *w = &d->win[d->z[i]];
		struct desk_rect r;

		if (!desktop_visible(w) || !inside(&w->frame, x, y))
			continue;
		h.win = w->id;
		for (int c = 0; c < 3; c++) {
			r = control_rect(d, w, controls[c]);
			if (inside(&r, x, y)) {
				h.part = controls[c];
				return h;
			}
		}
		r = desktop_title_rect(d, w);
		if (inside(&r, x, y)) {
			h.part = DESK_PART_TITLE;
			return h;
		}
		r = button_rect(d, w);
		if (inside(&r, x, y)) {
			h.part = DESK_PART_BUTTON;
			return h;
		}
		for (int tab = 0; tab < DESK_TABS && w->kind == DESK_WIN_VIEWER; tab++) {
			r = desktop_tab_rect(d, w, tab);
			if (inside(&r, x, y)) {
				h.part = DESK_PART_TAB;
				h.row = tab;
				return h;
			}
		}
		h.part = DESK_PART_CONTENT;
		r = desktop_content_rect(d, w);
		if (w->kind != DESK_WIN_TERMINAL && y >= r.y + pad(d))
			h.row = (y - r.y - pad(d)) / line_h(d);
		return h;
	}
	return h;
}

/* ---- clipped drawing -----------------------------------------------------------
 * Everything below draws through these, so a repaint never touches pixels
 * outside `clip`. Text that straddles the edge is drawn glyph by glyph.
 */

static struct desk_rect clip;

static void cfill(int x, int y, int w, int h, u32 rgb)
{
	struct desk_rect r = desk_rect_and((struct desk_rect){ x, y, w, h }, clip);

	if (!desk_rect_empty(r))
		ui_draw_fill(r.x, r.y, r.w, r.h, rgb);
}

static void cblend(int x, int y, u32 rgb, u8 alpha)
{
	if (alpha && inside(&clip, x, y))
		ui_draw_blend(x, y, rgb, alpha);
}

/* Share of a corner pixel inside the rounding circle, from a 4 x 4 sample grid. */
static u8 corner_cover(int dx, int dy, int r)
{
	int in = 0;

	for (int sy = 0; sy < 4; sy++)
		for (int sx = 0; sx < 4; sx++) {
			int px = 8 * dx + 2 * sx + 1, py = 8 * dy + 2 * sy + 1;

			in += px * px + py * py <= 64 * r * r;
		}
	return (u8)(in * 255 / 16);
}

static void cround(int x, int y, int w, int h, int r, u32 rgb)
{
	const struct desk_rect box = { x, y, w, h };

	if (!overlap(box, clip))
		return;
	if (within(&box, &clip)) {
		ui_fill_round(x, y, w, h, r, rgb);
		return;
	}
	r = MOS_MAX(0, MOS_MIN(r, MOS_MIN(w, h) / 2));
	cfill(x + r, y, w - 2 * r, h, rgb);
	cfill(x, y + r, r, h - 2 * r, rgb);
	cfill(x + w - r, y + r, r, h - 2 * r, rgb);
	for (int dy = 0; dy < r; dy++)
		for (int dx = 0; dx < r; dx++) {
			u8 a = corner_cover(dx, dy, r);

			cblend(x + r - 1 - dx, y + r - 1 - dy, rgb, a);
			cblend(x + w - r + dx, y + r - 1 - dy, rgb, a);
			cblend(x + r - 1 - dx, y + h - r + dy, rgb, a);
			cblend(x + w - r + dx, y + h - r + dy, rgb, a);
		}
}

static void cdisc(int x, int y, int size, u32 rgb)
{
	cround(x, y, size, size, size / 2, rgb);
}

static void partial_glyph(int x, int top, u32 cp, u32 rgb, int scale)
{
	u8 cov[UI_GLYPH_W * UI_MAX_SCALE * UI_RASTER_ROWS * UI_MAX_SCALE];
	const int w = UI_GLYPH_W * scale, h = UI_RASTER_ROWS * scale;
	struct ui_glyph g;

	(void)ui_glyph_lookup(cp, &g);
	if (g.base == ' ')
		return;
	ui_glyph_coverage(&g, scale, cov);
	for (int row = 0; row < h; row++)
		for (int col = 0; col < w; col++)
			cblend(x + col, top + row, rgb, cov[row * w + col]);
}

static void ctext(int x, int y, const char *s, u32 rgb, int scale)
{
	char run[128];
	int rn = 0, rx = x, cw, top, ch;

	scale = MOS_MAX(1, MOS_MIN(scale, UI_MAX_SCALE));
	cw = UI_GLYPH_W * scale;
	ch = UI_RASTER_ROWS * scale;
	top = y - 2 * scale;                    /* the raster starts two dot rows above */
	if (top >= clip.y + clip.h || top + ch <= clip.y || x >= clip.x + clip.w)
		return;
	{
		struct desk_rect all = { x, top, ui_text_cells(s) * cw, ch };

		if (within(&all, &clip)) {
			ui_draw_text(x, y, s, rgb, scale);
			return;
		}
	}
	while (*s && x < clip.x + clip.w) {
		const char *start = s;
		const u32 cp = utf8_next(&s);
		const int nb = (int)(s - start);
		const struct desk_rect cell = { x, top, cw, ch };
		const bool in = within(&cell, &clip);

		if (in && rn + nb < (int)sizeof(run)) {
			if (!rn)
				rx = x;
			memcpy(run + rn, start, (size_t)nb);
			rn += nb;
		} else {
			if (rn) {
				run[rn] = '\0';
				ui_draw_text(rx, y, run, rgb, scale);
				rn = 0;
			}
			if (in) {
				rx = x;
				memcpy(run, start, (size_t)nb);
				rn = nb;
			} else if (overlap(cell, clip)) {
				partial_glyph(x, top, cp, rgb, scale);
			}
		}
		x += cw;
	}
	if (rn) {
		run[rn] = '\0';
		ui_draw_text(rx, y, run, rgb, scale);
	}
}

static void clogo(int x, int y, int unit)
{
	const struct desk_rect box = { x, y, 16 * unit, 16 * unit };

	if (within(&box, &clip))                /* the bar is only ever repainted whole */
		ui_logo(x, y, unit);
}

/* Narrow the clip for a while (title text, window content). */
static struct desk_rect clip_push(struct desk_rect r)
{
	struct desk_rect old = clip;

	clip = desk_rect_and(clip, r);
	return old;
}

/* ---- drawing ------------------------------------------------------------------ */

/* Launcher icons, built from rounded boxes and discs at any size. */
static void icon(enum desk_action a, int x, int y, int s)
{
	const int u = s / 8;                    /* one eighth of the icon */

	switch (a) {
	case DESK_TERMINAL:                     /* dark screen with a prompt */
		cround(x, y + u, s, s - 2 * u, u, 0x11181F);
		cfill(x + u, y + 2 * u, 2 * u, u, 0x5CE08A);
		cfill(x + 2 * u, y + 3 * u, 2 * u, u, 0x5CE08A);
		cfill(x + u, y + 4 * u, 2 * u, u, 0x5CE08A);
		cfill(x + 4 * u, y + 5 * u, 3 * u, u, 0xE8ECF1);
		break;
	case DESK_FILES:                        /* folder: tab + body */
		cround(x, y + u, 4 * u, 2 * u, u / 2, 0xD99A2B);
		cround(x, y + 2 * u, s, s - 3 * u, u, 0xF2B94B);
		break;
	case DESK_PACKAGES:                     /* parcel with tape */
		cround(x, y + u, s, s - 2 * u, u, 0xB07A4A);
		cfill(x, y + 3 * u, s, u, 0x8A5A32);
		cfill(x + s / 2 - u / 2, y + u, u, s - 2 * u, 0xE4C9A4);
		break;
	case DESK_SYSTEM:                       /* monitor on a stand */
		cround(x, y + u / 2, s, 5 * u, u, 0x55606E);
		cfill(x + u, y + u + u / 2, s - 2 * u, 3 * u, 0x4FA3E0);
		cfill(x + s / 2 - u / 2, y + 5 * u, u, u + u / 2, 0x55606E);
		cfill(x + 2 * u, y + 6 * u + u / 2, s - 4 * u, u, 0x55606E);
		break;
	case DESK_HELP:                         /* dotted "i" on a disc */
		cdisc(x, y, s, 0x148F8F);
		cfill(x + s / 2 - u / 2, y + 2 * u - u / 2, u, u, 0xFFFFFF);
		cfill(x + s / 2 - u / 2, y + 3 * u, u, 3 * u, 0xFFFFFF);
		break;
	case DESK_POWER:                        /* ring with a bar */
		cdisc(x, y, s, 0xC0392B);
		cdisc(x + 2 * u, y + 2 * u, s - 4 * u, 0xE5574A);
		cfill(x + s / 2 - u / 2, y + u, u, 3 * u, 0xFFFFFF);
		break;
	case DESK_FENCE:                        /* shield: rounded top, tapering point */
		cround(x + u, y, s - 2 * u, 5 * u, u, 0x2C6FB7);
		cfill(x + 2 * u, y + 5 * u, s - 4 * u, u, 0x2C6FB7);
		cfill(x + 3 * u, y + 6 * u, s - 6 * u, u, 0x2C6FB7);
		cfill(x + s / 2 - u / 2, y + 7 * u, u, u, 0x2C6FB7);
		cround(x + 2 * u, y + u, s - 4 * u, 4 * u, u / 2, 0x7FB8F0);
		cfill(x + 3 * u, y + 5 * u, s - 6 * u, u, 0x7FB8F0);
		cfill(x + s / 2 - u / 2, y + 2 * u, u, 3 * u, 0xFFFFFF);
		break;
	default:
		break;
	}
}

static u32 mix(u32 a, u32 b, int t, int max)
{
	u32 out = 0;

	for (int ch = 0; ch < 24; ch += 8)
		out |= ((((a >> ch) & 0xFF) * (u32)(max - t) + ((b >> ch) & 0xFF) * (u32)t) / (u32)max)
		       << ch;
	return out;
}

static void window_controls(const struct desktop *d, const struct desk_window *w, u32 title_bg)
{
	const struct desk_rect t = desktop_title_rect(d, w);
	const int cy = t.y + t.h / 2;
	struct desk_rect r;

	switch (d->layout) {
	case DE_DOCK: {                         /* close, minimise, maximise: three discs */
		static const u32 colours[3] = { 0xED6A5E, 0xF4BF4F, 0x61C554 };

		for (int i = 0; i < 3; i++)
			cdisc(t.x + 10 + i * 20, cy - 6, 12, colours[i]);
		break;
	}
	case DE_TASKBAR:                        /* minimise, maximise/restore, close */
		r = control_rect(d, w, DESK_PART_MINIMISE);
		cfill(r.x + 8, cy + 4, 10, 2, 0xFFFFFF);
		r = control_rect(d, w, DESK_PART_MAXIMISE);
		if (w->state == DESK_MAXIMISED) {       /* two stacked boxes: restore */
			cfill(r.x + 11, cy - 6, 8, 8, 0xFFFFFF);
			cfill(r.x + 12, cy - 5, 6, 6, title_bg);
			cfill(r.x + 7, cy - 2, 8, 8, 0xFFFFFF);
			cfill(r.x + 8, cy - 1, 6, 6, title_bg);
		} else {
			cfill(r.x + 8, cy - 5, 10, 10, 0xFFFFFF);
			cfill(r.x + 10, cy - 3, 6, 6, title_bg);
		}
		r = control_rect(d, w, DESK_PART_CLOSE);
		cround(r.x + 4, cy - 9, 18, 18, 4, 0xC0392B);
		cfill(r.x + 9, cy - 1, 8, 2, 0xFFFFFF);
		break;
	case DE_RETRO:                          /* one square on the left: close */
		cfill(t.x + 6, cy - 6, 12, 12, 0xFFFFFF);
		cfill(t.x + 8, cy - 4, 8, 8, title_bg);
		break;
	default:
		break;
	}
}

/*
 * An image scaled to fit `box`, centred: whole multiples up to 8x when it is
 * smaller, nearest-neighbour shrinking when larger. Runs of equal pixels in
 * equal rows become one fill.
 */
static void draw_image(const struct desk_image *img, struct desk_rect box)
{
	int dw, dh, ox, oy;

	if (!img->rgb || img->w <= 0 || img->h <= 0 || box.w <= 0 || box.h <= 0)
		return;
	if (img->w <= box.w && img->h <= box.h) {
		const int k = MOS_MIN(8, MOS_MIN(box.w / img->w, box.h / img->h));

		dw = img->w * k;
		dh = img->h * k;
	} else if ((long)img->w * box.h >= (long)img->h * box.w) {
		dw = box.w;
		dh = MOS_MAX(1, (int)((long)img->h * box.w / img->w));
	} else {
		dh = box.h;
		dw = MOS_MAX(1, (int)((long)img->w * box.h / img->h));
	}
	ox = box.x + (box.w - dw) / 2;
	oy = box.y + (box.h - dh) / 2;
	for (int y = 0; y < dh;) {
		const int sy = (int)((long)y * img->h / dh);
		const u8 *row = img->rgb + (size_t)sy * (size_t)img->w * 3;
		int y2 = y + 1;

		while (y2 < dh && (int)((long)y2 * img->h / dh) == sy)
			y2++;
		if (oy + y2 > clip.y && oy + y < clip.y + clip.h)
			for (int x = 0; x < dw;) {
				const u8 *p = row + (size_t)((long)x * img->w / dw) * 3;
				const u32 rgb = (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
				int x2 = x + 1;

				while (x2 < dw) {
					const u8 *q = row + (size_t)((long)x2 * img->w / dw) * 3;

					if (q[0] != p[0] || q[1] != p[1] || q[2] != p[2])
						break;
					x2++;
				}
				cfill(ox + x, oy + y, x2 - x, y2 - y, rgb);
				x = x2;
			}
		y = y2;
	}
}

/* Viewer: tab strip, then text lines or a picture with a caption. */
static void draw_viewer(struct desktop *d, const struct desk_window *w, const struct desk_host *h)
{
	const struct theme *t = &themes[d->layout];
	const struct desk_rect c = desktop_content_rect(d, w);
	const int vs = viewer_scale(d, &c), lh = 12 * vs;
	const int top = c.y + strip_h(d) + 3 + pad(d);
	struct desk_rect saved = clip_push(c);
	struct desk_image img = { NULL, 0, 0 };

	cfill(c.x, c.y + strip_h(d) + 1, c.w, 1, mix(t->panel, t->panel_text, 1, 4));
	for (int i = 0; h && h->tab && i < DESK_TABS; i++) {
		const struct desk_rect r = desktop_tab_rect(d, w, i);
		bool active = false;
		const char *label = h->tab(h->ctx, i, &active);
		struct desk_rect in;

		if (!label)
			continue;
		cround(r.x, r.y, r.w, r.h, 3 * d->scale, active ? t->accent :
		       mix(t->panel, t->panel_text, 1, 6));
		in = clip_push(r);
		ctext(r.x + (r.w - ui_text_cells(label) * UI_GLYPH_W * d->scale) / 2,
		      r.y + (r.h - 7 * d->scale) / 2, label, active ? 0xFFFFFF : t->panel_text,
		      d->scale);
		clip = in;
	}
	if (h && h->image && h->image(h->ctx, &img)) {
		const char *cap = h->line ? h->line(h->ctx, w->kind, 0) : NULL;
		const struct desk_rect box = { c.x + pad(d), top, c.w - 2 * pad(d),
					       c.y + c.h - pad(d) - top - (cap ? lh + 4 : 0) };

		draw_image(&img, box);
		if (cap)
			ctext(c.x + pad(d), box.y + box.h + 4 + 2 * vs, cap, t->panel_text, vs);
	} else {
		(void)clip_push((struct desk_rect){ c.x + pad(d), c.y, c.w - 2 * pad(d), c.h });
		for (int i = 0; h && h->line; i++) {
			const int y = top + i * lh + 2 * vs;
			const char *s;

			if (y + 8 * vs > c.y + c.h)
				break;
			s = h->line(h->ctx, w->kind, i);
			if (!s)
				break;
			ctext(c.x + pad(d), y, s, t->panel_text, vs);
		}
	}
	clip = saved;
}

static const char *window_title(const struct desk_window *w, const struct desk_host *h)
{
	const char *t = NULL;

	if (h && w->kind == DESK_WIN_APP && h->title)
		t = h->title(h->ctx, w->id);
	if (!t && h && h->line)
		t = h->line(h->ctx, w->kind, DESK_LINE_TITLE);
	return t ? t : kind_title(w->kind);
}

static void draw_window(struct desktop *d, const struct desk_window *w, bool focused,
			const struct desk_host *h)
{
	const struct theme *t = &themes[d->layout];
	const struct desk_rect title = desktop_title_rect(d, w), c = desktop_content_rect(d, w);
	const u32 tbg = focused ? t->title : t->title_idle;
	const int tx = d->layout == DE_DOCK ? title.x + 76 :
		       d->layout == DE_TILING ? title.x + 8 : title.x + 28;
	struct desk_rect saved;

	/* Border and title bar. */
	cfill(w->frame.x, w->frame.y, w->frame.w, 1, focused ? t->border : mix(t->border, 0x808080, 1, 2));
	cfill(w->frame.x, w->frame.y + w->frame.h - 1, w->frame.w, 1, t->border);
	cfill(w->frame.x, w->frame.y + 1, 1, w->frame.h - 2, t->border);
	cfill(w->frame.x + w->frame.w - 1, w->frame.y + 1, 1, w->frame.h - 2, t->border);
	cfill(title.x, title.y, title.w, title.h, tbg);
	window_controls(d, w, tbg);
	saved = clip_push((struct desk_rect){ tx, title.y, title.x + title.w - tx -
					      (d->layout == DE_TASKBAR ? 80 : 4), title.h });
	ctext(tx, title.y + (title.h - 14) / 2, window_title(w, h),
	      focused ? t->title_text : mix(t->title_text, tbg, 1, 2), 2);
	clip = saved;

	if (w->kind == DESK_WIN_TERMINAL) {
		struct desk_rect area = desk_rect_and(c, clip);

		cfill(area.x, area.y, area.w, area.h, t->term_bg);
		if (h && h->term_paint && !desk_rect_empty(area)) {
			struct desk_rect painted = desk_rect_and(h->term_paint(h->ctx, area), c);

			clip = desk_rect_union(clip, painted);  /* windows above cover it again */
		}
		return;
	}

	if (w->kind == DESK_WIN_APP) {          /* the client's pixels cover the whole content area */
		struct desk_rect area = desk_rect_and(c, clip);

		if (h && h->app_paint && !desk_rect_empty(area))
			h->app_paint(h->ctx, w->id, c, area);
		else
			cfill(c.x, c.y, c.w, c.h, t->term_bg);
		return;
	}
	cfill(c.x, c.y, c.w, c.h, t->panel);
	if (w->kind == DESK_WIN_VIEWER) {
		draw_viewer(d, w, h);
		return;
	}
	saved = clip_push((struct desk_rect){ c.x + pad(d), c.y, c.w - 2 * pad(d), c.h });
	for (int i = 0; h && h->line; i++) {
		const int y = c.y + pad(d) + i * line_h(d) + 2 * d->scale;
		const char *s;

		if (y + 8 * d->scale > c.y + c.h)
			break;
		s = h->line(h->ctx, w->kind, i);
		if (!s)
			break;
		ctext(c.x + pad(d), y, s, t->panel_text, d->scale);
	}
	clip = saved;
	if (w->kind == DESK_WIN_FENCE) {
		const struct desk_rect b = button_rect(d, w);
		const char *label = h && h->line ? h->line(h->ctx, w->kind, DESK_LINE_BUTTON) : NULL;

		if (!label)
			label = "Toggle";
		cround(b.x, b.y, b.w, b.h, 4 * d->scale, t->accent);
		saved = clip_push(b);
		ctext(b.x + (b.w - ui_text_cells(label) * UI_GLYPH_W * d->scale) / 2,
		      b.y + (b.h - 7 * d->scale) / 2, label, 0xFFFFFF, d->scale);
		clip = saved;
	}
}

static void draw_status_area(const struct desktop *d)
{
	const struct theme *t = &themes[d->layout];
	struct desk_rect saved = clip_push((struct desk_rect){ d->status.x, d->status.y - 4,
							       d->status.w, d->status.h + 6 });

	cfill(d->status.x, d->status.y - 4, d->status.w, d->status.h + 6, t->bar);
	ctext(d->status.x, d->status.y, d->status_text, t->bar_text, 2);
	clip = saved;
}

static void draw_strip(const struct desktop *d)
{
	const struct theme *t = &themes[d->layout];
	const int n = open_count(d), top = desktop_top(d);
	int k = 0;

	for (int s = 0; s < DESK_MAX_WINDOWS; s++) {
		const struct desk_window *w = &d->win[s];

		if (!w->id)
			continue;
		if (d->layout == DE_DOCK) {     /* a dot under the window's launcher */
			for (int i = 0; i < DESK_ITEMS; i++) {
				const struct desk_rect *r = &d->item[i].r;

				if (d->item[i].action == kind_launcher(w->kind))
					cdisc(r->x + r->w / 2 - 3, r->y + r->h + 3, 6, 0x1C2733);
			}
		} else {
			const struct desk_rect r = task_rect(d, k++, n);
			const int ts = d->layout == DE_TASKBAR ? 2 : 1;
			const u32 bg = w->id == top ? mix(t->bar, 0xFFFFFF, 2, 5) :
				       w->state == DESK_MINIMISED ? mix(t->bar, 0x000000, 1, 3) :
								    mix(t->bar, 0xFFFFFF, 1, 6);
			struct desk_rect saved;

			cround(r.x, r.y, r.w, r.h, 3, bg);
			saved = clip_push((struct desk_rect){ r.x + 4, r.y, r.w - 8, r.h });
			ctext(r.x + 6, r.y + (r.h - 7 * ts) / 2, kind_title(w->kind), t->bar_text, ts);
			clip = saved;
		}
	}
}

static void draw_shell(const struct desktop *d)
{
	const struct theme *t = &themes[d->layout];

	/* Wallpaper: a vertical gradient in 16 bands (plain fill when both ends match). */
	for (int band = 0; band < 16; band++)
		cfill(0, d->sh * band / 16, d->sw, d->sh * (band + 1) / 16 - d->sh * band / 16,
		      mix(t->bg_top, t->bg_bottom, band, 15));

	cfill(d->bar.x, d->bar.y, d->bar.w, d->bar.h, t->bar);
	switch (d->layout) {
	case DE_TASKBAR:
		clogo(d->bar.x + 8, d->bar.y + 6, 2);
		break;
	case DE_DOCK:
		clogo(8, 2, 1);
		ctext(34, 7, "MergedOS  File  Edit  View  Window", t->bar_text, 2);
		cround(d->dock.x, d->dock.y, d->dock.w, d->dock.h, 16, 0xD4D9E2);
		break;
	case DE_TILING:
		cfill(4, 3, 26, d->bar.h - 6, 0x2ECC71);
		ctext(11, 6, "1  2  3  4", 0x0B1A12, 2);
		break;
	default:
		ctext(8, 6, "MergedOS", t->bar_text, 2);
		break;
	}
	for (int i = 0; i < DESK_ITEMS; i++)
		icon(d->item[i].action, d->item[i].r.x, d->item[i].r.y, d->item[i].r.w);
	draw_strip(d);
	draw_status_area(d);
}

void desktop_paint(struct desktop *d, struct desk_rect r, const struct desk_host *h)
{
	const int top = desktop_top(d);

	clip = desk_rect_and(r, (struct desk_rect){ 0, 0, d->sw, d->sh });
	if (desk_rect_empty(clip))
		return;
	draw_shell(d);
	for (int i = 0; i < d->nz; i++) {
		const struct desk_window *w = &d->win[d->z[i]];

		if (desktop_visible(w) && overlap(w->frame, clip))
			draw_window(d, w, w->id == top, h);
	}
}

void desktop_draw(struct desktop *d, const struct desk_host *h)
{
	desktop_paint(d, (struct desk_rect){ 0, 0, d->sw, d->sh }, h);
}

void desktop_draw_status(struct desktop *d, const char *text)
{
	size_t n = strlen(text);

	n = MOS_MIN(n, sizeof(d->status_text) - 1);
	memcpy(d->status_text, text, n);
	d->status_text[n] = '\0';
	clip = (struct desk_rect){ 0, 0, d->sw, d->sh };
	draw_status_area(d);
}

void desktop_outline_strips(struct desk_rect r, struct desk_rect out[4])
{
	out[0] = (struct desk_rect){ r.x, r.y, r.w, 2 };
	out[1] = (struct desk_rect){ r.x, r.y + r.h - 2, r.w, 2 };
	out[2] = (struct desk_rect){ r.x, r.y + 2, 2, r.h - 4 };
	out[3] = (struct desk_rect){ r.x + r.w - 2, r.y + 2, 2, r.h - 4 };
}

void desktop_draw_outline(const struct desktop *d, struct desk_rect r)
{
	struct desk_rect s[4];

	clip = (struct desk_rect){ 0, 0, d->sw, d->sh };
	desktop_outline_strips(r, s);
	for (int i = 0; i < 4; i++)
		cfill(s[i].x, s[i].y, s[i].w, s[i].h, themes[d->layout].accent);
}

/* ---- self-check ---------------------------------------------------------------- */

static int check_tiles(struct desktop *d)
{
	long long area = 0;
	int bad = 0;

	for (int a = 0; a < DESK_MAX_WINDOWS; a++) {
		const struct desk_window *w = &d->win[a];

		if (!desktop_visible(w))
			continue;
		area += (long long)w->frame.w * w->frame.h;
		bad += !within(&w->frame, &d->work) || desk_rect_empty(w->frame);
		for (int b = a + 1; b < DESK_MAX_WINDOWS; b++)
			bad += desktop_visible(&d->win[b]) && overlap(w->frame, d->win[b].frame);
	}
	/* Disjoint tiles inside the work area whose areas add up: they cover it. */
	return bad + (area != (long long)d->work.w * d->work.h);
}

static int check_layout(enum de_id id, int sw, int sh, int term_w, int term_h, bool draw)
{
	const struct desk_rect screen = { 0, 0, sw, sh };
	struct desktop d;
	struct desk_window *t;
	struct desk_rect c, tr, before;
	struct desk_hit hit;
	int bad = 0, files, sys, ids[DESK_MAX_WINDOWS];

	desktop_layout(&d, id, sw, sh, term_w, term_h);
	if (draw) {
		desktop_draw(&d, NULL);
		desktop_draw_status(&d, "self-check");
	}
	t = desktop_window(&d, 1);
	if (!t || t->kind != DESK_WIN_TERMINAL || desktop_top(&d) != 1)
		return 1;
	c = desktop_content_rect(&d, t);
	tr = desktop_title_rect(&d, t);
	/* The terminal shows the whole console and sits inside the work area. */
	bad += c.w < term_w || c.h < term_h || (!d.tiled && (c.w != term_w || c.h != term_h));
	bad += !within(&c, &t->frame) || !within(&t->frame, &d.work);
	bad += !within(&d.work, &screen) || !within(&d.status, &d.bar);
	bad += !desk_rect_empty(d.tasks) && (!within(&d.tasks, &d.bar) || overlap(d.tasks, d.status));
	/* Launchers: on screen, outside the window, disjoint, each hit at its centre. */
	for (int i = 0; i < DESK_ITEMS; i++) {
		const struct desk_rect *r = &d.item[i].r;

		hit = desktop_hit(&d, r->x + r->w / 2, r->y + r->h / 2);
		bad += !within(r, &screen) || overlap(*r, t->frame) || overlap(*r, d.tasks);
		bad += hit.part != DESK_PART_LAUNCHER || hit.action != d.item[i].action;
		bad += desktop_command(d.item[i].action) == NULL &&
		       desktop_launcher_kind(d.item[i].action) == DESK_WIN_NONE;
		bad += i > 0 && r->x < d.item[i - 1].r.x + d.item[i - 1].r.w;
	}
	/* Title bar, content, controls. */
	hit = desktop_hit(&d, tr.x + tr.w / 2, tr.y + 2);
	bad += hit.part != DESK_PART_TITLE || hit.win != 1;
	hit = desktop_hit(&d, c.x + 5, c.y + 5);
	bad += hit.part != DESK_PART_CONTENT || hit.win != 1;
	for (enum desk_part p = DESK_PART_CLOSE; p <= DESK_PART_MAXIMISE; p++) {
		struct desk_rect r = control_rect(&d, t, p);

		if (desk_rect_empty(r))
			continue;
		bad += !within(&r, &tr);
		bad += desktop_hit(&d, r.x + r.w / 2, r.y + r.h / 2).part != p;
	}
	bad += d.tiled != desk_rect_empty(control_rect(&d, t, DESK_PART_CLOSE));
	/* Moving: follows the pointer, never leaves the work area (tiles stay put). */
	before = t->frame;
	if (d.movable) {
		(void)desktop_move(&d, 1, -100000, -100000);
		bad += t->frame.x != d.work.x || t->frame.y != d.work.y;
		(void)desktop_move(&d, 1, 100000, 100000);
		bad += !within(&t->frame, &d.work);
		bad += desktop_move(&d, 1, 100000, 100000);     /* already at the edge */
		(void)desktop_move(&d, 1, before.x - t->frame.x, before.y - t->frame.y);
	} else {
		bad += desktop_move(&d, 1, 10, 10);
	}
	bad += t->frame.x != before.x || t->frame.y != before.y;

	/* Open two windows: newest on top, ids distinct, topmost wins the hit-test. */
	files = desktop_open(&d, DESK_WIN_FILES);
	sys = desktop_open(&d, DESK_WIN_SYSTEM);
	bad += !files || !sys || files == sys || files == 1 || desktop_top(&d) != sys;
	bad += desktop_open(&d, DESK_WIN_TERMINAL) != 0;        /* only one terminal */
	if (d.tiled) {
		bad += check_tiles(&d);
	} else {
		const struct desk_rect a = desktop_window(&d, files)->frame;
		const struct desk_rect b = desktop_window(&d, sys)->frame;
		const struct desk_rect o = desk_rect_and(a, b);

		bad += desk_rect_empty(o);                      /* cascaded: they overlap */
		bad += desktop_hit(&d, o.x + o.w - 1, o.y + o.h - 1).win != sys;
		bad += !desktop_focus(&d, files) || desktop_top(&d) != files;
		bad += desktop_hit(&d, o.x + o.w - 1, o.y + o.h - 1).win != files;
		bad += !desktop_covered(&d, 1) || desktop_covered(&d, files);
		/* Minimised windows leave the hit-test; restoring brings them back on top. */
		bad += !desktop_minimise(&d, files) || desktop_top(&d) != sys;
		bad += desktop_hit(&d, a.x + 2, a.y + a.h - 2).win == files;
		bad += !desktop_focus(&d, files) || desktop_top(&d) != files;
		/* Maximise fills the work area; restore returns the exact old frame. */
		bad += !desktop_toggle_max(&d, sys);
		bad += desktop_window(&d, sys)->frame.x != d.work.x ||
		       desktop_window(&d, sys)->frame.w != d.work.w ||
		       desktop_window(&d, sys)->frame.h != d.work.h || desktop_top(&d) != sys;
		bad += desktop_move(&d, sys, 5, 5);             /* maximised: not movable */
		bad += !desktop_toggle_max(&d, sys) || desktop_window(&d, sys)->frame.x != b.x ||
		       desktop_window(&d, sys)->frame.y != b.y ||
		       desktop_window(&d, sys)->frame.w != b.w || desktop_window(&d, sys)->frame.h != b.h;
	}
	/* The terminal cannot be closed: it is minimised and comes back. */
	bad += desktop_close(&d, 1) || !desktop_window(&d, 1) ||
	       desktop_window(&d, 1)->state != DESK_MINIMISED;
	bad += !desktop_focus(&d, 1) || desktop_top(&d) != 1 ||
	       desktop_window(&d, 1)->state == DESK_MINIMISED;
	bad += !desktop_close(&d, files) || desktop_window(&d, files) != NULL;
	/* Capacity: six windows; the seventh open fails and changes nothing. */
	{
		int n = 0;

		while (n < DESK_MAX_WINDOWS && desktop_open(&d, DESK_WIN_FENCE))
			n++;
		bad += open_count(&d) != DESK_MAX_WINDOWS || d.nz != DESK_MAX_WINDOWS;
		bad += desktop_open(&d, DESK_WIN_FILES) != 0 || d.nz != DESK_MAX_WINDOWS;
		if (d.tiled)
			bad += check_tiles(&d);
		if (draw)
			desktop_draw(&d, NULL);
		/* Close all but the terminal again. */
		for (int s = 0; s < DESK_MAX_WINDOWS; s++)
			ids[s] = d.win[s].id;
		for (int s = 0; s < DESK_MAX_WINDOWS; s++)
			if (ids[s] != 1)
				bad += !desktop_close(&d, ids[s]);
		bad += open_count(&d) != 1 || d.nz != 1 || desktop_top(&d) != 1;
	}
	/* Tiling geometry for 1 to 4 windows. */
	if (d.tiled)
		for (int n = 1; n <= 4; n++) {
			bad += check_tiles(&d);
			(void)desktop_open(&d, DESK_WIN_FILES);
		}
	return bad;
}

int desktop_selfcheck(int sw, int sh, int term_w, int term_h, bool draw)
{
	int bad = 0;

	for (int id = 0; id < DE_COUNT; id++)
		bad += check_layout((enum de_id)id, sw, sh, term_w, term_h, draw);
	return bad;
}
