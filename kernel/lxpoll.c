/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Readiness for the Linux personality: poll, ppoll, select, pselect6,
 * epoll (level triggered, with EPOLLET/EPOLLONESHOT) and eventfd. Any
 * pollable object calls poll_wake() when its state changes; sleepers
 * rescan. One CPU and few threads make that cheaper than wait queues.
 */
#include <mergedos/kernel.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>

#define A(i) (c->arg[i])
#define TASK (ctx->task)
#define LXFN(name) long name(struct abi_ctx *ctx, const struct abi_call *c)
#define EPOLLET      (1u << 31)
#define EPOLLONESHOT (1u << 30)

/* Ticks until `ns` from now (rounded up); ~0 for no timeout. */
static u64 deadline_of(s64 ns)
{
	if (ns < 0)
		return ~0ULL;
	return time_ticks() + ((u64)ns + 9999999ULL) / 10000000ULL;
}

/* Sleep until something changes or the deadline; false when the deadline passed. */
static bool wait_change(u64 deadline)
{
	u64 now = time_ticks();

	if (deadline != ~0ULL) {
		if (now >= deadline)
			return false;
		poll_wait(deadline - now);
	} else {
		poll_wait(0);
	}
	return true;
}

static s64 ts_ns(struct task *t, u64 uts, bool timeval, long *err)
{
	u64 ts[2];

	*err = 0;
	if (!uts)
		return -1;
	if (uaccess_copy_in(t, ts, uts, sizeof(ts))) {
		*err = -MERR_FAULT;
		return 0;
	}
	return (s64)(ts[0] * 1000000000ULL + ts[1] * (timeval ? 1000ULL : 1ULL));
}

struct pfd {
	s32 fd;
	s16 events, revents;
};

static long poll_scan(struct task *t, struct pfd *p, u64 n)
{
	long ready = 0;

	for (u64 i = 0; i < n; i++) {
		struct file *f = p[i].fd >= 0 ? fd_file(t, p[i].fd) : NULL;

		p[i].revents = 0;
		if (p[i].fd < 0)
			continue;
		if (!f)
			p[i].revents = POLLNVAL;
		else
			p[i].revents = (s16)(file_poll(f, (u32)p[i].events | POLLERR | POLLHUP) &
					     ((u32)p[i].events | POLLERR | POLLHUP | POLLNVAL));
		ready += p[i].revents != 0;
	}
	return ready;
}

static long do_poll(struct abi_ctx *ctx, u64 ufds, u64 n, s64 timeout_ns)
{
	struct pfd *p;
	u64 deadline = deadline_of(timeout_ns);
	long ready;

	if (n > TASK_FD_MAX)
		return -MERR_INVAL;
	p = kzalloc((n ? n : 1) * sizeof(*p));
	if (!p)
		return -MERR_NOMEM;
	if (uaccess_copy_in(TASK, p, ufds, n * sizeof(*p))) {
		kfree(p);
		return -MERR_FAULT;
	}
	for (;;) {
		ready = poll_scan(TASK, p, n);
		if (ready || timeout_ns == 0)
			break;
		if (task_signal_pending()) {
			ready = -MERR_INTR;
			break;
		}
		if (!wait_change(deadline)) {
			ready = poll_scan(TASK, p, n);
			break;
		}
	}
	if (ready >= 0 && uaccess_copy_out(TASK, ufds, p, n * sizeof(*p)))
		ready = -MERR_FAULT;
	kfree(p);
	return ready;
}

LXFN(lx_poll)
{
	s64 ms = (s32)A(2);

	return do_poll(ctx, A(0), A(1), ms < 0 ? -1 : ms * 1000000);
}

LXFN(lx_ppoll)
{
	long err;
	s64 ns = ts_ns(TASK, A(2), false, &err);

	return err ? err : do_poll(ctx, A(0), A(1), ns);
}

