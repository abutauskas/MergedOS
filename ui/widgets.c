/* SPDX-License-Identifier: BSD-2-Clause */
#include "ui/ui.h"

void ui_screen_init(struct ui_screen *s)
{
	memset(s, 0, sizeof(*s));
	s->focus = -1;
	s->default_id = -1;
}

struct ui_widget *ui_add(struct ui_screen *s, enum ui_kind kind, int id, int x, int y, int w,
			 int h, const char *label)
{
	struct ui_widget *wd;

	if (s->count == UI_MAX_WIDGETS)
		return NULL;
	wd = &s->w[s->count++];
	wd->kind = kind;
	wd->id = id;
	wd->x = x;
	wd->y = y;
	wd->w = w;
	wd->h = h;
	wd->label = label;
	wd->dirty = true;
	wd->max_len = UI_TEXT_MAX;
	wd->color = UI_COL_TEXT;
	wd->scale = 2;
	return wd;
}

struct ui_widget *ui_find(struct ui_screen *s, int id)
{
	for (int i = 0; i < s->count; i++)
		if (s->w[i].id == id)
			return &s->w[i];
	return NULL;
}

static bool interactive(const struct ui_widget *w)
{
	return w->kind != UI_LABEL && !w->disabled && !w->hidden;
}

int ui_hit(const struct ui_screen *s, int x, int y)
{
	for (int i = s->count - 1; i >= 0; i--) {
		const struct ui_widget *w = &s->w[i];

		if (interactive(w) && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h)
			return i;
	}
	return -1;
}

void ui_set_focus(struct ui_screen *s, int index)
{
	if (index == s->focus || index >= s->count || (index >= 0 && !interactive(&s->w[index])))
		return;
	if (s->focus >= 0)
		s->w[s->focus].dirty = true;
	s->focus = index;
	if (index >= 0)
		s->w[index].dirty = true;
}

void ui_focus_next(struct ui_screen *s)
{
	for (int step = 1; step <= s->count; step++) {
		int i = (s->focus + step + s->count) % s->count;

		if (interactive(&s->w[i])) {
			ui_set_focus(s, i);
			return;
		}
	}
}

static int default_button(struct ui_screen *s)
{
	struct ui_widget *b = s->default_id >= 0 ? ui_find(s, s->default_id) : NULL;

	return (b && interactive(b)) ? b->id : -1;
}

static int key_event(struct ui_screen *s, int key)
{
	struct ui_widget *w = s->focus >= 0 ? &s->w[s->focus] : NULL;

	if (key == '\t') {
		ui_focus_next(s);
		return -1;
	}
	if (!w || !interactive(w))
		return key == '\n' ? default_button(s) : -1;

	switch (w->kind) {
	case UI_BUTTON:
		if (key == '\n' || key == ' ')
			return w->id;
		break;
	case UI_CHECKBOX:
		if (key == ' ') {
			w->checked = !w->checked;
			w->dirty = true;
			return w->id;
		}
		break;
	case UI_LIST:
		if ((key == UI_KEY_UP && w->selected > 0) ||
		    (key == UI_KEY_DOWN && w->selected < w->nitems - 1)) {
			w->selected += key == UI_KEY_UP ? -1 : 1;
			w->dirty = true;
			return w->id;
		}
		break;
	case UI_TEXTBOX:
		if (key == '\b') {
			/* One whole character: its continuation bytes, then its lead byte. */
			while (w->len && ((u8)w->text[w->len - 1] & 0xC0) == 0x80)
				w->len--;
			if (w->len)
				w->len--;
			w->text[w->len] = '\0';
			w->dirty = true;
			return -1;
		}
		if (key >= ' ' && key != 0x7F && key <= 0x10FFFF) {
			char enc[4];
			int n = utf8_encode((u32)key, enc);

			if (ui_text_cells(w->text) < w->max_len && w->len + n <= UI_TEXT_MAX) {
				memcpy(w->text + w->len, enc, (size_t)n);
				w->len += n;
				w->text[w->len] = '\0';
				w->dirty = true;
			}
			return -1;
		}
		break;
	default:
		break;
	}
	return key == '\n' ? default_button(s) : -1;
}

int ui_dispatch(struct ui_screen *s, const struct ui_event *ev)
{
	int hit, result = -1;

	if (ev->type == UI_EV_KEY)
		return key_event(s, ev->key);

	hit = ui_hit(s, ev->x, ev->y);
	if (ev->type == UI_EV_MOUSE_DOWN) {
		struct ui_widget *w;

		if (hit < 0)
			return -1;
		w = &s->w[hit];
		ui_set_focus(s, hit);
		if (w->kind == UI_BUTTON) {
			w->pressed = true;
			w->dirty = true;
		} else if (w->kind == UI_CHECKBOX) {
			w->checked = !w->checked;
			w->dirty = true;
			result = w->id;
		} else if (w->kind == UI_LIST) {
			int row = (ev->y - w->y - 1) / UI_LIST_ROW_H;

			if (row >= 0 && row < w->nitems && row != w->selected) {
				w->selected = row;
				w->dirty = true;
				result = w->id;
			}
		}
		return result;
	}

	/* Mouse up: a button fires only if the press and the release are both on it. */
	for (int i = 0; i < s->count; i++) {
		struct ui_widget *w = &s->w[i];

		if (w->kind != UI_BUTTON || !w->pressed)
			continue;
		w->pressed = false;
		w->dirty = true;
		if (i == hit)
			result = w->id;
	}
	return result;
}

