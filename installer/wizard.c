/* SPDX-License-Identifier: BSD-2-Clause */
/* Setup wizard: screen construction and the step state machine. */
#include "installer/installer.h"

#define PANEL_W   680
#define PANEL_H   460
#define MARGIN    32
#define BTN_W     160
#define BTN_H     40
#define FIELD_W   400
#define FIELD_H   40

static int panel_x(const struct wizard *w) { return (w->screen_w - PANEL_W) / 2; }
static int panel_y(const struct wizard *w) { return (w->screen_h - PANEL_H) / 2; }

void wizard_init(struct wizard *w, int screen_w, int screen_h, wizard_random_fn random)
{
	memset(w, 0, sizeof(*w));
	w->screen_w = screen_w;
	w->screen_h = screen_h;
	w->random = random;
	w->iterations = PW_ITERATIONS;
	w->cfg.password_enabled = true;         /* secure by default */
	w->cfg.desktop = DESKTOP_AUTO;
}

void wizard_draw_frame(const struct wizard *w)
{
	int x = panel_x(w), y = panel_y(w);

	ui_draw_fill(0, 0, w->screen_w, w->screen_h, UI_COL_DESKTOP);
	ui_draw_fill(x, y, PANEL_W, PANEL_H, UI_COL_PANEL);
	ui_draw_fill(x, y, PANEL_W, 80, 0x101820);
	ui_logo(x + 24, y + 16, 3);
	ui_draw_text(x + 88, y + 30, "MergedOS", 0xFFFFFF, 3);
}

static struct ui_widget *label(struct ui_screen *s, int id, int x, int y, const char *text,
			       int scale, u32 color)
{
	struct ui_widget *l = ui_add(s, UI_LABEL, id, x, y, PANEL_W - 2 * MARGIN,
				     UI_GLYPH_H * scale + 2, text);

	if (l) {
		l->scale = scale;
		l->color = color;
	}
	return l;
}

static const char *T(const struct wizard *w, enum wiz_str id)
{
	return wizard_str(w->cfg.lang, id);
}

/* Text of storage choice `i`; i == ntargets is "do not save". */
static const char *target_text(struct wizard *w, int i)
{
	if (i >= w->ntargets)
		return T(w, STR_SKIP_DISK);
	mos_snprintf(w->option[i], sizeof(w->option[i]), "%s %d: %s (%u MiB)", T(w, STR_DISK),
		     w->target[i].disk, T(w, w->target[i].blank ? STR_USE_BLANK : STR_USE_FREE),
		     w->target[i].mib);
	return w->option[i];
}

/* Desktop list: row 0 is "recommended", rows 1.. are the profiles. */
static enum de_id desktop_of_row(const struct wizard *w, int row)
{
	return row ? (enum de_id)(row - 1) : w->rec.best;
}

static const char *desktop_text(struct wizard *w, int row)
{
	const struct de_profile *p = de_profile(desktop_of_row(w, row));

	if (row == 0)
		mos_snprintf(w->de_option[0], sizeof(w->de_option[0]), "%s %s",
			     T(w, STR_RECOMMENDED), p->title);
	else
		mos_snprintf(w->de_option[row], sizeof(w->de_option[row]), "%s (%s)", p->title,
			     p->style);
	return w->de_option[row];
}

