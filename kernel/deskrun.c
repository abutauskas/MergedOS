/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Kernel side of the graphical desktop.
 *
 * The portable window manager (desktop/desktop.c) keeps the windows; this
 * file feeds it the mouse and fills the windows. The terminal window is the
 * text console, placed inside the window frame; msh keeps running in the
 * main thread exactly as in text mode. Whenever another window overlaps the
 * terminal the console is muted (text still reaches the grid and serial)
 * and the damaged cells are repainted on demand, so it never draws over a
 * window above it. A second kernel thread watches the mouse: launchers open
 * windows or type commands, the title bar drags an outline and moves the
 * window on release, the controls close, minimise and maximise, and the
 * status text is refreshed each second.
 */
#include <mergedos/kernel.h>
#include <mergedos/boot.h>
#include <mergedos/console.h>
#include <mergedos/gfx.h>
#include <mergedos/initrd.h>
#include <mergedos/mm.h>
#include <mergedos/monitor.h>
#include <mergedos/sched.h>
#include <mergedos/setup.h>
#include "abi_dispatch/binfmt.h"
#include "desktop/desktop.h"
#include "mergedfence/fence.h"

static struct desktop desk;
static bool running, term_muted;
static int drag_id, drag_x, drag_y;     /* window being dragged and where it was grabbed */
static struct desk_rect outline;
static bool outline_on;

static void trace2(const char *what, const char *name)
{
	console_trace("\n[desk] ");
	console_trace(what);
	console_trace(name);
	console_trace("\n");
}

/* ---- window contents ------------------------------------------------------- */

static bool looks_text(const u8 *p, u64 n)
{
	for (u64 i = 0; i < n && i < 256; i++)
		if (p[i] < 0x20 && p[i] != '\n' && p[i] != '\r' && p[i] != '\t')
			return false;
	return true;
}

/* The i-th ramdisk file the Files window lists: programs and text files. */
static bool file_entry(int i, struct tar_entry *e, bool *exe)
{
	size_t off = 0;
	int n = 0;

	while (initrd_next(&off, e) > 0) {
		struct binfmt_info bi;

		if ((e->type != '0' && e->type != '\0') || strlen(e->name) > 100)
			continue;
		*exe = !binfmt_probe(e->data, (size_t)e->size, &bi) && bi.format != BINFMT_SCRIPT;
		if (!*exe && !looks_text(e->data, e->size))
			continue;
		if (n++ == i)
			return true;
	}
	return false;
}

static const char *system_line(char *buf, size_t cap, int i)
{
	const struct hw_profile *hw = system_hw();
	struct pmm_stats st;
	const char *brand = hw->cpu_brand;

	while (*brand == ' ')
		brand++;
	pmm_get_stats(&st);
	switch (i) {
	case 0: ksnprintf(buf, cap, "CPU      %s", *brand ? brand : hw->cpu_vendor); break;
	case 1: ksnprintf(buf, cap, "         %u logical, family %u model %u", hw->cpus,
			  hw->cpu_family, hw->cpu_model); break;
	case 2: ksnprintf(buf, cap, "Host     %s", hw->hypervisor[0] ? hw->hypervisor : "bare metal");
		break;
	case 3: ksnprintf(buf, cap, "Memory   %u MiB (firmware)", hw->ram_mib); break;
	case 4: ksnprintf(buf, cap, "         %llu KiB usable, %llu KiB free",
			  (unsigned long long)st.usable_frames * 4,
			  (unsigned long long)st.free_frames * 4); break;
	case 5: ksnprintf(buf, cap, "Kernel   %llu KiB resident",
			  (unsigned long long)(st.usable_frames - st.free_frames) * 4); break;
	case 6: ksnprintf(buf, cap, "Display  %ux%ux%u, %s", gfx_get()->width, gfx_get()->height,
			  gfx_get()->bpp, hw_gpu_tier_name(hw->gpu_tier)); break;
	case 7: ksnprintf(buf, cap, "GPU      %s", hw->ngpu && hw->gpu[0].name ? hw->gpu[0].name : "-");
		break;
	case 8: ksnprintf(buf, cap, "PCI      %u devices, %u storage, %u net", hw->pci_devices,
			  hw->n_storage, hw->n_net); break;
	default: return NULL;
	}
	return buf;
}

