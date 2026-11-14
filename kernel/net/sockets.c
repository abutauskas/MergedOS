/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * inet_socket_ops: AF_INET sockets on top of the stack core. Blocking is done
 * here: an operation that cannot finish sleeps on the socket until the stack
 * calls ns_wake() (netd, or any other thread's system call), a signal
 * arrives, or it can proceed. Every method runs with interrupts off, which
 * is the stack's lock on one CPU.
 */
#include <mergedos/compat.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include "kernel/net/netdrv.h"
#include "kernel/net/inet.h"

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
#define FIONREAD_    0x541B

static inline struct nsock *S(void *p)
{
	return p;
}

/* Sleep until the socket is woken. Returns -MERR_INTR if a signal is pending. */
static long park(struct nsock *s)
{
	if (task_signal_pending())
		return -MERR_INTR;
	/* The 1 s timeout is only a safety net against a lost wake-up. */
	thread_block(s, 0, SCHED_HZ);
	return 0;
}

static long sk_create(int type, int protocol, void **priv)
{
	struct nsock *s;
	u64 f;

	if (type != INET_SOCK_STREAM && type != INET_SOCK_DGRAM)
		return -MERR_PROTONOSUPPORT;
	if (protocol && protocol != (type == INET_SOCK_STREAM ? 6 : 17))
		return -MERR_PROTONOSUPPORT;
	net_start_poller();
	f = irq_save();
	net_enter();
	s = ns_sock_new(&net_stack, type == INET_SOCK_STREAM ? NS_STREAM : NS_DGRAM);
	irq_restore(f);
	if (!s)
		return -MERR_NOBUFS;
	*priv = s;
	return 0;
}

static long sk_bind(void *p, u32 ip, u16 port)
{
	u64 f = irq_save();
	long rc;

	net_enter();
	rc = ns_bind(S(p), ip, port);
	irq_restore(f);
	return rc;
}

static long sk_listen(void *p, int backlog)
{
	u64 f = irq_save();
	long rc;

	net_enter();
	rc = ns_listen(S(p), backlog);
	irq_restore(f);
	return rc;
}

static long sk_connect(void *p, u32 ip, u16 port, bool nonblock)
{
	struct nsock *s = S(p);
	u64 f = irq_save();
	long rc;

	net_enter();
	if (s->type == NS_STREAM && s->state == TS_CLOSED && s->err) {
		rc = -ns_take_error(s);                 /* report the failed earlier attempt once */
		goto out;
	}
	rc = ns_connect(s, ip, port);
	if (rc != -MERR_INPROGRESS || nonblock)
		goto out;
	for (;;) {
		u32 m = ns_poll(s);

		if (m & (NS_POLLOUT | NS_POLLERR | NS_POLLHUP))
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

static long sk_accept(void *p, bool nonblock, void **newpriv, u32 *ip, u16 *port)
{
	struct nsock *s = S(p), *c;
	u64 f = irq_save();
	long rc = 0;

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
	*newpriv = c;
	*ip = c->rip;
	*port = c->rport;
out:
	irq_restore(f);
	return rc;
}

static long sk_send(void *p, const void *buf, u32 len, u32 flags, bool nonblock,
		    const u32 *dip, u16 dport)
{
	struct nsock *s = S(p);
	u64 f = irq_save();
	long rc;

	net_enter();
	nonblock |= flags & INET_MSG_DONTWAIT;
	for (;;) {
		if (s->type == NS_DGRAM)
			rc = dip ? ns_sendto(s, buf, len, *dip, dport) : ns_send(s, buf, len);
		else
			rc = ns_send(s, buf, len);
		if (rc != -MERR_AGAIN || nonblock)
			break;
		rc = park(s);
		if (rc)
			break;
	}
	irq_restore(f);
	return rc;
}

static long sk_recv(void *p, void *buf, u32 len, u32 flags, bool nonblock, u32 *sip, u16 *sport)
{
	struct nsock *s = S(p);
	u64 f = irq_save();
	long rc;

	net_enter();
	nonblock |= flags & INET_MSG_DONTWAIT;
	for (;;) {
		rc = ns_recvfrom(s, buf, len, flags & INET_MSG_PEEK, sip, sport);
		ns_lo_drain(s->ns);
		if (rc != -MERR_AGAIN || nonblock)
			break;
		rc = park(s);
		if (rc)
			break;
	}
	irq_restore(f);
	return rc;
}

static long sk_shutdown(void *p, int how)
{
	u64 f = irq_save();
	long rc;

	net_enter();
	rc = how < 0 || how > 2 ? -MERR_INVAL : ns_shutdown(S(p), how);
	irq_restore(f);
	return rc;
}

static long put_int(void *val, u32 *len, int v)
{
	u32 n = MOS_MIN(*len, 4u);

	memcpy(val, &v, n);
	*len = n;
	return 0;
}

static long sk_getsockopt(void *p, int level, int opt, void *val, u32 *len)
{
	struct nsock *s = S(p);
	u64 f = irq_save();
	long rc = 0;

	if (*len < 1) {
		irq_restore(f);
		return -MERR_INVAL;
	}
	if (level == SOL_SOCKET) {
		switch (opt) {
		case SO_ERROR:     rc = put_int(val, len, ns_take_error(s)); break;
		case SO_TYPE:      rc = put_int(val, len, s->type == NS_STREAM ? 1 : 2); break;
		case SO_REUSEADDR: rc = put_int(val, len, s->reuseaddr); break;
		case SO_BROADCAST: rc = put_int(val, len, s->bcast); break;
		case SO_KEEPALIVE: rc = put_int(val, len, s->keepalive); break;
		case SO_SNDBUF:    rc = put_int(val, len, (int)s->scap); break;
		case SO_RCVBUF:    rc = put_int(val, len, (int)s->rcap); break;
		case SO_ACCEPTCONN: rc = put_int(val, len, s->state == TS_LISTEN); break;
		case SO_PROTOCOL:  rc = put_int(val, len, s->type == NS_STREAM ? 6 : 17); break;
		default:           rc = put_int(val, len, 0); break;    /* linger, timeouts, ... */
		}
	} else if (level == IPPROTO_TCP_ && s->type == NS_STREAM) {
		switch (opt) {
		case TCP_NODELAY_: rc = put_int(val, len, s->nodelay); break;
		case TCP_MAXSEG_:  rc = put_int(val, len, (int)s->mss); break;
		default:           rc = put_int(val, len, 0); break;
		}
	} else if (level == IPPROTO_IP_) {
		rc = put_int(val, len, opt == 2 ? 64 : 0);              /* IP_TTL, IP_TOS, ... */
	} else {
		rc = -MERR_NOPROTOOPT;
	}
	irq_restore(f);
	return rc;
}

static long sk_setsockopt(void *p, int level, int opt, const void *val, u32 len)
{
	struct nsock *s = S(p);
	int v = 0;

	if (len >= 4)
		memcpy(&v, val, 4);
	else if (len)
		v = *(const u8 *)val;
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
		if (level == IPPROTO_TCP_ && opt == TCP_NODELAY_) {
			u64 f = irq_save();

			s->nodelay = v != 0;
			irq_restore(f);
		}
		return 0;
	}
	return -MERR_NOPROTOOPT;
}

static u32 sk_poll(void *p)
{
	return ns_poll(S(p));
}

static long sk_getname(void *p, bool peer, u32 *ip, u16 *port)
{
	struct nsock *s = S(p);

	if (peer) {
		if (s->type == NS_STREAM ? (s->state == TS_CLOSED || s->state == TS_LISTEN ||
					    s->state == TS_SYN_SENT)
					 : !s->connected)
			return -MERR_NOTCONN;
		*ip = s->rip;
		*port = s->rport;
	} else {
		*ip = s->lip;
		*port = s->lport;
	}
	return 0;
}

static long sk_ioctl(void *p, u32 cmd, u32 *val)
{
	if (cmd != FIONREAD_)
		return -MERR_NOTTY;
	*val = ns_rx_avail(S(p));
	return 0;
}

static void sk_release(void *p)
{
	u64 f = irq_save();

	net_enter();
	ns_sock_close(S(p));
	irq_restore(f);
}

const struct inet_socket_ops inet_socket_ops = {
	.create = sk_create, .bind = sk_bind, .connect = sk_connect, .listen = sk_listen,
	.accept = sk_accept, .sendmsg = sk_send, .recvmsg = sk_recv, .shutdown = sk_shutdown,
	.getsockopt = sk_getsockopt, .setsockopt = sk_setsockopt, .poll = sk_poll,
	.getname = sk_getname, .ioctl = sk_ioctl, .release = sk_release,
};