void wizard_build(struct wizard *w, struct ui_screen *s)
{
	static const enum wiz_str titles[] = { STR_WELCOME, STR_NAME_TITLE, STR_PW_TITLE,
					       STR_STORAGE_TITLE, STR_WIFI_TITLE, STR_DE_TITLE,
					       STR_DONE_TITLE };
	const int x = panel_x(w) + MARGIN, y = panel_y(w);
	const int btn_y = y + PANEL_H - MARGIN - BTN_H;
	struct ui_widget *wd;
	int first_focus = ID_NEXT;

	ui_screen_init(s);
	s->default_id = ID_NEXT;
	label(s, ID_TITLE, x, y + 104, T(w, titles[w->step]), 3, UI_COL_TEXT);

	switch (w->step) {
	case WIZ_WELCOME:
		label(s, ID_HINT, x, y + 150, T(w, STR_CHOOSE_LANG), 2, UI_COL_MUTED);
		wd = ui_add(s, UI_LIST, ID_LANG, x, y + 180, FIELD_W, LANG_COUNT * UI_LIST_ROW_H + 2,
			    NULL);
		for (int i = 0; i < LANG_COUNT; i++)
			wd->items[i] = wizard_lang_name((enum wiz_lang)i);
		wd->nitems = LANG_COUNT;
		wd->selected = w->cfg.lang;
		first_focus = ID_LANG;
		break;
	case WIZ_NAME:
		label(s, ID_NAME_LABEL, x, y + 156, T(w, STR_NAME), 2, UI_COL_MUTED);
		wd = ui_add(s, UI_TEXTBOX, ID_NAME, x, y + 180, FIELD_W, FIELD_H, NULL);
		wd->max_len = 30;
		wd->len = (int)mos_strlcpy(wd->text, w->cfg.name, sizeof(wd->text));
		first_focus = ID_NAME;
		break;
	case WIZ_PASSWORD:
		wd = ui_add(s, UI_CHECKBOX, ID_PW_ENABLE, x, y + 148, PANEL_W - 2 * MARGIN, 28,
			    T(w, STR_PW_ENABLE));
		wd->checked = w->cfg.password_enabled;
		label(s, ID_PW_LABEL, x, y + 190, T(w, STR_PW), 2, UI_COL_MUTED);
		wd = ui_add(s, UI_TEXTBOX, ID_PW, x, y + 212, FIELD_W, FIELD_H, NULL);
		wd->masked = true;
		wd->max_len = 32;
		wd->disabled = !w->cfg.password_enabled;
		label(s, ID_PW2_LABEL, x, y + 264, T(w, STR_PW_CONFIRM), 2, UI_COL_MUTED);
		wd = ui_add(s, UI_TEXTBOX, ID_PW2, x, y + 286, FIELD_W, FIELD_H, NULL);
		wd->masked = true;
		wd->max_len = 32;
		wd->disabled = !w->cfg.password_enabled;
		first_focus = w->cfg.password_enabled ? ID_PW : ID_PW_ENABLE;
		break;
	case WIZ_STORAGE:
		label(s, ID_HINT, x, y + 150, T(w, STR_STORAGE_HINT), 2, UI_COL_MUTED);
		label(s, ID_DISK1, x, y + 180, w->disk_info[0], 1, UI_COL_TEXT);
		label(s, ID_DISK2, x, y + 194, w->disk_info[1], 1, UI_COL_TEXT);
		wd = ui_add(s, UI_LIST, ID_STORAGE, x, y + 214, PANEL_W - 2 * MARGIN,
			    (w->ntargets + 1) * UI_LIST_ROW_H + 2, NULL);
		for (int i = 0; i <= w->ntargets; i++)
			wd->items[i] = target_text(w, i);
		wd->nitems = w->ntargets + 1;
		wd->selected = w->target_sel;
		first_focus = ID_STORAGE;
		break;
	case WIZ_WIFI:
		/* No wireless stack yet: say so plainly; Next is labelled "Skip". */
		label(s, ID_LINE1, x, y + 156, T(w, STR_WIFI_NONE), 2, UI_COL_TEXT);
		label(s, ID_LINE2, x, y + 186, T(w, STR_WIFI_LATER), 2, UI_COL_MUTED);
		break;
	case WIZ_DESKTOP: {
		static const struct hw_profile unknown_hw;
		int row = w->cfg.desktop < DE_COUNT ? w->cfg.desktop + 1 : 0;

		de_recommend(w->hw ? w->hw : &unknown_hw, &w->rec);
		wd = ui_add(s, UI_LIST, ID_DESKTOP, x, y + 150, PANEL_W - 2 * MARGIN,
			    (DE_COUNT + 1) * UI_LIST_ROW_H + 2, NULL);
		for (int i = 0; i <= DE_COUNT; i++)
			wd->items[i] = desktop_text(w, i);
		wd->nitems = DE_COUNT + 1;
		wd->selected = row;
		/* Why: the policy's own verdict on the highlighted desktop for this machine. */
		label(s, ID_DE_NOTE, x, y + 304, w->rec.score[desktop_of_row(w, row)].reason, 1,
		      UI_COL_MUTED);
		first_focus = ID_DESKTOP;
		break;
	}
	case WIZ_SUMMARY:
		de_recommend(w->hw ? w->hw : &(const struct hw_profile){ 0 }, &w->rec);
		mos_snprintf(w->line[3], sizeof(w->line[3]), "%s %s%s", T(w, STR_DESKTOP),
			     de_profile(desktop_of_row(w, w->cfg.desktop < DE_COUNT
							       ? w->cfg.desktop + 1 : 0))->title,
			     w->cfg.desktop < DE_COUNT ? "" : " *");
		label(s, ID_LINE6, x, y + 276, w->line[3], 2, UI_COL_TEXT);
		mos_snprintf(w->line[2], sizeof(w->line[2]), "%s %s", T(w, STR_SAVE_TO),
			     target_text(w, w->target_sel));
		label(s, ID_LINE5, x, y + 246, w->line[2], 2, UI_COL_TEXT);
		mos_snprintf(w->line[0], sizeof(w->line[0]), "%s %s", T(w, STR_HELLO), w->cfg.name);
		mos_snprintf(w->line[1], sizeof(w->line[1]), "%s %s", T(w, STR_LANGUAGE),
			     wizard_lang_name(w->cfg.lang));
		label(s, ID_LINE1, x, y + 156, w->line[0], 2, UI_COL_TEXT);
		label(s, ID_LINE2, x, y + 186, w->line[1], 2, UI_COL_TEXT);
		label(s, ID_LINE3, x, y + 216,
		      T(w, w->cfg.password_enabled ? STR_PW_ON : STR_PW_OFF), 2, UI_COL_TEXT);
		label(s, ID_LINE4, x, y + 312, T(w, STR_MORE_STEPS), 2, UI_COL_MUTED);
		break;
	}

	label(s, ID_ERROR, x, btn_y - 30, w->error ? w->error : "", 2, UI_COL_ERROR);
	/* Tab order: the step's fields, then Next, then Back. */
	ui_add(s, UI_BUTTON, ID_NEXT, panel_x(w) + PANEL_W - MARGIN - BTN_W, btn_y, BTN_W, BTN_H,
	       T(w, w->step == WIZ_SUMMARY ? STR_FINISH : w->step == WIZ_WIFI ? STR_SKIP : STR_NEXT));
	if (w->step != WIZ_WELCOME)
		ui_add(s, UI_BUTTON, ID_BACK, x, btn_y, BTN_W, BTN_H, T(w, STR_BACK));

	for (int i = 0; i < s->count; i++)
		if (s->w[i].id == first_focus)
			ui_set_focus(s, i);
}

