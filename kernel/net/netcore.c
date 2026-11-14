/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Network core: checksum, Ethernet, ARP, IPv4, ICMP echo, UDP, DHCP client,
 * loopback, and the socket table shared with tcp.c. Written from RFC 791,
 * 792, 768, 826, 2131/2132. Portable (see net.h).
 */
#include "kernel/net/net.h"

#define ETH_IP  0x0800
#define ETH_ARP 0x0806

u16 net_csum(const void *data, u32 len, u32 init)
{
	const u8 *p = data;
	u32 sum = init;

	while (len > 1) {
		sum += (u32)((p[0] << 8) | p[1]);
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (u32)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (u16)~sum;
}

/* Checksum over the IPv4 pseudo header and a TCP/UDP segment. */
u16 net_l4_csum(u32 src, u32 dst, u8 proto, const void *seg, u32 len)
{
	u32 sum = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + len;
	const u8 *p = seg;
	u32 n = len;

	while (n > 1) {
		sum += (u32)((p[0] << 8) | p[1]);
		p += 2;
		n -= 2;
	}
	if (n)
		sum += (u32)p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (u16)~sum;
}

u32 ns_rand(struct nstack *ns)
{
	u32 x = ns->seed ? ns->seed : 0x9E3779B9u;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	ns->seed = x;
	return x;
}

void ns_wake(struct nstack *ns, struct nsock *s)
{
	if (ns->wake)
		ns->wake(ns, s);
}

void ns_init(struct nstack *ns, struct netdev *dev)
{
	memset(ns, 0, sizeof(*ns));
	ns->dev = dev;
	ns->ephem = 49152;
	ns->seed = 0x2545F491u;
	if (dev)
		for (int i = 0; i < 6; i++)
			ns->seed = ns->seed * 31 + dev->mac[i];
}

void ns_destroy(struct nstack *ns)
{
	for (int i = 0; i < NET_NSOCK; i++) {
		struct nsock *s = &ns->socks[i];

		if (!s->type)
			continue;
		s->released = true;
		tcp_free(s);
		while (s->uq) {
			struct udp_dgram *d = s->uq;

			s->uq = d->next;
			mos_free(d);
		}
		s->type = 0;
	}
	for (int i = 0; i < NET_ARP_N; i++)
		mos_free(ns->arp[i].pend);
	for (u32 i = 0; i < NET_LO_N; i++)
		mos_free(ns->lo[i]);
	memset(ns, 0, sizeof(*ns));
}

void ns_set_addr(struct nstack *ns, u32 ip, u32 mask, u32 gw)
{
	ns->ip = ip;
	ns->mask = mask;
	ns->gw = gw;
}

/* ---- ARP -------------------------------------------------------------------------- */

static struct arp_ent *arp_find(struct nstack *ns, u32 ip)
{
	for (int i = 0; i < NET_ARP_N; i++)
		if ((ns->arp[i].valid || ns->arp[i].pend || ns->arp[i].tries) && ns->arp[i].ip == ip)
			return &ns->arp[i];
	return NULL;
}

static struct arp_ent *arp_slot(struct nstack *ns, u32 ip)
{
	struct arp_ent *e = arp_find(ns, ip), *old = &ns->arp[0];

	if (e)
		return e;
	for (int i = 0; i < NET_ARP_N; i++) {
		if (!ns->arp[i].valid && !ns->arp[i].pend && !ns->arp[i].tries)
			return &ns->arp[i];
		if (ns->arp[i].stamp < old->stamp)
			old = &ns->arp[i];
	}
	mos_free(old->pend);
	memset(old, 0, sizeof(*old));
	return old;
}

static void eth_hdr(u8 *f, const u8 *dst, const u8 *src, u16 type)
{
	memcpy(f, dst, 6);
	memcpy(f + 6, src, 6);
	net_put16(f + 12, type);
}

static void arp_send(struct nstack *ns, u16 op, const u8 *tmac, u32 tip)
{
	static const u8 bcast[6] = { 255, 255, 255, 255, 255, 255 };
	static const u8 zero[6];
	u8 f[14 + 28];

	eth_hdr(f, op == 1 ? bcast : tmac, ns->dev->mac, ETH_ARP);
	net_put16(f + 14, 1);                   /* Ethernet */
	net_put16(f + 16, ETH_IP);
	f[18] = 6;
	f[19] = 4;
	net_put16(f + 20, op);
	memcpy(f + 22, ns->dev->mac, 6);
	net_put32(f + 28, ns->ip);
	memcpy(f + 32, op == 1 ? zero : tmac, 6);
	net_put32(f + 38, tip);
	ns->tx_frames++;
	ns->dev->send(ns->dev, f, sizeof(f));
}

static void arp_learn(struct nstack *ns, u32 ip, const u8 *mac)
{
	struct arp_ent *e;
	u8 *pend;
	u16 plen;

	if (!ip || ip == 0xFFFFFFFFu)
		return;
	e = arp_slot(ns, ip);
	e->ip = ip;
	memcpy(e->mac, mac, 6);
	e->valid = true;
	e->tries = 0;
	e->stamp = ns->now;
	pend = e->pend;
	plen = e->pend_len;
	e->pend = NULL;
	e->pend_len = 0;
	if (pend) {
		memcpy(pend, mac, 6);
		ns->tx_frames++;
		ns->dev->send(ns->dev, pend, plen);
		mos_free(pend);
	}
}

static void arp_input(struct nstack *ns, const u8 *p, u32 len)
{
	u32 sip, tip;

	if (len < 28 || net_get16(p) != 1 || net_get16(p + 2) != ETH_IP || p[4] != 6 || p[5] != 4)
		return;
	sip = net_get32(p + 14);
	tip = net_get32(p + 24);
	if (ns->ip && tip == ns->ip)
		arp_learn(ns, sip, p + 8);
	if (net_get16(p + 6) == 1 && ns->ip && tip == ns->ip)
		arp_send(ns, 2, p + 8, sip);
}

/* ---- IPv4 output ----------------------------------------------------------------- */

static bool is_local(const struct nstack *ns, u32 ip)
{
	return (ip >> 24) == 127 || (ns->ip && ip == ns->ip);
}

u32 ip_src_for(struct nstack *ns, u32 dst)
{
	return (dst >> 24) == 127 ? IP4(127, 0, 0, 1) : ns->ip;
}

static void lo_enqueue(struct nstack *ns, const u8 *ip_pkt, u32 len)
{
	u32 slot;
	u8 *copy;

	if (ns->lo_n == NET_LO_N)
		return;                         /* queue full: drop, TCP retransmits */
	copy = mos_zalloc(len);
	if (!copy)
		return;
	slot = (ns->lo_head + ns->lo_n++) % NET_LO_N;
	ns->lo[slot] = copy;
	ns->lo_len[slot] = (u16)len;
	memcpy(copy, ip_pkt, len);
}

/*
 * `f` has 14 bytes of Ethernet and 20 of IP header room in front of the
 * layer-4 data, which is l4len bytes long. Fills both headers and sends (or
 * parks the frame while ARP resolves the next hop).
 */
int ip_out(struct nstack *ns, u8 *f, u8 proto, u32 src, u32 dst, u32 l4len)
{
	u8 *ip = f + 14;
	u32 total = 20 + l4len, nexthop;
	struct arp_ent *e;
	static const u8 bcast[6] = { 255, 255, 255, 255, 255, 255 };

	if (total > NET_MTU)
		return -MERR_MSGSIZE;
	ip[0] = 0x45;
	ip[1] = 0;
	net_put16(ip + 2, (u16)total);
	net_put16(ip + 4, (u16)ns->ip_id++);
	net_put16(ip + 6, 0x4000);              /* DF */
	ip[8] = 64;
	ip[9] = proto;
	net_put16(ip + 10, 0);
	net_put32(ip + 12, src);
	net_put32(ip + 16, dst);
	net_put16(ip + 10, net_csum(ip, 20, 0));

	if (is_local(ns, dst)) {
		lo_enqueue(ns, ip, total);
		return 0;
	}
	if (!ns->dev)
		return -MERR_NETUNREACH;
	if (dst == 0xFFFFFFFFu || (ns->mask && ns->ip && dst == (ns->ip | ~ns->mask))) {
		eth_hdr(f, bcast, ns->dev->mac, ETH_IP);
		ns->tx_frames++;
		return ns->dev->send(ns->dev, f, 14 + total);
	}
	if (!ns->ip)
		return -MERR_NETUNREACH;
	nexthop = ((dst & ns->mask) == (ns->ip & ns->mask)) ? dst : ns->gw;
	if (!nexthop)
		return -MERR_NETUNREACH;
	e = arp_find(ns, nexthop);
	if (e && e->valid) {
		eth_hdr(f, e->mac, ns->dev->mac, ETH_IP);
		e->stamp = ns->now;
		ns->tx_frames++;
		return ns->dev->send(ns->dev, f, 14 + total);
	}
	/* Unresolved: keep this frame (replacing an older one) and ask. */
	e = arp_slot(ns, nexthop);
	mos_free(e->pend);
	e->ip = nexthop;
	e->valid = false;
	e->pend = mos_zalloc(14 + total);
	if (e->pend) {
		memcpy(e->pend, f, 14 + total);
		eth_hdr(e->pend, bcast, ns->dev->mac, ETH_IP);
		e->pend_len = (u16)(14 + total);
	}
	if (!e->tries) {
		e->tries = 1;
		e->stamp = ns->now;
		arp_send(ns, 1, NULL, nexthop);
	}
	return 0;
}

/* ---- ICMP ------------------------------------------------------------------------ */

int ns_ping(struct nstack *ns, u32 dst, u16 id, u16 seq)
{
	u8 f[NET_FRAME];
	u8 *icmp = f + 34;

	icmp[0] = 8;
	icmp[1] = 0;
	net_put16(icmp + 2, 0);
	net_put16(icmp + 4, id);
	net_put16(icmp + 6, seq);
	for (int i = 0; i < 32; i++)
		icmp[8 + i] = (u8)('a' + i % 23);
	net_put16(icmp + 2, net_csum(icmp, 40, 0));
	ns->ping_id = id;
	ns->ping_sent = ns->now;
	return ip_out(ns, f, 1, ip_src_for(ns, dst), dst, 40);
}

static void icmp_input(struct nstack *ns, u32 src, u32 dst, const u8 *p, u32 len)
{
	if (len < 8 || net_csum(p, len, 0))
		return;
	if (p[0] == 8 && p[1] == 0 && len <= NET_MTU - 20) {          /* echo request */
		u8 f[NET_FRAME];
		u8 *r = f + 34;

		memcpy(r, p, len);
		r[0] = 0;
		net_put16(r + 2, 0);
		net_put16(r + 2, net_csum(r, len, 0));
		ip_out(ns, f, 1, dst == 0xFFFFFFFFu ? ns->ip : dst, src, len);
	} else if (p[0] == 0 && p[1] == 0) {                          /* echo reply */
		if (net_get16(p + 4) == ns->ping_id) {
			ns->ping_replies++;
			ns->ping_seq_seen = net_get16(p + 6);
			ns->ping_rtt = ns->now - ns->ping_sent;
		}
	}
}

/* ---- UDP ------------------------------------------------------------------------- */

static void dhcp_input(struct nstack *ns, const u8 *p, u32 len);

static void udp_input(struct nstack *ns, u32 src, u32 dst, const u8 *p, u32 len)
{
	u16 sport, dport, ulen;
	struct nsock *best = NULL;
	struct udp_dgram *d;

	if (len < 8)
		return;
	ulen = net_get16(p + 4);
	if (ulen < 8 || ulen > len)
		return;
	len = ulen;
	if (net_get16(p + 6) && net_l4_csum(src, dst, 17, p, len))
		return;
	sport = net_get16(p);
	dport = net_get16(p + 2);
	if (dport == 68 && ns->dhcp_state != DHCP_OFF && sport == 67) {
		dhcp_input(ns, p + 8, len - 8);
		return;
	}
	for (int i = 0; i < NET_NSOCK; i++) {
		struct nsock *s = &ns->socks[i];

		if (s->type != NS_DGRAM || s->lport != dport)
			continue;
		if (s->lip && s->lip != dst && dst != 0xFFFFFFFFu)
			continue;
		if (s->connected && (s->rip != src || s->rport != sport))
			continue;
		if (!best || s->connected)
			best = s;
	}
	if (!best || best->uq_n >= NET_UDPQ_MAX || best->shut_rd)
		return;
	d = mos_zalloc(sizeof(*d) + len - 8);
	if (!d)
		return;
	d->ip = src;
	d->port = sport;
	d->len = (u16)(len - 8);
	memcpy(d->data, p + 8, len - 8);
	if (best->uq_tail)
		best->uq_tail->next = d;
	else
		best->uq = d;
	best->uq_tail = d;
	best->uq_n++;
	ns_wake(ns, best);
}

static int udp_emit(struct nstack *ns, u32 src, u16 sport, u32 dst, u16 dport,
		    const void *data, u32 len)
{
	u8 f[NET_FRAME];
	u8 *u = f + 34;
	u16 c;

	if (len > NET_MTU - 28)
		return -MERR_MSGSIZE;
	net_put16(u, sport);
	net_put16(u + 2, dport);
	net_put16(u + 4, (u16)(8 + len));
	net_put16(u + 6, 0);
	memcpy(u + 8, data, len);
	c = net_l4_csum(src, dst, 17, u, 8 + len);
	net_put16(u + 6, c ? c : 0xFFFF);
	return ip_out(ns, f, 17, src, dst, 8 + len);
}

long ns_sendto(struct nsock *s, const void *buf, u32 len, u32 ip, u16 port)
{
	struct nstack *ns = s->ns;
	u32 src;
	int rc;

	if (s->type != NS_DGRAM)
		return -MERR_NOTSUP;
	if (!ip || !port)
		return -MERR_INVAL;
	if (!s->lport)
		s->lport = ns_ephemeral(ns, NS_DGRAM);
	src = s->lip ? s->lip : ip_src_for(ns, ip);
	if (!src && ip != 0xFFFFFFFFu)
		return -MERR_NETUNREACH;
	rc = udp_emit(ns, src, s->lport, ip, port, buf, len);
	ns_lo_drain(ns);
	return rc < 0 ? rc : (long)len;
}

long ns_recvfrom(struct nsock *s, void *buf, u32 len, bool peek, u32 *ip, u16 *port)
{
	struct udp_dgram *d;
	u32 n;

	if (s->type == NS_STREAM) {
		if (ip)
			*ip = s->rip;
		if (port)
			*port = s->rport;
		return tcp_recv(s, buf, len, peek);
	}
	d = s->uq;
	if (!d) {
		if (s->err)
			return -ns_take_error(s);
		return s->shut_rd ? 0 : -MERR_AGAIN;
	}
	n = MOS_MIN(len, (u32)d->len);
	memcpy(buf, d->data, n);
	if (ip)
		*ip = d->ip;
	if (port)
		*port = d->port;
	if (!peek) {
		s->uq = d->next;
		if (!s->uq)
			s->uq_tail = NULL;
		s->uq_n--;
		mos_free(d);
	}
	return n;
}

/* ---- DHCP client ------------------------------------------------------------------ */

static void dhcp_send(struct nstack *ns, u8 type)
{
	u8 b[300];
	u8 *o;

	memset(b, 0, sizeof(b));
	b[0] = 1;                               /* BOOTREQUEST */
	b[1] = 1;
	b[2] = 6;
	net_put32(b + 4, ns->dhcp_xid);
	net_put16(b + 10, 0x8000);              /* ask for a broadcast reply */
	memcpy(b + 28, ns->dev->mac, 6);
	net_put32(b + 236, 0x63825363u);        /* magic cookie */
	o = b + 240;
	*o++ = 53; *o++ = 1; *o++ = type;
	*o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;      /* mask, router, DNS */
	if (type == 3) {
		*o++ = 50; *o++ = 4; net_put32(o, ns->dhcp_offer_ip); o += 4;
		*o++ = 54; *o++ = 4; net_put32(o, ns->dhcp_server); o += 4;
	}
	*o++ = 255;
	udp_emit(ns, 0, 68, 0xFFFFFFFFu, 67, b, sizeof(b));
}

void ns_dhcp_start(struct nstack *ns)
{
	ns->dhcp_xid = ns_rand(ns);
	ns->dhcp_state = DHCP_DISCOVER;
	ns->dhcp_tries = 0;
	ns->dhcp_deadline = ns->now + 1000;
	dhcp_send(ns, 1);
}

static void dhcp_input(struct nstack *ns, const u8 *p, u32 len)
{
	u32 yi, mask = 0, gw = 0, dns[2] = { 0, 0 }, lease = 0, server = 0, i;
	u8 type = 0;

	if (len < 241 || p[0] != 2 || net_get32(p + 4) != ns->dhcp_xid ||
	    memcmp(p + 28, ns->dev->mac, 6) || net_get32(p + 236) != 0x63825363u)
		return;
	yi = net_get32(p + 16);
	for (i = 240; i < len && p[i] != 255;) {
		u8 code = p[i], l;

		if (code == 0) {
			i++;
			continue;
		}
		if (i + 2 > len)
			break;
		l = p[i + 1];
		if (i + 2 + l > len)
			break;
		switch (code) {
		case 53: if (l >= 1) type = p[i + 2]; break;
		case 1:  if (l >= 4) mask = net_get32(p + i + 2); break;
		case 3:  if (l >= 4) gw = net_get32(p + i + 2); break;
		case 6:
			if (l >= 4)
				dns[0] = net_get32(p + i + 2);
			if (l >= 8)
				dns[1] = net_get32(p + i + 6);
			break;
		case 51: if (l >= 4) lease = net_get32(p + i + 2); break;
		case 54: if (l >= 4) server = net_get32(p + i + 2); break;
		}
		i += 2u + l;
	}
	if (ns->dhcp_state == DHCP_DISCOVER && type == 2 && yi) {
		ns->dhcp_offer_ip = yi;
		ns->dhcp_server = server;
		ns->dhcp_state = DHCP_REQUEST;
		ns->dhcp_tries = 0;
		ns->dhcp_deadline = ns->now + 1000;
		dhcp_send(ns, 3);
	} else if (ns->dhcp_state == DHCP_REQUEST && type == 5 && yi) {
		ns->ip = yi;
		ns->mask = mask ? mask : IP4(255, 255, 255, 0);
		ns->gw = gw;
		ns->dns[0] = dns[0];
		ns->dns[1] = dns[1];
		ns->lease_s = lease;
		ns->dhcp_state = DHCP_BOUND;
		if (ns->configured)
			ns->configured(ns);
	} else if (ns->dhcp_state == DHCP_REQUEST && type == 6) {
		ns_dhcp_start(ns);                      /* NAK: start over */
	}
}

static void dhcp_timer(struct nstack *ns)
{
	if ((ns->dhcp_state != DHCP_DISCOVER && ns->dhcp_state != DHCP_REQUEST) ||
	    ns->now < ns->dhcp_deadline)
		return;
	ns->dhcp_tries++;
	ns->dhcp_deadline = ns->now + (ns->dhcp_tries < 3 ? 1000u << ns->dhcp_tries : 4000);
	dhcp_send(ns, ns->dhcp_state == DHCP_DISCOVER ? 1 : 3);
}

/* ---- IPv4 input ------------------------------------------------------------------ */

static void ip_input(struct nstack *ns, const u8 *p, u32 len)
{
	u32 ihl, total, src, dst;
	u8 proto;
	bool bcast;

	if (len < 20 || (p[0] >> 4) != 4)
		goto bad;
	ihl = (u32)(p[0] & 15) * 4;
	total = net_get16(p + 2);
	if (ihl < 20 || total < ihl || total > len || net_csum(p, ihl, 0))
		goto bad;
	if (net_get16(p + 6) & 0x3FFF)          /* fragment: not reassembled */
		return;
	src = net_get32(p + 12);
	dst = net_get32(p + 16);
	bcast = dst == 0xFFFFFFFFu || (ns->mask && ns->ip && dst == (ns->ip | ~ns->mask));
	if (!(is_local(ns, dst) || bcast || (!ns->ip && p[9] == 17)))
		return;
	proto = p[9];
	p += ihl;
	total -= ihl;
	switch (proto) {
	case 1:  icmp_input(ns, src, dst, p, total); break;
	case 6:  if (!bcast)
			 tcp_input(ns, src, dst, p, total);
		 break;
	case 17: udp_input(ns, src, dst, p, total); break;
	default: break;
	}
	return;
bad:
	ns->rx_bad++;
}

void ns_lo_drain(struct nstack *ns)
{
	if (ns->in_drain)
		return;
	ns->in_drain = true;
	while (ns->lo_n) {
		u8 *pkt = ns->lo[ns->lo_head];
		u16 n = ns->lo_len[ns->lo_head];

		ns->lo[ns->lo_head] = NULL;
		ns->lo_head = (ns->lo_head + 1) % NET_LO_N;
		ns->lo_n--;
		ip_input(ns, pkt, n);
		mos_free(pkt);
	}
	ns->in_drain = false;
}

void ns_input(struct nstack *ns, const u8 *f, u32 len)
{
	u16 type;

	ns->rx_frames++;
	if (len < 14 || len > NET_FRAME + 4)
		return;
	if (memcmp(f, ns->dev->mac, 6) && !(f[0] & 1))
		return;                         /* not for us */
	type = net_get16(f + 12);
	if (type == ETH_ARP)
		arp_input(ns, f + 14, len - 14);
	else if (type == ETH_IP)
		ip_input(ns, f + 14, len - 14);
	ns_lo_drain(ns);
}

void ns_pump(struct nstack *ns, u64 now)
{
	ns->now = now;
	for (int i = 0; i < NET_ARP_N; i++) {
		struct arp_ent *e = &ns->arp[i];

		if (e->valid || !e->tries || now - e->stamp < 1000)
			continue;
		if (e->tries >= 4) {            /* give up: drop the queued frame */
			mos_free(e->pend);
			memset(e, 0, sizeof(*e));
			continue;
		}
		e->tries++;
		e->stamp = now;
		arp_send(ns, 1, NULL, e->ip);
	}
	dhcp_timer(ns);
	tcp_timers(ns);
	ns_lo_drain(ns);
}

/* ---- sockets --------------------------------------------------------------------- */

struct nsock *ns_sock_new(struct nstack *ns, int type)
{
	if (type != NS_STREAM && type != NS_DGRAM)
		return NULL;
	for (int i = 0; i < NET_NSOCK; i++) {
		struct nsock *s = &ns->socks[i];

		if (s->type)
			continue;
		memset(s, 0, sizeof(*s));
		s->ns = ns;
		s->type = (u8)type;
		s->mss = NET_MTU - 40;
		s->scap = s->rcap = NET_BUF_DEF;
		s->rto = 1000;
		return s;
	}
	return NULL;
}

bool ns_port_in_use(struct nstack *ns, u8 type, u32 ip, u16 port, const struct nsock *except)
{
	for (int i = 0; i < NET_NSOCK; i++) {
		const struct nsock *o = &ns->socks[i];
		bool reuse = except && except->reuseaddr;

		if (o == except || o->type != type || o->lport != port)
			continue;
		if (o->lip && ip && o->lip != ip)
			continue;
		if (type == NS_DGRAM) {
			if (reuse && o->reuseaddr)
				continue;
		} else if (o->state != TS_LISTEN && o->state != TS_CLOSED && reuse) {
			continue;               /* connections and TIME_WAIT do not block a reuser */
		}
		return true;
	}
	return false;
}

u16 ns_ephemeral(struct nstack *ns, u8 type)
{
	for (int tries = 0; tries < 20000; tries++) {
		u16 p = ns->ephem;
		bool used = false;

		ns->ephem = p == 65535 ? 49152 : (u16)(p + 1);
		for (int i = 0; i < NET_NSOCK && !used; i++)
			used = ns->socks[i].type == type && ns->socks[i].lport == p;
		if (!used)
			return p;
	}
	return 0;
}

void ns_sock_close(struct nsock *s)
{
	struct nstack *ns = s->ns;

	if (s->type == NS_STREAM) {
		tcp_close(s);
	} else {
		while (s->uq) {
			struct udp_dgram *d = s->uq;

			s->uq = d->next;
			mos_free(d);
		}
		s->type = 0;
	}
	ns_lo_drain(ns);
}

int ns_bind(struct nsock *s, u32 ip, u16 port)
{
	struct nstack *ns = s->ns;

	if (s->lport || (s->type == NS_STREAM && s->state != TS_CLOSED))
		return -MERR_INVAL;
	if (ip && !is_local(ns, ip) && ip != 0xFFFFFFFFu)
		return -MERR_ADDRNOTAVAIL;
	if (port && ns_port_in_use(ns, s->type, ip, port, s))
		return -MERR_ADDRINUSE;
	if (!port)
		port = ns_ephemeral(ns, s->type);
	s->lip = ip;
	s->lport = port;
	return 0;
}

int ns_listen(struct nsock *s, int backlog)
{
	if (s->type != NS_STREAM)
		return -MERR_NOTSUP;
	return tcp_listen(s, backlog);
}

int ns_connect(struct nsock *s, u32 ip, u16 port)
{
	struct nstack *ns = s->ns;
	int rc;

	if (!port)
		return -MERR_INVAL;
	if (!ip)
		ip = IP4(127, 0, 0, 1);
	if (s->type == NS_DGRAM) {
		if (!s->lport)
			s->lport = ns_ephemeral(ns, NS_DGRAM);
		s->rip = ip;
		s->rport = port;
		s->connected = true;
		return 0;
	}
	s->rip = ip;
	s->rport = port;
	rc = tcp_connect(s);
	ns_lo_drain(ns);
	return rc;
}

struct nsock *ns_accept(struct nsock *s)
{
	struct nsock *c = s->accq;

	if (s->type != NS_STREAM || s->state != TS_LISTEN || !c)
		return NULL;
	s->accq = c->acc_next;
	c->acc_next = NULL;
	c->parent = NULL;
	s->nacc--;
	return c;
}

long ns_send(struct nsock *s, const void *buf, u32 len)
{
	long rc;

	if (s->type == NS_DGRAM) {
		if (!s->connected)
			return -MERR_DESTADDRREQ;
		return ns_sendto(s, buf, len, s->rip, s->rport);
	}
	rc = tcp_send(s, buf, len);
	ns_lo_drain(s->ns);
	return rc;
}

long ns_recv(struct nsock *s, void *buf, u32 len, bool peek)
{
	long rc = ns_recvfrom(s, buf, len, peek, NULL, NULL);

	ns_lo_drain(s->ns);
	return rc;
}

int ns_shutdown(struct nsock *s, int how)
{
	if (s->type == NS_DGRAM) {
		if (how == 0 || how == 2)
			s->shut_rd = true;
		if (how == 1 || how == 2)
			s->shut_wr = true;
		return 0;
	}
	return tcp_shutdown(s, how);
}

u32 ns_poll(struct nsock *s)
{
	if (s->type == NS_STREAM)
		return tcp_poll(s);
	return (s->uq ? NS_POLLIN : 0) | NS_POLLOUT | (s->err ? NS_POLLERR : 0) |
	       (s->shut_rd ? NS_POLLIN : 0);
}

int ns_take_error(struct nsock *s)
{
	int e = s->err;

	s->err = 0;
	return e;
}

u32 ns_rx_avail(struct nsock *s)
{
	if (s->type == NS_STREAM)
		return s->rlen;
	return s->uq ? s->uq->len : 0;
}

int ns_set_buf(struct nsock *s, bool send, u32 size)
{
	if (size < NET_BUF_MIN)
		size = NET_BUF_MIN;
	if (size > NET_BUF_MAX)
		size = NET_BUF_MAX;
	if (send ? !s->sbuf : !s->rbuf) {
		if (send)
			s->scap = size;
		else
			s->rcap = size;
	}
	return 0;
}
