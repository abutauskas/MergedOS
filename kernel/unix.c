/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * AF_UNIX: stream and datagram sockets, socketpair, bind/listen/accept/
 * connect on ramfs paths, descriptor passing (SCM_RIGHTS). Every endpoint
 * owns a queue of chunks (one per send); a stream receive may span chunks,
 * a datagram receive takes exactly one. Descriptors travel with the chunk
 * they were sent with and are delivered with its first byte.
 */
#include <mergedos/kernel.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>
#include <mergedos/socket.h>

#define AF_UNIX 1
#define UNIX_QUEUE_MAX (208u * 1024)
#define UNIX_PATH_MAX 108

struct chunk {
	struct chunk *next;
	u8 *data;
	u32 len, off;
	struct file *fds[SOCK_MAX_FDS];
	int nfds;
	char from[UNIX_PATH_MAX];
};

struct usock {
	struct socket *sock;            /* NULL while queued for accept() */
	int type;
	struct usock *peer;
	struct chunk *head, *tail;
	u32 queued;
	bool listening, hup, rd_shut, wr_shut, connected;
	struct usock *acceptq, *anext;  /* pending connections (listeners) / link */
	char path[UNIX_PATH_MAX];       /* bound name, "" if none */
	struct rnode *node;
};

static void wake(struct usock *u)
{
	thread_wake(u, 0, SCHED_MAX_THREADS);
	poll_wake();
}

static struct usock *usock_new(int type)
{
	struct usock *u = kzalloc(sizeof(*u));

	if (u)
		u->type = type;
	return u;
}

static long u_create(struct socket *s, int proto)
{
	if (proto != 0 || (s->type != SOCK_STREAM && s->type != SOCK_DGRAM && s->type != SOCK_SEQPACKET))
		return proto ? -MERR_PROTONOSUPPORT : -MERR_PROTOTYPE;
	s->priv = usock_new(s->type);
	if (!s->priv)
		return -MERR_NOMEM;
	((struct usock *)s->priv)->sock = s;
	return 0;
}

static void chunk_free(struct chunk *c)
{
	for (int i = 0; i < c->nfds; i++)
		file_put(c->fds[i]);
	kfree(c->data);
	kfree(c);
}

static void usock_free(struct usock *u)
{
	for (struct chunk *c = u->head, *n; c; c = n) {
		n = c->next;
		chunk_free(c);
	}
	kfree(u);
}

static void u_release(struct socket *s)
{
	struct usock *u = s->priv;

	if (u->peer) {
		u->peer->peer = NULL;
		u->peer->hup = true;
		wake(u->peer);
	}
	for (struct usock *p = u->acceptq, *n; p; p = n) {      /* connections nobody accepted */
		n = p->anext;
		if (p->peer) {
			p->peer->peer = NULL;
			p->peer->hup = true;
			wake(p->peer);
		}
		usock_free(p);
	}
	if (u->node) {
		u->node->sock = NULL;
		ramfs_close(u->node);
	}
	usock_free(u);
}

long unix_socketpair(struct socket *a, struct socket *b)
{
	struct usock *x = usock_new(a->type), *y = usock_new(b->type);

	if (!x || !y) {
		kfree(x);
		kfree(y);
		return -MERR_NOMEM;
	}
	x->peer = y;
	y->peer = x;
	x->sock = a;
	y->sock = b;
	x->connected = y->connected = true;
	a->priv = x;
	b->priv = y;
	return 0;
}

/* sockaddr_un -> path (NUL-terminated); abstract names are not supported. */
static long addr_path(const u8 *addr, u32 len, char *path)
{
	u16 fam;

	if (len < 3 || len > 2 + UNIX_PATH_MAX)
		return -MERR_INVAL;
	memcpy(&fam, addr, 2);
	if (fam != AF_UNIX)
		return -MERR_AFNOSUPPORT;
	if (!addr[2])
		return -MERR_NOTSUP;
	memset(path, 0, UNIX_PATH_MAX);
	memcpy(path, addr + 2, len - 2);
	path[UNIX_PATH_MAX - 1] = '\0';
	return 0;
}

static long u_bind(struct task *t, struct socket *s, const u8 *addr, u32 len)
{
	struct usock *u = s->priv;
	char path[UNIX_PATH_MAX];
	struct rnode *n;
	long rc = addr_path(addr, len, path);

	if (rc)
		return rc;
	if (u->path[0])
		return -MERR_INVAL;
	rc = vfs_sock_path(t, path, true, &n);
	if (rc)
		return rc;
	n->sock = u;
	ramfs_open(n);
	u->node = n;
	memcpy(u->path, path, sizeof(path));
	return 0;
}

