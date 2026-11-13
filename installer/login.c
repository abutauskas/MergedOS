/* SPDX-License-Identifier: BSD-2-Clause */
/* Login: password verification against the saved setup, and the login screen. */
#include "installer/installer.h"

#define LOGIN_W 480
#define LOGIN_H 260

bool login_check(const struct install_config *c, const char *password, size_t len)
{
	u8 hash[SHA256_LEN], diff = 0;

	if (!c->password_enabled)
		return true;
	if (!c->iterations)
		return false;                   /* enabled but no usable hash: deny */
	pbkdf2_hmac_sha256(password, len, c->salt, PW_SALT_LEN, c->iterations, hash);
	for (int i = 0; i < SHA256_LEN; i++)    /* no early exit: timing tells nothing */
		diff |= (u8)(hash[i] ^ c->hash[i]);
	secure_wipe(hash, sizeof(hash));
	return diff == 0;
}

void login_draw_frame(int screen_w, int screen_h)
{
	int x = (screen_w - LOGIN_W) / 2, y = (screen_h - LOGIN_H) / 2;

	ui_draw_fill(0, 0, screen_w, screen_h, UI_COL_DESKTOP);
	ui_draw_fill(x, y, LOGIN_W, LOGIN_H, UI_COL_PANEL);
	ui_draw_fill(x, y, LOGIN_W, 64, 0x101820);
	ui_logo(x + 16, y + 8, 3);
	ui_draw_text(x + 80, y + 22, "MergedOS", 0xFFFFFF, 3);
}

void login_build(struct ui_screen *s, int screen_w, int screen_h, const struct install_config *c,
		 char *title, size_t title_cap, bool wrong)
{
	const int x = (screen_w - LOGIN_W) / 2 + 32, y = (screen_h - LOGIN_H) / 2;
	const int w = LOGIN_W - 64;
	struct ui_widget *wd;

	ui_screen_init(s);
	s->default_id = ID_LOGIN_BTN;
	mos_snprintf(title, title_cap, "%s %s", wizard_str(c->lang, STR_HELLO), c->name);
	ui_add(s, UI_LABEL, ID_LOGIN_TITLE, x, y + 84, w, 16, title);
	wd = ui_add(s, UI_LABEL, ID_LOGIN_LABEL, x, y + 112, w, 16, wizard_str(c->lang, STR_PASSWORD));
	wd->color = UI_COL_MUTED;
	wd = ui_add(s, UI_TEXTBOX, ID_LOGIN_PW, x, y + 134, w, 40, NULL);
	wd->masked = true;
	wd->max_len = 32;
	wd = ui_add(s, UI_LABEL, ID_LOGIN_ERR, x, y + 182, w, 16,
		    wrong ? wizard_str(c->lang, STR_ERR_WRONG) : "");
	wd->color = UI_COL_ERROR;
	ui_add(s, UI_BUTTON, ID_LOGIN_BTN, x + w - 160, y + 206, 160, 40,
	       wizard_str(c->lang, STR_LOGIN));
	ui_set_focus(s, 2);
}
