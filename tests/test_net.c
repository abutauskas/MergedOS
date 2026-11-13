/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Network stack tests: two nstack instances joined by a virtual wire that can
 * drop, duplicate and reorder frames, a scripted DHCP server, loopback, and
 * malformed-frame fuzzing under ASan/UBSan.
 */
#include "tests/test.h"
#include "kernel/net/net.h"

#define WQ 512

struct wire {
	struct nstack *peer;
	u8 *q[WQ];
	u16 len[WQ];
	int head, n;
	int drop_every, dup_every, reorder;     /* 0 = off */
	int count;
	u8 *held;                               /* reorder: one frame delayed */
	u16 held_len;
	bool tap_dhcp;
	u32 sent;
};

struct host {
	struct nstack ns;
	struct netdev dev;
	struct wire w;                          /* frames this host sends */
	u32 woken;
};

static u64 clock_ms;

static void wire_push(struct wire *w, const u8 *f, u32 len)
{
	if (w->n == WQ)
		return;
	w->q[(w->head + w->n) % WQ] = memcpy(malloc(len), f, len);
	w->len[(w->head + w->n) % WQ] = (u16)len;
	w->n++;
}

static int host_send(struct netdev *d, const void *f, u32 len)
{
	struct host *h = d->priv;
	struct wire *w = &h->w;

	w->sent++;
	w->count++;
	if (w->drop_every && w->count % w->drop_every == 0)
		return 0;
	if (w->reorder && w->count % w->reorder == 0 && !w->held) {
		w->held = memcpy(malloc(len), f, len);
		w->held_len = (u16)len;
		return 0;
	}
	wire_push(w, f, len);
	if (w->held) {
		wire_push(w, w->held, w->held_len);
		free(w->held);
		w->held = NULL;
	}
	if (w->dup_every && w->count % w->dup_every == 0)
		wire_push(w, f, len);
	return 0;
}

static void host_wake(struct nstack *ns, struct nsock *s)
{
	(void)s;
	((struct host *)ns->dev->priv)->woken++;
}

static void host_init(struct host *h, u8 last, u32 ip)
{
	memset(h, 0, sizeof(*h));
	h->dev.mac[0] = 0x02;
	h->dev.mac[5] = last;
	h->dev.send = host_send;
	h->dev.priv = h;
	ns_init(&h->ns, &h->dev);
	h->ns.wake = host_wake;
	ns_set_addr(&h->ns, ip, IP4(255, 255, 255, 0), 0);
}

static void host_free(struct host *h)
{
	for (int i = 0; i < h->w.n; i++)
		free(h->w.q[(h->w.head + i) % WQ]);
	free(h->w.held);
	ns_destroy(&h->ns);
}

/* One millisecond: deliver everything queued each way, then run timers. */
static void tick(struct host *a, struct host *b)
{
	struct host *hs[2] = { a, b };

	clock_ms++;
	for (int i = 0; i < 2; i++) {
		struct host *src = hs[i], *dst = hs[1 - i];
		int n = src->w.n;

		while (n-- > 0 && src->w.n) {
			u8 *f = src->w.q[src->w.head];
			u16 l = src->w.len[src->w.head];

			src->w.head = (src->w.head + 1) % WQ;
			src->w.n--;
			dst->ns.now = clock_ms;
			ns_input(&dst->ns, f, l);
			free(f);
		}
	}
	ns_pump(&a->ns, clock_ms);
	ns_pump(&b->ns, clock_ms);
}

static void settle(struct host *a, struct host *b, int ms)
{
	while (ms-- > 0)
		tick(a, b);
}

static u32 pat(u32 i, u32 salt)
{
	return (i * 2654435761u + salt) >> 24;
}

