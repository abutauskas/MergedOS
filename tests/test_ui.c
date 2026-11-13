/* SPDX-License-Identifier: BSD-2-Clause */
#include "tests/test.h"
#include "installer/installer.h"

static void key(struct ui_screen *s, int k)
{
	struct ui_event ev = { .type = UI_EV_KEY, .key = k };

	(void)ui_dispatch(s, &ev);
}

static u32 decode(const char *s, int *used)
{
	const char *p = s;
	u32 cp = utf8_next(&p);

	*used = (int)(p - s);
	return cp;
}

static void test_antialiasing(void)
{
	u8 cov[UI_GLYPH_W * UI_MAX_SCALE * UI_RASTER_ROWS * UI_MAX_SCALE];
	struct ui_glyph g;

	for (int scale = 1; scale <= UI_MAX_SCALE; scale++) {
		const int w = UI_GLYPH_W * scale, h = UI_RASTER_ROWS * scale;
		int partial = 0, full = 0, ink_a = 0, ink_e = 0;

		/* Space is empty at every size. */
		ui_glyph_lookup(' ', &g);
		memset(cov, 0xEE, sizeof(cov));
		ui_glyph_coverage(&g, scale, cov);
		for (int i = 0; i < w * h; i++)
			CHECK_EQ(cov[i], 0);

		/* 'O' is mirror-symmetric in the font, so its rendering must be too. */
		ui_glyph_lookup('O', &g);
		ui_glyph_coverage(&g, scale, cov);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < 5 * scale; x++)
				CHECK_EQ(cov[y * w + x], cov[y * w + 5 * scale - 1 - x]);
		for (int i = 0; i < w * h; i++) {
			partial += cov[i] > 0 && cov[i] < 255;
			full += cov[i] == 255;
		}
		if (scale >= 2) {
			CHECK(partial > 0);     /* edges are blended, not stepped */
			CHECK(full > 0);        /* strokes still have a solid core */
		}
		/* A letter stays inside its rows: nothing above the cell without a mark. */
		for (int i = 0; i < w * scale; i++)
			CHECK_EQ(cov[i], 0);

		/* An accent adds ink, above the letter for a capital. */
		ui_glyph_lookup('A', &g);
		ui_glyph_coverage(&g, scale, cov);
		for (int i = 0; i < w * h; i++)
			ink_a += cov[i];
		ui_glyph_lookup(0xC1, &g);              /* A with acute */
		ui_glyph_coverage(&g, scale, cov);
		for (int i = 0; i < w * h; i++)
			ink_e += cov[i];
		CHECK(ink_e > ink_a);
		for (int i = 0, top = 0; i <= w * 2 * scale; i++) {
			if (i == w * 2 * scale)
				CHECK(top > 0);
			else
				top += cov[i];
		}
	}

	/* Rounded rectangle: the extreme corner pixel is left alone, edges are blended. */
	mock_fill_count = mock_blend_count = 0;
	ui_fill_round(0, 0, 100, 40, 8, 0x123456);
	CHECK(mock_fill_count == 3 && mock_blend_count > 0 && mock_blend_count < 4 * 8 * 8);
	mock_blend_count = 0;
	ui_fill_round(0, 0, 100, 40, 0, 0x123456);      /* radius 0: a plain rectangle */
	CHECK_EQ(mock_blend_count, 0);
	ui_fill_round(0, 0, 10, 4, 99, 0x123456);       /* oversized radius is clamped */

	/* Logo: hexagon covers most, not all, of its box; four squares on top. */
	mock_fill_count = mock_blend_count = 0;
	ui_logo(0, 0, 8);
	CHECK(mock_blend_count > 128 * 128 * 55 / 100 && mock_blend_count < 128 * 128 * 80 / 100);
	CHECK_EQ(mock_fill_count, 4);
}