static long do_select(struct abi_ctx *ctx, u64 nfds, u64 ur, u64 uw, u64 ue, s64 timeout_ns)
{
	u64 nbytes = (nfds + 7) / 8, deadline = deadline_of(timeout_ns);
	u8 *in[3] = { NULL }, *out[3] = { NULL };
	const u64 uptr[3] = { ur, uw, ue };
	long ready = 0;

	if (nfds > TASK_FD_MAX)
		return -MERR_INVAL;
	for (int k = 0; k < 3; k++) {
		in[k] = kzalloc(nbytes * 2 + 1);
		if (!in[k]) {
			ready = -MERR_NOMEM;
			goto done;
		}
		out[k] = in[k] + nbytes;
		if (uptr[k] && uaccess_copy_in(TASK, in[k], uptr[k], (size_t)nbytes)) {
			ready = -MERR_FAULT;
			goto done;
		}
	}
	for (;;) {
		ready = 0;
		for (int k = 0; k < 3; k++)
			memset(out[k], 0, (size_t)nbytes);
		for (u64 fd = 0; fd < nfds; fd++) {
			u32 want = 0, got;
			struct file *f;

			if (in[0][fd / 8] >> (fd % 8) & 1) want |= POLLIN;
			if (in[1][fd / 8] >> (fd % 8) & 1) want |= POLLOUT;
			if (in[2][fd / 8] >> (fd % 8) & 1) want |= POLLPRI;
			if (!want)
				continue;
			f = fd_file(TASK, (int)fd);
			if (!f) {
				ready = -MERR_BADF;
				goto done;
			}
			got = file_poll(f, want | POLLERR | POLLHUP);
			if ((want & POLLIN) && (got & (POLLIN | POLLHUP | POLLERR))) { out[0][fd / 8] |= (u8)(1u << (fd % 8)); ready++; }
			if ((want & POLLOUT) && (got & (POLLOUT | POLLERR))) { out[1][fd / 8] |= (u8)(1u << (fd % 8)); ready++; }
			if ((want & POLLPRI) && (got & POLLPRI)) { out[2][fd / 8] |= (u8)(1u << (fd % 8)); ready++; }
		}
		if (ready || timeout_ns == 0)
			break;
		if (task_signal_pending()) {
			ready = -MERR_INTR;
			goto done;
		}
		if (!wait_change(deadline))
			break;
	}
	for (int k = 0; k < 3; k++)
		if (uptr[k] && uaccess_copy_out(TASK, uptr[k], out[k], (size_t)nbytes)) {
			ready = -MERR_FAULT;
			break;
		}
done:
	for (int k = 0; k < 3; k++)
		kfree(in[k]);
	return ready;
}

LXFN(lx_select)
{
	long err;
	s64 ns = ts_ns(TASK, A(4), true, &err);

	return err ? err : do_select(ctx, A(0), A(1), A(2), A(3), ns);
}

LXFN(lx_pselect6)
{
	long err;
	s64 ns = ts_ns(TASK, A(4), false, &err);

	return err ? err : do_select(ctx, A(0), A(1), A(2), A(3), ns);
}

/* ---- epoll ---------------------------------------------------------------------------- */

struct epitem {
	struct epitem *next;
	int fd;
	struct file *f;                 /* identity only, not referenced */
	u32 events;
	u64 data;
	u32 last;
};

struct epoll {
	struct epitem *items;
};

static u32 ep_poll(struct file *f, u32 events)
{
	struct epoll *e = f->priv;
	u32 r = 0;

	for (struct epitem *i = e->items; i; i = i->next)
		if (i->events && file_poll(i->f, i->events | POLLERR | POLLHUP))
			r |= POLLIN;
	return r & events;
}

static void ep_release(struct file *f)
{
	struct epoll *e = f->priv;

	while (e->items) {
		struct epitem *n = e->items->next;

		kfree(e->items);
		e->items = n;
	}
	kfree(e);
}

static const struct fops epoll_fops = { .poll = ep_poll, .release = ep_release, .mode = S_IFREG | 0600 };

LXFN(lx_epoll_create)
{
	struct epoll *e = kzalloc(sizeof(*e));
	struct file *f;

	if (!e)
		return -MERR_NOMEM;
	f = file_new_ops(&epoll_fops, e, VFS_O_RDWR);
	if (!f) {
		kfree(e);
		return -MERR_NOMEM;
	}
	return fd_install(TASK, f, 0, c->nr == 291 && (A(0) & 0x80000));
}

LXFN(lx_epoll_ctl)
{
	struct file *ef = fd_file(TASK, (int)A(0)), *tf = fd_file(TASK, (int)A(2));
	struct epoll *e;
	struct epitem **pp, *it;
	u8 ev[12] = { 0 };
	u32 events;

	if (!ef || !tf)
		return -MERR_BADF;
	if (ef->type != F_OPS || ef->ops != &epoll_fops || ef == tf)
		return -MERR_INVAL;
	e = ef->priv;
	for (pp = &e->items; *pp && (*pp)->fd != (int)A(2); pp = &(*pp)->next)
		;
	it = *pp;
	if (A(1) == 2) {                        /* DEL */
		if (!it)
			return -MERR_NOENT;
		*pp = it->next;
		kfree(it);
		return 0;
	}
	if (uaccess_copy_in(TASK, ev, A(3), sizeof(ev)))
		return -MERR_FAULT;
	memcpy(&events, ev, 4);
	if (A(1) == 1) {                        /* ADD */
		if (it)
			return -MERR_EXIST;
		it = kzalloc(sizeof(*it));
		if (!it)
			return -MERR_NOMEM;
		it->next = e->items;
		e->items = it;
		it->fd = (int)A(2);
	} else if (!it) {                       /* MOD */
		return -MERR_NOENT;
	}
	it->f = tf;
	it->events = events;
	memcpy(&it->data, ev + 4, 8);
	it->last = 0;
	poll_wake();
	return 0;
}