/* Move `total` bytes each way over an established pair; true if both arrive intact. */
static bool exchange(struct host *a, struct nsock *sa, struct host *b, struct nsock *sb,
		     u32 total, int max_ms)
{
	u32 sent_a = 0, sent_b = 0, got_a = 0, got_b = 0;
	bool bad = false;

	for (int t = 0; t < max_ms && (got_a < total || got_b < total) && !bad; t++) {
		u8 buf[1500];

		while (sent_a < total) {
			u32 n = MOS_MIN(total - sent_a, 1000u);
			long r;

			for (u32 i = 0; i < n; i++)
				buf[i] = (u8)pat(sent_a + i, 1);
			r = ns_send(sa, buf, n);
			if (r <= 0)
				break;
			sent_a += (u32)r;
		}
		while (sent_b < total) {
			u32 n = MOS_MIN(total - sent_b, 777u);
			long r;

			for (u32 i = 0; i < n; i++)
				buf[i] = (u8)pat(sent_b + i, 2);
			r = ns_send(sb, buf, n);
			if (r <= 0)
				break;
			sent_b += (u32)r;
		}
		for (;;) {
			long r = ns_recv(sb, buf, sizeof(buf), false);

			if (r <= 0)
				break;
			for (long i = 0; i < r; i++)
				bad |= buf[i] != (u8)pat(got_b + (u32)i, 1);
			got_b += (u32)r;
		}
		for (;;) {
			long r = ns_recv(sa, buf, 700, false);

			if (r <= 0)
				break;
			for (long i = 0; i < r; i++)
				bad |= buf[i] != (u8)pat(got_a + (u32)i, 2);
			got_a += (u32)r;
		}
		tick(a, b);
	}
	return !bad && got_a == total && got_b == total;
}

static bool connect_pair(struct host *a, struct host *b, u16 port, struct nsock **ca,
			 struct nsock **cb, struct nsock **lis)
{
	struct nsock *l = ns_sock_new(&b->ns, NS_STREAM), *c = ns_sock_new(&a->ns, NS_STREAM);
	int rc;

	if (!l || !c || ns_bind(l, 0, port) || ns_listen(l, 4))
		return false;
	rc = ns_connect(c, b->ns.ip, port);
	if (rc != -MERR_INPROGRESS)
		return false;
	for (int t = 0; t < 3000 && !(ns_poll(c) & NS_POLLOUT); t++)
		tick(a, b);
	if (!(ns_poll(c) & NS_POLLOUT))
		return false;
	for (int t = 0; t < 100 && !l->accq; t++)
		tick(a, b);
	*cb = ns_accept(l);
	*ca = c;
	*lis = l;
	return *cb != NULL;
}

static void test_basics(void)
{
	/* RFC 1071 section 3 worked example. */
	static const u8 v[8] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
	u8 odd[3] = { 0xff, 0xff, 0xff };
	struct host a, b;
	struct nsock *u1, *u2;
	u8 buf[64];
	u32 ip;
	u16 port;

	CHECK_EQ(net_csum(v, 8, 0), 0x220d);
	CHECK_EQ(net_csum(odd, 3, 0), 0x00ff);
	CHECK_EQ(net_csum(NULL, 0, 0), 0xffff);

	host_init(&a, 1, IP4(10, 0, 0, 1));
	host_init(&b, 2, IP4(10, 0, 0, 2));
	/* ARP + ICMP echo */
	CHECK_EQ(ns_ping(&a.ns, b.ns.ip, 7, 1), 0);
	settle(&a, &b, 20);
	CHECK_EQ(a.ns.ping_replies, 1);
	CHECK_EQ(a.ns.ping_seq_seen, 1);
	/* UDP */
	u1 = ns_sock_new(&a.ns, NS_DGRAM);
	u2 = ns_sock_new(&b.ns, NS_DGRAM);
	CHECK_EQ(ns_bind(u2, 0, 5000), 0);
	CHECK_EQ(ns_sendto(u1, "hello", 5, b.ns.ip, 5000), 5);
	settle(&a, &b, 10);
	CHECK_EQ(ns_recvfrom(u2, buf, sizeof(buf), false, &ip, &port), 5);
	CHECK(!memcmp(buf, "hello", 5));
	CHECK_EQ(ip, a.ns.ip);
	CHECK(port >= 49152);
	CHECK_EQ(ns_recvfrom(u2, buf, sizeof(buf), false, NULL, NULL), -MERR_AGAIN);
	CHECK_EQ(ns_sendto(u1, buf, 1600, b.ns.ip, 5000), -MERR_MSGSIZE);
	/* address in use */
	{
		struct nsock *u3 = ns_sock_new(&b.ns, NS_DGRAM);

		CHECK_EQ(ns_bind(u3, 0, 5000), -MERR_ADDRINUSE);
		ns_sock_close(u3);
	}
	/* UDP over loopback on one host */
	{
		struct nsock *l1 = ns_sock_new(&a.ns, NS_DGRAM), *l2 = ns_sock_new(&a.ns, NS_DGRAM);

		CHECK_EQ(ns_bind(l1, IP4(127, 0, 0, 1), 6000), 0);
		CHECK_EQ(ns_sendto(l2, "lo", 2, IP4(127, 0, 0, 1), 6000), 2);
		CHECK_EQ(ns_recvfrom(l1, buf, sizeof(buf), false, &ip, NULL), 2);
		CHECK_EQ(ip, IP4(127, 0, 0, 1));
		ns_sock_close(l1);
		ns_sock_close(l2);
	}
	ns_sock_close(u1);
	ns_sock_close(u2);
	host_free(&a);
	host_free(&b);
}

