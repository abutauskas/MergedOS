/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * AF_INET for the generic socket layer (kernel/socket.c): the family's
 * sock_ops table over the stack core. Blocking is done here: an operation
 * that cannot finish sleeps on the socket until the stack calls ns_wake(), a
 * signal arrives, or it can proceed. Every method runs with interrupts off,
 * which is the stack's lock on one CPU. Addresses are struct sockaddr_in
 * (16 bytes, network byte order); option numbers are the Linux ones.
 */
#include <mergedos/compat.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>
#include <mergedos/socket.h>
#include "kernel/net/netdrv.h"

#define AF_INET_     2
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_BROADCAST 6
#define SO_SNDBUF    7
#define SO_RCVBUF    8
#define SO_KEEPALIVE 9
#define SO_ACCEPTCONN 30
#define SO_PROTOCOL  38
#define IPPROTO_IP_  0
#define IPPROTO_TCP_ 6
#define TCP_NODELAY_ 1
#define TCP_MAXSEG_  2

static inline struct nsock *NS(struct socket *s)
{
	return s->priv;
}

/* Sleep until the socket is woken. Returns -MERR_INTR if a signal is pending. */
static long park(struct nsock *s)
{
	if (task_signal_pending())
		return -MERR_INTR;
	thread_block(s, 0, SCHED_HZ);           /* the timeout is only a safety net */
	return 0;
}

static long parse_addr(const u8 *a, u32 len, u32 *ip, u16 *port)
{
	if (len < 16)
		return -MERR_INVAL;
	if (a[0] != AF_INET_ || a[1] != 0)
		return -MERR_AFNOSUPPORT;
	*port = net_get16(a + 2);
	*ip = net_get32(a + 4);
	return 0;
}

static void fill_addr(u8 *out, u32 *len, u32 ip, u16 port)
{
	memset(out, 0, 16);
	out[0] = AF_INET_;
	net_put16(out + 2, port);
	net_put32(out + 4, ip);
	*len = 16;
}

static long i_create(struct socket *s, int proto)
{
	struct nsock *n;
	u64 f;

	if (s->type != SOCK_STREAM && s->type != SOCK_DGRAM)
		return -MERR_PROTOTYPE;
	if (proto && proto != (s->type == SOCK_STREAM ? 6 : 17))
		return -MERR_PROTONOSUPPORT;
	net_start_poller();
	f = irq_save();
	net_enter();
	n = ns_sock_new(&net_stack, s->type == SOCK_STREAM ? NS_STREAM : NS_DGRAM);
	irq_restore(f);
	if (!n)
		return -MERR_NOBUFS;
	n->user = s;
	s->priv = n;
	return 0;
}

static void i_release(struct socket *s)
{
	u64 f;

	if (!s->priv)
		return;
	f = irq_save();
	net_enter();
	NS(s)->user = NULL;
	ns_sock_close(NS(s));
	irq_restore(f);
	s->priv = NULL;
}

static long i_bind(struct task *t, struct socket *s, const u8 *addr, u32 len)
{
	u32 ip;
	u16 port;
	long rc = parse_addr(addr, len, &ip, &port);
	u64 f;

	(void)t;
	if (rc)
		return rc;
	f = irq_save();
	net_enter();
	rc = ns_bind(NS(s), ip, port);
	irq_restore(f);
	return rc;
}

static long i_listen(struct socket *s, int backlog)
{
	u64 f = irq_save();
	long rc;

	net_enter();
	rc = ns_listen(NS(s), backlog);
	irq_restore(f);
	return rc;
}

static long i_connect(struct task *t, struct socket *so, const u8 *addr, u32 len, bool nonblock)
{
	struct nsock *s = NS(so);
	u32 ip;
	u16 port;
	long rc = parse_addr(addr, len, &ip, &port);
	u64 f;

	(void)t;
	if (rc)
		return rc;
	f = irq_save();
	net_enter();
	if (s->type == NS_STREAM && s->state == TS_CLOSED && s->err) {
		rc = -ns_take_error(s);                 /* report the failed earlier attempt once */
		goto out;
	}
	rc = ns_connect(s, ip, port);
	if (rc != -MERR_INPROGRESS || nonblock)
		goto out;
	for (;;) {
		if (ns_poll(s) & (NS_POLLOUT | NS_POLLERR | NS_POLLHUP))
			break;
		rc = park(s);
		if (rc)
			goto out;
	}
	rc = s->err ? -ns_take_error(s) : (s->state == TS_ESTAB ? 0 : -MERR_CONNREFUSED);
out:
	irq_restore(f);
	return rc;
}

