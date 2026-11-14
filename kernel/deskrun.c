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
#include <mergedos/io.h>
#include <mergedos/mwm.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>
#include <mergedos/mm.h>
#include <mergedos/monitor.h>
#include <mergedos/sched.h>
#include <mergedos/setup.h>
#include "abi_dispatch/abi.h"
#include "abi_dispatch/binfmt.h"
#include "desktop/desktop.h"
#include "filetype/filetype.h"
#include "mergedfence/fence.h"
#include "src/msh/msh.h"

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

/* ---- the Viewer ------------------------------------------------------------- */

/*
 * One document at a time, owned by the desktop thread. The shell asks for a
 * file through view_request(); the thread picks it up within 20 ms. Images
 * up to 256 x 192 pixels are decoded (PNG scanlines need the scratch area,
 * which also holds a decompressed tar.gz for its listing).
 */
#define VIEW_MAX_PX   (256 * 192)
#define VIEW_SCRATCH  (200 * 1024)

#define VIEW_RGB_BYTES  (VIEW_MAX_PX * 3)
#define VIEW_PAGES(b)   (((b) + PAGE_SIZE - 1) / PAGE_SIZE)

/* Allocated while the Viewer is open (or for one decode), freed on close. */
static u8 *view_rgb, *view_scratch;
static u64 view_rgb_phys, view_scratch_phys;
static struct ft_doc vdoc;
static struct ft_image vimg;
static unsigned vview;                  /* FT_VIEW_* shown                  */
static int vscroll;                     /* first line shown                 */
static char vtitle[80], vline[FT_LINE_MAX];
static char vreq_path[128];
static volatile u32 vreq_seq;

enum { TAB_TEXT, TAB_HEX, TAB_IMAGE, TAB_INFO, TAB_UP, TAB_DOWN, TAB_COUNT };

static bool view_buffers_get(void)
{
	if (!view_rgb_phys)
		view_rgb_phys = pmm_alloc_contig(VIEW_PAGES(VIEW_RGB_BYTES));
	if (!view_scratch_phys)
		view_scratch_phys = pmm_alloc_contig(VIEW_PAGES(VIEW_SCRATCH));
	view_rgb = view_rgb_phys ? phys_to_virt(view_rgb_phys) : NULL;
	view_scratch = view_scratch_phys ? phys_to_virt(view_scratch_phys) : NULL;
	return view_rgb && view_scratch;
}

static void view_buffers_put(void)
{
	if (view_rgb_phys)
		pmm_free_contig(view_rgb_phys, VIEW_PAGES(VIEW_RGB_BYTES));
	if (view_scratch_phys)
		pmm_free_contig(view_scratch_phys, VIEW_PAGES(VIEW_SCRATCH));
	view_rgb_phys = view_scratch_phys = 0;
	view_rgb = view_scratch = NULL;
	memset(&vdoc, 0, sizeof(vdoc));
	memset(&vimg, 0, sizeof(vimg));
}

/* Which action the i-th visible tab stands for, or -1. */
static int view_tab(int i)
{
	static const unsigned bits[4] = { FT_VIEW_TEXT, FT_VIEW_HEX, FT_VIEW_IMAGE, FT_VIEW_INFO };
	int n = 0;

	if (!vdoc.views)
		return -1;
	for (int t = 0; t < TAB_COUNT; t++) {
		bool on = t < TAB_UP ? (vdoc.views & bits[t]) != 0 : vview != FT_VIEW_IMAGE;

		if (on && n++ == i)
			return t;
	}
	return -1;
}

static unsigned tab_view(int t)
{
	static const unsigned bits[4] = { FT_VIEW_TEXT, FT_VIEW_HEX, FT_VIEW_IMAGE, FT_VIEW_INFO };

	return t >= 0 && t < TAB_UP ? bits[t] : 0;
}

static const char *host_tab(void *ctx, int i, bool *active)
{
	static const char *const labels[TAB_COUNT] = { "Text", "Hex", "Image", "Info", "Up",
						       "Down" };
	const int t = view_tab(i);

	(void)ctx;
	if (t < 0)
		return NULL;
	*active = tab_view(t) == vview;
	return labels[t];
}