static void test_tcp_clean(void)
{
	struct host a, b;
	struct nsock *ca, *cb, *l;

	host_init(&a, 1, IP4(10, 0, 0, 1));
	host_init(&b, 2, IP4(10, 0, 0, 2));
	CHECK(connect_pair(&a, &b, 8080, &ca, &cb, &l));
	CHECK_EQ(ca->state, TS_ESTAB);
	CHECK_EQ(cb->state, TS_ESTAB);
	CHECK(ca->mss == 1460);
	CHECK_EQ(cb->rport, ca->lport);
	CHECK(exchange(&a, ca, &b, cb, 200000, 20000));
	/* orderly close from both sides */
	CHECK_EQ(ns_shutdown(ca, 1), 0);
	settle(&a, &b, 50);
	CHECK(ns_poll(cb) & NS_POLLIN);
	CHECK_EQ(ns_recv(cb, (u8[4]){ 0 }, 4, false), 0);          /* EOF */
	CHECK_EQ(cb->state, TS_CLOSEWAIT);
	CHECK_EQ(ca->state, TS_FIN2);
	ns_sock_close(cb);
	settle(&a, &b, 50);
	CHECK_EQ(ca->state, TS_TIMEWAIT);
	ns_sock_close(ca);                      /* orphan in TIME_WAIT */
	settle(&a, &b, 2500);
	ns_sock_close(l);
	settle(&a, &b, 100);
	for (int i = 0; i < NET_NSOCK; i++) {
		CHECK(!a.ns.socks[i].type);
		CHECK(!b.ns.socks[i].type);
	}
	host_free(&a);
	host_free(&b);
}

static void test_tcp_lossy(void)
{
	static const struct { int drop, dup, reorder; } cases[] = {
		{ 7, 0, 0 }, { 0, 5, 0 }, { 0, 0, 3 }, { 11, 13, 5 },
	};

	for (unsigned k = 0; k < ARRAY_SIZE(cases); k++) {
		struct host a, b;
		struct nsock *ca, *cb, *l;

		host_init(&a, 1, IP4(10, 0, 0, 1));
		host_init(&b, 2, IP4(10, 0, 0, 2));
		/* resolve ARP first so the cases exercise TCP, not the ARP retry */
		ns_ping(&a.ns, b.ns.ip, 1, 1);
		settle(&a, &b, 20);
		a.w.drop_every = b.w.drop_every = cases[k].drop;
		a.w.dup_every = b.w.dup_every = cases[k].dup;
		a.w.reorder = b.w.reorder = cases[k].reorder;
		CHECK(connect_pair(&a, &b, 9000, &ca, &cb, &l));
		CHECK(exchange(&a, ca, &b, cb, 120000, 400000));
		if (cases[k].drop)
			CHECK(a.ns.tcp_rexmit + b.ns.tcp_rexmit > 0);
		ns_sock_close(ca);
		ns_sock_close(cb);
		ns_sock_close(l);
		settle(&a, &b, 5000);
		host_free(&a);
		host_free(&b);
	}
}

