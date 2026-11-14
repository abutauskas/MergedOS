/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Minimal Wayland compositor state machine (docs/WAYLAND.md). Written from the
 * protocol XML descriptions. Opcode numbers are the request/event order there.
 */
#include "src/wayland/wl.h"
#include <stdarg.h>
#include <string.h>

#define WL_MIN(a, b) ((a) < (b) ? (a) : (b))

static void copy_str(char *dst, const char *src, size_t n)
{
	size_t l = strlen(src);

	if (l >= n)
		l = n - 1;
	memcpy(dst, src, l);
	dst[l] = 0;
}

enum { G_COMP = 1, G_SUBC, G_SHM, G_WM, G_SEAT, G_OUT, G_XOUT, G_DDM, G_VP, G_PC, G_REL, G_N };

static const struct { const char *name; uint8_t iface, ver; } globals[G_N] = {
	[G_COMP] = { "wl_compositor", WI_COMPOSITOR, 5 },
	[G_SUBC] = { "wl_subcompositor", WI_SUBCOMP, 1 },
	[G_SHM]  = { "wl_shm", WI_SHM, 1 },
	[G_WM]   = { "xdg_wm_base", WI_WMBASE, 5 },
	[G_SEAT] = { "wl_seat", WI_SEAT, 7 },
	[G_OUT]  = { "wl_output", WI_OUTPUT, 4 },
	[G_XOUT] = { "zxdg_output_manager_v1", WI_XOUTMGR, 2 },
	[G_DDM]  = { "wl_data_device_manager", WI_DDM, 3 },
	[G_VP]   = { "wp_viewporter", WI_VPORTER, 1 },
	[G_PC]   = { "zwp_pointer_constraints_v1", WI_PCONS, 1 },
	[G_REL]  = { "zwp_relative_pointer_manager_v1", WI_RELMGR, 1 },
};

/* ---- event sender: u = u32, i = int, x = pixels as 24.8 fixed, s = string,
 *      a = array (ptr, len), f = fd ---- */
static void ev(struct wl_client *c, uint32_t obj, uint16_t op, const char *fmt, ...)
{
	struct wl_out o = wl_begin(&c->conn, obj, op);
	va_list ap;

	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		switch (*fmt) {
		case 'u': wl_put_u32(&o, va_arg(ap, uint32_t)); break;
		case 'i': wl_put_u32(&o, (uint32_t)va_arg(ap, int)); break;
		case 'x': wl_put_u32(&o, (uint32_t)(va_arg(ap, int) * 256)); break;
		case 's': wl_put_str(&o, va_arg(ap, const char *)); break;
		case 'a': {
			const void *p = va_arg(ap, const void *);

			wl_put_arr(&o, p, va_arg(ap, uint32_t));
			break;
		}
		case 'f': wl_put_fd(&o, va_arg(ap, int)); break;
		}
	}
	va_end(ap);
	wl_end(&o);
}

static bool proto_err(struct wl_client *c, uint32_t obj, uint32_t code, const char *msg)
{
	ev(c, 1, 0, "uus", obj, code, msg);
	c->conn.err = true;
	return false;
}

static uint32_t now(struct wl_client *c)
{
	return c->srv->host.now_ms ? c->srv->host.now_ms(c->srv->host.ctx) : 0;
}

static uint32_t next_serial(struct wl_client *c)
{
	return ++c->serial;
}

/* ---- objects ---- */

static struct wl_obj *obj_of(struct wl_client *c, uint32_t id)
{
	return id > 0 && id < WL_MAX_OBJ ? &c->obj[id] : NULL;
}

static bool newobj(struct wl_client *c, uint32_t id, enum wl_iface i, unsigned ver, unsigned idx)
{
	if (id == 0 || id >= WL_MAX_OBJ)
		return proto_err(c, 1, 2, "object id out of range");
	if (c->obj[id].iface)
		return proto_err(c, 1, 0, "object id in use");
	c->obj[id] = (struct wl_obj){ (uint8_t)i, (uint8_t)ver, (uint16_t)idx };
	return true;
}

static void del(struct wl_client *c, uint32_t id)
{
	if (id > 0 && id < WL_MAX_OBJ && c->obj[id].iface) {
		c->obj[id].iface = 0;
		ev(c, 1, 1, "u", id);                   /* wl_display.delete_id */
	}
}

static int alloc_slot(bool *used, size_t stride, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!*(bool *)((char *)used + i * stride))
			return (int)i;
	return -1;
}

#define ALLOC(arr) alloc_slot(&(arr)[0].used, sizeof((arr)[0]), sizeof(arr) / sizeof((arr)[0]))

/* ---- shm ---- */

static void pool_unref(struct wl_client *c, int pi)
{
	struct wl_pool *p = &c->pool[pi];

	if (--p->refs > 0)
		return;
	if (c->srv->host.unmap && p->base)
		c->srv->host.unmap(c->srv->host.ctx, p->base, p->size);
	if (c->srv->host.close_fd && p->fd >= 0)
		c->srv->host.close_fd(c->srv->host.ctx, p->fd);
	memset(p, 0, sizeof(*p));
}

static void buffer_free(struct wl_client *c, int bi)
{
	pool_unref(c, c->buf[bi].pool);
	memset(&c->buf[bi], 0, sizeof(c->buf[bi]));
}

static bool buffer_in_use(struct wl_client *c, int bi)
{
	for (int i = 0; i < WL_MAX_SURF; i++)
		if (c->surf[i].used && c->surf[i].cur == bi)
			return true;
	return false;
}

/* The surface stops showing its committed buffer (replaced or surface gone). */
static void buffer_release(struct wl_client *c, int bi)
{
	if (bi < 0 || buffer_in_use(c, bi))
		return;
	if (c->buf[bi].zombie)
		buffer_free(c, bi);
	else
		ev(c, c->buf[bi].id, 0, "");            /* wl_buffer.release */
}

/* ---- surfaces and windows ---- */

static int root_of(struct wl_client *c, int si)
{
	for (int depth = 0; depth < 16 && si >= 0; depth++) {
		if (c->surf[si].role == 1 || c->surf[si].parent < 0)
			return si;
		si = c->surf[si].parent;
	}
	return -1;
}

static void abs_pos(struct wl_client *c, int si, int *x, int *y)
{
	*x = *y = 0;
	for (int depth = 0; depth < 16 && si >= 0 && c->surf[si].role != 1; depth++) {
		*x += c->surf[si].x;
		*y += c->surf[si].y;
		si = c->surf[si].parent;
	}
}

static const uint32_t *buf_pixels(struct wl_client *c, const struct wl_buffer *b)
{
	return (const uint32_t *)(const void *)(c->pool[b->pool].base + b->offset);
}