static void set_error(struct wizard *w, struct ui_screen *s, const char *msg)
{
	struct ui_widget *e = ui_find(s, ID_ERROR);

	w->error = msg;
	if (e) {
		e->label = msg ? msg : "";
		e->dirty = true;
	}
}

static void forget_passwords(struct ui_screen *s)
{
	static const int ids[] = { ID_PW, ID_PW2 };

	for (unsigned i = 0; i < ARRAY_SIZE(ids); i++) {
		struct ui_widget *b = ui_find(s, ids[i]);

		if (b) {
			secure_wipe(b->text, sizeof(b->text));
			b->len = 0;
			b->dirty = true;
		}
	}
}

/* Keep the typed name when leaving its screen in either direction. */
static void save_name(struct wizard *w, struct ui_screen *s)
{
	struct ui_widget *b = ui_find(s, ID_NAME);
	const char *p;
	size_t n;

	if (!b)
		return;
	p = b->text;
	n = (size_t)b->len;
	while (n && *p == ' ')
		p++, n--;
	while (n && p[n - 1] == ' ')
		n--;
	mos_fieldcpy(w->cfg.name, sizeof(w->cfg.name), p, n);
}

static bool next(struct wizard *w, struct ui_screen *s)
{
	switch (w->step) {
	case WIZ_WELCOME:
		break;
	case WIZ_NAME:
		save_name(w, s);
		if (!w->cfg.name[0]) {
			set_error(w, s, T(w, STR_ERR_NAME));
			return false;
		}
		break;
	case WIZ_PASSWORD: {
		struct ui_widget *p1 = ui_find(s, ID_PW), *p2 = ui_find(s, ID_PW2);

		memset(w->cfg.salt, 0, sizeof(w->cfg.salt));
		memset(w->cfg.hash, 0, sizeof(w->cfg.hash));
		w->cfg.iterations = 0;
		if (w->cfg.password_enabled) {
			if (p1->len < PW_MIN_LEN) {
				set_error(w, s, T(w, STR_ERR_SHORT));
				return false;
			}
			if (p1->len != p2->len || memcmp(p1->text, p2->text, (size_t)p1->len)) {
				set_error(w, s, T(w, STR_ERR_MISMATCH));
				return false;
			}
			/* Only a salted, stretched hash leaves this function. */
			w->random(w->cfg.salt, sizeof(w->cfg.salt));
			w->cfg.iterations = w->iterations;
			pbkdf2_hmac_sha256(p1->text, (size_t)p1->len, w->cfg.salt,
					   sizeof(w->cfg.salt), w->iterations, w->cfg.hash);
		}
		forget_passwords(s);
		break;
	}
	case WIZ_STORAGE:
	case WIZ_WIFI:
	case WIZ_DESKTOP:
		break;
	case WIZ_SUMMARY:
		w->done = true;
		return false;
	}
	w->error = NULL;
	w->step++;
	return true;
}

bool wizard_handle(struct wizard *w, struct ui_screen *s, int id)
{
	struct ui_widget *wd = ui_find(s, id);

	switch (id) {
	case ID_LANG:
		w->cfg.lang = (enum wiz_lang)wd->selected;
		return true;                    /* every string on screen changes */
	case ID_PW_ENABLE: {
		static const int ids[] = { ID_PW, ID_PW2 };

		w->cfg.password_enabled = wd->checked;
		forget_passwords(s);
		for (unsigned i = 0; i < ARRAY_SIZE(ids); i++)
			ui_find(s, ids[i])->disabled = !wd->checked;
		set_error(w, s, NULL);
		return false;
	}
	case ID_STORAGE:
		w->target_sel = wd->selected;
		return false;
	case ID_DESKTOP: {
		struct ui_widget *note = ui_find(s, ID_DE_NOTE);

		w->cfg.desktop = wd->selected ? (u8)(wd->selected - 1) : DESKTOP_AUTO;
		note->label = w->rec.score[desktop_of_row(w, wd->selected)].reason;
		note->dirty = true;
		return false;
	}
	case ID_BACK:
		if (w->step == WIZ_WELCOME)
			return false;
		save_name(w, s);
		forget_passwords(s);
		w->error = NULL;
		w->step--;
		return true;
	case ID_NEXT:
		return next(w, s);
	default:
		return false;
	}
}