static void test_tcp_edges(void)
{
	struct host a, b;
	struct nsock *ca, *cb, *l, *x;
	u8 buf[4096];
	u32 total = 0;
	long r;

	host_init(&a, 1, IP4(10, 0, 0, 1));
	host_init(&b, 2, IP4(10, 0, 0, 2));

	/* connection refused */
	x = ns_sock_new(&a.ns, NS_STREAM);
	CHECK_EQ(ns_connect(x, b.ns.ip, 9), -MERR_INPROGRESS);
	settle(&a, &b, 100);
	CHECK(ns_poll(x) & NS_POLLERR);
	CHECK_EQ(ns_take_error(x), MERR_CONNREFUSED);
	ns_sock_close(x);

	/* connect to a silent address times out */
	x = ns_sock_new(&a.ns, NS_STREAM);
	a.ns.now = clock_ms;
	CHECK_EQ(ns_connect(x, IP4(10, 0, 0, 99), 80), -MERR_INPROGRESS);
	settle(&a, &b, 130000);
	CHECK_EQ(ns_take_error(x), MERR_TIMEDOUT);
	ns_sock_close(x);

	/* zero window: B does not read until A has filled everything */
	CHECK(connect_pair(&a, &b, 7000, &ca, &cb, &l));
	for (int t = 0; t < 3000; t++) {
		u8 chunk[1000];

		memset(chunk, 'z', sizeof(chunk));
		while ((r = ns_send(ca, chunk, sizeof(chunk))) > 0)
			total += (u32)r;
		tick(&a, &b);
	}
	CHECK(total >= 60000 && total <= 140000);       /* both buffers full, window closed */
	CHECK_EQ(cb->rlen, cb->rcap);
	{
		u32 got = 0;

		for (int t = 0; t < 30000 && got < total; t++) {
			u8 chunk[1000];

			while ((r = ns_send(ca, chunk, 0)) > 0)
				;
			while ((r = ns_recv(cb, buf, 1000, false)) > 0) {
				for (long i = 0; i < r; i++)
					CHECK(buf[i] == 'z');
				got += (u32)r;
			}
			tick(&a, &b);
		}
		CHECK_EQ(got, total);
	}

	/* abortive close: receiver sees ECONNRESET */
	ns_sock_close(ca);
	settle(&a, &b, 50);
	CHECK(cb->fin_rcvd);
	ns_sock_close(cb);
	settle(&a, &b, 2200);
	ns_sock_close(l);

	/* data left unread at close sends RST */
	CHECK(connect_pair(&a, &b, 7001, &ca, &cb, &l));
	CHECK_EQ(ns_send(ca, "hi", 2), 2);
	settle(&a, &b, 30);
	ns_sock_close(cb);                      /* b closes with "hi" unread */
	settle(&a, &b, 30);
	CHECK_EQ(ca->state, TS_CLOSED);
	CHECK_EQ(ns_send(ca, "x", 1) < 0, 1);
	ns_sock_close(ca);
	ns_sock_close(l);

	/* TCP over loopback on one host, and a closed listener refuses */
	{
		struct nsock *ll = ns_sock_new(&a.ns, NS_STREAM), *lc = ns_sock_new(&a.ns, NS_STREAM), *ls;

		CHECK_EQ(ns_bind(ll, IP4(127, 0, 0, 1), 4444), 0);
		CHECK_EQ(ns_listen(ll, 2), 0);
		CHECK_EQ(ns_connect(lc, IP4(127, 0, 0, 1), 4444), -MERR_INPROGRESS);
		CHECK(ns_poll(lc) & NS_POLLOUT);        /* handshake completed inside the call */
		ls = ns_accept(ll);
		CHECK(ls != NULL);
		CHECK_EQ(ns_send(lc, "loop", 4), 4);
		CHECK_EQ(ns_recv(ls, buf, sizeof(buf), false), 4);
		CHECK(!memcmp(buf, "loop", 4));
		ns_sock_close(lc);
		ns_sock_close(ls);
		ns_sock_close(ll);
		lc = ns_sock_new(&a.ns, NS_STREAM);
		CHECK_EQ(ns_connect(lc, IP4(127, 0, 0, 1), 4444), -MERR_INPROGRESS);
		CHECK_EQ(ns_take_error(lc), MERR_CONNREFUSED);
		ns_sock_close(lc);
	}
	settle(&a, &b, 3000);
	host_free(&a);
	host_free(&b);
}

