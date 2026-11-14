/* SPDX-License-Identifier: BSD-2-Clause */
/* Wayland wire protocol: framing, argument codecs, fd queues (docs/WAYLAND.md). */
#include "src/wayland/wl.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return v;
}

int wl_next(struct wl_conn *c, struct wl_msg *m)
{
	uint32_t w1;
	size_t size;

	if (c->in_len < 8)
		return 0;
	w1 = rd32(c->in + 4);
	size = w1 >> 16;
	if (size < 8 || (size & 3) || size > WL_MSG_MAX)
		return -1;
	if (c->in_len < size)
		return 0;
	m->obj = rd32(c->in);
	m->op = (uint16_t)(w1 & 0xffff);
	m->size = (uint16_t)size;
	m->p = c->in + 8;
	m->end = c->in + size;
	m->c = c;
	m->bad = false;
	return 1;
}

void wl_pop(struct wl_conn *c, const struct wl_msg *m)
{
	memmove(c->in, c->in + m->size, c->in_len - m->size);
	c->in_len -= m->size;
}

bool wl_feed(struct wl_conn *c, const void *data, size_t n, const int *fds, int nfd)
{
	if (n > WL_IN_BUF - c->in_len || nfd > WL_MAX_FDS - c->in_nfd)
		return false;
	if (n)
		memcpy(c->in + c->in_len, data, n);
	c->in_len += n;
	for (int i = 0; i < nfd; i++)
		c->in_fd[c->in_nfd++] = fds[i];
	return true;
}

uint32_t wl_u32(struct wl_msg *m)
{
	uint32_t v;

	if (m->bad || m->end - m->p < 4) {
		m->bad = true;
		return 0;
	}
	v = rd32(m->p);
	m->p += 4;
	return v;
}

int32_t wl_i32(struct wl_msg *m)
{
	return (int32_t)wl_u32(m);
}

const uint8_t *wl_arr(struct wl_msg *m, uint32_t *len)
{
	const uint32_t n = wl_u32(m);
	const size_t padded = ((size_t)n + 3) & ~(size_t)3;
	const uint8_t *r = m->p;

	*len = 0;
	if (m->bad || padded > (size_t)(m->end - m->p)) {
		m->bad = true;
		return NULL;
	}
	m->p += padded;
	*len = n;
	return r;
}

const char *wl_str(struct wl_msg *m)
{
	uint32_t n;
	const uint8_t *s = wl_arr(m, &n);

	if (!s)
		return NULL;
	if (n == 0)
		return "";
	if (s[n - 1] != 0) {
		m->bad = true;
		return NULL;
	}
	return (const char *)s;
}

int wl_fd(struct wl_msg *m)
{
	struct wl_conn *c = m->c;
	int fd;

	if (m->bad || c->in_nfd == 0) {
		m->bad = true;
		return -1;
	}
	fd = c->in_fd[0];
	for (int i = 1; i < c->in_nfd; i++)
		c->in_fd[i - 1] = c->in_fd[i];
	c->in_nfd--;
	return fd;
}

struct wl_out wl_begin(struct wl_conn *c, uint32_t obj, uint16_t op)
{
	struct wl_out o = { c, c->out_len, false };
	const uint32_t w1 = op;

	if (c->out_len + 8 > WL_OUT_BUF) {
		o.bad = true;
		return o;
	}
	memcpy(c->out + c->out_len, &obj, 4);
	memcpy(c->out + c->out_len + 4, &w1, 4);
	c->out_len += 8;
	return o;
}

void wl_put_u32(struct wl_out *o, uint32_t v)
{
	if (o->bad || o->c->out_len + 4 > WL_OUT_BUF) {
		o->bad = true;
		return;
	}
	memcpy(o->c->out + o->c->out_len, &v, 4);
	o->c->out_len += 4;
}

void wl_put_arr(struct wl_out *o, const void *p, uint32_t len)
{
	const size_t padded = ((size_t)len + 3) & ~(size_t)3;

	wl_put_u32(o, len);
	if (o->bad || o->c->out_len + padded > WL_OUT_BUF) {
		o->bad = true;
		return;
	}
	if (len)
		memcpy(o->c->out + o->c->out_len, p, len);
	memset(o->c->out + o->c->out_len + len, 0, padded - len);
	o->c->out_len += padded;
}

void wl_put_str(struct wl_out *o, const char *s)
{
	wl_put_arr(o, s, (uint32_t)strlen(s) + 1);
}

void wl_put_fd(struct wl_out *o, int fd)
{
	if (o->bad || o->c->out_nfd >= WL_MAX_FDS) {
		o->bad = true;
		return;
	}
	o->c->out_fd[o->c->out_nfd++] = fd;
}

void wl_end(struct wl_out *o)
{
	struct wl_conn *c = o->c;
	const size_t size = c->out_len - o->start;
	uint32_t w1;

	if (o->bad || size > WL_MSG_MAX) {
		c->out_len = o->start;
		c->err = true;
		return;
	}
	memcpy(&w1, c->out + o->start + 4, 4);
	w1 |= (uint32_t)size << 16;
	memcpy(c->out + o->start + 4, &w1, 4);
}

void wl_sent(struct wl_conn *c, size_t n)
{
	memmove(c->out, c->out + n, c->out_len - n);
	c->out_len -= n;
	c->out_nfd = 0;
}
