/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_INSTALLER_H
#define MERGEDOS_INSTALLER_H

/*
 * First-boot setup wizard. Portable logic only: screens are built from
 * ui/ widgets and the caller feeds events. Steps so far:
 *   1 language   2 name   3 password   4 storage   5 Wi-Fi   6 desktop   (summary)
 */
#include "ui/ui.h"
#include "de_selector/de_selector.h"

/* ---- hashing: see installer/sha256.h ------------------------------------- */
#include "installer/sha256.h"

/* ---- strings (i18n.c) ------------------------------------------------------ */
enum wiz_lang { LANG_EN = 0, LANG_NL, LANG_ID, LANG_ES, LANG_DE, LANG_COUNT };

enum wiz_str {
	STR_WELCOME, STR_CHOOSE_LANG, STR_NAME_TITLE, STR_NAME, STR_PW_TITLE, STR_PW_ENABLE,
	STR_PW, STR_PW_CONFIRM, STR_NEXT, STR_BACK, STR_FINISH, STR_ERR_NAME, STR_ERR_SHORT,
	STR_ERR_MISMATCH, STR_DONE_TITLE, STR_HELLO, STR_LANGUAGE, STR_PW_ON, STR_PW_OFF,
	STR_MORE_STEPS, STR_STORAGE_TITLE, STR_STORAGE_HINT, STR_USE_FREE, STR_USE_BLANK, STR_DISK,
	STR_SKIP_DISK, STR_SAVE_TO, STR_WIFI_TITLE, STR_WIFI_NONE, STR_WIFI_LATER, STR_SKIP,
	STR_DE_TITLE, STR_RECOMMENDED, STR_DESKTOP, STR_PASSWORD, STR_LOGIN, STR_ERR_WRONG, STR_COUNT
};

const char *wizard_str(enum wiz_lang lang, enum wiz_str id);
const char *wizard_lang_name(enum wiz_lang lang);

/* ---- wizard (wizard.c) ------------------------------------------------------ */
#define PW_MIN_LEN       8
#define PW_SALT_LEN      16
/*
 * PBKDF2 work factor. Measured at about 70 ms on a current x86-64 core with
 * the kernel's code generation (-Os, no SIMD), so roughly a second on a CPU
 * ten times slower. Below OWASP's 2023 figure of 600,000 for this KDF; the
 * count is stored with the hash so it can be raised per machine later.
 */
#define PW_ITERATIONS    100000

enum wiz_step {
	WIZ_WELCOME = 0, WIZ_NAME, WIZ_PASSWORD, WIZ_STORAGE, WIZ_WIFI, WIZ_DESKTOP, WIZ_SUMMARY
};

enum wiz_id {
	ID_TITLE = 1, ID_HINT, ID_LANG, ID_NAME_LABEL, ID_NAME, ID_PW_ENABLE, ID_PW_LABEL, ID_PW,
	ID_PW2_LABEL, ID_PW2, ID_STORAGE, ID_DISK1, ID_DISK2, ID_LINE1, ID_LINE2, ID_LINE3, ID_LINE4,
	ID_LINE5, ID_LINE6, ID_DESKTOP, ID_DE_NOTE, ID_ERROR, ID_NEXT, ID_BACK,
	ID_LOGIN_TITLE, ID_LOGIN_LABEL, ID_LOGIN_PW, ID_LOGIN_ERR, ID_LOGIN_BTN
};

struct install_config {
	enum wiz_lang lang;
	char name[UI_TEXT_MAX + 1];
	bool password_enabled;                  /* default: true */
	u8 salt[PW_SALT_LEN];
	u8 hash[SHA256_LEN];
	u32 iterations;
	u8 desktop;                             /* enum de_id, or DESKTOP_AUTO */
};

/* "Whatever suits this hardware": resolved by de_recommend() at every boot. */
#define DESKTOP_AUTO 0xFF

typedef void (*wizard_random_fn)(u8 *buf, size_t n);

/* A place the setup can be saved: filled in by the host from disk_plan(). */
#define WIZ_MAX_TARGETS 4
struct wiz_target {
	int disk;
	bool blank;                             /* whole blank disk, else free space */
	u32 mib;
};

struct wizard {
	enum wiz_step step;
	struct install_config cfg;
	int screen_w, screen_h;
	const char *error;
	wizard_random_fn random;
	u32 iterations;
	bool done;
	struct wiz_target target[WIZ_MAX_TARGETS];
	int ntargets;
	int target_sel;                         /* index into target[]; ntargets = do not save */
	char disk_info[2][112];                 /* what is on the first two disks */
	char option[WIZ_MAX_TARGETS][72];
	char line[4][112];                      /* summary text */
	const struct hw_profile *hw;            /* for the desktop recommendation */
	struct de_recommendation rec;
	char de_option[DE_COUNT + 1][64];
};

void wizard_init(struct wizard *w, int screen_w, int screen_h, wizard_random_fn random);
void wizard_draw_frame(const struct wizard *w);         /* background, panel, logo */
void wizard_build(struct wizard *w, struct ui_screen *s);
/* React to a widget id from ui_dispatch(). True if the screen must be rebuilt. */
bool wizard_handle(struct wizard *w, struct ui_screen *s, int id);

/* Login (login.c). True if `password` matches, or if password login is off. */
bool login_check(const struct install_config *c, const char *password, size_t len);
void login_draw_frame(int screen_w, int screen_h);
void login_build(struct ui_screen *s, int screen_w, int screen_h, const struct install_config *c,
		 char *title, size_t title_cap, bool wrong);

/* Saved setup <-> one disk sector (config.c). */
void config_pack(const struct install_config *c, u8 sector[512]);
int  config_unpack(const u8 sector[512], struct install_config *c);

/* Scripted run through every screen; returns the number of failed checks. */
int installer_selfcheck(int screen_w, int screen_h, bool draw);

#endif /* MERGEDOS_INSTALLER_H */