/* A scripted DHCP server on the far end of the wire. */
static void dhcp_reply(struct host *srv, struct host *cli, const u8 *req, u8 type)
{
	u8 f[14 + 20 + 8 + 300];
	u8 *ip = f + 14, *u = ip + 20, *p = u + 8, *o;
	u32 total = 20 + 8 + 300;

	memset(f, 0, sizeof(f));
	memcpy(f, cli->dev.mac, 6);
	memcpy(f + 6, srv->dev.mac, 6);
	net_put16(f + 12, 0x0800);
	ip[0] = 0x45;
	net_put16(ip + 2, (u16)total);
	ip[8] = 64;
	ip[9] = 17;
	net_put32(ip + 12, IP4(10, 0, 2, 2));
	net_put32(ip + 16, 0xFFFFFFFFu);
	net_put16(ip + 10, net_csum(ip, 20, 0));
	net_put16(u, 67);
	net_put16(u + 2, 68);
	net_put16(u + 4, 308);
	p[0] = 2;
	p[1] = 1;
	p[2] = 6;
	memcpy(p + 4, req + 4, 4);              /* xid */
	net_put32(p + 16, IP4(10, 0, 2, 15));
	memcpy(p + 28, req + 28, 6);
	net_put32(p + 236, 0x63825363u);
	o = p + 240;
	*o++ = 53; *o++ = 1; *o++ = type;
	*o++ = 1; *o++ = 4; net_put32(o, IP4(255, 255, 255, 0)); o += 4;
	*o++ = 3; *o++ = 4; net_put32(o, IP4(10, 0, 2, 2)); o += 4;
	*o++ = 6; *o++ = 8; net_put32(o, IP4(10, 0, 2, 3)); net_put32(o + 4, IP4(8, 8, 8, 8)); o += 8;
	*o++ = 51; *o++ = 4; net_put32(o, 86400); o += 4;
	*o++ = 54; *o++ = 4; net_put32(o, IP4(10, 0, 2, 2)); o += 4;
	*o++ = 255;
	net_put16(u + 6, 0);                    /* UDP checksum optional */
	wire_push(&srv->w, f, sizeof(f));
}

static void test_dhcp(void)
{
	struct host cli, srv;
	u8 *req;
	int stage = 0;

	host_init(&cli, 1, 0);
	host_init(&srv, 2, IP4(10, 0, 2, 2));
	ns_set_addr(&cli.ns, 0, 0, 0);
	cli.ns.now = clock_ms;
	ns_dhcp_start(&cli.ns);
	for (int t = 0; t < 5000 && cli.ns.dhcp_state != DHCP_BOUND; t++) {
		while (cli.w.n) {
			req = cli.w.q[cli.w.head];
			if (cli.w.len[cli.w.head] > 300 && req[23] == 17 && net_get16(req + 34) == 68) {
				u8 mt = req[14 + 20 + 8 + 240 + 2];

				if (mt == 1 && stage == 0) {
					dhcp_reply(&srv, &cli, req + 42, 2);
					stage = 1;
				} else if (mt == 3 && stage == 1) {
					dhcp_reply(&srv, &cli, req + 42, 5);
					stage = 2;
				}
			}
			free(req);
			cli.w.head = (cli.w.head + 1) % WQ;
			cli.w.n--;
		}
		while (srv.w.n) {
			u8 *f = srv.w.q[srv.w.head];
			u16 l = srv.w.len[srv.w.head];

			srv.w.head = (srv.w.head + 1) % WQ;
			srv.w.n--;
			ns_input(&cli.ns, f, l);
			free(f);
		}
		clock_ms++;
		ns_pump(&cli.ns, clock_ms);
	}
	CHECK_EQ(cli.ns.dhcp_state, DHCP_BOUND);
	CHECK_EQ(cli.ns.ip, IP4(10, 0, 2, 15));
	CHECK_EQ(cli.ns.mask, IP4(255, 255, 255, 0));
	CHECK_EQ(cli.ns.gw, IP4(10, 0, 2, 2));
	CHECK_EQ(cli.ns.dns[0], IP4(10, 0, 2, 3));
	CHECK_EQ(cli.ns.dns[1], IP4(8, 8, 8, 8));
	CHECK_EQ(cli.ns.lease_s, 86400);
	host_free(&cli);
	host_free(&srv);
}

