/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Scripted walk through the setup wizard, driven by synthetic input events.
 * Runs in the kernel boot self-test (drawing to the real framebuffer) and in
 * the host unit tests. Covers: hashing against published vectors, widget
 * hit-testing, Tab focus order, language switching, validation, and the
 * password on/off paths.
 */
#include "installer/installer.h"

static int failures;
static struct wizard wiz;
static struct ui_screen scr;
static bool drawing;

#define EXPECT(cond)                    \
	do {                            \
		if (!(cond))            \
			failures++;     \
	} while (0)

static void fixed_random(u8 *buf, size_t n)
{
	for (size_t i = 0; i < n; i++)
		buf[i] = (u8)(0xA0 + i);
}

static void refresh(bool rebuild)
{
	if (rebuild) {
		wizard_build(&wiz, &scr);
		if (drawing)
			wizard_draw_frame(&wiz);
	}
	if (drawing)
		ui_draw(&scr, rebuild);
}

static void act(const struct ui_event *ev)
{
	int id = ui_dispatch(&scr, ev);

	refresh(id >= 0 && wizard_handle(&wiz, &scr, id));
}

static void key(int k)
{
	struct ui_event ev = { .type = UI_EV_KEY, .key = k };

	act(&ev);
}

static void type(const char *s)
{
	for (; *s; s++)
		key(*s);
}

static void click(int id)
{
	const struct ui_widget *w = ui_find(&scr, id);
	struct ui_event ev = { .type = UI_EV_MOUSE_DOWN };

	if (!w) {
		failures++;
		return;
	}
	ev.x = w->x + w->w / 2;
	ev.y = w->y + w->h / 2;
	act(&ev);
	ev.type = UI_EV_MOUSE_UP;
	act(&ev);
}

static int focus_id(void)
{
	return scr.focus >= 0 ? scr.w[scr.focus].id : -1;
}

static const char *title(void)
{
	return ui_find(&scr, ID_TITLE)->label;
}

static bool hex_eq(const u8 *bin, const char *hex)
{
	static const char digits[] = "0123456789abcdef";

	for (int i = 0; i < SHA256_LEN; i++)
		if (hex[2 * i] != digits[bin[i] >> 4] || hex[2 * i + 1] != digits[bin[i] & 15])
			return false;
	return true;
}