static long u_listen(struct socket *s, int backlog)
{
	struct usock *u = s->priv;

	(void)backlog;
	if (s->type == SOCK_DGRAM)
		return -MERR_NOTSUP;
	if (!u->path[0])
		return -MERR_INVAL;
	u->listening = true;
	return 0;
}

static struct usock *lookup(struct task *t, const char *path, long *err)
{
	struct rnode *n;
	long rc = vfs_sock_path(t, path, false, &n);

	if (rc || !n->sock) {
		*err = rc ? rc : -MERR_CONNREFUSED;
		return NULL;
	}
	return n->sock;
}

static long u_connect(struct task *t, struct socket *s, const u8 *addr, u32 len, bool nonblock)
{
	struct usock *u = s->priv, *l, *srv;
	char path[UNIX_PATH_MAX];
	long rc = addr_path(addr, len, path), err = 0;

	(void)nonblock;
	if (rc)
		return rc;
	if (u->connected && s->type != SOCK_DGRAM)
		return -MERR_ISCONN;
	l = lookup(t, path, &err);
	if (!l)
		return err;
	if (l->type != u->type)
		return -MERR_PROTOTYPE;
	if (s->type == SOCK_DGRAM) {
		u->peer = l;                    /* one-way: datagrams go to l's queue */
		u->connected = true;
		return 0;
	}
	if (!l->listening)
		return -MERR_CONNREFUSED;
	srv = usock_new(u->type);
	if (!srv)
		return -MERR_NOMEM;
	srv->peer = u;
	u->peer = srv;
	srv->connected = u->connected = true;
	memcpy(srv->path, l->path, sizeof(srv->path));
	srv->anext = NULL;
	{
		struct usock **pp = &l->acceptq;

		while (*pp)
			pp = &(*pp)->anext;
		*pp = srv;
	}
	wake(l);
	return 0;
}

static bool signalled(void)
{
	return task_signal_pending();
}

static long u_accept(struct task *t, struct socket *s, struct socket *news, bool nonblock)
{
	struct usock *l = s->priv, *c;

	(void)t;
	if (!l->listening)
		return -MERR_INVAL;
	while (!l->acceptq) {
		if (nonblock)
			return -MERR_AGAIN;
		if (signalled())
			return -MERR_INTR;
		thread_block(l, 0, 0);
	}
	c = l->acceptq;
	l->acceptq = c->anext;
	c->anext = NULL;
	c->sock = news;
	news->priv = c;
	return 0;
}

static long u_send(struct task *t, struct socket *s, const u8 *data, size_t len, struct file **fds,
		   int nfds, u32 flags, const u8 *to, u32 tolen)
{
	struct usock *u = s->priv, *dst = u->peer;
	struct chunk *c;
	char path[UNIX_PATH_MAX];
	long err = 0;

	if (to && tolen) {
		long rc = addr_path(to, tolen, path);

		if (rc)
			return rc;
		dst = lookup(t, path, &err);
		if (!dst)
			return err;
	}
	if (!dst) {
		if (u->hup && s->type != SOCK_DGRAM) {
			if (!(flags & MSG_NOSIGNAL))
				task_signal(t, 13);
			return -MERR_PIPE;
		}
		return s->type == SOCK_DGRAM ? -MERR_DESTADDRREQ : -MERR_NOTCONN;
	}
	if (u->wr_shut) {
		if (!(flags & MSG_NOSIGNAL))
			task_signal(t, 13);
		return -MERR_PIPE;
	}
	if (s->type != SOCK_STREAM && len > UNIX_QUEUE_MAX)
		return -MERR_MSGSIZE;
	while (dst->queued && dst->queued + len > UNIX_QUEUE_MAX) {
		if (flags & MSG_DONTWAIT)
			return -MERR_AGAIN;
		if (signalled())
			return -MERR_INTR;
		thread_block(dst, 1, 0);
		if (u->peer != dst && !(to && tolen))
			return -MERR_PIPE;
	}
	c = kzalloc(sizeof(*c));
	if (!c)
		return -MERR_NOMEM;
	c->data = kzalloc(len ? len : 1);
	if (!c->data) {
		kfree(c);
		return -MERR_NOMEM;
	}
	memcpy(c->data, data, len);
	c->len = (u32)len;
	memcpy(c->from, u->path, sizeof(c->from));
	for (int i = 0; i < nfds; i++)
		c->fds[c->nfds++] = file_get(fds[i]);
	if (dst->tail)
		dst->tail->next = c;
	else
		dst->head = c;
	dst->tail = c;
	dst->queued += c->len;
	wake(dst);
	return (long)len;
}