static bool host_image(void *ctx, struct desk_image *img)
{
	(void)ctx;
	if (vview != FT_VIEW_IMAGE || !vimg.rgb)
		return false;
	*img = (struct desk_image){ vimg.rgb, (int)vimg.w, (int)vimg.h };
	return true;
}

static const char *viewer_line(int i)
{
	if (i == DESK_LINE_TITLE)
		return vtitle[0] ? vtitle : NULL;
	if (i < 0 || !vdoc.views)
		return i == 0 ? "No file open: click one in Files, or type open <path>." : NULL;
	if (vview == FT_VIEW_IMAGE) {
		if (i)
			return NULL;
		ksnprintf(vline, sizeof(vline), "%u x %u pixels, %s, %llu bytes", (unsigned)vimg.w,
			  (unsigned)vimg.h, vdoc.type.name, (unsigned long long)vdoc.len);
		return vline;
	}
	return ft_doc_line(&vdoc, vview, vscroll + i, vline, sizeof(vline)) ? vline : NULL;
}

static void view_request(const char *path)
{
	mos_strlcpy(vreq_path, path, sizeof(vreq_path));
	__atomic_add_fetch(&vreq_seq, 1, __ATOMIC_RELEASE);
}

/* ---- window contents ------------------------------------------------------- */