static const char *fence_line(char *buf, size_t cap, int i)
{
	const struct fence *f = monitor_fence();
	const struct fence_stats *st = f ? fence_stats(f) : NULL;

	if (i == DESK_LINE_BUTTON)
		return f && f->enabled ? "Turn off" : "Turn on";
	if (!f) {
		static const char *const idle[] = { "Blocklist not loaded yet.",
						    "Turn on, or type mergedfence", "status in the terminal." };

		return i < 3 ? idle[i] : NULL;
	}
	switch (i) {
	case 0: ksnprintf(buf, cap, "Mergedfence is %s", f->enabled ? "ON" : "off"); break;
	case 1: ksnprintf(buf, cap, "Rules    %u (%u exceptions)", st->rules, st->exceptions); break;
	case 2: ksnprintf(buf, cap, "Checks   %u", st->checks); break;
	case 3: ksnprintf(buf, cap, "Blocked  %u, excepted %u", st->blocked, st->excepted); break;
	case 4: ksnprintf(buf, cap, "Skipped  %u lines, %u dropped", st->ignored, st->dropped); break;
	default: return NULL;
	}
	return buf;
}

static const char *host_line(void *ctx, enum desk_kind kind, int i)
{
	static char buf[112];
	struct tar_entry e;
	bool exe;

	(void)ctx;
	switch (kind) {
	case DESK_WIN_FILES:
		if (i < 0 || !file_entry(i, &e, &exe))
			return NULL;
		ksnprintf(buf, sizeof(buf), "%s  /%s", exe ? "run" : "cat", e.name);
		return buf;
	case DESK_WIN_SYSTEM:
		return i < 0 ? NULL : system_line(buf, sizeof(buf), i);
	case DESK_WIN_FENCE:
		return fence_line(buf, sizeof(buf), i);
	default:
		return NULL;
	}
}

static struct desk_rect host_term(void *ctx, struct desk_rect r)
{
	(void)ctx;
	console_fb_repaint(&r.x, &r.y, &r.w, &r.h);
	return r;
}

static const struct desk_host host = { NULL, host_line, host_term };

/* ---- repainting ---------------------------------------------------------------- */

static const struct desk_window *terminal(void)
{
	return desktop_find_kind(&desk, DESK_WIN_TERMINAL);
}

static void draw_status(void)
{
	char text[40];
	u64 s = time_ticks() / SCHED_HZ;

	ksnprintf(text, sizeof(text), "%s  %02llu:%02llu:%02llu", system_user(),
		  (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60),
		  (unsigned long long)(s % 60));
	desktop_draw_status(&desk, text);
	gfx_cursor_refresh();
}

/*
 * Repaint what the last window operations spoiled. The console is muted
 * first if anything now covers the terminal, and unmuted (one full
 * repaint) once it is uncovered again.
 */
static void flush(void)
{
	const struct desk_window *t = terminal();
	const struct desk_rect c = desktop_content_rect(&desk, t);
	const bool hide = !desktop_visible(t) || desktop_covered(&desk, t->id);

	if (hide && !term_muted)
		console_fb_mute(true);
	console_fb_frame(c.x, c.y, c.x, c.y, c.w, c.h);
	for (int i = 0; i < desk.ndamage; i++)
		desktop_paint(&desk, desk.damage[i], &host);
	if (desk.bar_dirty) {
		desktop_paint(&desk, desk.bar, &host);
		desktop_paint(&desk, desk.dock, &host);
	}
	desk.ndamage = 0;
	desk.bar_dirty = false;
	if (!hide && term_muted)
		console_fb_mute(false);
	term_muted = hide;
	gfx_cursor_refresh();
}

/* System and Mergedfence windows show live numbers: repaint when they change. */
static u64 content_sig(enum desk_kind kind)
{
	const struct fence *f;
	struct pmm_stats st;

	if (kind == DESK_WIN_SYSTEM) {
		pmm_get_stats(&st);
		return st.free_frames;
	}
	f = monitor_fence();
	if (kind != DESK_WIN_FENCE || !f)
		return 0;
	return 1 + f->enabled + ((u64)f->st.checks << 1) + ((u64)f->st.rules << 32);
}