static long i_accept(struct task *t, struct socket *so, struct socket *news, bool nonblock)
{
	struct nsock *s = NS(so), *c;
	u64 f = irq_save();
	long rc = 0;

	(void)t;
	net_enter();
	if (s->type != NS_STREAM || s->state != TS_LISTEN) {
		rc = -MERR_INVAL;
		goto out;
	}
	while (!(c = ns_accept(s))) {
		if (nonblock) {
			rc = -MERR_AGAIN;
			goto out;
		}
		rc = park(s);
		if (rc)
			goto out;
	}
	c->user = news;
	news->priv = c;
out:
	irq_restore(f);
	return rc;
}

static long i_send(struct task *t, struct socket *so, const u8 *data, size_t len, struct file **fds,
		   int nfds, u32 flags, const u8 *to, u32 tolen)
{
	struct nsock *s = NS(so);
	bool nonblock = flags & MSG_DONTWAIT;
	u32 ip = 0;
	u16 port = 0;
	size_t done = 0;
	long rc = 0;
	u64 f;

	(void)fds;
	(void)nfds;
	if (to && (rc = parse_addr(to, tolen, &ip, &port)))
		return rc;
	f = irq_save();
	net_enter();
	do {
		if (s->type == NS_DGRAM)
			rc = to ? ns_sendto(s, data, (u32)len, ip, port) : ns_send(s, data, (u32)len);
		else
			rc = ns_send(s, data + done, (u32)MOS_MIN(len - done, (size_t)NET_BUF_MAX));
		if (rc >= 0) {
			done += (size_t)rc;
			if (s->type == NS_DGRAM)
				break;
			continue;
		}
		if (rc != -MERR_AGAIN || nonblock)
			break;
		rc = park(s);
		if (rc)
			break;
	} while (done < len);
	irq_restore(f);
	if (done)
		return (long)done;
	if (rc == -MERR_PIPE && !(flags & MSG_NOSIGNAL))
		task_signal(t, 13);                     /* SIGPIPE */
	return rc;
}

static long i_recv(struct task *t, struct socket *so, u8 *buf, size_t cap, struct file **fds,
		   int *nfds, u32 flags, u8 *from, u32 *fromlen, u32 *outflags)
{
	struct nsock *s = NS(so);
	bool nonblock = flags & MSG_DONTWAIT;
	u32 ip = 0;
	u16 port = 0;
	long rc;
	u64 f = irq_save();

	(void)t;
	if (nfds)
		*nfds = 0;
	if (outflags)
		*outflags = 0;
	net_enter();
	for (;;) {
		rc = ns_recvfrom(s, buf, (u32)MOS_MIN(cap, (size_t)0x7FFFFFFF), flags & MSG_PEEK, &ip, &port);
		ns_lo_drain(s->ns);
		if (rc != -MERR_AGAIN || nonblock)
			break;
		rc = park(s);
		if (rc)
			break;
	}
	irq_restore(f);
	(void)fds;
	if (rc >= 0 && from && fromlen)
		fill_addr(from, fromlen, ip, port);
	return rc;
}

static long i_shutdown(struct socket *s, int how)
{
	u64 f = irq_save();
	long rc;

	net_enter();
	rc = how < 0 || how > 2 ? -MERR_INVAL : ns_shutdown(NS(s), how);
	irq_restore(f);
	return rc;
}

static u32 i_poll(struct socket *s, u32 events)
{
	u64 f = irq_save();
	u32 m = ns_poll(NS(s));

	irq_restore(f);
	return m & (events | NS_POLLERR | NS_POLLHUP);
}