/* ---- drawing ---------------------------------------------------------------- */

/* Rounded box with a border. The panel colour goes down first so that the
 * anti-aliased corners always blend against a clean background. */
static void frame(const struct ui_widget *w, u32 border, int t, u32 fill)
{
	const int r = MOS_MIN(6, w->h / 4);

	ui_draw_fill(w->x, w->y, w->w, w->h, UI_COL_PANEL);
	ui_fill_round(w->x, w->y, w->w, w->h, r, border);
	ui_fill_round(w->x + t, w->y + t, w->w - 2 * t, w->h - 2 * t, r - t, fill);
}

static void draw_one(const struct ui_widget *w, bool focused)
{
	const int th = UI_GLYPH_H * 2;          /* text height at scale 2 */
	const u32 ink = w->disabled ? UI_COL_BORDER : UI_COL_TEXT;

	if (w->hidden)
		return;
	switch (w->kind) {
	case UI_LABEL:
		/* Clear the rows above and below too: accents and cedillas reach there. */
		ui_draw_fill(w->x, w->y - 2 * w->scale, w->w, w->h + 3 * w->scale, UI_COL_PANEL);
		if (w->label)
			ui_draw_text(w->x, w->y, w->label, w->color, w->scale);
		break;
	case UI_BUTTON: {
		int tw = ui_text_cells(w->label) * UI_GLYPH_W * 2;
		u32 face = w->disabled ? UI_COL_DISABLED : w->pressed ? UI_COL_TEXT : UI_COL_ACCENT;

		frame(w, focused ? UI_COL_TEXT : face, 2, face);
		ui_draw_text(w->x + (w->w - tw) / 2, w->y + (w->h - th) / 2, w->label,
			     w->disabled ? UI_COL_BORDER : 0xFFFFFF, 2);
		break;
	}
	case UI_TEXTBOX: {
		char shown[UI_TEXT_MAX + 1];
		int cells = ui_text_cells(w->text);

		mos_strlcpy(shown, w->text, sizeof(shown));
		if (w->masked) {                /* one star per character, not per byte */
			memset(shown, '*', (size_t)cells);
			shown[cells] = '\0';
		}
		frame(w, focused ? UI_COL_ACCENT : UI_COL_BORDER, focused ? 2 : 1,
		      w->disabled ? UI_COL_DISABLED : UI_COL_FIELD);
		ui_draw_text(w->x + 8, w->y + (w->h - th) / 2, shown, ink, 2);
		if (focused)                    /* caret */
			ui_draw_fill(w->x + 8 + cells * UI_GLYPH_W * 2, w->y + 6, 2, w->h - 12,
				     UI_COL_ACCENT);
		break;
	}
	case UI_CHECKBOX: {
		struct ui_widget box = *w;

		ui_draw_fill(w->x, w->y, w->w, w->h, UI_COL_PANEL);
		box.w = box.h = 20;
		box.y = w->y + (w->h - 20) / 2;
		frame(&box, focused ? UI_COL_ACCENT : UI_COL_BORDER, 2, UI_COL_FIELD);
		if (w->checked)
			ui_draw_fill(box.x + 5, box.y + 5, 10, 10, UI_COL_ACCENT);
		ui_draw_text(w->x + 30, w->y + (w->h - th) / 2, w->label, ink, 2);
		break;
	}
	case UI_LIST:
		ui_draw_fill(w->x, w->y, w->w, w->h, focused ? UI_COL_ACCENT : UI_COL_BORDER);
		ui_draw_fill(w->x + 1, w->y + 1, w->w - 2, w->h - 2, UI_COL_FIELD);
		for (int i = 0; i < w->nitems; i++) {
			int ry = w->y + 1 + i * UI_LIST_ROW_H;
			bool sel = i == w->selected;

			if (sel)
				ui_draw_fill(w->x + 1, ry, w->w - 2, UI_LIST_ROW_H, UI_COL_ACCENT);
			ui_draw_text(w->x + 10, ry + (UI_LIST_ROW_H - th) / 2, w->items[i],
				     sel ? 0xFFFFFF : ink, 2);
		}
		break;
	}
}

void ui_draw(struct ui_screen *s, bool all)
{
	for (int i = 0; i < s->count; i++) {
		if (all || s->w[i].dirty)
			draw_one(&s->w[i], i == s->focus);
		s->w[i].dirty = false;
	}
}