static void test_login(void)
{
	static struct ui_screen s;
	struct install_config c;
	struct ui_event ev = { .type = UI_EV_KEY };
	char title[96];

	memset(&c, 0, sizeof(c));
	c.lang = LANG_DE;
	strcpy(c.name, "J\xC3\xBCrgen");
	c.password_enabled = true;
	c.iterations = 500;
	memset(c.salt, 7, sizeof(c.salt));
	pbkdf2_hmac_sha256("Stra\xC3\x9F" "e 9", 9, c.salt, PW_SALT_LEN, c.iterations, c.hash);

	CHECK(login_check(&c, "Stra\xC3\x9F" "e 9", 9));
	CHECK(!login_check(&c, "Strasse 9", 9));
	CHECK(!login_check(&c, "Stra\xC3\x9F" "e 9 ", 10));       /* longer */
	CHECK(!login_check(&c, "Stra\xC3\x9F" "e ", 8));          /* prefix */
	CHECK(!login_check(&c, "", 0));
	c.salt[0] ^= 1;                                         /* other salt: other hash */
	CHECK(!login_check(&c, "Stra\xC3\x9F" "e 9", 9));
	c.salt[0] ^= 1;
	c.iterations = 0;                                       /* enabled, but no usable hash */
	CHECK(!login_check(&c, "Stra\xC3\x9F" "e 9", 9));
	c.iterations = 500;
	c.password_enabled = false;                             /* login switched off */
	CHECK(login_check(&c, "whatever", 8));
	c.password_enabled = true;

	/* The screen: greeting in the saved language, masked box focused, Enter submits. */
	login_build(&s, 1024, 768, &c, title, sizeof(title), false);
	CHECK_STR(title, "Hallo, J\xC3\xBCrgen");
	CHECK(s.focus >= 0 && s.w[s.focus].id == ID_LOGIN_PW && s.w[s.focus].masked);
	CHECK_STR(ui_find(&s, ID_LOGIN_BTN)->label, "Anmelden");
	CHECK_STR(ui_find(&s, ID_LOGIN_ERR)->label, "");
	for (const char *p = "geheim"; *p; p++) {
		ev.key = *p;
		CHECK_EQ(ui_dispatch(&s, &ev), -1);
	}
	mock_draw_len = 0;
	ui_draw(&s, true);
	CHECK(strstr(mock_draw_log, "******") && !strstr(mock_draw_log, "geheim"));
	ev.key = '\n';
	CHECK_EQ(ui_dispatch(&s, &ev), ID_LOGIN_BTN);
	login_build(&s, 1024, 768, &c, title, sizeof(title), true);
	CHECK_STR(ui_find(&s, ID_LOGIN_ERR)->label, "Falsches Passwort. Bitte erneut versuchen.");
	CHECK_EQ(ui_find(&s, ID_LOGIN_PW)->len, 0);             /* rebuilt box starts empty */
}