static void refresh_contents(void)
{
	static u64 sig[DESK_KINDS];

	for (int k = DESK_WIN_SYSTEM; k <= DESK_WIN_FENCE; k++) {
		u64 now = content_sig((enum desk_kind)k);

		if (now == sig[k])
			continue;
		sig[k] = now;
		for (int s = 0; s < DESK_MAX_WINDOWS; s++)
			if (desk.win[s].kind == (enum desk_kind)k && desktop_visible(&desk.win[s]))
				desktop_paint(&desk, desktop_content_rect(&desk, &desk.win[s]), &host);
		gfx_cursor_refresh();
	}
}

/* ---- mouse ------------------------------------------------------------------------ */

static void launcher(enum desk_action a)
{
	const enum desk_kind kind = desktop_launcher_kind(a);
	const char *cmd = desktop_command(a);

	trace2("launcher: ", desktop_action_name(a));
	if (kind != DESK_WIN_NONE) {
		const struct desk_window *w = desktop_find_kind(&desk, kind);
		bool opened;

		/* Tiles have no close button: a launcher toggles its focused window. */
		if (desk.tiled && w && w->id == desktop_top(&desk) && kind != DESK_WIN_TERMINAL) {
			(void)desktop_close(&desk, w->id);
			trace2("window close ", desktop_kind_name(kind));
		} else if ((w = desktop_window(&desk, desktop_launch(&desk, kind, &opened)))) {
			const struct desk_rect t = desktop_title_rect(&desk, w);
			char where[48];

			trace2(opened ? "window open " : "window focus ", desktop_kind_name(kind));
			/* Where its title bar is, for test rigs that drag it. */
			ksnprintf(where, sizeof(where), "%s title %d %d", desktop_kind_name(kind),
				  t.x + t.w / 2, t.y + t.h / 2);
			trace2("window ", where);
		} else {
			trace2("no free window slot for ", desktop_kind_name(kind));
		}
	}
	if (cmd) {
		console_inject(cmd);
		console_inject("\n");
	}
}

static void file_clicked(int row)
{
	struct tar_entry e;
	char cmd[128];
	bool exe;

	if (row < 0 || !file_entry(row, &e, &exe))
		return;
	ksnprintf(cmd, sizeof(cmd), "%s /%s\n", exe ? "run" : "cat", e.name);
	trace2("files: ", e.name);
	console_inject(cmd);
	(void)desktop_focus(&desk, terminal()->id);     /* show the output */
}

static void press(int x, int y)
{
	const struct desk_hit h = desktop_hit(&desk, x, y);
	const struct desk_window *w = desktop_window(&desk, h.win);

	switch (h.part) {
	case DESK_PART_LAUNCHER:
		launcher(h.action);
		break;
	case DESK_PART_TASK:
		(void)desktop_focus(&desk, h.win);
		break;
	case DESK_PART_TITLE:
		(void)desktop_focus(&desk, h.win);
		if (desk.movable && w->state == DESK_NORMAL) {
			drag_id = h.win;
			drag_x = x;
			drag_y = y;
			outline_on = false;
		}
		break;
	case DESK_PART_CLOSE:
		trace2("window close ", desktop_kind_name(w->kind));
		(void)desktop_close(&desk, h.win);
		break;
	case DESK_PART_MINIMISE:
		(void)desktop_minimise(&desk, h.win);
		break;
	case DESK_PART_MAXIMISE:
		(void)desktop_toggle_max(&desk, h.win);
		break;
	case DESK_PART_CONTENT:
		(void)desktop_focus(&desk, h.win);
		if (w->kind == DESK_WIN_FILES)
			file_clicked(h.row);
		break;
	case DESK_PART_BUTTON: {
		const struct fence *f = monitor_fence();

		(void)desktop_focus(&desk, h.win);
		trace2("fence: ", f && f->enabled ? "off" : "on");
		console_inject(f && f->enabled ? "mergedfence off\n" : "mergedfence on\n");
		break;
	}
	default:
		break;
	}
	flush();
}

