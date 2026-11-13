/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Kernel side of the setup wizard: draws the portable ui/ toolkit on the
 * framebuffer, turns keyboard and mouse state into ui events, and supplies
 * randomness for password salts.
 */
#include <mergedos/kernel.h>
#include <mergedos/console.h>
#include <mergedos/gfx.h>
#include <mergedos/io.h>
#include <mergedos/monitor.h>
#include <mergedos/setup.h>

void ui_draw_fill(int x, int y, int w, int h, u32 rgb)
{
	gfx_fill(x, y, w, h, rgb);
}

void ui_draw_blend(int x, int y, u32 rgb, u8 alpha)
{
	gfx_blend(x, y, rgb, alpha);
}

void ui_draw_text(int x, int y, const char *s, u32 rgb, int scale)
{
	gfx_text(x, y, s, rgb, scale);
}

/*
 * Salt source. A salt must be unique, not secret, so a hash over the CPU's
 * hardware generator (when present) and the cycle counter sampled at every
 * key press and click is sufficient here. It is not a general-purpose CSPRNG.
 */
static struct sha256 pool;
static u32 pool_counter;

static u64 rdtsc(void)
{
	u32 lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

static void entropy_stir(void)
{
	u64 t = rdtsc();

	sha256_update(&pool, &t, sizeof(t));
	if (system_hw()->features & HWF_RDRAND) {
		u64 v = 0;
		u8 ok;

		__asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
		if (ok)
			sha256_update(&pool, &v, sizeof(v));
	}
}

static void kernel_random(u8 *buf, size_t n)
{
	while (n) {
		struct sha256 c;
		u8 out[SHA256_LEN];
		size_t take = MOS_MIN(n, sizeof(out));

		entropy_stir();
		c = pool;
		sha256_update(&c, &pool_counter, sizeof(pool_counter));
		pool_counter++;
		sha256_final(&c, out);
		memcpy(buf, out, take);
		buf += take;
		n -= take;
	}
}

/* ---- disks ------------------------------------------------------------------- */

static struct block_dev *disks[SETUP_MAX_DISKS];
static struct disk_layout layouts[SETUP_MAX_DISKS];
static struct disk_plan plans[SETUP_MAX_DISKS];
static int plan_rc[SETUP_MAX_DISKS];
static int ndisks;

static struct block_dev *disk_at(int i)
{
	return disks[i];
}

static void rescan(int i)
{
	struct block_dev *dev = disk_at(i);

	plan_rc[i] = disk_scan(dev, &layouts[i]);
	if (!plan_rc[i])
		plan_rc[i] = disk_plan(dev, &layouts[i], &plans[i]);
}

void storage_init(void)
{
	char text[200];

	/* SATA through AHCI first; legacy IDE ports as the fallback (both may be present). */
	int sata = ahci_init(), ide = ata_init();

	for (int i = 0; i < sata; i++)
		disks[ndisks++] = ahci_disk(i);
	for (int i = 0; i < ide; i++)
		disks[ndisks++] = ata_disk(i);
	if (!ndisks)
		kprintf("[disk] no SATA or IDE disk found (NVMe and USB storage: not supported yet)\n");
	for (int i = 0; i < ndisks; i++) {
		rescan(i);
		disk_describe(&layouts[i], text, sizeof(text));
		kprintf("[disk] %s: %s\n", disk_at(i)->name, text);
	}
}

bool setup_load_config(struct install_config *out)
{
	u8 sector[SECTOR_SIZE];

	for (int i = 0; i < ndisks; i++)
		if (!disk_find_mergedos(disk_at(i), &layouts[i], sector) &&
		    !config_unpack(sector, out))
			return true;
	return false;
}

/* Add the MergedOS partition planned for `disk` and store the setup in it. */
static int save_config(int disk, const struct install_config *cfg)
{
	u8 sector[SECTOR_SIZE];
	int rc;

	if (disk < 0 || disk >= ndisks || plan_rc[disk])
		return -MERR_INVAL;
	config_pack(cfg, sector);
	rc = disk_apply(disk_at(disk), &layouts[disk], &plans[disk], sector);
	kprintf("[disk] %s: MergedOS partition at sector %llu, %llu MiB: %s (%d)\n",
		disk_at(disk)->name, (unsigned long long)plans[disk].start,
		(unsigned long long)(plans[disk].sectors / 2048), rc ? "FAILED" : "written", rc);
	rescan(disk);
	return rc;
}

int storage_selftest(void)
{
	struct install_config want, got;
	bool ok;

	memset(&want, 0, sizeof(want));
	want.lang = LANG_ES;
	mos_strlcpy(want.name, "Zo\xC3\xAB N\xC3\xBA\xC3\xB1" "ez", sizeof(want.name));
	want.password_enabled = true;
	want.iterations = 1000;
	kernel_random(want.salt, sizeof(want.salt));
	pbkdf2_hmac_sha256("abc12345", 8, want.salt, sizeof(want.salt), want.iterations, want.hash);

	ok = !save_config(0, &want) && setup_load_config(&got) && got.lang == want.lang &&
	     !strcmp(got.name, want.name) && got.password_enabled && got.iterations == 1000 &&
	     !memcmp(got.salt, want.salt, sizeof(want.salt)) &&
	     !memcmp(got.hash, want.hash, sizeof(want.hash));
	kprintf("[disk] install self-test: %s\n", ok ? "ok" : "FAILED");
	return ok ? 0 : 1;
}

/* ---- input events ------------------------------------------------------------ */

/* Bytes >= 0x80 arrive from the serial line as UTF-8: reassemble code points. */
static int utf8_feed(int byte)
{
	static u32 cp;
	static int need;

	if ((byte & 0xC0) == 0x80) {
		if (!need)
			return -1;
		cp = (cp << 6) | (u32)(byte & 0x3F);
		return --need ? -1 : (int)cp;
	}
	need = (byte & 0xE0) == 0xC0 ? 1 : (byte & 0xF0) == 0xE0 ? 2 : (byte & 0xF8) == 0xF0 ? 3 : 0;
	cp = (u32)byte & (0x3Fu >> need);
	return -1;
}

static u32 seen_presses, seen_releases;

static void events_begin(void)
{
	seen_presses = mouse_get()->presses;
	seen_releases = mouse_get()->releases;
}

/* Fetch one pending key or click, if any. Keeps the pointer drawn while idle. */
static bool next_event(struct ui_event *ev)
{
	const struct mouse_state *m = mouse_get();
	int c = console_poll();                 /* also pumps the mouse */

	memset(ev, 0, sizeof(*ev));
	if (c >= 0x80 && c <= 0xFF)
		c = utf8_feed(c);
	if (c >= 0) {
		ev->type = UI_EV_KEY;
		ev->key = c;
	} else if (m->presses != seen_presses) {
		seen_presses++;
		ev->type = UI_EV_MOUSE_DOWN;
		ev->x = m->press_x;
		ev->y = m->press_y;
	} else if (m->releases != seen_releases) {
		seen_releases++;
		ev->type = UI_EV_MOUSE_UP;
		ev->x = m->release_x;
		ev->y = m->release_y;
	} else {
		gfx_cursor_refresh();
		cpu_relax();
		return false;
	}
	entropy_stir();
	return true;
}

/* ---- boot splash ------------------------------------------------------------- */

#define SPLASH_BAR_W 320

void splash_show(u32 percent)
{
	static bool drawn;
	const struct gfx_info *g = gfx_get();
	const int cx = (int)g->width / 2, cy = (int)g->height / 2;

	if (!g->active)
		return;
	if (!drawn) {
		console_fb_mute(true);          /* the log keeps going to the serial line */
		gfx_fill(0, 0, (int)g->width, (int)g->height, UI_COL_DESKTOP);
		ui_logo(cx - 64, cy - 150, 8);
		gfx_text(cx - 96, cy + 4, "MergedOS", 0xFFFFFF, 4);
		gfx_fill(cx - SPLASH_BAR_W / 2, cy + 70, SPLASH_BAR_W, 8, 0x2A3644);
		drawn = true;
	}
	gfx_fill(cx - SPLASH_BAR_W / 2, cy + 70, SPLASH_BAR_W * (int)MOS_MIN(percent, 100u) / 100, 8,
		 UI_COL_ACCENT);
}

void splash_done(void)
{
	console_fb_mute(false);
}

/* ---- setup wizard ------------------------------------------------------------ */

bool setup_run(struct install_config *out)
{
	static const char *const screens[] = { "language", "name", "password", "storage",
					       "wifi", "desktop", "summary" };
	static struct wizard wiz;
	static struct ui_screen scr;
	const struct gfx_info *g = gfx_get();
	bool rebuild = true, saved = false;

	sha256_init(&pool);
	wizard_init(&wiz, (int)g->width, (int)g->height, kernel_random);
	wiz.hw = system_hw();
	for (int i = 0; i < ndisks; i++) {
		if (i < 2) {
			ksnprintf(wiz.disk_info[i], sizeof(wiz.disk_info[i]), "%s: ", disk_at(i)->name);
			disk_describe(&layouts[i], wiz.disk_info[i] + strlen(wiz.disk_info[i]),
				      sizeof(wiz.disk_info[i]) - strlen(wiz.disk_info[i]));
		}
		if (!plan_rc[i] && wiz.ntargets < WIZ_MAX_TARGETS)
			wiz.target[wiz.ntargets++] = (struct wiz_target){
				i, plans[i].blank, (u32)(plans[i].sectors / 2048) };
	}
	if (!ndisks)
		mos_strlcpy(wiz.disk_info[0], "No SATA or IDE disk found. NVMe and USB disks are "
			    "not supported yet.", sizeof(wiz.disk_info[0]));
	console_fb_mute(true);                  /* the wizard owns the screen */
	events_begin();

	while (!wiz.done) {
		struct ui_event ev;
		int id;

		if (rebuild) {
			wizard_build(&wiz, &scr);
			wizard_draw_frame(&wiz);
			ui_draw(&scr, true);
			kprintf("[setup] screen: %s\n", screens[wiz.step]);
			rebuild = false;
		}
		if (!next_event(&ev))
			continue;
		id = ui_dispatch(&scr, &ev);
		if (id >= 0)
			rebuild = wizard_handle(&wiz, &scr, id);
		if (!rebuild)
			ui_draw(&scr, false);
		gfx_cursor_refresh();
	}

	*out = wiz.cfg;
	if (wiz.target_sel < wiz.ntargets)
		saved = !save_config(wiz.target[wiz.target_sel].disk, out);
	else
		kprintf("[setup] not saved to disk: the wizard will run again on the next boot\n");
	console_fb_mute(false);
	return saved;
}

/* ---- login ------------------------------------------------------------------- */

/* Each wrong attempt costs twice as long as the one before: 1, 2, 4, 8, 16 s. */
static void login_penalty(int fails)
{
	kprintf("[login] wrong password (attempt %d)\n", fails);
	time_delay_ms(1000u << MOS_MIN(fails - 1, 4));
}

/* Without a framebuffer: prompt on the text console; typed characters are not echoed. */
static void login_text(const struct install_config *cfg)
{
	char pw[UI_TEXT_MAX + 1];

	for (int fails = 0;;) {
		size_t n = 0;
		bool ok;
		int c;

		kprintf("Password for %s: ", cfg->name);
		while ((c = console_getc()) != '\n') {
			if (c == '\b')
				n -= n ? 1 : 0;
			else if (c >= ' ' && c < 0x7F && n < UI_TEXT_MAX)
				pw[n++] = (char)c;
		}
		kprintf("\n");
		ok = login_check(cfg, pw, n);
		secure_wipe(pw, sizeof(pw));
		if (ok)
			return;
		login_penalty(++fails);
	}
}

void login_run(const struct install_config *cfg)
{
	static struct ui_screen scr;
	const struct gfx_info *g = gfx_get();
	char title[96];
	bool rebuild = true, wrong = false;
	int fails = 0;

	if (!cfg->password_enabled)
		return;
	kprintf("[login] password required for user \"%s\"\n", cfg->name);
	if (!g->active) {
		login_text(cfg);
		kprintf("[login] ok\n");
		return;
	}
	console_fb_mute(true);
	events_begin();
	for (;;) {
		struct ui_event ev;

		if (rebuild) {
			login_build(&scr, (int)g->width, (int)g->height, cfg, title, sizeof(title),
				    wrong);
			login_draw_frame((int)g->width, (int)g->height);
			ui_draw(&scr, true);
			rebuild = false;
		}
		if (!next_event(&ev))
			continue;
		if (ui_dispatch(&scr, &ev) == ID_LOGIN_BTN) {
			struct ui_widget *box = ui_find(&scr, ID_LOGIN_PW);
			bool ok = login_check(cfg, box->text, (size_t)box->len);

			secure_wipe(box->text, sizeof(box->text));      /* never keep it around */
			box->len = 0;
			if (ok)
				break;
			login_penalty(++fails);
			wrong = rebuild = true;
			continue;
		}
		ui_draw(&scr, false);
		gfx_cursor_refresh();
	}
	kprintf("[login] ok\n");
	console_fb_mute(false);
}

int setup_selftest(void)
{
	const struct gfx_info *g = gfx_get();
	int bad;

	/* With a framebuffer the wizard is really drawn; without one only its logic runs. */
	console_fb_mute(true);
	bad = installer_selfcheck(g->active ? (int)g->width : GFX_DEFAULT_WIDTH,
				  g->active ? (int)g->height : GFX_DEFAULT_HEIGHT, g->active);
	console_fb_mute(false);
	return bad;
}