static void test_utf8(void)
{
	static const u32 samples[] = { 0x24, 0x7F, 0x80, 0xE9, 0x7FF, 0x800, 0x20AC, 0xFFFF,
				       0x10000, 0x1F600, 0x10FFFF };
	static struct ui_screen s;
	struct ui_widget *box;
	struct ui_event ev = { .type = UI_EV_KEY };
	struct ui_glyph g;
	char enc[5];
	int used;

	/* Encode and decode agree for 1- to 4-byte characters. */
	for (size_t i = 0; i < ARRAY_SIZE(samples); i++) {
		int n = utf8_encode(samples[i], enc);

		enc[n] = '\0';
		CHECK_EQ(decode(enc, &used), samples[i]);
		CHECK_EQ(used, n);
	}
	/* Malformed input never decodes to something else, and always advances. */
	CHECK_EQ(decode("\xC0\x80", &used), 0xFFFD);             /* overlong NUL       */
	CHECK_EQ(decode("\xE0\x80\xAF", &used), 0xFFFD);         /* overlong '/'       */
	CHECK_EQ(decode("\xED\xA0\x80", &used), 0xFFFD);         /* UTF-16 surrogate   */
	CHECK_EQ(decode("\xF4\x90\x80\x80", &used), 0xFFFD);     /* beyond U+10FFFF    */
	CHECK_EQ(decode("\x80", &used), 0xFFFD);                 /* stray continuation */
	CHECK_EQ(used, 1);
	CHECK_EQ(decode("\xC3", &used), 0xFFFD);                 /* truncated at NUL   */
	CHECK_EQ(used, 1);
	CHECK_EQ(decode("\xE2\x82" "A", &used), 0xFFFD);         /* truncated, then 'A' */
	CHECK_EQ(used, 2);
	CHECK_EQ(decode("\xFF", &used), 0xFFFD);
	CHECK_EQ(ui_text_cells("Zo\xC3\xAB N\xC3\xBA\xC3\xB1" "ez"), 9);    /* 12 bytes */
	CHECK_EQ(ui_text_cells("\xC3\x28\xFF"), 3);              /* garbage still terminates */

	/* Glyph composition. */
	CHECK(ui_glyph_lookup(0xE9, &g) && g.base == 'e' && g.mark == UI_MARK_ACUTE && !g.tall);
	CHECK(ui_glyph_lookup(0xD1, &g) && g.base == 'N' && g.mark == UI_MARK_TILDE && g.tall);
	CHECK(ui_glyph_lookup(0xFC, &g) && g.base == 'u' && g.mark == UI_MARK_DIAERESIS);
	CHECK(ui_glyph_lookup(0xE7, &g) && g.base == 'c' && g.mark == UI_MARK_CEDILLA);
	CHECK(ui_glyph_lookup(0xC5, &g) && g.base == 'A' && g.mark == UI_MARK_RING);
	CHECK(ui_glyph_lookup(0xE8, &g) && g.base == 'e' && g.mark == UI_MARK_GRAVE);
	CHECK(ui_glyph_lookup(0xF4, &g) && g.base == 'o' && g.mark == UI_MARK_CIRCUMFLEX);
	CHECK(ui_glyph_lookup(0xFF, &g) && g.base == 'y' && g.mark == UI_MARK_DIAERESIS);
	CHECK(ui_glyph_lookup(0xDF, &g) && !g.base && g.special == UI_SP_SHARP_S);
	CHECK(ui_glyph_lookup(0xBF, &g) && g.special == UI_SP_IQUEST);
	CHECK(ui_glyph_lookup(0xE6, &g) && g.special == UI_SP_ae);
	CHECK(ui_glyph_lookup('A', &g) && g.base == 'A' && g.mark == UI_MARK_NONE);
	CHECK(!ui_glyph_lookup(0x4E2D, &g) && g.special == UI_SP_BOX);      /* CJK: no glyph */
	CHECK(!ui_glyph_lookup(0x0416, &g));                                /* Cyrillic      */
	CHECK(!ui_glyph_lookup(0xFFFD, &g));
	for (u32 cp = 0; cp < 0x3000; cp++)             /* total over a wide range: no crash */
		(void)ui_glyph_lookup(cp, &g);

	/* Text boxes count characters, not bytes. */
	ui_screen_init(&s);
	box = ui_add(&s, UI_TEXTBOX, 1, 0, 0, 200, 40, NULL);
	box->masked = true;
	box->max_len = 3;
	ui_set_focus(&s, 0);
	ev.key = 0xF1;                                  /* n with tilde */
	(void)ui_dispatch(&s, &ev);
	ev.key = 0x20AC;                                /* euro sign: 3 bytes */
	(void)ui_dispatch(&s, &ev);
	ev.key = 'a';
	(void)ui_dispatch(&s, &ev);
	(void)ui_dispatch(&s, &ev);                     /* fourth character: over the limit */
	CHECK_EQ(box->len, 6);
	CHECK_STR(box->text, "\xC3\xB1\xE2\x82\xAC" "a");
	mock_draw_len = 0;
	ui_draw(&s, true);
	CHECK(strstr(mock_draw_log, "***\n") && !strstr(mock_draw_log, "****"));
	ev.key = '\b';
	(void)ui_dispatch(&s, &ev);
	(void)ui_dispatch(&s, &ev);                     /* removes the whole 3-byte character */
	CHECK_STR(box->text, "\xC3\xB1");
	(void)ui_dispatch(&s, &ev);
	(void)ui_dispatch(&s, &ev);
	CHECK_EQ(box->len, 0);
	ev.key = UI_KEY_DOWN;                           /* a key code is not a character */
	(void)ui_dispatch(&s, &ev);
	CHECK_EQ(box->len, 0);
}

