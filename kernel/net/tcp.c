/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * TCP (RFC 793, 1122, 5681 slow start / congestion avoidance / fast
 * retransmit, 6298 retransmission timer). Written from the RFCs.
 *
 * Simplifications: no window scaling, SACK or timestamps; every data segment
 * is acknowledged at once (no delayed ACK); keep-alive is accepted but not
 * sent; out-of-order data is held as a short list of segments. Portable.
 */
#include "kernel/net/net.h"

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define SEQ_LT(a, b) ((s32)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((s32)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((s32)((a) - (b)) > 0)
#define SEQ_GE(a, b) ((s32)((a) - (b)) >= 0)

#define RTO_MIN   200u
#define RTO_MAX   60000u
#define TW_MS     2000u
#define FIN2_MS   15000u
#define SYN_TRIES 6u
#define DATA_TRIES 8u

static void tcp_output(struct nsock *s);
static void drop_bufs(struct nsock *s);

static void seg_out(struct nstack *ns, u32 src, u32 dst, u16 sport, u16 dport, u32 seq, u32 ack,
		    u8 flags, u16 win, const u8 *d1, u32 n1, const u8 *d2, u32 n2, bool mssopt)
{
	u8 f[NET_FRAME];
	u8 *t = f + 34;
	u32 hl = mssopt ? 24 : 20;
	u16 c;

	net_put16(t, sport);
	net_put16(t + 2, dport);
	net_put32(t + 4, seq);
	net_put32(t + 8, ack);
	t[12] = (u8)((hl / 4) << 4);
	t[13] = flags;
	net_put16(t + 14, win);
	net_put16(t + 16, 0);
	net_put16(t + 18, 0);
	if (mssopt) {
		t[20] = 2;
		t[21] = 4;
		net_put16(t + 22, NET_MTU - 40);
	}
	if (n1)
		memcpy(t + hl, d1, n1);
	if (n2)
		memcpy(t + hl + n1, d2, n2);
	c = net_l4_csum(src, dst, 6, t, hl + n1 + n2);
	net_put16(t + 16, c);
	ip_out(ns, f, 6, src, dst, hl + n1 + n2);
}

static u32 rwin(const struct nsock *s)
{
	u32 w = s->rcap - s->rlen;

	return w > 65535 ? 65535 : w;
}

/* One segment of this connection; `off` bytes into the send ring, n of them. */
static void sock_seg(struct nsock *s, u8 flags, u32 seq, u32 off, u32 n)
{
	const u8 *d1 = NULL, *d2 = NULL;
	u32 n1 = 0, n2 = 0, w = rwin(s);
	u8 fl = flags;

	if (n) {
		u32 pos = (s->shead + off) % s->scap;

		n1 = MOS_MIN(n, s->scap - pos);
		n2 = n - n1;
		d1 = s->sbuf + pos;
		d2 = s->sbuf;
	}
	if (s->state != TS_SYN_SENT)
		fl |= F_ACK;
	s->adv_edge = s->rcv_nxt + w;
	seg_out(s->ns, s->lip, s->rip, s->lport, s->rport, seq, s->rcv_nxt, fl, (u16)w,
		d1, n1, d2, n2, flags & F_SYN);
}

static void send_ack(struct nsock *s)
{
	sock_seg(s, 0, s->snd_nxt, 0, 0);
}

/* Reset in answer to a segment that has no connection. */
static void send_rst_for(struct nstack *ns, u32 src, u32 dst, u16 sport, u16 dport, u32 seq,
			 u32 ack, u8 flags, u32 dlen)
{
	if (flags & F_ACK)
		seg_out(ns, dst, src, dport, sport, ack, 0, F_RST, 0, NULL, 0, NULL, 0, false);
	else
		seg_out(ns, dst, src, dport, sport, 0,
			seq + dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0),
			F_RST | F_ACK, 0, NULL, 0, NULL, 0, false);
}

static void arm_rto(struct nsock *s)
{
	s->deadline = s->ns->now + s->rto;
}

static bool alloc_bufs(struct nsock *s)
{
	s->sbuf = mos_zalloc(s->scap);
	s->rbuf = mos_zalloc(s->rcap);
	if (!s->sbuf || !s->rbuf) {
		mos_free(s->sbuf);
		mos_free(s->rbuf);
		s->sbuf = s->rbuf = NULL;
		return false;
	}
	s->shead = s->slen = s->rhead = s->rlen = 0;
	return true;
}

void tcp_free(struct nsock *s)
{
	struct nstack *ns = s->ns;

	if (s->type != NS_STREAM)
		return;
	/* A listener takes its unaccepted children with it. */
	if (s->state == TS_LISTEN) {
		for (int i = 0; i < NET_NSOCK; i++) {
			struct nsock *c = &ns->socks[i];

			if (c->type == NS_STREAM && c->parent == s) {
				if (c->state != TS_CLOSED)
					seg_out(ns, c->lip, c->rip, c->lport, c->rport, c->snd_nxt,
						c->rcv_nxt, F_RST | F_ACK, 0, NULL, 0, NULL, 0, false);
				c->parent = NULL;
				c->released = true;
				tcp_free(c);
			}
		}
	}
	while (s->ooo) {
		struct ooo_seg *o = s->ooo;

		s->ooo = o->next;
		mos_free(o);
	}
	mos_free(s->sbuf);
	mos_free(s->rbuf);
	s->sbuf = s->rbuf = NULL;
	s->type = 0;
	s->state = TS_CLOSED;
}

static void to_closed(struct nsock *s, int err)
{
	if (err && !s->err)
		s->err = err;
	s->state = TS_CLOSED;
	s->deadline = 0;
	s->tw_deadline = 0;
	if (s->released) {
		tcp_free(s);
		return;
	}
	ns_wake(s->ns, s);
	if (s->parent)
		ns_wake(s->ns, s->parent);
}

static void rtt_sample(struct nsock *s, u32 r)
{
	if (!s->srtt) {
		s->srtt = r ? r : 1;
		s->rttvar = s->srtt / 2;
	} else {
		u32 d = r > s->srtt ? r - s->srtt : s->srtt - r;

		s->rttvar = (3 * s->rttvar + d) / 4;
		s->srtt = (7 * s->srtt + r) / 8;
	}
	s->rto = s->srtt + MOS_MAX(10u, 4 * s->rttvar);
	s->rto = MOS_MAX(RTO_MIN, MOS_MIN(s->rto, RTO_MAX));
}

/* ---- user calls ---------------------------------------------------------------- */

int tcp_listen(struct nsock *s, int backlog)
{
	if (s->state != TS_CLOSED && s->state != TS_LISTEN)
		return -MERR_INVAL;
	if (!s->lport) {
		s->lport = ns_ephemeral(s->ns, NS_STREAM);
		if (!s->lport)
			return -MERR_ADDRINUSE;
	}
	s->backlog = backlog < 1 ? 1 : backlog > 32 ? 32 : (u32)backlog;
	s->state = TS_LISTEN;
	return 0;
}

int tcp_connect(struct nsock *s)
{
	struct nstack *ns = s->ns;

	if (s->state == TS_ESTAB)
		return -MERR_ISCONN;
	if (s->state == TS_SYN_SENT || s->state == TS_SYN_RCVD)
		return -MERR_ALREADY;
	if (s->state != TS_CLOSED)
		return -MERR_INVAL;
	if (!s->lip) {
		s->lip = ip_src_for(ns, s->rip);
		if (!s->lip)
			return -MERR_NETUNREACH;
	}
	if (!s->lport)
		s->lport = ns_ephemeral(ns, NS_STREAM);
	if (!s->sbuf && !alloc_bufs(s))
		return -MERR_NOMEM;
	s->err = 0;
	s->iss = ns_rand(ns);
	s->snd_una = s->iss;
	s->snd_nxt = s->snd_max = s->iss + 1;
	s->mss = 536;
	s->cwnd = 4 * s->mss;
	s->ssthresh = 0x7FFFFFFFu;
	s->rto = 1000;
	s->retries = 0;
	s->state = TS_SYN_SENT;
	s->adv_edge = 0;
	sock_seg(s, F_SYN, s->iss, 0, 0);
	arm_rto(s);
	return -MERR_INPROGRESS;
}

long tcp_send(struct nsock *s, const u8 *buf, u32 len)
{
	u32 room, n, tail, first;

	if (s->err)
		return -ns_take_error(s);
	if (s->state == TS_SYN_SENT || s->state == TS_SYN_RCVD)
		return -MERR_AGAIN;
	if (s->state == TS_CLOSED)
		return s->rport ? -MERR_PIPE : -MERR_NOTCONN;
	if (s->state == TS_LISTEN)
		return -MERR_NOTCONN;
	if (s->shut_wr || s->fin_wanted || (s->state != TS_ESTAB && s->state != TS_CLOSEWAIT))
		return -MERR_PIPE;
	if (!len)
		return 0;
	room = s->scap - s->slen;
	if (!room)
		return -MERR_AGAIN;
	n = MOS_MIN(len, room);
	tail = (s->shead + s->slen) % s->scap;
	first = MOS_MIN(n, s->scap - tail);
	memcpy(s->sbuf + tail, buf, first);
	memcpy(s->sbuf, buf + first, n - first);
	s->slen += n;
	tcp_output(s);
	return n;
}

long tcp_recv(struct nsock *s, u8 *buf, u32 len, bool peek)
{
	u32 n, first;

	if (!s->rlen) {
		if (s->err)
			return -ns_take_error(s);
		if (s->state == TS_LISTEN)
			return -MERR_INVAL;
		if (s->state == TS_SYN_SENT || s->state == TS_SYN_RCVD)
			return -MERR_AGAIN;
		if (s->state == TS_CLOSED && !s->rport)
			return -MERR_NOTCONN;
		if (s->fin_rcvd || s->shut_rd || s->state == TS_CLOSED)
			return 0;
		return -MERR_AGAIN;
	}
	n = MOS_MIN(len, s->rlen);
	first = MOS_MIN(n, s->rcap - s->rhead);
	memcpy(buf, s->rbuf + s->rhead, first);
	memcpy(buf + first, s->rbuf, n - first);
	if (peek)
		return n;
	s->rhead = (s->rhead + n) % s->rcap;
	s->rlen -= n;
	/* Tell the peer when the window has opened by a worthwhile amount. */
	if (s->state != TS_CLOSED && (s32)(s->rcv_nxt + rwin(s) - s->adv_edge) >=
	    (s32)MOS_MIN(s->mss, s->rcap / 2))
		send_ack(s);
	return n;
}

static void tcp_abort(struct nsock *s)
{
	if (s->state != TS_CLOSED && s->state != TS_LISTEN && s->state != TS_SYN_SENT)
		seg_out(s->ns, s->lip, s->rip, s->lport, s->rport, s->snd_nxt, s->rcv_nxt,
			F_RST | F_ACK, 0, NULL, 0, NULL, 0, false);
}

void tcp_close(struct nsock *s)
{
	s->released = true;
	s->user = NULL;
	switch (s->state) {
	case TS_CLOSED:
	case TS_LISTEN:
	case TS_SYN_SENT:
		tcp_free(s);
		return;
	case TS_SYN_RCVD:
		tcp_abort(s);
		tcp_free(s);
		return;
	case TS_ESTAB:
	case TS_CLOSEWAIT:
		if (s->rlen) {                  /* unread data: reset, as RFC 1122 4.2.2.13 says */
			tcp_abort(s);
			tcp_free(s);
			return;
		}
		s->fin_wanted = true;
		tcp_output(s);
		return;
	case TS_FIN2:
		s->tw_deadline = s->ns->now + FIN2_MS;
		return;
	case TS_TIMEWAIT:
		drop_bufs(s);
		return;
	default:
		return;
	}
}

int tcp_shutdown(struct nsock *s, int how)
{
	if (s->state == TS_CLOSED || s->state == TS_LISTEN || s->state == TS_SYN_SENT)
		return -MERR_NOTCONN;
	if (how == 0 || how == 2)
		s->shut_rd = true;
	if (how == 1 || how == 2) {
		s->shut_wr = true;
		s->fin_wanted = true;
		tcp_output(s);
	}
	ns_wake(s->ns, s);
	return 0;
}

u32 tcp_poll(struct nsock *s)
{
	u32 m = 0;

	switch (s->state) {
	case TS_LISTEN:
		return s->accq ? NS_POLLIN : 0;
	case TS_SYN_SENT:
	case TS_SYN_RCVD:
		return s->err ? NS_POLLERR : 0;
	case TS_CLOSED:
		if (!s->rport)
			return NS_POLLOUT | NS_POLLHUP;
		return (s->rlen ? NS_POLLIN : NS_POLLIN | NS_POLLHUP) | (s->err ? NS_POLLERR : 0) |
		       NS_POLLOUT;
	default:
		break;
	}
	if (s->rlen || s->fin_rcvd || s->shut_rd)
		m |= NS_POLLIN;
	if (s->fin_rcvd)
		m |= NS_POLLRDHUP;
	if ((s->state == TS_ESTAB || s->state == TS_CLOSEWAIT) && !s->shut_wr && !s->fin_wanted &&
	    s->slen < s->scap)
		m |= NS_POLLOUT;
	if (s->err)
		m |= NS_POLLERR;
	if (s->fin_rcvd && (s->fin_sent || s->state == TS_TIMEWAIT))
		m |= NS_POLLHUP;
	return m;
}

/* ---- output ---------------------------------------------------------------------- */

static void tcp_output(struct nsock *s)
{
	struct nstack *ns = s->ns;
	u32 flight, unsent, wnd, n;

	if (s->state != TS_ESTAB && s->state != TS_CLOSEWAIT && s->state != TS_FIN1 &&
	    s->state != TS_LASTACK && s->state != TS_CLOSING && s->state != TS_FIN2)
		return;
	for (;;) {
		flight = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
		unsent = s->slen > flight ? s->slen - flight : 0;
		wnd = MOS_MIN(s->snd_wnd, s->cwnd);
		n = wnd > flight ? wnd - flight : 0;
		n = MOS_MIN(n, MOS_MIN(unsent, s->mss));
		if (!n)
			break;
		if (!s->nodelay && n < s->mss && flight > 0 && !s->fin_wanted)
			break;                  /* Nagle: wait for the outstanding data's ACK */
		sock_seg(s, (n == unsent ? F_PSH : 0), s->snd_nxt, flight, n);
		if (!s->timing && !s->fin_sent) {
			s->timing = true;
			s->rtt_seq = s->snd_nxt + n;
			s->rtt_time = ns->now;
		}
		s->snd_nxt += n;
		if (SEQ_GT(s->snd_nxt, s->snd_max))
			s->snd_max = s->snd_nxt;
		if (!s->deadline)
			arm_rto(s);
	}
	flight = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
	unsent = s->slen > flight ? s->slen - flight : 0;
	if (s->fin_wanted && !s->fin_sent && !unsent) {
		sock_seg(s, F_FIN, s->snd_nxt, 0, 0);
		s->fin_seq = s->snd_nxt;
		s->snd_nxt++;
		if (SEQ_GT(s->snd_nxt, s->snd_max))
			s->snd_max = s->snd_nxt;
		s->fin_sent = s->fin_out = true;
		if (s->state == TS_ESTAB)
			s->state = TS_FIN1;
		else if (s->state == TS_CLOSEWAIT)
			s->state = TS_LASTACK;
		if (!s->deadline)
			arm_rto(s);
	}
	/* Data waiting behind a closed window and nothing in flight: probe. */
	if (unsent && !s->snd_wnd && !flight) {
		if (!s->persist) {
			s->persist = true;
			s->retries = 0;
			s->deadline = ns->now + MOS_MIN(s->rto, 1000u);
		}
	} else if (s->snd_wnd) {
		s->persist = false;
	}
}

static void on_rto(struct nsock *s)
{
	struct nstack *ns = s->ns;
	u32 flight;

	s->timing = false;
	if (s->persist && !s->snd_wnd) {
		/* A segment with an old sequence number makes the peer restate its window. */
		seg_out(ns, s->lip, s->rip, s->lport, s->rport, s->snd_una - 1, s->rcv_nxt, F_ACK,
			(u16)rwin(s), NULL, 0, NULL, 0, false);
		s->deadline = ns->now + (500u << MOS_MIN(s->retries, 4u));
		s->retries++;
		return;
	}
	s->retries++;
	if (s->retries > (s->state == TS_SYN_SENT || s->state == TS_SYN_RCVD ? SYN_TRIES : DATA_TRIES)) {
		if (s->state != TS_SYN_SENT)
			tcp_abort(s);
		to_closed(s, MERR_TIMEDOUT);
		return;
	}
	s->rto = MOS_MIN(s->rto * 2, RTO_MAX);
	ns->tcp_rexmit++;
	if (s->state == TS_SYN_SENT) {
		sock_seg(s, F_SYN, s->iss, 0, 0);
	} else if (s->state == TS_SYN_RCVD) {
		sock_seg(s, F_SYN | F_ACK, s->iss, 0, 0);
	} else {
		flight = s->snd_nxt - s->snd_una;
		if (!flight) {
			s->deadline = 0;
			return;
		}
		s->ssthresh = MOS_MAX(flight / 2, 2 * s->mss);
		s->cwnd = s->mss;
		s->dupacks = 0;
		s->snd_nxt = s->snd_una;                /* go back and resend from the oldest byte */
		s->fin_sent = false;
		s->deadline = 0;
		tcp_output(s);
		if (!s->deadline)
			arm_rto(s);
		return;
	}
	arm_rto(s);
}

void tcp_timers(struct nstack *ns)
{
	for (int i = 0; i < NET_NSOCK; i++) {
		struct nsock *s = &ns->socks[i];

		if (s->type != NS_STREAM)
			continue;
		if (s->state == TS_TIMEWAIT && ns->now >= s->tw_deadline) {
			to_closed(s, 0);
			continue;
		}
		if (s->state == TS_FIN2 && s->released && s->tw_deadline && ns->now >= s->tw_deadline) {
			to_closed(s, 0);
			continue;
		}
		if (s->deadline && ns->now >= s->deadline &&
		    s->state != TS_CLOSED && s->state != TS_LISTEN)
			on_rto(s);
	}
}

/* ---- input ----------------------------------------------------------------------- */

static struct nsock *tcp_find(struct nstack *ns, u32 dst, u16 dport, u32 src, u16 sport)
{
	struct nsock *l = NULL;

	for (int i = 0; i < NET_NSOCK; i++) {
		struct nsock *s = &ns->socks[i];

		if (s->type != NS_STREAM || s->lport != dport)
			continue;
		if (s->state == TS_LISTEN) {
			if (!s->lip || s->lip == dst)
				l = s;
		} else if (s->state != TS_CLOSED && s->rport == sport && s->rip == src &&
			   (!s->lip || s->lip == dst)) {
			return s;
		}
	}
	return l;
}

/* A TIME_WAIT orphan keeps only its 4-tuple: give the buffers back. */
static void drop_bufs(struct nsock *s)
{
	if (!s->released)
		return;
	mos_free(s->sbuf);
	mos_free(s->rbuf);
	s->sbuf = s->rbuf = NULL;
	s->slen = s->rlen = 0;
}

static void enter_timewait(struct nsock *s)
{
	drop_bufs(s);
	s->state = TS_TIMEWAIT;
	s->deadline = 0;
	s->tw_deadline = s->ns->now + TW_MS;
	ns_wake(s->ns, s);
}

static void ooo_insert(struct nsock *s, u32 seq, const u8 *d, u32 n)
{
	struct ooo_seg *o;

	if (s->ooo_n >= NET_OOO_MAX)
		return;
	for (o = s->ooo; o; o = o->next)
		if (o->seq == seq && o->len >= n)
			return;                 /* duplicate */
	o = mos_zalloc(sizeof(*o) + n);
	if (!o)
		return;
	o->seq = seq;
	o->len = (u16)n;
	memcpy(o->data, d, n);
	o->next = s->ooo;
	s->ooo = o;
	s->ooo_n++;
}

static u32 rx_append(struct nsock *s, const u8 *d, u32 n)
{
	u32 room = s->rcap - s->rlen, tail, first;

	if (s->shut_rd) {
		s->rcv_nxt += n;                /* read side closed: take and discard */
		return n;
	}
	n = MOS_MIN(n, room);
	tail = (s->rhead + s->rlen) % s->rcap;
	first = MOS_MIN(n, s->rcap - tail);
	memcpy(s->rbuf + tail, d, first);
	memcpy(s->rbuf, d + first, n - first);
	s->rlen += n;
	s->rcv_nxt += n;
	return n;
}

static void ooo_drain(struct nsock *s)
{
	bool again = true;

	while (again) {
		struct ooo_seg **pp;

		again = false;
		for (pp = &s->ooo; *pp; pp = &(*pp)->next) {
			struct ooo_seg *o = *pp;

			if (SEQ_GT(o->seq, s->rcv_nxt))
				continue;
			if (SEQ_GT(o->seq + o->len, s->rcv_nxt)) {
				u32 skip = s->rcv_nxt - o->seq;

				if (rx_append(s, o->data + skip, o->len - skip) < o->len - skip)
					return;         /* receive buffer full: keep the rest */
			}
			*pp = o->next;
			s->ooo_n--;
			mos_free(o);
			again = true;
			break;
		}
	}
}

static void sync_input(struct nsock *s, u32 seq, u32 ack, u8 flags, u16 win, const u8 *data,
		       u32 dlen)
{
	struct nstack *ns = s->ns;
	bool fin = flags & F_FIN, need_ack = false, ok;
	u32 rw = s->rcap - s->rlen, seg_len = dlen + (fin ? 1 : 0);

	/* 1. Is any of it inside the receive window? */
	if (!seg_len)
		ok = rw ? (SEQ_GE(seq, s->rcv_nxt) && SEQ_LT(seq, s->rcv_nxt + rw)) : seq == s->rcv_nxt;
	else
		ok = rw && ((SEQ_GE(seq, s->rcv_nxt) && SEQ_LT(seq, s->rcv_nxt + rw)) ||
			    (SEQ_GE(seq + seg_len - 1, s->rcv_nxt) &&
			     SEQ_LT(seq + seg_len - 1, s->rcv_nxt + rw)) ||
			    (SEQ_LT(seq, s->rcv_nxt) && SEQ_GE(seq + seg_len, s->rcv_nxt)));
	if (!ok) {
		if (!(flags & F_RST))
			send_ack(s);
		return;
	}
	/* 2. RST */
	if (flags & F_RST) {
		if (s->state == TS_SYN_RCVD && s->parent) {
			s->released = true;
			s->parent = NULL;
			tcp_free(s);
			return;
		}
		to_closed(s, s->state == TS_CLOSEWAIT ? MERR_PIPE : MERR_CONNRESET);
		return;
	}
	/* 3. SYN in a synchronised state: the peer lost track; restate our position. */
	if (flags & F_SYN) {
		send_ack(s);
		return;
	}
	if (!(flags & F_ACK))
		return;
	/* 4. ACK */
	if (s->state == TS_SYN_RCVD) {
		if (!(SEQ_LT(s->snd_una, ack) && SEQ_LE(ack, s->snd_nxt))) {
			seg_out(ns, s->lip, s->rip, s->lport, s->rport, ack, 0, F_RST, 0, NULL, 0, NULL, 0, false);
			return;
		}
		s->snd_una = ack;
		s->state = TS_ESTAB;
		s->deadline = 0;
		s->retries = 0;
		s->snd_wnd = win;
		s->snd_wl1 = seq;
		s->snd_wl2 = ack;
		if (s->parent) {
			struct nsock **pp = &s->parent->accq;

			while (*pp)
				pp = &(*pp)->acc_next;
			*pp = s;
			s->acc_next = NULL;
			ns_wake(ns, s->parent);
		}
	} else if (SEQ_GT(ack, s->snd_max)) {
		send_ack(s);                    /* ACKs something we have not sent */
		return;
	} else if (SEQ_GT(ack, s->snd_una)) {
		u32 acked = ack - s->snd_una, dacked = acked;
		bool fin_ok = false;

		if (s->fin_out && SEQ_GT(ack, s->fin_seq)) {
			fin_ok = true;
			dacked--;
		}
		if (SEQ_GT(ack, s->snd_nxt))
			s->snd_nxt = ack;       /* after a go-back-N, the old data arrived after all */
		dacked = MOS_MIN(dacked, s->slen);
		s->shead = (s->shead + dacked) % s->scap;
		s->slen -= dacked;
		s->snd_una = ack;
		s->dupacks = 0;
		s->retries = 0;
		if (s->timing && SEQ_GE(ack, s->rtt_seq)) {
			rtt_sample(s, (u32)(ns->now - s->rtt_time));
			s->timing = false;
		}
		if (s->cwnd < s->ssthresh)
			s->cwnd += MOS_MIN(dacked, s->mss);
		else
			s->cwnd += MOS_MAX(1u, s->mss * s->mss / s->cwnd);
		s->deadline = s->snd_una == s->snd_nxt ? 0 : ns->now + s->rto;
		if (s->persist && win)
			s->persist = false;
		if (fin_ok)
			s->fin_acked = true;
		ns_wake(ns, s);
	} else if (ack == s->snd_una && !dlen && !fin && s->snd_nxt != s->snd_una &&
		   win == s->snd_wnd) {
		if (++s->dupacks == 3) {         /* fast retransmit */
			u32 flight = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
			u32 n = MOS_MIN(MOS_MIN(s->mss, s->slen), flight);

			s->ssthresh = MOS_MAX(flight / 2, 2 * s->mss);
			s->cwnd = s->ssthresh + 3 * s->mss;
			s->timing = false;
			ns->tcp_rexmit++;
			if (n)
				sock_seg(s, 0, s->snd_una, 0, n);
		} else if (s->dupacks > 3) {
			s->cwnd += s->mss;
		}
	}
	/* Window update (RFC 793 SND.WL1/WL2). */
	if (SEQ_LT(s->snd_wl1, seq) || (s->snd_wl1 == seq && SEQ_LE(s->snd_wl2, ack))) {
		s->snd_wnd = win;
		s->snd_wl1 = seq;
		s->snd_wl2 = ack;
		if (win)
			s->persist = false;
	}
	if (s->fin_acked) {
		if (s->state == TS_FIN1) {
			s->state = TS_FIN2;
			if (s->released)
				s->tw_deadline = ns->now + FIN2_MS;
			ns_wake(ns, s);
		} else if (s->state == TS_CLOSING) {
			enter_timewait(s);
		} else if (s->state == TS_LASTACK) {
			to_closed(s, 0);
			return;
		}
	}
	/* 5. Data and FIN. */
	if (s->state == TS_ESTAB || s->state == TS_FIN1 || s->state == TS_FIN2) {
		if (SEQ_LT(seq, s->rcv_nxt)) {          /* trim what was already received */
			u32 skip = s->rcv_nxt - seq;

			if (skip > dlen) {
				fin = false;            /* the FIN itself is old */
				skip = dlen;
			}
			data += skip;
			dlen -= skip;
			seq = s->rcv_nxt;
			if (!dlen && !fin)
				need_ack = true;
		}
		if ((dlen || fin) && seq == s->rcv_nxt) {
			u32 n = dlen ? rx_append(s, data, dlen) : 0;

			if (n < dlen)
				fin = false;
			else if (s->ooo)
				ooo_drain(s);
			if (fin && seq + dlen == s->rcv_nxt) {
				s->rcv_nxt++;
				s->fin_rcvd = true;
				if (s->state == TS_ESTAB)
					s->state = TS_CLOSEWAIT;
				else if (s->state == TS_FIN1)
					s->state = TS_CLOSING;
				else if (s->state == TS_FIN2)
					enter_timewait(s);
			}
			need_ack = true;
			ns_wake(ns, s);
		} else if (dlen && SEQ_GT(seq, s->rcv_nxt)) {
			ooo_insert(s, seq, data, dlen);
			need_ack = true;                /* duplicate ACK tells the sender what is missing */
		}
	} else if (dlen || fin) {
		need_ack = true;                        /* data after the peer's FIN: ignore */
		if (fin && s->state == TS_TIMEWAIT)
			enter_timewait(s);
	}
	if (need_ack)
		send_ack(s);
	if (s->state != TS_CLOSED)
		tcp_output(s);
}

void tcp_input(struct nstack *ns, u32 src, u32 dst, const u8 *seg, u32 len)
{
	u32 hl, seq, ack, dlen, peer_mss = 0;
	u16 sport, dport, win;
	u8 flags;
	const u8 *data;
	struct nsock *s;

	if (len < 20)
		return;
	hl = (u32)(seg[12] >> 4) * 4;
	if (hl < 20 || hl > len || net_l4_csum(src, dst, 6, seg, len))
		return;
	sport = net_get16(seg);
	dport = net_get16(seg + 2);
	seq = net_get32(seg + 4);
	ack = net_get32(seg + 8);
	flags = seg[13];
	win = net_get16(seg + 14);
	data = seg + hl;
	dlen = len - hl;
	for (u32 i = 20; i < hl;) {             /* options: only MSS matters */
		u8 k = seg[i];

		if (k == 0)
			break;
		if (k == 1) {
			i++;
			continue;
		}
		if (i + 1 >= hl || seg[i + 1] < 2 || i + seg[i + 1] > hl)
			break;
		if (k == 2 && seg[i + 1] == 4)
			peer_mss = net_get16(seg + i + 2);
		i += seg[i + 1];
	}

	s = tcp_find(ns, dst, dport, src, sport);
	if (!s) {
		if (!(flags & F_RST))
			send_rst_for(ns, src, dst, sport, dport, seq, ack, flags, dlen);
		return;
	}
	switch (s->state) {
	case TS_LISTEN: {
		struct nsock *c;
		u32 kids = 0;

		if (flags & F_RST)
			return;
		if (flags & F_ACK) {
			send_rst_for(ns, src, dst, sport, dport, seq, ack, flags, dlen);
			return;
		}
		if (!(flags & F_SYN))
			return;
		for (int i = 0; i < NET_NSOCK; i++)
			kids += ns->socks[i].type == NS_STREAM && ns->socks[i].parent == s;
		if (kids >= s->backlog)
			return;                 /* full: the client retransmits its SYN */
		c = ns_sock_new(ns, NS_STREAM);
		if (!c)
			return;
		c->lip = dst;
		c->lport = dport;
		c->rip = src;
		c->rport = sport;
		c->parent = s;
		c->nodelay = s->nodelay;
		c->reuseaddr = s->reuseaddr;
		c->scap = s->scap;
		c->rcap = s->rcap;
		if (!alloc_bufs(c)) {
			c->released = true;
			c->parent = NULL;
			tcp_free(c);
			return;
		}
		c->irs = seq;
		c->rcv_nxt = seq + 1;
		c->iss = ns_rand(ns);
		c->snd_una = c->iss;
		c->snd_nxt = c->snd_max = c->iss + 1;
		c->snd_wnd = win;
		c->snd_wl1 = seq;
		c->mss = MOS_MIN(NET_MTU - 40u, peer_mss ? peer_mss : 536u);
		c->cwnd = 4 * c->mss;
		c->ssthresh = 0x7FFFFFFFu;
		c->state = TS_SYN_RCVD;
		sock_seg(c, F_SYN | F_ACK, c->iss, 0, 0);
		arm_rto(c);
		return;
	}
	case TS_SYN_SENT: {
		bool good = (flags & F_ACK) && ack == s->iss + 1;

		if ((flags & F_ACK) && !good) {
			if (!(flags & F_RST))
				send_rst_for(ns, src, dst, sport, dport, seq, ack, flags, dlen);
			return;
		}
		if (flags & F_RST) {
			if (good)
				to_closed(s, MERR_CONNREFUSED);
			return;
		}
		if (!(flags & F_SYN))
			return;
		s->irs = seq;
		s->rcv_nxt = seq + 1;
		s->mss = MOS_MIN(NET_MTU - 40u, peer_mss ? peer_mss : 536u);
		s->cwnd = 4 * s->mss;
		s->snd_wnd = win;
		s->snd_wl1 = seq;
		s->snd_wl2 = ack;
		if (good) {
			s->snd_una = ack;
			s->state = TS_ESTAB;
			s->deadline = 0;
			s->retries = 0;
			send_ack(s);
			ns_wake(ns, s);
		} else {                        /* simultaneous open */
			s->state = TS_SYN_RCVD;
			sock_seg(s, F_SYN | F_ACK, s->iss, 0, 0);
		}
		return;
	}
	case TS_CLOSED:
		return;
	default:
		sync_input(s, seq, ack, flags, win, data, dlen);
		return;
	}
}