static void erase_outline(void)
{
	struct desk_rect s[4];

	if (!outline_on)
		return;
	desktop_outline_strips(outline, s);
	for (int i = 0; i < 4; i++)
		desktop_paint(&desk, s[i], &host);
	outline_on = false;
}

/* While the button is held on a title bar: a 2-pixel outline follows the pointer. */
static void drag_to(int x, int y)
{
	const struct desk_window *w = desktop_window(&desk, drag_id);
	struct desk_rect r;

	if (!w)
		return;
	r = desktop_moved_frame(&desk, w, x - drag_x, y - drag_y);
	if (outline_on && r.x == outline.x && r.y == outline.y)
		return;
	erase_outline();
	if (r.x != w->frame.x || r.y != w->frame.y) {
		desktop_draw_outline(&desk, r);
		outline = r;
		outline_on = true;
	}
	gfx_cursor_refresh();
}

static void release(int x, int y)
{
	if (!drag_id)
		return;
	erase_outline();
	if (desktop_move(&desk, drag_id, x - drag_x, y - drag_y)) {
		flush();
		console_trace("\n[desk] window moved\n");
	}
	gfx_cursor_refresh();
	drag_id = 0;
}

static void desk_thread(void *arg)
{
	const struct mouse_state *m = mouse_get();
	u32 presses = m->presses, releases = m->releases;
	u64 last_status = 0;

	(void)arg;
	for (;;) {
		if (m->presses != presses) {
			presses = m->presses;
			press(m->press_x, m->press_y);
		}
		if (drag_id && m->releases == releases && (m->buttons & MOUSE_LEFT))
			drag_to(m->x, m->y);
		if (m->releases != releases) {
			releases = m->releases;
			release(m->release_x, m->release_y);
		}
		if (time_ticks() - last_status >= SCHED_HZ) {
			last_status = time_ticks();
			draw_status();
			refresh_contents();
		}
		thread_sleep_ms(20);
	}
}

bool desktop_start(enum de_id layout)
{
	const struct gfx_info *g = gfx_get();
	struct desk_rect t;
	int cw, ch;

	if (!g->active)
		return false;
	console_fb_size(&cw, &ch);
	desktop_layout(&desk, layout, (int)g->width, (int)g->height, cw, ch);
	t = desktop_title_rect(&desk, terminal());
	/* Positions for test rigs that drive the pointer (logged, then cleared off screen). */
	for (int i = 0; i < DESK_ITEMS; i++)
		kprintf("[desk] icon %s %d %d\n", desktop_action_name(desk.item[i].action),
			desk.item[i].r.x + desk.item[i].r.w / 2, desk.item[i].r.y + desk.item[i].r.h / 2);
	kprintf("[desk] title %d %d pointer %d %d\n", t.x + t.w / 2, t.y + t.h / 2,
		mouse_get()->x, mouse_get()->y);
	console_set_region(0, VGA_ROWS - 1);
	console_set_attr(desktop_console_attr(layout));
	console_clear();
	if (boot_cmdline_get("desk_demo", NULL, 0)) {   /* overlapping windows to look at */
		(void)desktop_open(&desk, DESK_WIN_SYSTEM);
		(void)desktop_open(&desk, DESK_WIN_FILES);
	}
	/* Paint everything once with the console muted; flush() unmutes it if uncovered. */
	console_fb_mute(true);
	term_muted = true;
	desk.ndamage = 1;
	desk.damage[0] = (struct desk_rect){ 0, 0, desk.sw, desk.sh };
	desk.bar_dirty = false;
	flush();
	draw_status();
	if (!running) {
		running = thread_create(desk_thread, NULL, "desktop") >= 0;
		if (!running)
			kprintf("[desk] no thread slot: launchers and windows are off\n");
	}
	return true;
}

int desktop_selftest(void)
{
	const struct gfx_info *g = gfx_get();
	int cw = 960, ch = 600, bad;

	if (g->active)
		console_fb_size(&cw, &ch);
	console_fb_mute(true);
	bad = desktop_selfcheck(g->active ? (int)g->width : GFX_DEFAULT_WIDTH,
				g->active ? (int)g->height : GFX_DEFAULT_HEIGHT, cw, ch, g->active);
	console_fb_mute(false);
	return bad;
}