/* Copy (and scale for a viewport) a surface into its toplevel's window. */
static void blit(struct wl_client *c, int si)
{
	struct wl_surface *s = &c->surf[si];
	const int ri = root_of(c, si);
	const struct wl_host *h = &c->srv->host;
	const struct wl_surface *r;
	const struct wl_buffer *b;
	const uint32_t *px;
	int ax, ay, dx, dy, sw, sh, cw, ch;
	static uint32_t row[4096];

	if (ri < 0 || s->cur < 0 || !h->win_pixels)
		return;
	r = &c->surf[ri];
	if (!r->win || c->surf[ri].role != 1)
		return;
	b = &c->buf[s->cur];
	px = buf_pixels(c, b);
	abs_pos(c, si, &ax, &ay);
	dx = ax - (si == ri ? 0 : r->gx);
	dy = ay - (si == ri ? 0 : r->gy);
	sw = s->dw > 0 ? s->dw : b->w;
	sh = s->dh > 0 ? s->dh : b->h;
	cw = r->cw;
	ch = r->ch;
	if (si == ri) {                         /* the visible window geometry of the root */
		int gx = r->gx, gy = r->gy;

		if (r->gw > 0 && !s->dw) {
			if (gx < 0 || gy < 0 || gx + r->gw > b->w || gy + r->gh > b->h)
				gx = gy = 0;
			h->win_pixels(h->ctx, r->win, 0, 0, r->gw, r->gh,
				      px + (size_t)gy * (b->stride / 4) + (size_t)gx, b->stride / 4);
			return;
		}
	}
	if (s->dw <= 0 && s->dh <= 0) {
		int x0 = dx < 0 ? -dx : 0, y0 = dy < 0 ? -dy : 0;
		int w = sw - x0, hh = sh - y0;

		if (dx + x0 + w > cw)
			w = cw - dx - x0;
		if (dy + y0 + hh > ch)
			hh = ch - dy - y0;
		if (w > 0 && hh > 0)
			h->win_pixels(h->ctx, r->win, dx + x0, dy + y0, w, hh,
				      px + (size_t)y0 * (b->stride / 4) + (size_t)x0, b->stride / 4);
		return;
	}
	if (sw > 4096 || sw <= 0 || sh <= 0)
		return;
	for (int y = 0; y < sh; y++) {          /* nearest-neighbour viewport scaling */
		const uint32_t *src = px + (size_t)((int64_t)y * b->h / sh) * (b->stride / 4);

		if (dy + y < 0 || dy + y >= ch)
			continue;
		for (int x = 0; x < sw; x++)
			row[x] = src[(int64_t)x * b->w / sw];
		h->win_pixels(h->ctx, r->win, dx, dy + y, WL_MIN(sw, cw - dx), 1, row, (size_t)sw);
	}
}

static void blit_tree(struct wl_client *c, int ri)
{
	blit(c, ri);
	for (int i = 0; i < WL_MAX_SURF; i++)
		if (c->surf[i].used && c->surf[i].mapped && i != ri && root_of(c, i) == ri &&
		    c->surf[i].role != 1)
			blit(c, i);
}

static void configure_toplevel(struct wl_client *c, struct wl_surface *s)
{
	const uint32_t serial = next_serial(c);
	const struct wl_obj *t = obj_of(c, s->top);

	if (t && t->ver >= 4)
		ev(c, s->top, 2, "ii", c->srv->host.out_w, c->srv->host.out_h);
	if (t && t->ver >= 5)
		ev(c, s->top, 3, "a", "", 0u);
	ev(c, s->top, 0, "iia", 0, 0, "", 0u);
	ev(c, s->xsurf, 0, "u", serial);
	s->configured = true;
}

static void unmap_surface(struct wl_client *c, int si)
{
	struct wl_surface *s = &c->surf[si];
	const int old = s->cur;

	s->mapped = false;
	s->cur = -1;
	buffer_release(c, old);
	if (s->role == 1 && s->win) {
		if (c->srv->host.win_destroy)
			c->srv->host.win_destroy(c->srv->host.ctx, s->win);
		if (c->srv->focus_win == s->win)
			c->srv->focus_win = 0;
		s->win = 0;
		s->configured = false;
	}
	if (c->kbd_surf == si)
		c->kbd_surf = -1;
	if (c->ptr_surf == si)
		c->ptr_surf = -1;
}

static void surface_destroy(struct wl_client *c, int si)
{
	struct wl_surface *s = &c->surf[si];
	const int ri = root_of(c, si);

	unmap_surface(c, si);
	for (int i = 0; i < WL_MAX_SURF; i++)
		if (c->surf[i].used && c->surf[i].parent == si)
			c->surf[i].parent = -1;
	for (int i = 0; i < s->nframes; i++)
		del(c, s->frames[i]);
	memset(s, 0, sizeof(*s));
	if (ri >= 0 && ri != si && c->surf[ri].used && c->surf[ri].win)
		blit_tree(c, ri);
}

static void surface_commit(struct wl_client *c, int si)
{
	struct wl_surface *s = &c->surf[si];
	const struct wl_host *h = &c->srv->host;
	bool relayout = false;
	int ri;

	for (int i = 0; i < WL_MAX_SURF; i++)       /* pending positions apply with the parent */
		if (c->surf[i].used && c->surf[i].parent == si && c->surf[i].role == 3 &&
		    (c->surf[i].x != c->surf[i].px || c->surf[i].y != c->surf[i].py)) {
			c->surf[i].x = c->surf[i].px;
			c->surf[i].y = c->surf[i].py;
			relayout = true;
		}
	if (s->role == 3 && (s->x != s->px || s->y != s->py)) {
		s->x = s->px;
		s->y = s->py;
		relayout = true;
	}
	if (s->attached) {
		const int old = s->cur;

		s->attached = false;
		s->cur = s->buf;
		s->buf = -1;
		if (s->cur < 0) {
			s->mapped = false;
			relayout = true;
		}
		if (old != s->cur)
			buffer_release(c, old);
	}
	if (s->cur >= 0) {                      /* committed size: the root's is its window */
		const struct wl_buffer *b = &c->buf[s->cur];
		int nw = s->dw > 0 ? s->dw : b->w, nh = s->dh > 0 ? s->dh : b->h;

		if (s->role == 1 && s->gw > 0) {
			nw = s->gw;
			nh = s->gh;
		}
		if (nw != s->cw || nh != s->ch)
			relayout = true;
		s->cw = nw;
		s->ch = nh;
		s->mapped = true;
	}
	if (s->role == 1 && !s->configured && s->top)
		configure_toplevel(c, s);
	ri = root_of(c, si);
	if (s->role == 1 && s->cur >= 0) {
		if (!s->win) {
			if (h->win_create)
				s->win = h->win_create(h->ctx, s->cw, s->ch);
			if (s->win < 0)
				s->win = 0;
			if (s->win && h->win_title && s->title[0])
				h->win_title(h->ctx, s->win, s->title);
			relayout = true;
		} else if (relayout && h->win_resize) {
			h->win_resize(h->ctx, s->win, s->cw, s->ch);
		}
	}
	if (ri >= 0 && c->surf[ri].win && s->cur >= 0 && c->surf[ri].mapped) {
		if (relayout || si == ri)
			blit_tree(c, ri);
		else
			blit(c, si);
	}
	for (int i = 0; i < s->nframes; i++) {
		ev(c, s->frames[i], 0, "u", now(c));
		del(c, s->frames[i]);
	}
	s->nframes = 0;
}