/* The i-th ramdisk file the Files window lists: every regular file. */
static bool file_entry(int i, struct tar_entry *e, bool *exe)
{
	size_t off = 0;
	int n = 0;

	while (initrd_next(&off, e) > 0) {
		struct binfmt_info bi;

		if ((e->type != '0' && e->type != '\0') || strlen(e->name) > 100)
			continue;
		*exe = !binfmt_probe(e->data, (size_t)e->size, &bi) && bi.format != BINFMT_SCRIPT;
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
	if (kind == DESK_WIN_VIEWER)
		return viewer_line(i);
	if (i == DESK_LINE_TITLE)
		return NULL;
	switch (kind) {
	case DESK_WIN_FILES:
		if (i < 0 || !file_entry(i, &e, &exe))
			return NULL;
		ksnprintf(buf, sizeof(buf), "%s  /%s", exe ? "run " : "open", e.name);
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

static const char *host_app_title(void *ctx, int win);
static void host_app_paint(void *ctx, int win, struct desk_rect content, struct desk_rect clip);

static const struct desk_host host = { NULL, host_line, host_term, host_tab, host_image,
				       host_app_title, host_app_paint };

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

/* ---- /dev/mwm: windows with client pixels (docs/WAYLAND.md) ------------------------- */

/*
 * System calls only touch the slot table, pixel buffers and the event queue;
 * everything that touches the window manager (open, close, resize, repaint)
 * is requested through `state` and done by the desktop thread, which owns
 * `desk`. Syscalls run with interrupts off, the desktop thread reads shared
 * fields under irq_save(), so no further locking is needed on one CPU.
 */
enum { AW_FREE, AW_OPEN, AW_LIVE, AW_CLOSE };

struct app_win {
	u8 state;
	int handle, id;                 /* client handle, desktop window id */
	u32 *px;
	u64 phys, pages;
	int w, h;
	char title[48];
	struct desk_rect dirty;         /* content coordinates; empty = clean */
	u64 old_phys, old_pages;        /* replaced by a resize, freed by the desktop thread */
	bool resized;
};

static struct app_win appw[MWM_MAX_WIN];
static bool mwm_opened;
static struct mwm_event evq[64];
static u32 ev_head, ev_tail;
static int app_grab;                    /* handle holding the left button, 0 */

static struct app_win *app_by_handle(int handle)
{
	for (int i = 0; i < MWM_MAX_WIN; i++)
		if (appw[i].state != AW_FREE && appw[i].handle == handle)
			return &appw[i];
	return NULL;
}

static struct app_win *app_by_id(int id)
{
	for (int i = 0; i < MWM_MAX_WIN; i++)
		if (appw[i].state == AW_LIVE && appw[i].id == id)
			return &appw[i];
	return NULL;
}

static void app_event(u32 type, int handle, int x, int y, u32 code, u32 state)
{
	evq[ev_head++ % ARRAY_SIZE(evq)] = (struct mwm_event){ type, (u32)handle, x, y, code, state,
							      (u32)(time_ticks() * 1000 / SCHED_HZ), 0 };
	if (ev_head - ev_tail > ARRAY_SIZE(evq))
		ev_tail = ev_head - ARRAY_SIZE(evq);
	thread_wake(&ev_head, 0, SCHED_MAX_THREADS);
	poll_wake();
}

static const char *host_app_title(void *ctx, int win)
{
	const struct app_win *a = app_by_id(win);

	(void)ctx;
	return a && a->title[0] ? a->title : NULL;
}

static void host_app_paint(void *ctx, int win, struct desk_rect content, struct desk_rect clip)
{
	const struct app_win *a = app_by_id(win);
	const int ox = clip.x - content.x, oy = clip.y - content.y;
	int w = clip.w, h = clip.h;

	(void)ctx;
	if (!a || !a->px) {
		gfx_fill(clip.x, clip.y, clip.w, clip.h, 0);
		return;
	}
	if (ox < a->w && oy < a->h) {
		w = MOS_MIN(clip.w, a->w - ox);
		h = MOS_MIN(clip.h, a->h - oy);
		gfx_blit(clip.x, clip.y, w, h, a->px + (size_t)oy * (size_t)a->w + (size_t)ox,
			 (size_t)a->w);
	} else {
		w = h = 0;
	}
	if (w < clip.w)                         /* the window is larger than its buffer */
		gfx_fill(clip.x + w, clip.y, clip.w - w, clip.h, 0);
	if (h < clip.h)
		gfx_fill(clip.x, clip.y + h, MOS_MIN(clip.w, w), clip.h - h, 0);
}

static void app_free_buf(u64 phys, u64 pages)
{
	if (phys)
		pmm_free_contig(phys, pages);
}

static bool app_alloc_buf(struct app_win *a, int w, int h)
{
	const u64 pages = ((u64)w * (u64)h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
	const u64 phys = pmm_alloc_contig(pages);

	if (!phys)
		return false;
	a->phys = phys;
	a->pages = pages;
	a->px = phys_to_virt(phys);
	a->w = w;
	a->h = h;
	return true;
}

static long mwm_read(bool nonblock, struct task *t, u64 ubuf, u64 len);
static long mwm_write(struct task *t, u64 ubuf, u64 len);

static long mwm_f_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	return mwm_read((f->flags & VFS_O_NONBLOCK) != 0, t, ubuf, len);
}

static long mwm_f_write(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	(void)f;
	return mwm_write(t, ubuf, len);
}

static u32 mwm_f_poll(struct file *f, u32 events)
{
	(void)f;
	return (ev_head != ev_tail ? POLLIN : 0) & events ? POLLIN : 0;
}

static void mwm_f_release(struct file *f)
{
	(void)f;
	mwm_close();
}

const struct fops mwm_ops = { .read = mwm_f_read, .write = mwm_f_write, .poll = mwm_f_poll,
			      .release = mwm_f_release, .mode = S_IFCHR | 0666 };

long mwm_open(void)
{
	if (!running)
		return -MERR_NOENT;
	if (mwm_opened)
		return -MERR_EXIST;
	mwm_opened = true;
	ev_head = ev_tail = 0;
	return 0;
}

/* The compositor went away: every window it made is closed by the desktop thread. */
void mwm_close(void)
{
	mwm_opened = false;
	for (int i = 0; i < MWM_MAX_WIN; i++)
		if (appw[i].state != AW_FREE)
			appw[i].state = AW_CLOSE;
	app_grab = 0;
}

static long mwm_read(bool nonblock, struct task *t, u64 ubuf, u64 len)
{
	long done = 0;

	if (len < sizeof(struct mwm_event))
		return -MERR_INVAL;
	while (ev_head == ev_tail) {
		if (nonblock)
			return -MERR_AGAIN;
		if (task_signal_pending())
			return -MERR_INTR;
		thread_block(&ev_head, 0, 0);
	}
	while (ev_head != ev_tail && len >= sizeof(struct mwm_event)) {
		if (uaccess_copy_out(t, ubuf + (u64)done, &evq[ev_tail % ARRAY_SIZE(evq)],
				     sizeof(struct mwm_event)))
			return done ? done : -MERR_FAULT;
		ev_tail++;
		done += (long)sizeof(struct mwm_event);
		len -= sizeof(struct mwm_event);
	}
	return done;
}

static long mwm_write(struct task *t, u64 ubuf, u64 len)
{
	struct mwm_cmd c;
	struct app_win *a;

	if (len < sizeof(c) || uaccess_copy_in(t, &c, ubuf, sizeof(c)))
		return len < sizeof(c) ? -MERR_INVAL : -MERR_FAULT;
	if (len != sizeof(c) + c.len)
		return -MERR_INVAL;
	a = app_by_handle((int)c.win);
	ubuf += sizeof(c);
	switch (c.op) {
	case MWM_CREATE: {
		int slot = -1;

		if (a || c.win == 0 || c.win > 255 || c.a <= 0 || c.b <= 0 || c.a > MWM_MAX_W ||
		    c.b > MWM_MAX_H)
			return a ? -MERR_EXIST : -MERR_INVAL;
		for (int i = 0; i < MWM_MAX_WIN && slot < 0; i++)
			if (appw[i].state == AW_FREE)
				slot = i;
		if (slot < 0)
			return -MERR_MFILE;
		a = &appw[slot];
		memset(a, 0, sizeof(*a));
		if (!app_alloc_buf(a, c.a, c.b))
			return -MERR_NOMEM;
		a->handle = (int)c.win;
		a->dirty = (struct desk_rect){ 0, 0, 0, 0 };
		a->state = AW_OPEN;
		return (long)len;
	}
	case MWM_DESTROY:
		if (!a)
			return -MERR_NOENT;
		a->state = AW_CLOSE;
		return (long)len;
	case MWM_TITLE: {
		if (!a)
			return -MERR_NOENT;
		memset(a->title, 0, sizeof(a->title));
		if (uaccess_copy_in(t, a->title, ubuf, MOS_MIN((u64)c.len, sizeof(a->title) - 1)))
			return -MERR_FAULT;
		return (long)len;
	}
	case MWM_PIXELS: {
		const u64 rowb = (u64)(u32)c.c * 4;
		struct desk_rect r = { c.a, c.b, c.c, c.d };

		if (!a)
			return -MERR_NOENT;
		if (c.a < 0 || c.b < 0 || c.c <= 0 || c.d <= 0 || c.a + c.c > a->w ||
		    c.b + c.d > a->h || c.len != rowb * (u64)(u32)c.d)
			return -MERR_INVAL;
		for (int row = 0; row < c.d; row++)
			if (uaccess_copy_in(t, a->px + (size_t)(c.b + row) * (size_t)a->w + (size_t)c.a,
					    ubuf + (u64)row * rowb, (size_t)rowb))
				return -MERR_FAULT;
		a->dirty = desk_rect_empty(a->dirty) ? r : desk_rect_union(a->dirty, r);
		return (long)len;
	}
	case MWM_RESIZE: {
		struct app_win n = { 0 };

		if (!a)
			return -MERR_NOENT;
		if (c.a <= 0 || c.b <= 0 || c.a > MWM_MAX_W || c.b > MWM_MAX_H || a->old_phys)
			return a->old_phys ? -MERR_AGAIN : -MERR_INVAL;
		if (!app_alloc_buf(&n, c.a, c.b))
			return -MERR_NOMEM;
		a->old_phys = a->phys;
		a->old_pages = a->pages;
		a->phys = n.phys;
		a->pages = n.pages;
		a->px = n.px;
		a->w = n.w;
		a->h = n.h;
		a->dirty = (struct desk_rect){ 0, 0, 0, 0 };
		a->resized = true;
		return (long)len;
	}
	}
	return -MERR_INVAL;
}

/* Desktop thread: open, resize, repaint and close what the client asked for. */
static void app_service(void)
{
	bool changed = false;

	for (int i = 0; i < MWM_MAX_WIN; i++) {
		struct app_win *a = &appw[i];
		struct desk_rect d;
		u64 flags, old_phys = 0, old_pages = 0;
		bool resized;

		if (a->state == AW_OPEN) {
			a->id = desktop_open_app(&desk, a->w, a->h);
			a->state = a->id ? AW_LIVE : AW_CLOSE;
			if (!a->id) {
				trace2("no free window slot for ", "app");
			} else {
				const struct desk_rect c = desktop_content_rect(&desk, desktop_window(&desk, a->id));
				char where[48];

				ksnprintf(where, sizeof(where), "content %d %d %d %d", c.x, c.y, c.w, c.h);
				trace2("app window ", where);
			}
			changed = true;
		}
		if (a->state == AW_CLOSE) {
			if (a->id)
				(void)desktop_close(&desk, a->id);
			flags = irq_save();
			app_free_buf(a->phys, a->pages);
			app_free_buf(a->old_phys, a->old_pages);
			if (app_grab == a->handle)
				app_grab = 0;
			memset(a, 0, sizeof(*a));
			irq_restore(flags);
			changed = true;
			continue;
		}
		if (a->state != AW_LIVE)
			continue;
		flags = irq_save();
		resized = a->resized;
		a->resized = false;
		old_phys = a->old_phys;
		old_pages = a->old_pages;
		a->old_phys = 0;
		d = a->dirty;
		a->dirty = (struct desk_rect){ 0, 0, 0, 0 };
		irq_restore(flags);
		if (resized) {
			(void)desktop_set_content(&desk, a->id, a->w, a->h);
			changed = true;
		}
		if (old_phys)
			app_free_buf(old_phys, old_pages);
		if (!resized && !desk_rect_empty(d)) {
			const struct desk_window *w = desktop_window(&desk, a->id);
			const struct desk_rect c = desktop_content_rect(&desk, w);

			if (desktop_visible(w)) {
				desktop_paint(&desk, (struct desk_rect){ c.x + d.x, c.y + d.y, d.w, d.h },
					      &host);
				gfx_cursor_refresh();
			}
		}
	}
	if (changed)
		flush();
}

/* Pointer, focus and keys for the focused client window (desktop thread). */
static int app_handle_at(int x, int y, struct desk_rect *content)
{
	const struct desk_hit h = desktop_hit(&desk, x, y);
	const struct desk_window *w = desktop_window(&desk, h.win);
	const struct app_win *a;

	if (h.part != DESK_PART_CONTENT || !w || w->kind != DESK_WIN_APP || !(a = app_by_id(w->id)))
		return 0;
	*content = desktop_content_rect(&desk, w);
	return a->handle;
}

static bool app_rel(int handle, int x, int y, int *rx, int *ry)
{
	const struct app_win *a = app_by_handle(handle);
	const struct desk_window *w = a && a->state == AW_LIVE ? desktop_window(&desk, a->id) : NULL;
	struct desk_rect c;

	if (!w)
		return false;
	c = desktop_content_rect(&desk, w);
	*rx = x - c.x;
	*ry = y - c.y;
	return true;
}

static void app_press(int handle, int x, int y)
{
	int rx, ry;

	if (app_rel(handle, x, y, &rx, &ry)) {
		app_grab = handle;
		app_event(MWM_EV_BUTTON, handle, rx, ry, 0, 1);
	}
}

static void app_release(int x, int y)
{
	int rx, ry;

	if (app_grab && app_rel(app_grab, x, y, &rx, &ry))
		app_event(MWM_EV_BUTTON, app_grab, rx, ry, 0, 0);
	app_grab = 0;
}

static void app_pointer(const struct mouse_state *m)
{
	static int last_x = -1, last_y = -1, over, last_top;
	static u8 last_btn;
	struct desk_rect c;
	const int under = app_handle_at(m->x, m->y, &c);
	const int target = app_grab ? app_grab : under;
	const int top = desktop_top(&desk);
	int rx, ry;

	if (top != last_top) {
		const struct app_win *o = app_by_id(last_top), *n = app_by_id(top);

		if (o)
			app_event(MWM_EV_FOCUS, o->handle, 0, 0, 0, 0);
		if (n)
			app_event(MWM_EV_FOCUS, n->handle, 0, 0, 0, 1);
		last_top = top;
	}
	if (over != target) {
		if (over && app_by_handle(over))
			app_event(MWM_EV_LEAVE, over, 0, 0, 0, 0);
		over = target;
	}
	if (target && app_rel(target, m->x, m->y, &rx, &ry)) {
		if (m->x != last_x || m->y != last_y)
			app_event(MWM_EV_MOTION, target, rx, ry, 0, 0);
		for (int b = 1; b <= 2; b++) {          /* right, middle: left comes through press() */
			const u8 bit = b == 1 ? MOUSE_RIGHT : MOUSE_MIDDLE;

			if ((m->buttons ^ last_btn) & bit)
				app_event(MWM_EV_BUTTON, target, rx, ry, (u32)b, (m->buttons & bit) ? 1 : 0);
		}
	}
	last_x = m->x;
	last_y = m->y;
	last_btn = m->buttons;
}

/* console_poll() hook: keys go to the focused client window instead of the shell. */
static bool app_key(int c)
{
	const struct app_win *a = app_by_id(desktop_top(&desk));

	if (!a || c == 3)                       /* Ctrl-C still reaches the shell job */
		return false;
	app_event(MWM_EV_KEY, a->handle, 0, 0, (u32)c, 1);
	return true;
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
			if (kind == DESK_WIN_VIEWER)
				view_buffers_put();
			trace2("window close ", desktop_kind_name(kind));
		} else if ((w = desktop_window(&desk, desktop_launch(&desk, kind, &opened)))) {
			const struct desk_rect t = desktop_title_rect(&desk, w);
			char where[48];

			trace2(opened ? "window open " : "window focus ", desktop_kind_name(kind));
			/* Where its title bar is, for test rigs that drag it. */
			ksnprintf(where, sizeof(where), "%s title %d %d", desktop_kind_name(kind),
				  t.x + t.w / 2, t.y + t.h / 2);
			trace2("window ", where);
			if (kind == DESK_WIN_FILES) {
				const struct desk_rect r = desktop_row_rect(&desk, w, 0);

				ksnprintf(where, sizeof(where), "files row0 %d %d", r.x + 16,
					  r.y + r.h / 2);
				trace2("window ", where);
			}
		} else {
			trace2("no free window slot for ", desktop_kind_name(kind));
		}
	}
	if (cmd) {
		console_inject(cmd);
		console_inject("\n");
	}
}

/* Open a ramdisk file (native path) in the Viewer window. Desktop thread only. */
static void viewer_open(const char *path)
{
	const u8 *data;
	size_t size;
	const struct desk_window *w;
	bool opened;
	char where[48];
	int rc;

	while (*path == '/')
		path++;
	if (initrd_find(path, &data, &size)) {
		trace2("viewer: no such file ", path);
		return;
	}
	if (!view_buffers_get()) {
		view_buffers_put();
		trace2("viewer: out of memory for ", path);
		return;
	}
	ft_doc_open(&vdoc, data, size, path, view_scratch, VIEW_SCRATCH);
	vimg.rgb = NULL;
	if (vdoc.views & FT_VIEW_IMAGE) {
		rc = ft_image_decode(data, size, view_rgb, VIEW_RGB_BYTES, view_scratch,
				     VIEW_SCRATCH, &vimg);
		if (rc) {
			vdoc.views &= ~FT_VIEW_IMAGE;
			ksnprintf(vdoc.note, sizeof(vdoc.note), "image not shown: %s",
				  ft_image_error(rc));
		}
	}
	vview = ft_doc_default_view(&vdoc);
	vscroll = 0;
	ksnprintf(vtitle, sizeof(vtitle), "Viewer - %s", vdoc.name);
	w = desktop_window(&desk, desktop_launch(&desk, DESK_WIN_VIEWER, &opened));
	ksnprintf(vline, sizeof(vline), "/%s: %s (%s)%s", path, vdoc.type.name, vdoc.type.mime,
		  vimg.rgb ? ", image decoded" : "");
	trace2("viewer: ", vline);
	if (!w) {
		trace2("no free window slot for ", "viewer");
		return;
	}
	ksnprintf(where, sizeof(where), "viewer title %d %d", desktop_title_rect(&desk, w).x +
		  desktop_title_rect(&desk, w).w / 2, desktop_title_rect(&desk, w).y + 8);
	trace2("window ", where);
	flush();
	desktop_paint(&desk, w->frame, &host);  /* new content in a reused window */
	gfx_cursor_refresh();
}

static void viewer_tab(const struct desk_window *w, int slot)
{
	const int t = view_tab(slot), rows = desktop_viewer_rows(&desk, w);

	if (t < 0)
		return;
	if (t == TAB_UP) {
		vscroll = MOS_MAX(0, vscroll - rows);
	} else if (t == TAB_DOWN) {
		if (ft_doc_line(&vdoc, vview, vscroll + rows, vline, sizeof(vline)))
			vscroll += rows;
	} else {
		vview = tab_view(t);
		vscroll = 0;
	}
	desktop_paint(&desk, desktop_content_rect(&desk, w), &host);
}

static void file_clicked(int row)
{
	struct tar_entry e;
	char cmd[128];
	bool exe;

	if (row < 0 || !file_entry(row, &e, &exe))
		return;
	trace2("files: ", e.name);
	if (!exe) {
		viewer_open(e.name);
		return;
	}
	ksnprintf(cmd, sizeof(cmd), "run /%s\n", e.name);
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
		if (w->kind == DESK_WIN_APP) {          /* the client decides: it gets a close request */
			const struct app_win *a = app_by_id(w->id);

			if (a)
				app_event(MWM_EV_CLOSE, a->handle, 0, 0, 0, 0);
			break;
		}
		trace2("window close ", desktop_kind_name(w->kind));
		if (w->kind == DESK_WIN_VIEWER)
			view_buffers_put();
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
		if (w->kind == DESK_WIN_APP) {
			const struct app_win *a = app_by_id(w->id);

			if (a)
				app_press(a->handle, x, y);
		}
		break;
	case DESK_PART_TAB:
		(void)desktop_focus(&desk, h.win);
		flush();
		viewer_tab(w, h.row);
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
	u32 seq = 0;

	(void)arg;
	for (;;) {
		if (__atomic_load_n(&vreq_seq, __ATOMIC_ACQUIRE) != seq) {
			char path[sizeof(vreq_path)];

			seq = __atomic_load_n(&vreq_seq, __ATOMIC_ACQUIRE);
			mos_strlcpy(path, vreq_path, sizeof(path));
			viewer_open(path);
		}
		app_service();
		if (m->presses != presses) {
			presses = m->presses;
			press(m->press_x, m->press_y);
		}
		app_pointer(m);
		if (drag_id && m->releases == releases && (m->buttons & MOUSE_LEFT))
			drag_to(m->x, m->y);
		if (m->releases != releases) {
			releases = m->releases;
			app_release(m->release_x, m->release_y);
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
	{
		char path[96];

		if (boot_cmdline_get("view", path, sizeof(path)))       /* view=/home/x.png */
			view_request(path);
	}
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
	console_key_hook = app_key;
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

bool desktop_view(const char *path)
{
	if (!running)
		return false;
	view_request(path);
	return true;
}

int desktop_view_decode(const u8 *data, size_t len, u32 *w, u32 *h)
{
	struct ft_image im;
	int rc;

	if (running)
		return -MERR_AGAIN;
	if (!view_buffers_get()) {
		view_buffers_put();
		return -MERR_NOMEM;
	}
	rc = ft_image_decode(data, len, view_rgb, VIEW_RGB_BYTES, view_scratch,
			     VIEW_SCRATCH, &im);
	view_buffers_put();
	*w = im.w;
	*h = im.h;
	return rc;
}