static long u_recv(struct task *t, struct socket *s, u8 *buf, size_t cap, struct file **fds, int *nfds,
		   u32 flags, u8 *from, u32 *fromlen, u32 *outflags)
{
	struct usock *u = s->priv;
	size_t got = 0;

	(void)t;
	*nfds = 0;
	if (fromlen)
		*fromlen = 0;
	for (;;) {
		while (u->head) {
			struct chunk *c = u->head;
			size_t n = MOS_MIN((size_t)(c->len - c->off), cap - got);

			if (got && c->nfds)
				return (long)got;       /* descriptors are a boundary */
			memcpy(buf + got, c->data + c->off, n);
			if (!got && fromlen && from && c->from[0]) {
				u16 fam = AF_UNIX;

				memcpy(from, &fam, 2);
				memcpy(from + 2, c->from, UNIX_PATH_MAX);
				*fromlen = 2 + (u32)strlen(c->from) + 1;
			}
			if (c->nfds && !(flags & MSG_PEEK)) {
				for (int i = 0; i < c->nfds; i++)
					fds[(*nfds)++] = c->fds[i];
				c->nfds = 0;
			}
			got += n;
			if (flags & MSG_PEEK)
				return (long)got;
			c->off += (u32)n;
			u->queued -= (u32)n;
			if (s->type != SOCK_STREAM) {
				if (c->off < c->len && outflags)
					*outflags |= MSG_TRUNC;
				u->queued -= c->len - c->off;
				c->off = c->len;
			}
			if (c->off >= c->len) {
				u->head = c->next;
				if (!u->head)
					u->tail = NULL;
				chunk_free(c);
			}
			thread_wake(u, 1, SCHED_MAX_THREADS);   /* a blocked sender may go on */
			poll_wake();
			if (s->type != SOCK_STREAM || got >= cap)
				return (long)got;
		}
		if (got)
			return (long)got;
		if (u->hup || u->rd_shut || (!u->peer && s->type == SOCK_STREAM && u->connected))
			return 0;
		if (flags & MSG_DONTWAIT)
			return -MERR_AGAIN;
		if (signalled())
			return -MERR_INTR;
		thread_block(u, 0, 0);
	}
}

static u32 u_poll(struct socket *s, u32 events)
{
	struct usock *u = s->priv;
	u32 r = 0;

	if (u->head || u->acceptq)
		r |= POLLIN;
	if (u->hup || u->rd_shut)
		r |= POLLIN | POLLRDHUP | (u->hup ? POLLHUP : 0);
	if (!u->listening && (!u->peer || u->peer->queued < UNIX_QUEUE_MAX))
		r |= POLLOUT;
	return r & (events | POLLERR | POLLHUP);
}

static long u_shutdown(struct socket *s, int how)
{
	struct usock *u = s->priv;

	if (!u->connected)
		return -MERR_NOTCONN;
	if (how == 0 || how == 2)
		u->rd_shut = true;
	if (how == 1 || how == 2) {
		u->wr_shut = true;
		if (u->peer) {
			u->peer->hup = true;    /* the peer reads end of file */
			wake(u->peer);
		}
	}
	wake(u);
	return 0;
}

static long u_getname(struct socket *s, bool peer, u8 *addr, u32 *len)
{
	struct usock *u = peer ? ((struct usock *)s->priv)->peer : s->priv;
	u16 fam = AF_UNIX;

	if (!u)
		return peer ? -MERR_NOTCONN : -MERR_INVAL;
	memset(addr, 0, 2 + UNIX_PATH_MAX);
	memcpy(addr, &fam, 2);
	memcpy(addr + 2, u->path, UNIX_PATH_MAX);
	*len = u->path[0] ? 2 + (u32)strlen(u->path) + 1 : 2;
	return 0;
}

static const struct sock_ops unix_ops = {
	.family = AF_UNIX, .create = u_create, .release = u_release, .bind = u_bind, .listen = u_listen,
	.accept = u_accept, .connect = u_connect, .send = u_send, .recv = u_recv, .poll = u_poll,
	.shutdown = u_shutdown, .getname = u_getname,
};

void unix_init(void)
{
	sock_register(&unix_ops);
}