static void popup_place(struct wl_client *c, struct wl_surface *s, const struct wl_pos *p,
			const struct wl_surface *parent)
{
	int x = p->ax, y = p->ay;

	switch (p->anchor) {                    /* top 1 bottom 2 left 3 right 4 + corners 5..8 */
	case 0: x += p->aw / 2; y += p->ah / 2; break;
	case 1: x += p->aw / 2; break;
	case 2: x += p->aw / 2; y += p->ah; break;
	case 3: y += p->ah / 2; break;
	case 4: x += p->aw; y += p->ah / 2; break;
	case 5: break;
	case 6: y += p->ah; break;
	case 7: x += p->aw; break;
	case 8: x += p->aw; y += p->ah; break;
	}
	switch (p->gravity) {
	case 0: x -= p->w / 2; y -= p->h / 2; break;
	case 1: x -= p->w / 2; y -= p->h; break;
	case 2: x -= p->w / 2; break;
	case 3: x -= p->w; y -= p->h / 2; break;
	case 4: y -= p->h / 2; break;
	case 5: x -= p->w; y -= p->h; break;
	case 6: x -= p->w; break;
	case 7: y -= p->h; break;
	case 8: break;
	}
	x += p->ox;
	y += p->oy;
	s->px = s->x = x + (parent ? parent->gx : 0);
	s->py = s->y = y + (parent ? parent->gy : 0);
	(void)c;
}

/* ---- request handlers ---- */

static bool h_registry_bind(struct wl_client *c, struct wl_msg *m)
{
	const uint32_t name = wl_u32(m);
	const char *iface = wl_str(m);
	const uint32_t ver = wl_u32(m), id = wl_u32(m);
	const struct wl_host *h = &c->srv->host;

	if (m->bad)
		return false;
	if (name == 0 || name >= G_N || strcmp(iface, globals[name].name) || ver == 0 ||
	    ver > globals[name].ver)
		return proto_err(c, m->obj, 0, "no such global");
	if (!newobj(c, id, (enum wl_iface)globals[name].iface, ver, 0))
		return false;
	switch (name) {
	case G_SHM:
		ev(c, id, 0, "u", 0u);          /* argb8888 */
		ev(c, id, 0, "u", 1u);          /* xrgb8888 */
		break;
	case G_SEAT:
		ev(c, id, 0, "u", 3u);          /* pointer | keyboard */
		if (ver >= 2)
			ev(c, id, 1, "s", "seat0");
		break;
	case G_OUT:
		ev(c, id, 0, "iiiiissi", 0, 0, 340, 190, 0, "MergedOS", "Display", 0);
		ev(c, id, 1, "uiii", 3u, h->out_w, h->out_h, 60000);
		if (ver >= 2)
			ev(c, id, 3, "i", 1);
		if (ver >= 4)
			ev(c, id, 4, "s", "MOS-1");
		if (ver >= 2)
			ev(c, id, 2, "");
		break;
	}
	return true;
}

