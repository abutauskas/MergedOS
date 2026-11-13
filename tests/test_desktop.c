/* SPDX-License-Identifier: BSD-2-Clause */
#include "tests/test.h"
#include "desktop/desktop.h"

static int term_calls;
static struct desk_rect term_last;

static const char *mock_line(void *ctx, enum desk_kind kind, int i)
{
	static const char *const files[] = { "bin/hello.elf          run",
					     "etc/motd               cat", NULL };

	(void)ctx;
	if (i == DESK_LINE_TITLE)
		return NULL;
	if (i == DESK_LINE_BUTTON)
		return kind == DESK_WIN_FENCE ? "Turn on" : NULL;
	if (kind == DESK_WIN_FILES)
		return i < 2 ? files[i] : NULL;
	if (kind == DESK_WIN_SYSTEM)
		return i < 3 ? "CPU: QEMU Virtual CPU, 1 logical processor" : NULL;
	return i < 2 ? "Mergedfence: disabled" : NULL;
}

static struct desk_rect mock_term(void *ctx, struct desk_rect clip)
{
	(void)ctx;
	term_calls++;
	term_last = clip;
	return clip;
}

static const struct desk_host host = { NULL, mock_line, mock_term, NULL, NULL };

static bool rect_in(struct desk_rect in, struct desk_rect out)
{
	return in.x >= out.x && in.y >= out.y && in.x + in.w <= out.x + out.w &&
	       in.y + in.h <= out.y + out.h;
}

static bool disjoint(struct desk_rect a, struct desk_rect b)
{
	return desk_rect_empty(desk_rect_and(a, b));
}

/* Repaint `r`; nothing may be drawn outside it. */
static bool paint_stays_inside(struct desktop *d, struct desk_rect r)
{
	mock_bounds_reset();
	desktop_paint(d, r, &host);
	if (mock_x1 < mock_x0)
		return true;                    /* nothing drawn at all */
	return mock_x0 >= r.x && mock_y0 >= r.y && mock_x1 <= r.x + r.w && mock_y1 <= r.y + r.h;
}

static void test_tiles(void)
{
	struct desktop d;

	desktop_layout(&d, DE_TILING, 1024, 768, 960, 600);
	for (int n = 1; n <= 4; n++) {
		long long area = 0;
		int seen = 0;

		if (n > 1)
			CHECK(desktop_open(&d, n == 3 ? DESK_WIN_SYSTEM : DESK_WIN_FILES) != 0);
		for (int a = 0; a < DESK_MAX_WINDOWS; a++) {
			if (!desktop_visible(&d.win[a]))
				continue;
			seen++;
			area += (long long)d.win[a].frame.w * d.win[a].frame.h;
			CHECK(rect_in(d.win[a].frame, d.work));
			for (int b = a + 1; b < DESK_MAX_WINDOWS; b++)
				if (desktop_visible(&d.win[b]))
					CHECK(disjoint(d.win[a].frame, d.win[b].frame));
		}
		CHECK_EQ(seen, n);
		CHECK_EQ(area, (long long)d.work.w * d.work.h);   /* union covers the work area */
	}
	/* 1: full; 2: halves side by side; 3+: master on the left, stack on the right. */
	CHECK_EQ(d.win[0].frame.w, d.work.w / 2);
	CHECK_EQ(d.win[0].frame.h, d.work.h);
	CHECK_EQ(d.win[1].frame.x, d.work.x + d.work.w / 2);
	CHECK(d.win[2].frame.y > d.win[1].frame.y && d.win[3].frame.y > d.win[2].frame.y);
	/* No controls, no dragging, no maximise; the title only focuses. */
	{
		struct desk_rect t = desktop_title_rect(&d, &d.win[0]);
		struct desk_hit h = desktop_hit(&d, t.x + 12, t.y + 3);

		CHECK(h.part == DESK_PART_TITLE && h.win == 1);
		CHECK(!desktop_move(&d, 1, 10, 10) && !desktop_toggle_max(&d, 1));
	}
	/* Closing one re-tiles the rest. */
	CHECK(desktop_close(&d, d.win[1].id));
	CHECK_EQ(d.win[0].frame.w, d.work.w / 2);
	CHECK(d.win[2].frame.y == d.work.y && d.win[3].frame.y > d.work.y);
	CHECK(paint_stays_inside(&d, (struct desk_rect){ 500, 300, 40, 40 }));
}