void test_ui(void)
{
	static struct ui_screen s;
	struct ui_widget *box, *a, *b;
	struct ui_event ev;
	u8 out[SHA256_LEN], long_pw[100];
	static const u8 long_expect[SHA256_LEN] = {
		0xee, 0x2e, 0x59, 0xd3, 0xf2, 0xc8, 0xf9, 0x7f, 0xa6, 0xa2, 0xe2, 0x22, 0x6e, 0xe5, 0x89, 0x8a,
		0x8b, 0xd1, 0x9d, 0x69, 0xd0, 0x1e, 0x4c, 0x74, 0x04, 0xad, 0x47, 0x06, 0x79, 0x3d, 0x54, 0x05,
	};

	/* The scripted wizard walk: hashing vectors, hit-testing, Tab order,
	 * language switching, validation, password on/off. */
	CHECK_EQ(installer_selfcheck(1024, 768, true), 0);
	CHECK_EQ(installer_selfcheck(800, 600, false), 0);
	CHECK(mock_fill_count > 0);

	/* A masked box never hands its contents to the drawing layer. */
	ui_screen_init(&s);
	box = ui_add(&s, UI_TEXTBOX, 1, 10, 10, 200, 40, NULL);
	box->masked = true;
	ui_set_focus(&s, 0);
	for (const char *p = "secret"; *p; p++)
		key(&s, *p);
	mock_draw_len = 0;
	ui_draw(&s, true);
	CHECK(strstr(mock_draw_log, "******") != NULL);
	CHECK(strstr(mock_draw_log, "secret") == NULL);
	box->masked = false;
	mock_draw_len = 0;
	ui_draw(&s, true);
	CHECK(strstr(mock_draw_log, "secret") != NULL);

	/* Length limits, backspace on empty, control characters ignored. */
	box->max_len = 8;
	for (int i = 0; i < 100; i++)
		key(&s, 'x');
	CHECK_EQ(box->len, 8);
	for (int i = 0; i < 20; i++)
		key(&s, '\b');
	CHECK_EQ(box->len, 0);
	key(&s, 0x01);
	key(&s, UI_KEY_LEFT);
	CHECK_EQ(box->len, 0);
	box->max_len = 1000;                    /* a bad limit cannot overflow the buffer */
	for (int i = 0; i < 100; i++)
		key(&s, 'y');
	CHECK_EQ(box->len, UI_TEXT_MAX);
	CHECK_EQ(box->text[UI_TEXT_MAX], '\0');

	/* Overlap: the widget added last is on top; hidden and disabled ones are skipped. */
	ui_screen_init(&s);
	a = ui_add(&s, UI_BUTTON, 1, 0, 0, 100, 40, "A");
	b = ui_add(&s, UI_BUTTON, 2, 50, 0, 100, 40, "B");
	CHECK_EQ(ui_hit(&s, 75, 20), 1);
	CHECK_EQ(ui_hit(&s, 25, 20), 0);
	b->hidden = true;
	CHECK_EQ(ui_hit(&s, 75, 20), 0);
	b->hidden = false;
	b->disabled = true;
	CHECK_EQ(ui_hit(&s, 125, 20), -1);
	/* Tab with one usable widget stays on it; with none, focus stays empty. */
	ui_focus_next(&s);
	CHECK_EQ(s.focus, 0);
	ui_focus_next(&s);
	CHECK_EQ(s.focus, 0);
	a->disabled = true;
	s.focus = -1;
	ui_focus_next(&s);
	CHECK_EQ(s.focus, -1);
	/* A disabled default button does not fire on Enter. */
	s.default_id = 1;
	ev = (struct ui_event){ .type = UI_EV_KEY, .key = '\n' };
	CHECK_EQ(ui_dispatch(&s, &ev), -1);
	a->disabled = false;
	CHECK_EQ(ui_dispatch(&s, &ev), 1);

	/* The widget array is bounded. */
	ui_screen_init(&s);
	for (int i = 0; i < UI_MAX_WIDGETS; i++)
		CHECK(ui_add(&s, UI_LABEL, i, 0, 0, 1, 1, "") != NULL);
	CHECK(ui_add(&s, UI_LABEL, 99, 0, 0, 1, 1, "") == NULL);

	/* Every string exists in every language, is drawable, and fits the panel. */
	for (int lang = 0; lang < LANG_COUNT; lang++) {
		for (int id = 0; id < STR_COUNT; id++) {
			const char *str = wizard_str((enum wiz_lang)lang, (enum wiz_str)id);
			bool is_title = id == STR_WELCOME || id == STR_NAME_TITLE ||
					id == STR_PW_TITLE || id == STR_DONE_TITLE ||
					id == STR_STORAGE_TITLE || id == STR_WIFI_TITLE ||
					id == STR_DE_TITLE;

			CHECK(str && str[0]);
			for (const char *p = str; *p;) {        /* every character has a glyph */
				struct ui_glyph g;

				CHECK(ui_glyph_lookup(utf8_next(&p), &g));
			}
			CHECK(ui_text_cells(str) * UI_GLYPH_W * (is_title ? 3 : 2) <= 616);
		}
		CHECK(wizard_lang_name((enum wiz_lang)lang)[0]);
	}
	CHECK(strcmp(wizard_str(LANG_EN, STR_NEXT), wizard_str(LANG_NL, STR_NEXT)) != 0);
	CHECK_STR(wizard_str((enum wiz_lang)99, STR_NEXT), "Next");       /* falls back */

	test_utf8();
	test_login();
	test_antialiasing();

	/* PBKDF2 with a password longer than one hash block (the key is pre-hashed). */
	memset(long_pw, 'p', sizeof(long_pw));
	pbkdf2_hmac_sha256(long_pw, sizeof(long_pw), (const u8 *)"NaCl", 4, 3, out);
	CHECK(!memcmp(out, long_expect, SHA256_LEN));
}