static void check_hashing(void)
{
	struct sha256 c;
	u8 out[SHA256_LEN], buf[1000];

	/* FIPS 180-4 examples. */
	sha256_init(&c);
	sha256_update(&c, "abc", 3);
	sha256_final(&c, out);
	EXPECT(hex_eq(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	sha256_init(&c);
	sha256_final(&c, out);
	EXPECT(hex_eq(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	sha256_init(&c);
	sha256_update(&c, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
	sha256_final(&c, out);
	EXPECT(hex_eq(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	/* One million 'a', fed in uneven pieces to exercise buffering. */
	memset(buf, 'a', sizeof(buf));
	sha256_init(&c);
	for (int i = 0; i < 1000; i++) {
		sha256_update(&c, buf, 7);
		sha256_update(&c, buf + 7, sizeof(buf) - 7);
	}
	sha256_final(&c, out);
	EXPECT(hex_eq(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

	/* PBKDF2-HMAC-SHA256, password "password", salt "salt". */
	pbkdf2_hmac_sha256("password", 8, (const u8 *)"salt", 4, 1, out);
	EXPECT(hex_eq(out, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
	pbkdf2_hmac_sha256("password", 8, (const u8 *)"salt", 4, 2, out);
	EXPECT(hex_eq(out, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
	pbkdf2_hmac_sha256("password", 8, (const u8 *)"salt", 4, 4096, out);
	EXPECT(hex_eq(out, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
}

int installer_selfcheck(int screen_w, int screen_h, bool draw)
{
	static const struct hw_profile strong_hw = { .ram_mib = 4096, .cpus = 4, .ngpu = 1,
						     .gpu_tier = HW_GPU_INTEGRATED };
	static const u8 zero[SHA256_LEN];
	u8 expect[SHA256_LEN];
	const struct ui_widget *w;

	failures = 0;
	drawing = draw;
	check_hashing();

	wizard_init(&wiz, screen_w, screen_h, fixed_random);
	wiz.hw = &strong_hw;                    /* 4 GiB, integrated GPU, 4 CPUs: dock fits best */
	wiz.iterations = 1000;                  /* keep the self-test fast */
	refresh(true);

	/* -- step 1: welcome + language ------------------------------------------ */
	EXPECT(wiz.step == WIZ_WELCOME && focus_id() == ID_LANG);
	EXPECT(ui_find(&scr, ID_BACK) == NULL);
	/* Hit-testing: centre of Next is Next; the panel background is nothing. */
	w = ui_find(&scr, ID_NEXT);
	EXPECT(ui_hit(&scr, w->x + w->w / 2, w->y + w->h / 2) == (int)(w - scr.w));
	EXPECT(ui_hit(&scr, w->x - 5, w->y) == -1);
	EXPECT(ui_hit(&scr, w->x + w->w, w->y) == -1);     /* right edge is exclusive */
	EXPECT(ui_hit(&scr, 0, 0) == -1);
	/* Tab order: list -> Next -> list (labels are skipped). */
	key('\t');
	EXPECT(focus_id() == ID_NEXT);
	key('\t');
	EXPECT(focus_id() == ID_LANG);
	/* Language switching re-renders every string. */
	EXPECT(!strcmp(title(), "Welcome to MergedOS"));
	key(UI_KEY_DOWN);
	EXPECT(wiz.cfg.lang == LANG_NL && !strcmp(title(), "Welkom bij MergedOS"));
	EXPECT(!strcmp(ui_find(&scr, ID_NEXT)->label, "Volgende") && focus_id() == ID_LANG);
	key(UI_KEY_DOWN);
	EXPECT(wiz.cfg.lang == LANG_ID && !strcmp(title(), "Selamat datang di MergedOS"));
	key(UI_KEY_DOWN);                       /* UTF-8 strings arrive intact */
	EXPECT(wiz.cfg.lang == LANG_ES && !strcmp(title(), "Bienvenido a MergedOS"));
	EXPECT(!strcmp(wizard_str(LANG_ES, STR_NAME_TITLE), "\xC2\xBF" "C\xC3\xB3mo te llamas?"));
	key(UI_KEY_DOWN);
	EXPECT(wiz.cfg.lang == LANG_DE && !strcmp(wizard_str(LANG_DE, STR_BACK), "Zur\xC3\xBC" "ck"));
	key(UI_KEY_DOWN);                       /* already at the last entry */
	EXPECT(wiz.cfg.lang == LANG_DE);
	/* Clicking a list row selects it. */
	w = ui_find(&scr, ID_LANG);
	{
		struct ui_event ev = { UI_EV_MOUSE_DOWN, w->x + 20, w->y + 5, 0 };

		act(&ev);
	}
	EXPECT(wiz.cfg.lang == LANG_EN && !strcmp(title(), "Welcome to MergedOS"));
	click(ID_NEXT);

	/* -- step 2: name ---------------------------------------------------------- */
	EXPECT(wiz.step == WIZ_NAME && focus_id() == ID_NAME);
	key('\n');                              /* Enter in a text box = Next */
	EXPECT(wiz.step == WIZ_NAME && wiz.error != NULL);
	type("  Ada Lovelace ");
	key('\b');
	key('\t');
	EXPECT(focus_id() == ID_NEXT);
	key('\t');
	EXPECT(focus_id() == ID_BACK);
	key('\t');
	EXPECT(focus_id() == ID_NAME);
	key('\n');
	EXPECT(wiz.step == WIZ_PASSWORD && !strcmp(wiz.cfg.name, "Ada Lovelace"));

	/* -- step 3: password ------------------------------------------------------ */
	EXPECT(ui_find(&scr, ID_PW_ENABLE)->checked);          /* on by default */
	EXPECT(focus_id() == ID_PW && ui_find(&scr, ID_PW)->masked);
	type("abc");
	key('\n');
	EXPECT(wiz.step == WIZ_PASSWORD && !strcmp(wiz.error, wizard_str(LANG_EN, STR_ERR_SHORT)));
	type("12345");
	key('\t');
	EXPECT(focus_id() == ID_PW2);
	type("abc12346");
	key('\n');
	EXPECT(wiz.step == WIZ_PASSWORD &&
	       !strcmp(wiz.error, wizard_str(LANG_EN, STR_ERR_MISMATCH)));
	key('\b');
	key('5');
	key('\n');
	EXPECT(wiz.step == WIZ_STORAGE && wiz.cfg.password_enabled);
	/* -- step 4: storage (no disk offered here: the only choice is "do not save") */
	EXPECT(focus_id() == ID_STORAGE && ui_find(&scr, ID_STORAGE)->nitems == 1);
	key('\n');
	EXPECT(wiz.step == WIZ_WIFI && wiz.target_sel == wiz.ntargets);
	/* -- step 5: Wi-Fi placeholder: the forward button says "Skip" ------------- */
	EXPECT(!strcmp(ui_find(&scr, ID_NEXT)->label, "Skip"));
	key('\n');
	/* -- step 6: desktop: "recommended" is preselected and follows the hardware -- */
	EXPECT(wiz.step == WIZ_DESKTOP && focus_id() == ID_DESKTOP);
	w = ui_find(&scr, ID_DESKTOP);
	EXPECT(w->nitems == DE_COUNT + 1 && w->selected == 0 && wiz.cfg.desktop == DESKTOP_AUTO);
	EXPECT(!strcmp(w->items[0], "Recommended: Dock layout") && wiz.rec.best == DE_DOCK);
	EXPECT(!strcmp(w->items[2], "Dock layout (Mac-like layout)"));
	key(UI_KEY_DOWN);
	EXPECT(wiz.cfg.desktop == DE_TASKBAR);
	w = ui_find(&scr, ID_DE_NOTE);
	EXPECT(mos_memstr(w->label, strlen(w->label), "fit ") != NULL);
	for (int i = 0; i < 3; i++)
		key(UI_KEY_DOWN);
	EXPECT(wiz.cfg.desktop == DE_RETRO);
	key('\n');
	EXPECT(wiz.step == WIZ_SUMMARY && mos_memstr(wiz.line[3], strlen(wiz.line[3]), "Retro"));
	/* -- login check against what the wizard stored ---------------------------- */
	EXPECT(login_check(&wiz.cfg, "abc12345", 8));
	EXPECT(!login_check(&wiz.cfg, "abc12346", 8) && !login_check(&wiz.cfg, "abc1234", 7));
	EXPECT(!login_check(&wiz.cfg, "", 0));
	pbkdf2_hmac_sha256("abc12345", 8, wiz.cfg.salt, PW_SALT_LEN, 1000, expect);
	EXPECT(!memcmp(wiz.cfg.hash, expect, SHA256_LEN) && wiz.cfg.iterations == 1000);
	EXPECT(wiz.cfg.salt[0] == 0xA0 && memcmp(wiz.cfg.hash, zero, SHA256_LEN));

	/* -- back, then the "no password" path ------------------------------------- */
	for (int i = 0; i < 3; i++)
		click(ID_BACK);
	EXPECT(wiz.step == WIZ_STORAGE && wiz.cfg.desktop == DE_RETRO);       /* choice kept */
	click(ID_BACK);
	EXPECT(wiz.step == WIZ_PASSWORD && ui_find(&scr, ID_PW)->len == 0);
	click(ID_PW_ENABLE);
	EXPECT(!wiz.cfg.password_enabled && ui_find(&scr, ID_PW)->disabled);
	w = ui_find(&scr, ID_PW);
	EXPECT(ui_hit(&scr, w->x + 5, w->y + 5) == -1);        /* disabled: not clickable */
	key('\t');                              /* focus skips the disabled boxes */
	EXPECT(focus_id() == ID_NEXT);
	click(ID_NEXT);
	EXPECT(wiz.step == WIZ_STORAGE);
	for (int i = 0; i < 3; i++)
		click(ID_NEXT);
	EXPECT(wiz.step == WIZ_SUMMARY && !wiz.cfg.password_enabled);
	EXPECT(login_check(&wiz.cfg, "anything", 8));           /* password login off */
	EXPECT(!memcmp(wiz.cfg.hash, zero, SHA256_LEN) && wiz.cfg.iterations == 0);

	/* A press that is released elsewhere must not activate the button. */
	w = ui_find(&scr, ID_NEXT);
	{
		struct ui_event down = { UI_EV_MOUSE_DOWN, w->x + 5, w->y + 5, 0 };
		struct ui_event up = { UI_EV_MOUSE_UP, w->x - 50, w->y, 0 };

		act(&down);
		act(&up);
	}
	EXPECT(!wiz.done);
	click(ID_NEXT);
	EXPECT(wiz.done);
	return failures;
}