static long i_getname(struct socket *so, bool peer, u8 *addr, u32 *len)
{
	struct nsock *s = NS(so);

	if (peer && (s->type == NS_STREAM
		     ? (s->state == TS_CLOSED || s->state == TS_LISTEN || s->state == TS_SYN_SENT)
		     : !s->connected))
		return -MERR_NOTCONN;
	fill_addr(addr, len, peer ? s->rip : s->lip, peer ? s->rport : s->lport);
	return 0;
}

static long put_int(u8 *val, u32 *len, int v)
{
	u32 n = MOS_MIN(*len, 4u);

	memcpy(val, &v, n);
	*len = n;
	return 0;
}

static long i_getsockopt(struct socket *so, int level, int opt, u8 *val, u32 *len)
{
	struct nsock *s = NS(so);
	u64 f = irq_save();
	long rc;

	if (level == SOL_SOCKET) {
		switch (opt) {
		case SO_ERROR:     rc = put_int(val, len, ns_take_error(s)); break;
		case SO_TYPE:      rc = put_int(val, len, so->type); break;
		case SO_REUSEADDR: rc = put_int(val, len, s->reuseaddr); break;
		case SO_BROADCAST: rc = put_int(val, len, s->bcast); break;
		case SO_KEEPALIVE: rc = put_int(val, len, s->keepalive); break;
		case SO_SNDBUF:    rc = put_int(val, len, (int)s->scap * 2); break;
		case SO_RCVBUF:    rc = put_int(val, len, (int)s->rcap * 2); break;
		case SO_ACCEPTCONN: rc = put_int(val, len, s->state == TS_LISTEN); break;
		case SO_PROTOCOL:  rc = put_int(val, len, s->type == NS_STREAM ? 6 : 17); break;
		default:           rc = put_int(val, len, 0); break;    /* linger, timeouts, ... */
		}
	} else if (level == IPPROTO_TCP_ && s->type == NS_STREAM) {
		rc = put_int(val, len, opt == TCP_NODELAY_ ? s->nodelay : opt == TCP_MAXSEG_ ? (int)s->mss : 0);
	} else if (level == IPPROTO_IP_) {
		rc = put_int(val, len, opt == 2 ? 64 : 0);              /* IP_TTL, IP_TOS, ... */
	} else {
		rc = -MERR_NOPROTOOPT;
	}
	irq_restore(f);
	return rc;
}

static long i_setsockopt(struct socket *so, int level, int opt, const u8 *val, u32 len)
{
	struct nsock *s = NS(so);
	int v = 0;

	if (len >= 4)
		memcpy(&v, val, 4);
	else if (len)
		v = val[0];
	if (level == SOL_SOCKET) {
		switch (opt) {
		case SO_REUSEADDR: s->reuseaddr = v != 0; break;
		case SO_BROADCAST: s->bcast = v != 0; break;
		case SO_KEEPALIVE: s->keepalive = v != 0; break;     /* accepted; no probes are sent */
		case SO_SNDBUF:    ns_set_buf(s, true, (u32)v / 2); break;
		case SO_RCVBUF:    ns_set_buf(s, false, (u32)v / 2); break;
		default: break;                                       /* linger, timeouts, ...: accepted */
		}
		return 0;
	}
	if (level == IPPROTO_TCP_ || level == IPPROTO_IP_) {
		if (level == IPPROTO_TCP_ && opt == TCP_NODELAY_)
			s->nodelay = v != 0;
		return 0;
	}
	return -MERR_NOPROTOOPT;
}

static long i_avail(struct socket *s)
{
	return ns_rx_avail(NS(s));
}

static const struct sock_ops inet_ops = {
	.family = AF_INET_, .create = i_create, .release = i_release, .bind = i_bind,
	.listen = i_listen, .accept = i_accept, .connect = i_connect, .send = i_send,
	.recv = i_recv, .poll = i_poll, .shutdown = i_shutdown, .getname = i_getname,
	.setsockopt = i_setsockopt, .getsockopt = i_getsockopt, .avail = i_avail,
};

void inet_init(void)
{
	sock_register(&inet_ops);
}