static void test_windows(void)
{
	struct desktop d;
	struct desk_window *t, *f, *s;
	struct desk_rect o, saved;
	struct desk_hit h;
	int files, sys;
	bool opened;

	desktop_layout(&d, DE_DOCK, 1024, 768, 960, 600);
	t = desktop_window(&d, 1);
	CHECK(t && t->kind == DESK_WIN_TERMINAL && d.nz == 1 && desktop_top(&d) == 1);
	CHECK_EQ(d.ndamage, 0);

	/* Open order is z order; launching an open kind focuses it instead. */
	files = desktop_launch(&d, DESK_WIN_FILES, &opened);
	CHECK(files > 1 && opened && desktop_top(&d) == files);
	CHECK(d.ndamage > 0 && d.bar_dirty);
	sys = desktop_open(&d, DESK_WIN_SYSTEM);
	CHECK(sys > files && desktop_top(&d) == sys && d.nz == 3);
	CHECK_EQ(desktop_launch(&d, DESK_WIN_FILES, &opened), files);
	CHECK(!opened && desktop_top(&d) == files && d.z[2] == 1 && d.z[1] == 2);
	f = desktop_window(&d, files);
	s = desktop_window(&d, sys);

	/* Overlap: the topmost window wins the hit-test, and the terminal is covered. */
	o = desk_rect_and(f->frame, s->frame);
	CHECK(!desk_rect_empty(o));
	CHECK_EQ(desktop_hit(&d, o.x + o.w / 2, o.y + o.h / 2).win, files);
	CHECK(desktop_focus(&d, sys));
	CHECK_EQ(desktop_hit(&d, o.x + o.w / 2, o.y + o.h / 2).win, sys);
	CHECK(desktop_covered(&d, 1) && desktop_covered(&d, files) && !desktop_covered(&d, sys));
	CHECK(!desktop_focus(&d, sys));                 /* already on top */

	/* Dock controls: close, minimise, maximise discs on the left of the title. */
	{
		struct desk_rect tr = desktop_title_rect(&d, s);

		CHECK_EQ(desktop_hit(&d, tr.x + 16, tr.y + tr.h / 2).part, DESK_PART_CLOSE);
		CHECK_EQ(desktop_hit(&d, tr.x + 36, tr.y + tr.h / 2).part, DESK_PART_MINIMISE);
		CHECK_EQ(desktop_hit(&d, tr.x + 56, tr.y + tr.h / 2).part, DESK_PART_MAXIMISE);
		CHECK_EQ(desktop_hit(&d, tr.x + 200, tr.y + tr.h / 2).part, DESK_PART_TITLE);
	}
	/* Content rows for the Files list. */
	{
		struct desk_rect c = desktop_content_rect(&d, f);

		CHECK(desktop_focus(&d, files));
		h = desktop_hit(&d, c.x + 30, c.y + 8 + 24 + 5);
		CHECK(h.part == DESK_PART_CONTENT && h.win == files && h.row == 1);
	}

	/* Minimise hides from the hit-test and drops to the bottom; focus restores it. */
	CHECK(desktop_minimise(&d, files));
	CHECK(f->state == DESK_MINIMISED && desktop_top(&d) == sys && d.z[0] == 1);
	h = desktop_hit(&d, f->frame.x + 3, f->frame.y + 3);
	CHECK(h.win != files);
	CHECK(desktop_focus(&d, files) && f->state == DESK_NORMAL && desktop_top(&d) == files);

	/* Maximise fills the work area; restore returns the exact old frame. */
	saved = s->frame;
	CHECK(desktop_toggle_max(&d, sys));
	CHECK(s->state == DESK_MAXIMISED && desktop_top(&d) == sys);
	CHECK(s->frame.x == d.work.x && s->frame.y == d.work.y && s->frame.w == d.work.w &&
	      s->frame.h == d.work.h);
	CHECK(!desktop_move(&d, sys, 10, 10));
	/* Minimised while maximised: comes back maximised. */
	CHECK(desktop_minimise(&d, sys) && desktop_focus(&d, sys) && s->state == DESK_MAXIMISED);
	CHECK(desktop_toggle_max(&d, sys) && s->state == DESK_NORMAL);
	CHECK(s->frame.x == saved.x && s->frame.y == saved.y && s->frame.w == saved.w &&
	      s->frame.h == saved.h);

	/* Moves follow the delta, clamp to the work area, and damage old and new frames. */
	saved = s->frame;
	d.ndamage = 0;
	CHECK(desktop_move(&d, sys, 37, 21));
	CHECK(s->frame.x == saved.x + 37 && s->frame.y == saved.y + 21);
	CHECK(d.ndamage == 2 && rect_in(saved, d.damage[0]) && rect_in(s->frame, d.damage[1]));
	CHECK(desktop_move(&d, sys, -100000, 100000));
	CHECK(s->frame.x == d.work.x && s->frame.y + s->frame.h == d.work.y + d.work.h);
	{
		struct desk_rect m = desktop_moved_frame(&d, s, 100000, -100000);

		CHECK(m.x + m.w == d.work.x + d.work.w && m.y == d.work.y && m.w == s->frame.w);
	}

	/* The terminal cannot be closed: its close button minimises it. */
	CHECK(!desktop_close(&d, 1) && desktop_window(&d, 1) && t->state == DESK_MINIMISED);
	CHECK(!desktop_covered(&d, 1));
	CHECK(desktop_focus(&d, 1) && t->state == DESK_NORMAL && desktop_top(&d) == 1);
	CHECK(desktop_open(&d, DESK_WIN_TERMINAL) == 0);

	/* Close removes it from the stack; the next window down gets the focus. */
	CHECK(desktop_focus(&d, files) && desktop_close(&d, files));
	CHECK(desktop_window(&d, files) == NULL && d.nz == 2 && desktop_top(&d) == 1);
	CHECK(!desktop_close(&d, files));

	/* Capacity: six windows; the seventh open fails cleanly. */
	while (d.nz < DESK_MAX_WINDOWS)
		CHECK(desktop_open(&d, DESK_WIN_FENCE) != 0);
	CHECK_EQ(desktop_open(&d, DESK_WIN_FILES), 0);
	CHECK_EQ(d.nz, DESK_MAX_WINDOWS);
	CHECK(desktop_launch(&d, DESK_WIN_FILES, &opened) == 0 && opened);
}