static bool h_display(struct wl_client *c, struct wl_msg *m)
{
	const uint32_t id = wl_u32(m);

	if (m->bad)
		return false;
	if (m->op == 0) {                       /* sync */
		if (!newobj(c, id, WI_CALLBACK, 1, 0))
			return false;
		ev(c, id, 0, "u", now(c));
		del(c, id);
		return true;
	}
	if (m->op == 1) {                       /* get_registry */
		if (!newobj(c, id, WI_REGISTRY, 1, 0))
			return false;
		for (unsigned g = 1; g < G_N; g++)
			ev(c, id, 0, "usu", g, globals[g].name, (unsigned)globals[g].ver);
		return true;
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_compositor(struct wl_client *c, struct wl_msg *m)
{
	const uint32_t id = wl_u32(m);
	int si;

	if (m->bad)
		return false;
	if (m->op == 1)                         /* create_region: nothing is kept */
		return newobj(c, id, WI_REGION, 1, 0);
	if (m->op != 0)
		return proto_err(c, m->obj, 1, "invalid method");
	si = ALLOC(c->surf);
	if (si < 0)
		return proto_err(c, m->obj, 2, "too many surfaces");
	if (!newobj(c, id, WI_SURFACE, c->obj[m->obj].ver, (unsigned)si))
		return false;
	c->surf[si] = (struct wl_surface){ .used = true, .id = id, .parent = -1, .buf = -1, .cur = -1 };
	return true;
}

static bool h_region(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 0) {
		del(c, m->obj);
		return true;
	}
	if (m->op > 2)
		return proto_err(c, m->obj, 1, "invalid method");
	return true;
}

static bool h_surface(struct wl_client *c, struct wl_msg *m)
{
	const int si = c->obj[m->obj].idx;
	struct wl_surface *s = &c->surf[si];

	switch (m->op) {
	case 0:                                 /* destroy */
		surface_destroy(c, si);
		del(c, m->obj);
		return true;
	case 1: {                               /* attach */
		const uint32_t bid = wl_u32(m);
		const struct wl_obj *o = obj_of(c, bid);

		(void)wl_i32(m);
		(void)wl_i32(m);
		if (m->bad)
			return false;
		s->attached = true;
		s->buf = -1;
		if (bid) {
			if (!o || o->iface != WI_BUFFER)
				return proto_err(c, m->obj, 0, "attach: not a buffer");
			s->buf = o->idx;
		}
		return true;
	}
	case 2: case 9:                         /* damage, damage_buffer: whole buffers are copied */
		for (int i = 0; i < 4; i++)
			(void)wl_i32(m);
		return !m->bad;
	case 3: {                               /* frame */
		const uint32_t id = wl_u32(m);

		if (m->bad || !newobj(c, id, WI_CALLBACK, 1, 0))
			return false;
		if (s->nframes == WL_FRAMES) {  /* too many: complete the oldest now */
			ev(c, s->frames[0], 0, "u", now(c));
			del(c, s->frames[0]);
			memmove(s->frames, s->frames + 1, sizeof(s->frames[0]) * (WL_FRAMES - 1));
			s->nframes--;
		}
		s->frames[s->nframes++] = id;
		return true;
	}
	case 4: case 5:                         /* opaque / input region */
		(void)wl_u32(m);
		return !m->bad;
	case 6:                                 /* commit */
		surface_commit(c, si);
		return true;
	case 7: case 8:                         /* transform, scale */
		(void)wl_i32(m);
		return !m->bad;
	case 10:                                /* offset */
		(void)wl_i32(m);
		(void)wl_i32(m);
		return !m->bad;
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_shm(struct wl_client *c, struct wl_msg *m)
{
	const uint32_t id = wl_u32(m);
	const int fd = wl_fd(m);
	const int32_t size = wl_i32(m);
	const struct wl_host *h = &c->srv->host;
	int pi;

	if (m->op != 0)
		return proto_err(c, m->obj, 1, "invalid method");
	if (m->bad)
		return false;
	pi = ALLOC(c->pool);
	if (size <= 0 || pi < 0 || !h->map) {
		if (fd >= 0 && h->close_fd)
			h->close_fd(h->ctx, fd);
		return proto_err(c, m->obj, size <= 0 ? 1 : 2, "bad shm pool");
	}
	c->pool[pi] = (struct wl_pool){ .used = true, .size = (size_t)size, .fd = fd, .refs = 1 };
	c->pool[pi].base = h->map(h->ctx, fd, (size_t)size);
	if (!c->pool[pi].base) {
		if (h->close_fd)
			h->close_fd(h->ctx, fd);
		memset(&c->pool[pi], 0, sizeof(c->pool[pi]));
		return proto_err(c, m->obj, 2, "cannot map pool");
	}
	return newobj(c, id, WI_POOL, 1, (unsigned)pi);
}

static bool h_pool(struct wl_client *c, struct wl_msg *m)
{
	const int pi = c->obj[m->obj].idx;
	struct wl_pool *p = &c->pool[pi];

	switch (m->op) {
	case 0: {                               /* create_buffer */
		const uint32_t id = wl_u32(m);
		const int32_t off = wl_i32(m), w = wl_i32(m), hh = wl_i32(m), stride = wl_i32(m);
		const uint32_t fmt = wl_u32(m);
		int bi;

		if (m->bad)
			return false;
		if (fmt > 1)
			return proto_err(c, m->obj, 0, "unsupported format");
		if (off < 0 || w <= 0 || hh <= 0 || (int64_t)stride < (int64_t)w * 4 || (stride & 3) || (off & 3) ||
		    (uint64_t)off + (uint64_t)(hh - 1) * (uint64_t)stride + (uint64_t)w * 4 > p->size)
			return proto_err(c, m->obj, 1, "invalid buffer geometry");
		bi = ALLOC(c->buf);
		if (bi < 0)
			return proto_err(c, m->obj, 2, "too many buffers");
		if (!newobj(c, id, WI_BUFFER, 1, (unsigned)bi))
			return false;
		c->buf[bi] = (struct wl_buffer){ true, (uint16_t)pi, id, (uint32_t)off, (uint32_t)stride,
						 fmt, w, hh, false };
		p->refs++;
		return true;
	}
	case 1:                                 /* destroy */
		pool_unref(c, pi);
		del(c, m->obj);
		return true;
	case 2: {                               /* resize: grow only */
		const int32_t size = wl_i32(m);

		if (m->bad)
			return false;
		if (size < 0 || (size_t)size < p->size)
			return proto_err(c, m->obj, 1, "pools only grow");
		if ((size_t)size > p->size) {
			uint8_t *nb = c->srv->host.map(c->srv->host.ctx, p->fd, (size_t)size);

			if (!nb)
				return proto_err(c, m->obj, 2, "cannot grow pool");
			c->srv->host.unmap(c->srv->host.ctx, p->base, p->size);
			p->base = nb;
			p->size = (size_t)size;
		}
		return true;
	}
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_buffer(struct wl_client *c, struct wl_msg *m)
{
	const int bi = c->obj[m->obj].idx;

	if (m->op != 0)
		return proto_err(c, m->obj, 1, "invalid method");
	if (buffer_in_use(c, bi))
		c->buf[bi].zombie = true;
	else
		buffer_free(c, bi);
	del(c, m->obj);
	return true;
}

static bool h_subcomp(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 0) {
		del(c, m->obj);
		return true;
	}
	if (m->op == 1) {
		const uint32_t id = wl_u32(m), sid = wl_u32(m), pid = wl_u32(m);
		const struct wl_obj *so = obj_of(c, sid), *po = obj_of(c, pid);
		struct wl_surface *s;

		if (m->bad)
			return false;
		if (!so || so->iface != WI_SURFACE || !po || po->iface != WI_SURFACE)
			return proto_err(c, m->obj, 0, "get_subsurface: bad surface");
		s = &c->surf[so->idx];
		if (s->role)
			return proto_err(c, m->obj, 0, "surface already has a role");
		if (!newobj(c, id, WI_SUBSURF, 1, so->idx))
			return false;
		s->role = 3;
		s->parent = po->idx;
		s->subsurf = id;
		return true;
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_subsurf(struct wl_client *c, struct wl_msg *m)
{
	const int si = c->obj[m->obj].idx;
	struct wl_surface *s = &c->surf[si];

	switch (m->op) {
	case 0: {
		const int ri = root_of(c, si);

		unmap_surface(c, si);
		s->role = 0;
		s->parent = -1;
		del(c, m->obj);
		if (ri >= 0 && ri != si && c->surf[ri].win)
			blit_tree(c, ri);
		return true;
	}
	case 1:
		s->px = wl_i32(m);
		s->py = wl_i32(m);
		return !m->bad;
	case 2: case 3:
		(void)wl_u32(m);
		return !m->bad;
	case 4: case 5:
		return true;
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_wmbase(struct wl_client *c, struct wl_msg *m)
{
	switch (m->op) {
	case 0:
		del(c, m->obj);
		return true;
	case 1: {                               /* create_positioner */
		const uint32_t id = wl_u32(m);
		int pi = ALLOC(c->pos);

		if (m->bad)
			return false;
		if (pi < 0)
			return proto_err(c, m->obj, 2, "too many positioners");
		c->pos[pi] = (struct wl_pos){ .used = true };
		return newobj(c, id, WI_POSITIONER, c->obj[m->obj].ver, (unsigned)pi);
	}
	case 2: {                               /* get_xdg_surface */
		const uint32_t id = wl_u32(m), sid = wl_u32(m);
		const struct wl_obj *so = obj_of(c, sid);

		if (m->bad)
			return false;
		if (!so || so->iface != WI_SURFACE)
			return proto_err(c, m->obj, 0, "get_xdg_surface: not a surface");
		if (c->surf[so->idx].xsurf)
			return proto_err(c, m->obj, 0, "surface already has an xdg_surface");
		if (!newobj(c, id, WI_XSURF, c->obj[m->obj].ver, so->idx))
			return false;
		c->surf[so->idx].xsurf = id;
		return true;
	}
	case 3:                                 /* pong */
		(void)wl_u32(m);
		return !m->bad;
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_positioner(struct wl_client *c, struct wl_msg *m)
{
	struct wl_pos *p = &c->pos[c->obj[m->obj].idx];

	switch (m->op) {
	case 0:
		p->used = false;
		del(c, m->obj);
		return true;
	case 1: p->w = wl_i32(m); p->h = wl_i32(m); break;
	case 2: p->ax = wl_i32(m); p->ay = wl_i32(m); p->aw = wl_i32(m); p->ah = wl_i32(m); break;
	case 3: p->anchor = wl_u32(m); break;
	case 4: p->gravity = wl_u32(m); break;
	case 5: (void)wl_u32(m); break;
	case 6: p->ox = wl_i32(m); p->oy = wl_i32(m); break;
	case 7: break;
	case 8: (void)wl_i32(m); (void)wl_i32(m); break;
	case 9: (void)wl_u32(m); break;
	default: return proto_err(c, m->obj, 1, "invalid method");
	}
	if (p->anchor > 8 || p->gravity > 8)
		return proto_err(c, m->obj, 0, "invalid anchor or gravity");
	return !m->bad;
}

static bool h_xsurf(struct wl_client *c, struct wl_msg *m)
{
	const int si = c->obj[m->obj].idx;
	struct wl_surface *s = &c->surf[si];

	switch (m->op) {
	case 0:
		if (s->role == 1 || s->role == 2)
			return proto_err(c, m->obj, 6, "xdg_surface destroyed before its role");
		s->xsurf = 0;
		del(c, m->obj);
		return true;
	case 1: {                               /* get_toplevel */
		const uint32_t id = wl_u32(m);

		if (m->bad)
			return false;
		if (s->role)
			return proto_err(c, m->obj, 0, "surface already has a role");
		if (!newobj(c, id, WI_TOPLEVEL, c->obj[m->obj].ver, (unsigned)si))
			return false;
		s->role = 1;
		s->top = id;
		return true;
	}
	case 2: {                               /* get_popup */
		const uint32_t id = wl_u32(m), parent = wl_u32(m), pos = wl_u32(m);
		const struct wl_obj *po = obj_of(c, parent), *qo = obj_of(c, pos);
		const struct wl_pos *p;

		if (m->bad)
			return false;
		if (s->role)
			return proto_err(c, m->obj, 0, "surface already has a role");
		if (!po || po->iface != WI_XSURF || !qo || qo->iface != WI_POSITIONER)
			return proto_err(c, m->obj, 3, "get_popup: parent or positioner invalid");
		p = &c->pos[qo->idx];
		if (!newobj(c, id, WI_POPUP, c->obj[m->obj].ver, (unsigned)si))
			return false;
		s->role = 2;
		s->popup = id;
		s->parent = po->idx;
		popup_place(c, s, p, &c->surf[po->idx]);
		ev(c, id, 0, "iiii", s->x - c->surf[po->idx].gx, s->y - c->surf[po->idx].gy, p->w, p->h);
		ev(c, m->obj, 0, "u", next_serial(c));
		s->configured = true;
		return true;
	}
	case 3:                                 /* set_window_geometry */
		s->gx = wl_i32(m);
		s->gy = wl_i32(m);
		s->gw = wl_i32(m);
		s->gh = wl_i32(m);
		if (m->bad)
			return false;
		if (s->gw < 0 || s->gh < 0)
			return proto_err(c, m->obj, 4, "negative window geometry");
		return true;
	case 4:                                 /* ack_configure */
		(void)wl_u32(m);
		return !m->bad;
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_toplevel(struct wl_client *c, struct wl_msg *m)
{
	const int si = c->obj[m->obj].idx;
	struct wl_surface *s = &c->surf[si];
	const struct wl_host *h = &c->srv->host;

	switch (m->op) {
	case 0:
		unmap_surface(c, si);
		s->role = 0;
		s->top = 0;
		del(c, m->obj);
		return true;
	case 1: case 12:                        /* set_parent, unset_fullscreen */
		if (m->op == 1)
			(void)wl_u32(m);
		break;
	case 2: {                               /* set_title */
		const char *t = wl_str(m);

		if (m->bad)
			return false;
		copy_str(s->title, t, sizeof(s->title));
		if (s->win && h->win_title)
			h->win_title(h->ctx, s->win, s->title);
		return true;
	}
	case 3:
		(void)wl_str(m);
		break;
	case 4:                                 /* show_window_menu */
		for (int i = 0; i < 5; i++)
			(void)wl_u32(m);
		break;
	case 5:                                 /* move */
		(void)wl_u32(m);
		(void)wl_u32(m);
		break;
	case 6:                                 /* resize */
		for (int i = 0; i < 3; i++)
			(void)wl_u32(m);
		break;
	case 7: case 8:
		(void)wl_i32(m);
		(void)wl_i32(m);
		break;
	case 9: case 10: case 13:
		break;
	case 11:
		(void)wl_u32(m);
		break;
	default:
		return proto_err(c, m->obj, 1, "invalid method");
	}
	return !m->bad;
}

static bool h_popup(struct wl_client *c, struct wl_msg *m)
{
	const int si = c->obj[m->obj].idx;

	switch (m->op) {
	case 0: {
		const int ri = root_of(c, si);

		unmap_surface(c, si);
		c->surf[si].role = 0;
		c->surf[si].popup = 0;
		c->surf[si].parent = -1;
		del(c, m->obj);
		if (ri >= 0 && ri != si && c->surf[ri].win)
			blit_tree(c, ri);
		return true;
	}
	case 1:
		(void)wl_u32(m);
		(void)wl_u32(m);
		return !m->bad;
	case 2: {                               /* reposition */
		const uint32_t pid = wl_u32(m), token = wl_u32(m);
		const struct wl_obj *qo = obj_of(c, pid);
		struct wl_surface *s = &c->surf[si];
		const struct wl_surface *par = s->parent >= 0 ? &c->surf[s->parent] : NULL;

		if (m->bad)
			return false;
		if (!qo || qo->iface != WI_POSITIONER)
			return proto_err(c, m->obj, 0, "reposition: bad positioner");
		popup_place(c, s, &c->pos[qo->idx], par);
		ev(c, m->obj, 2, "u", token);
		ev(c, m->obj, 0, "iiii", s->x - (par ? par->gx : 0), s->y - (par ? par->gy : 0),
		   c->pos[qo->idx].w, c->pos[qo->idx].h);
		ev(c, s->xsurf, 0, "u", next_serial(c));
		return true;
	}
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

/* Keyboard focus may already be on this client's toplevel when it asks for the keyboard. */
static int toplevel_of_win(struct wl_client *c, int win)
{
	for (int i = 0; win && i < WL_MAX_SURF; i++)
		if (c->surf[i].used && c->surf[i].role == 1 && c->surf[i].win == win)
			return i;
	return -1;
}

static void kbd_enter(struct wl_client *c, uint32_t kid, int si)
{
	ev(c, kid, 1, "uua", next_serial(c), c->surf[si].id, "", 0u);
	ev(c, kid, 4, "uuuuu", next_serial(c), c->srv->mods, 0u, 0u, 0u);
}

static bool h_seat(struct wl_client *c, struct wl_msg *m)
{
	const struct wl_obj *self = &c->obj[m->obj];

	switch (m->op) {
	case 0: {                               /* get_pointer */
		const uint32_t id = wl_u32(m);

		return !m->bad && newobj(c, id, WI_POINTER, self->ver, 0);
	}
	case 1: {                               /* get_keyboard */
		const uint32_t id = wl_u32(m);
		struct wl_host *h = &c->srv->host;
		uint32_t size = 0;
		int fd, si;

		if (m->bad || !newobj(c, id, WI_KEYBOARD, self->ver, 0))
			return false;
		fd = h->keymap_fd ? h->keymap_fd(h->ctx, &size) : -1;
		if (fd >= 0)
			ev(c, id, 0, "ufu", 1u, fd, size);      /* xkb_v1 */
		if (self->ver >= 4)
			ev(c, id, 5, "ii", 25, 600);
		si = toplevel_of_win(c, c->srv->focus_win);
		if (si >= 0) {
			c->kbd_surf = si;
			kbd_enter(c, id, si);
		}
		return true;
	}
	case 2:
		return proto_err(c, m->obj, 0, "no touch capability");
	case 3:
		del(c, m->obj);
		return true;
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_release_only(struct wl_client *c, struct wl_msg *m, unsigned destroy_op)
{
	if (m->op != destroy_op)
		return proto_err(c, m->obj, 1, "invalid method");
	del(c, m->obj);
	return true;
}

static bool h_pointer(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 0) {                       /* set_cursor: images are not shown */
		(void)wl_u32(m);
		(void)wl_u32(m);
		(void)wl_i32(m);
		(void)wl_i32(m);
		return !m->bad;
	}
	return h_release_only(c, m, 1);
}

static bool h_output_like(struct wl_client *c, struct wl_msg *m)
{
	const struct wl_obj *o = &c->obj[m->obj];

	if (o->iface == WI_XOUTMGR) {
		if (m->op == 1) {               /* get_xdg_output */
			const uint32_t id = wl_u32(m), out = wl_u32(m);
			const struct wl_obj *oo = obj_of(c, out);

			if (m->bad || !oo || oo->iface != WI_OUTPUT)
				return !m->bad && proto_err(c, m->obj, 0, "not an output");
			if (!newobj(c, id, WI_XOUTPUT, o->ver, 0))
				return false;
			ev(c, id, 0, "ii", 0, 0);
			ev(c, id, 1, "ii", c->srv->host.out_w, c->srv->host.out_h);
			if (o->ver >= 2)
				ev(c, id, 3, "s", "MOS-1");
			ev(c, id, 2, "");
			return true;
		}
		return h_release_only(c, m, 0);
	}
	return h_release_only(c, m, 0);
}

static bool h_ddm(struct wl_client *c, struct wl_msg *m)
{
	const uint32_t id = wl_u32(m);

	if (m->bad)
		return false;
	if (m->op == 0)
		return newobj(c, id, WI_DSOURCE, c->obj[m->obj].ver, 0);
	if (m->op == 1) {
		(void)wl_u32(m);
		return !m->bad && newobj(c, id, WI_DDEV, c->obj[m->obj].ver, 0);
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_dsource(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 0)
		(void)wl_str(m);
	else if (m->op == 2)
		(void)wl_u32(m);
	else
		return h_release_only(c, m, 1);
	return !m->bad;
}

static bool h_ddev(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 0) {
		for (int i = 0; i < 4; i++)
			(void)wl_u32(m);
		return !m->bad;
	}
	if (m->op == 1) {
		(void)wl_u32(m);
		(void)wl_u32(m);
		return !m->bad;
	}
	return h_release_only(c, m, 2);
}

static bool h_vporter(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 1) {
		const uint32_t id = wl_u32(m), sid = wl_u32(m);
		const struct wl_obj *so = obj_of(c, sid);

		if (m->bad)
			return false;
		if (!so || so->iface != WI_SURFACE)
			return proto_err(c, m->obj, 0, "get_viewport: not a surface");
		if (c->surf[so->idx].viewport)
			return proto_err(c, m->obj, 0, "viewport already exists");
		if (!newobj(c, id, WI_VIEWPORT, 1, so->idx))
			return false;
		c->surf[so->idx].viewport = id;
		return true;
	}
	return h_release_only(c, m, 0);
}

static bool h_viewport(struct wl_client *c, struct wl_msg *m)
{
	struct wl_surface *s = &c->surf[c->obj[m->obj].idx];

	switch (m->op) {
	case 0:
		s->viewport = 0;
		s->dw = s->dh = 0;
		del(c, m->obj);
		return true;
	case 1:
		for (int i = 0; i < 4; i++)
			(void)wl_i32(m);
		return !m->bad;
	case 2:
		s->dw = wl_i32(m);
		s->dh = wl_i32(m);
		if (m->bad)
			return false;
		if ((s->dw <= 0) != (s->dh <= 0) || s->dw < -1 || s->dh < -1)
			return proto_err(c, m->obj, 1, "bad destination size");
		if (s->dw < 0)
			s->dw = s->dh = 0;
		return true;
	}
	return proto_err(c, m->obj, 1, "invalid method");
}

static bool h_pcons(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 1 || m->op == 2) {         /* lock_pointer, confine_pointer */
		const uint32_t id = wl_u32(m);

		for (int i = 0; i < 4; i++)
			(void)wl_u32(m);
		if (m->bad || !newobj(c, id, WI_PLOCK, 1, 0))
			return false;
		ev(c, id, 0, "");               /* locked / confined: both are event 0 */
		return true;
	}
	return h_release_only(c, m, 0);
}

static bool h_plock(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 0) {
		del(c, m->obj);
		return true;
	}
	for (const uint8_t *p = m->p; p < m->end; p++)    /* hint / region: ignored */
		(void)*p;
	return true;
}

static bool h_relmgr(struct wl_client *c, struct wl_msg *m)
{
	if (m->op == 1) {
		const uint32_t id = wl_u32(m);

		(void)wl_u32(m);
		return !m->bad && newobj(c, id, WI_RELPTR, 1, 0);
	}
	return h_release_only(c, m, 0);
}

static bool request(struct wl_client *c, struct wl_msg *m)
{
	const struct wl_obj *o = obj_of(c, m->obj);

	if (m->obj == 1)
		return h_display(c, m);
	if (!o || !o->iface)
		return proto_err(c, m->obj, 0, "no such object");
	switch (o->iface) {
	case WI_REGISTRY:   return m->op == 0 ? h_registry_bind(c, m) : proto_err(c, m->obj, 1, "invalid method");
	case WI_COMPOSITOR: return h_compositor(c, m);
	case WI_REGION:     return h_region(c, m);
	case WI_SURFACE:    return h_surface(c, m);
	case WI_SHM:        return h_shm(c, m);
	case WI_POOL:       return h_pool(c, m);
	case WI_BUFFER:     return h_buffer(c, m);
	case WI_SUBCOMP:    return h_subcomp(c, m);
	case WI_SUBSURF:    return h_subsurf(c, m);
	case WI_WMBASE:     return h_wmbase(c, m);
	case WI_POSITIONER: return h_positioner(c, m);
	case WI_XSURF:      return h_xsurf(c, m);
	case WI_TOPLEVEL:   return h_toplevel(c, m);
	case WI_POPUP:      return h_popup(c, m);
	case WI_SEAT:       return h_seat(c, m);
	case WI_POINTER:    return h_pointer(c, m);
	case WI_KEYBOARD:   return h_release_only(c, m, 0);
	case WI_OUTPUT:
	case WI_XOUTPUT:
	case WI_XOUTMGR:    return h_output_like(c, m);
	case WI_DDM:        return h_ddm(c, m);
	case WI_DSOURCE:    return h_dsource(c, m);
	case WI_DDEV:       return h_ddev(c, m);
	case WI_VPORTER:    return h_vporter(c, m);
	case WI_VIEWPORT:   return h_viewport(c, m);
	case WI_PCONS:      return h_pcons(c, m);
	case WI_PLOCK:      return h_plock(c, m);
	case WI_RELMGR:     return h_relmgr(c, m);
	case WI_RELPTR:     return h_release_only(c, m, 0);
	default:            return proto_err(c, m->obj, 1, "object takes no requests");
	}
}

/* ---- clients ---- */

void wl_server_init(struct wl_server *s, const struct wl_host *h)
{
	memset(s, 0, sizeof(*s));
	s->host = *h;
}

struct wl_client *wl_client_add(struct wl_server *s)
{
	for (int i = 0; i < WL_MAX_CLIENTS; i++) {
		struct wl_client *c = &s->cl[i];

		if (c->used)
			continue;
		memset(c, 0, sizeof(*c));
		c->used = true;
		c->srv = s;
		c->ptr_surf = c->kbd_surf = -1;
		c->obj[1].iface = WI_DISPLAY;
		c->obj[1].ver = 1;
		return c;
	}
	return NULL;
}

bool wl_client_dispatch(struct wl_client *c)
{
	struct wl_msg m;
	int r;

	while (!c->conn.err && (r = wl_next(&c->conn, &m)) != 0) {
		if (r < 0)
			return proto_err(c, 1, 1, "malformed message");
		if (!request(c, &m) || m.bad) {
			if (!c->conn.err)
				proto_err(c, m.obj, 1, "malformed request");
			return false;
		}
		wl_pop(&c->conn, &m);
	}
	return !c->conn.err;
}

void wl_client_drop(struct wl_client *c)
{
	struct wl_server *s = c->srv;
	const struct wl_host *h = &s->host;

	for (int i = 0; i < WL_MAX_SURF; i++)
		if (c->surf[i].used && c->surf[i].win && h->win_destroy) {
			if (s->focus_win == c->surf[i].win)
				s->focus_win = 0;
			h->win_destroy(h->ctx, c->surf[i].win);
		}
	for (int i = 0; i < WL_MAX_POOL; i++)
		if (c->pool[i].used && c->pool[i].base && h->unmap)
			h->unmap(h->ctx, c->pool[i].base, c->pool[i].size);
	if (h->close_fd) {
		for (int i = 0; i < c->conn.in_nfd; i++)
			h->close_fd(h->ctx, c->conn.in_fd[i]);
		for (int i = 0; i < c->conn.out_nfd; i++)
			h->close_fd(h->ctx, c->conn.out_fd[i]);
	}
	memset(c, 0, sizeof(*c));
}

/* ---- input ---- */

#define FOR_OBJ(c, i, kind) \
	for (uint32_t i = 2; i < WL_MAX_OBJ; i++) if ((c)->obj[i].iface == (kind))

static struct wl_client *client_of_win(struct wl_server *s, int win, int *si)
{
	for (int k = 0; win && k < WL_MAX_CLIENTS; k++) {
		if (!s->cl[k].used)
			continue;
		*si = toplevel_of_win(&s->cl[k], win);
		if (*si >= 0)
			return &s->cl[k];
	}
	return NULL;
}

/* Topmost surface of a toplevel's tree under a point (window content coordinates). */
static int hit_surface(struct wl_client *c, int ri, int x, int y, int *lx, int *ly)
{
	const struct wl_surface *r = &c->surf[ri];
	int best = ri;

	for (int i = 0; i < WL_MAX_SURF; i++) {
		const struct wl_surface *s = &c->surf[i];
		int ax, ay;

		if (!s->used || !s->mapped || i == ri || s->role == 1 || root_of(c, i) != ri)
			continue;
		abs_pos(c, i, &ax, &ay);
		ax -= r->gx;
		ay -= r->gy;
		if (x >= ax && y >= ay && x < ax + s->cw && y < ay + s->ch && i >= best)
			best = i;
	}
	if (best == ri) {
		*lx = x + r->gx;
		*ly = y + r->gy;
	} else {
		int ax, ay;

		abs_pos(c, best, &ax, &ay);
		*lx = x - (ax - r->gx);
		*ly = y - (ay - r->gy);
	}
	return best;
}

static void ptr_frame(struct wl_client *c)
{
	FOR_OBJ(c, i, WI_POINTER)
		if (c->obj[i].ver >= 5)
			ev(c, i, 5, "");
}

static void ptr_to(struct wl_client *c, int target, int lx, int ly)
{
	if (target == c->ptr_surf)
		return;
	if (c->ptr_surf >= 0 && c->surf[c->ptr_surf].used)
		FOR_OBJ(c, i, WI_POINTER)
			ev(c, i, 1, "uu", next_serial(c), c->surf[c->ptr_surf].id);
	c->ptr_surf = target;
	if (target >= 0)
		FOR_OBJ(c, i, WI_POINTER)
			ev(c, i, 0, "uuxx", next_serial(c), c->surf[target].id, lx, ly);
	ptr_frame(c);
}

void wl_in_pointer_motion(struct wl_server *s, int win, int x, int y)
{
	int si, lx, ly, t;
	struct wl_client *c = client_of_win(s, win, &si);

	if (!c)
		return;
	t = hit_surface(c, si, x, y, &lx, &ly);
	ptr_to(c, t, lx, ly);
	FOR_OBJ(c, i, WI_POINTER)
		ev(c, i, 2, "uxx", now(c), lx, ly);
	ptr_frame(c);
	FOR_OBJ(c, i, WI_RELPTR)
		ev(c, i, 0, "uuxxxx", 0u, now(c) * 1000u, x - c->ptr_x, y - c->ptr_y, x - c->ptr_x,
		   y - c->ptr_y);
	c->ptr_x = x;
	c->ptr_y = y;
}

void wl_in_pointer_button(struct wl_server *s, int win, int x, int y, unsigned btn, bool down)
{
	int si, lx, ly, t;
	struct wl_client *c = client_of_win(s, win, &si);

	if (!c)
		return;
	t = hit_surface(c, si, x, y, &lx, &ly);
	ptr_to(c, t, lx, ly);
	FOR_OBJ(c, i, WI_POINTER)
		ev(c, i, 3, "uuuu", next_serial(c), now(c), btn, down ? 1u : 0u);
	ptr_frame(c);
}

void wl_in_pointer_leave(struct wl_server *s)
{
	for (int k = 0; k < WL_MAX_CLIENTS; k++)
		if (s->cl[k].used)
			ptr_to(&s->cl[k], -1, 0, 0);
}

void wl_in_mods(struct wl_server *s, unsigned mods)
{
	int si;
	struct wl_client *c = client_of_win(s, s->focus_win, &si);

	if (!c || mods == s->mods)
		return;
	s->mods = mods;
	FOR_OBJ(c, i, WI_KEYBOARD)
		ev(c, i, 4, "uuuuu", next_serial(c), mods, 0u, 0u, 0u);
}

void wl_in_key(struct wl_server *s, unsigned code, bool down)
{
	int si;
	struct wl_client *c = client_of_win(s, s->focus_win, &si);

	if (!c)
		return;
	FOR_OBJ(c, i, WI_KEYBOARD)
		ev(c, i, 3, "uuuu", next_serial(c), now(c), code, down ? 1u : 0u);
}

void wl_in_focus(struct wl_server *s, int win, bool focused)
{
	int si;
	struct wl_client *c;

	if (!focused) {
		if (s->focus_win != win)
			return;
		c = client_of_win(s, win, &si);
		s->focus_win = 0;
		if (c) {
			FOR_OBJ(c, i, WI_KEYBOARD)
				ev(c, i, 2, "uu", next_serial(c), c->surf[si].id);
			c->kbd_surf = -1;
		}
		return;
	}
	if (s->focus_win && s->focus_win != win)
		wl_in_focus(s, s->focus_win, false);
	c = client_of_win(s, win, &si);
	s->focus_win = c ? win : 0;
	if (c) {
		c->kbd_surf = si;
		FOR_OBJ(c, i, WI_KEYBOARD)
			kbd_enter(c, i, si);
	}
}

void wl_in_close(struct wl_server *s, int win)
{
	int si;
	struct wl_client *c = client_of_win(s, win, &si);

	if (c && c->surf[si].top)
		ev(c, c->surf[si].top, 1, "");
}

/* ---- console characters to evdev keys (US layout) ---- */

unsigned wl_ascii_to_key(int ch, unsigned *mods)
{
	static const char row1[] = "1234567890-=";
	static const char rowq[] = "qwertyuiop[]";
	static const char rowa[] = "asdfghjkl;'`";
	static const char rowz[] = "\\zxcvbnm,./";
	static const char shifted_digits[] = "!@#$%^&*()_+";
	static const char shifted_punct[] = "{}:\"~|<>?";
	const char *p;

	*mods = 0;
	if (ch == 0x110001) return 103;         /* KEY_UP */
	if (ch == 0x110002) return 108;
	if (ch == 0x110003) return 105;
	if (ch == 0x110004) return 106;
	if (ch == '\n' || ch == '\r') return 28;
	if (ch == '\b' || ch == 0x7f) return 14;
	if (ch == '\t') return 15;
	if (ch == 27) return 1;
	if (ch == ' ') return 57;
	if (ch >= 1 && ch <= 26) {              /* Ctrl + letter */
		*mods = 4;
		ch += 'a' - 1;
	}
	if (ch >= 'A' && ch <= 'Z') {
		*mods |= 1;
		ch += 'a' - 'A';
	}
	if ((p = memchr(shifted_digits, ch, sizeof(shifted_digits) - 1))) {
		*mods |= 1;
		ch = row1[p - shifted_digits];
	} else if ((p = memchr(shifted_punct, ch, sizeof(shifted_punct) - 1))) {
		static const char base[] = "[];'`\\,./";   /* what { } : " ~ | < > ? sit on */

		*mods |= 1;
		ch = base[p - shifted_punct];
	}
	if ((p = memchr(row1, ch, sizeof(row1) - 1)))
		return 2 + (unsigned)(p - row1);
	if ((p = memchr(rowq, ch, sizeof(rowq) - 1)))
		return 16 + (unsigned)(p - rowq);
	if ((p = memchr(rowa, ch, sizeof(rowa) - 1)))
		return 30 + (unsigned)(p - rowa);
	if ((p = memchr(rowz, ch, sizeof(rowz) - 1)))
		return 43 + (unsigned)(p - rowz);
	return 0;
}