static long ep_scan(struct task *t, struct epoll *e, u8 *out, long max)
{
	long n = 0;
	struct epitem **pp = &e->items;

	while (*pp && n < max) {
		struct epitem *i = *pp;
		u32 r, mask;

		if (fd_file(t, i->fd) != i->f) {        /* closed (or reused): gone from the set */
			*pp = i->next;
			kfree(i);
			continue;
		}
		pp = &i->next;
		if (!(i->events & 0xffffu))
			continue;
		mask = (i->events & 0x2fffu) | POLLERR | POLLHUP;
		r = file_poll(i->f, mask) & mask;
		if (i->events & EPOLLET) {
			u32 fresh = r & ~i->last;

			i->last = r;
			r = fresh;
		}
		if (!r)
			continue;
		memcpy(out + 12 * n, &r, 4);
		memcpy(out + 12 * n + 4, &i->data, 8);
		n++;
		if (i->events & EPOLLONESHOT)
			i->events &= ~0xffffu;
	}
	return n;
}

LXFN(lx_epoll_wait)
{
	struct file *ef = fd_file(TASK, (int)A(0));
	long max = (long)(s32)A(2), n;
	s64 ms = (s32)A(3);
	u64 deadline = deadline_of(ms < 0 ? -1 : ms * 1000000);
	u8 *out;

	if (!ef)
		return -MERR_BADF;
	if (ef->type != F_OPS || ef->ops != &epoll_fops || max <= 0 || max > 1024)
		return -MERR_INVAL;
	out = kzalloc((size_t)max * 12);
	if (!out)
		return -MERR_NOMEM;
	for (;;) {
		n = ep_scan(TASK, ef->priv, out, max);
		if (n || ms == 0)
			break;
		if (task_signal_pending()) {
			n = -MERR_INTR;
			break;
		}
		if (!wait_change(deadline))
			break;
	}
	if (n > 0 && uaccess_copy_out(TASK, A(1), out, (size_t)n * 12))
		n = -MERR_FAULT;
	kfree(out);
	return n;
}

/* ---- eventfd --------------------------------------------------------------------------- */

struct evfd {
	u64 count;
	bool semaphore;
};

static long ev_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	struct evfd *e = f->priv;
	u64 v;

	if (len < 8)
		return -MERR_INVAL;
	while (!e->count) {
		if (f->flags & VFS_O_NONBLOCK)
			return -MERR_AGAIN;
		if (task_signal_pending())
			return -MERR_INTR;
		thread_block(e, 0, 0);
	}
	v = e->semaphore ? 1 : e->count;
	e->count -= v;
	if (uaccess_copy_out(t, ubuf, &v, 8))
		return -MERR_FAULT;
	poll_wake();
	return 8;
}

static long ev_write(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	struct evfd *e = f->priv;
	u64 v;

	if (len < 8)
		return -MERR_INVAL;
	if (uaccess_copy_in(t, &v, ubuf, 8))
		return -MERR_FAULT;
	if (v == ~0ULL)
		return -MERR_INVAL;
	e->count += v;
	thread_wake(e, 0, SCHED_MAX_THREADS);
	poll_wake();
	return 8;
}

static u32 ev_poll(struct file *f, u32 events)
{
	struct evfd *e = f->priv;

	return ((e->count ? POLLIN : 0) | POLLOUT) & events;
}

static void ev_release(struct file *f)
{
	kfree(f->priv);
}

static const struct fops evfd_fops = {
	.read = ev_read, .write = ev_write, .poll = ev_poll, .release = ev_release, .mode = S_IFREG | 0600,
};

LXFN(lx_eventfd)
{
	struct evfd *e = kzalloc(sizeof(*e));
	u32 flags = c->nr == 290 ? (u32)A(1) : 0;
	struct file *f;

	if (!e)
		return -MERR_NOMEM;
	e->count = (u32)A(0);
	e->semaphore = flags & 1;
	f = file_new_ops(&evfd_fops, e, VFS_O_RDWR | ((flags & 0x800) ? VFS_O_NONBLOCK : 0));
	if (!f) {
		kfree(e);
		return -MERR_NOMEM;
	}
	return fd_install(TASK, f, 0, flags & 0x80000);
}