static void test_repaint(void)
{
	struct desktop d;
	int files, sys, fence;
	const struct desk_rect probes[] = {
		{ 0, 0, 1024, 768 },  { 10, 10, 300, 200 },  { 37, 41, 3, 90 },
		{ 100, 52, 200, 3 },  { 0, 0, 40, 20 },     { 300, 690, 400, 70 },
		{ 60, 60, 61, 47 },   { 2, 30, 1022, 700 },  { 520, 400, 7, 7 },
	};

	desktop_layout(&d, DE_TASKBAR, 1024, 768, 960, 600);
	files = desktop_open(&d, DESK_WIN_FILES);
	sys = desktop_open(&d, DESK_WIN_SYSTEM);
	fence = desktop_open(&d, DESK_WIN_FENCE);
	CHECK(files && sys && fence);
	desktop_draw_status(&d, "guest  00:01:02");

	/* Full paint: window text comes from the host, controls from primitives. */
	mock_draw_len = 0;
	term_calls = 0;
	desktop_draw(&d, &host);
	CHECK(strstr(mock_draw_log, "Mergedfence") && strstr(mock_draw_log, "Turn on"));
	CHECK(strstr(mock_draw_log, "etc/motd") && strstr(mock_draw_log, "guest"));
	CHECK(term_calls == 1 && rect_in(term_last, desktop_content_rect(&d, desktop_window(&d, 1))));

	/* Partial repaints draw nothing outside the damaged rectangle, whatever it cuts. */
	for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++)
		CHECK(paint_stays_inside(&d, probes[i]));
	{
		struct desk_window *w = desktop_window(&d, sys);
		struct desk_rect t = desktop_title_rect(&d, w), strips[4];

		CHECK(paint_stays_inside(&d, (struct desk_rect){ t.x + 30, t.y + 5, 9, 5 }));
		CHECK(paint_stays_inside(&d, (struct desk_rect){ t.x + t.w - 60, t.y, 50, 9 }));
		desktop_outline_strips(w->frame, strips);
		for (int k = 0; k < 4; k++)
			CHECK(paint_stays_inside(&d, strips[k]));
	}
	/* The terminal's damaged part is handed to the host, clipped to the damage. */
	{
		struct desk_rect c = desktop_content_rect(&d, desktop_window(&d, 1));
		struct desk_rect r = { c.x + c.w - 50, c.y + c.h - 40, 30, 20 };

		term_calls = 0;
		CHECK(paint_stays_inside(&d, r));
		CHECK(term_calls == 1 && rect_in(term_last, r));
	}
	/* Damage after a raise covers the raised window. */
	d.ndamage = 0;
	CHECK(desktop_focus(&d, files));
	CHECK(d.ndamage >= 1 && rect_in(desktop_window(&d, files)->frame, d.damage[0]));
	/* Taskbar strip: one button per window; a click there names the window. */
	{
		struct desk_hit h = desktop_hit(&d, d.tasks.x + 5, d.tasks.y + 5);

		CHECK(h.part == DESK_PART_TASK && h.win == 1);
	}
	/* Fence window: the toggle button. */
	{
		struct desk_window *w = desktop_window(&d, fence);
		struct desk_rect c = desktop_content_rect(&d, w);

		CHECK(desktop_focus(&d, fence));
		CHECK_EQ(desktop_hit(&d, c.x + 20, c.y + c.h - 20).part, DESK_PART_BUTTON);
	}
}