/* Malformed input: truncations, bit flips and random frames must never crash. */
static void test_fuzz(void)
{
	struct host a, b;
	struct nsock *ca, *cb, *l, *u;
	u8 *cap[64];
	u16 caplen[64];
	int ncap = 0;
	u32 rng = 12345;

	host_init(&a, 1, IP4(10, 0, 0, 1));
	host_init(&b, 2, IP4(10, 0, 0, 2));
	u = ns_sock_new(&b.ns, NS_DGRAM);
	ns_bind(u, 0, 53);
	ns_ping(&a.ns, b.ns.ip, 1, 1);
	settle(&a, &b, 20);
	CHECK(connect_pair(&a, &b, 80, &ca, &cb, &l));
	ns_send(ca, "GET / HTTP/1.0\r\n\r\n", 18);
	ns_sendto(ns_sock_new(&a.ns, NS_DGRAM), "q", 1, b.ns.ip, 53);
	/* capture the frames A sends, then mutate them into B */
	for (int t = 0; t < 10; t++) {
		while (a.w.n && ncap < 64) {
			cap[ncap] = a.w.q[a.w.head];
			caplen[ncap++] = a.w.len[a.w.head];
			a.w.head = (a.w.head + 1) % WQ;
			a.w.n--;
		}
		tick(&a, &b);
	}
	CHECK(ncap > 0);
	for (int round = 0; round < 3000; round++) {
		u8 f[NET_FRAME + 8];
		int k = (int)(rng % (u32)ncap);
		u32 len = caplen[k];

		rng = rng * 1664525u + 1013904223u;
		memcpy(f, cap[k], len);
		switch (round % 4) {
		case 0: len = rng % (len + 1); break;                   /* truncate */
		case 1: f[rng % len] ^= (u8)(1u << (rng >> 8) % 8); break;
		case 2: for (int i = 0; i < 4; i++) f[rng % len] = (u8)(rng >> (i * 3)); break;
		case 3: for (u32 i = 14; i < len; i++) f[i] = (u8)(rng = rng * 1664525u + 1013904223u) >> 24; break;
		}
		memcpy(f, b.dev.mac, 6);                                /* make them reach IP */
		ns_input(&b.ns, f, len);
		if (round % 50 == 0)
			ns_pump(&b.ns, ++clock_ms);
	}
	for (int i = 0; i < 2000; i++) {                                /* pure noise */
		u8 f[200];

		for (unsigned j = 0; j < sizeof(f); j++)
			f[j] = (u8)((rng = rng * 1664525u + 1013904223u) >> 24);
		memcpy(f, b.dev.mac, 6);
		net_put16(f + 12, i & 1 ? 0x0800 : 0x0806);
		f[14] = 0x45;
		ns_input(&b.ns, f, 14 + (u32)(i % 186));
	}
	/* TCP header with absurd option lengths and data offsets */
	for (int i = 0; i < 500; i++) {
		u8 f[14 + 20 + 60];

		memset(f, 0, sizeof(f));
		memcpy(f, b.dev.mac, 6);
		net_put16(f + 12, 0x0800);
		f[14] = 0x45;
		net_put16(f + 16, sizeof(f) - 14);
		f[22] = 64;
		f[23] = 6;
		net_put32(f + 26, a.ns.ip);
		net_put32(f + 30, b.ns.ip);
		net_put16(f + 24, net_csum(f + 14, 20, 0));
		net_put16(f + 34, ca->lport);
		net_put16(f + 36, 80);
		f[34 + 12] = (u8)((i % 16) << 4);
		f[34 + 13] = (u8)i;
		for (int j = 20; j < 40; j++)
			f[34 + j] = (u8)(i * j);
		net_put16(f + 34 + 16, net_l4_csum(a.ns.ip, b.ns.ip, 6, f + 34, 60));
		ns_input(&b.ns, f, sizeof(f));
	}
	for (int i = 0; i < ncap; i++)
		free(cap[i]);
	ns_sock_close(ca);
	ns_sock_close(cb);
	ns_sock_close(l);
	settle(&a, &b, 100);
	host_free(&a);
	host_free(&b);
}

void test_net(void)
{
	test_basics();
	test_tcp_clean();
	test_tcp_lossy();
	test_tcp_edges();
	test_dhcp();
	test_fuzz();
}