void test_desktop(void)
{
	struct desktop d;

	/* Layout and window rules hold for every layout at several sizes. */
	CHECK_EQ(desktop_selfcheck(1024, 768, 960, 600, true), 0);
	CHECK_EQ(desktop_selfcheck(1280, 800, 960, 600, false), 0);
	CHECK_EQ(desktop_selfcheck(1920, 1080, 960, 600, false), 0);
	CHECK_EQ(desktop_selfcheck(800, 600, 480, 300, false), 0);
	CHECK_EQ(desktop_selfcheck(640, 480, 480, 300, true), 0);
	CHECK(mock_fill_count > 0 && mock_blend_count > 0);

	/* Dock layout: launchers sit in the dock, below the window. */
	desktop_layout(&d, DE_DOCK, 1024, 768, 960, 600);
	CHECK(d.dock.w > 0 && d.item[0].r.y >= d.dock.y);
	CHECK(d.win[0].frame.y + d.win[0].frame.h <= d.dock.y);
	CHECK(d.bar.y == 0 && d.movable && desk_rect_empty(d.tasks));
	/* Taskbar layout: panel along the bottom edge, launchers and window buttons inside. */
	desktop_layout(&d, DE_TASKBAR, 1024, 768, 960, 600);
	CHECK(d.bar.y + d.bar.h == 768 && d.item[0].r.y > d.bar.y && d.dock.w == 0);
	CHECK(rect_in(d.tasks, d.bar) && d.tasks.w > 100);
	/* Retro: one close box; no minimise or maximise. */
	desktop_layout(&d, DE_RETRO, 640, 480, 480, 300);
	{
		struct desk_rect tr = desktop_title_rect(&d, &d.win[0]);

		CHECK_EQ(desktop_hit(&d, tr.x + 12, tr.y + tr.h / 2).part, DESK_PART_CLOSE);
		CHECK_EQ(desktop_hit(&d, tr.x + tr.w - 12, tr.y + tr.h / 2).part, DESK_PART_TITLE);
	}

	test_tiles();
	test_windows();
	test_repaint();

	CHECK_STR(desktop_command(DESK_FILES), "ls /");
	CHECK_STR(desktop_command(DESK_POWER), "poweroff");
	CHECK_STR(desktop_command(DESK_HELP), "help");
	CHECK(desktop_command(DESK_SYSTEM) == NULL && desktop_command(DESK_NONE) == NULL);
	CHECK(desktop_launcher_kind(DESK_FENCE) == DESK_WIN_FENCE);
	CHECK(desktop_launcher_kind(DESK_PACKAGES) == DESK_WIN_NONE);
	CHECK_STR(desktop_kind_name(DESK_WIN_FILES), "files");
	for (int id = 0; id < DE_COUNT; id++)
		CHECK(desktop_console_attr((enum de_id)id) != 0);
}
