/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_NET_H
#define MERGEDOS_NET_H
/*
 * MergedOS network stack core (portable: linked into the kernel and the host
 * unit tests). One struct nstack is one IP host with one interface plus
 * loopback. See docs/NETWORKING.md.
 *
 * Callers serialise access (the kernel runs every entry point with
 * interrupts off). Addresses and ports are in host byte order here.
 */
#include <mergedos/compat.h>

#define NET_MTU      1500
#define NET_FRAME    (14 + NET_MTU)
#define NET_NSOCK    48
#define NET_ARP_N    16
#define NET_LO_N     16
#define NET_UDPQ_MAX 16
#define NET_OOO_MAX  16
#define NET_BUF_DEF  32768
#define NET_BUF_MAX  65535
#define NET_BUF_MIN  2048

#define IP4(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

static inline u16 net_get16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline u32 net_get32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static inline void net_put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static inline void net_put32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* Linux poll bits, used as the readiness mask everywhere. */
#define NS_POLLIN   0x001
#define NS_POLLOUT  0x004
#define NS_POLLERR  0x008
#define NS_POLLHUP  0x010
#define NS_POLLRDHUP 0x2000

enum { NS_STREAM = 1, NS_DGRAM = 2 };

struct nstack;

struct netdev {
	char name[8];
	u8 mac[6];
	int (*send)(struct netdev *dev, const void *frame, u32 len);   /* 0 or -MERR_* */
	void *priv;
};

enum tcp_state {
	TS_CLOSED, TS_LISTEN, TS_SYN_SENT, TS_SYN_RCVD, TS_ESTAB, TS_FIN1, TS_FIN2,
	TS_CLOSING, TS_TIMEWAIT, TS_CLOSEWAIT, TS_LASTACK
};

struct ooo_seg {
	struct ooo_seg *next;
	u32 seq;
	u16 len;
	u8 data[];
};

struct udp_dgram {
	struct udp_dgram *next;
	u32 ip;
	u16 port, len;
	u8 data[];
};

struct nsock {
	struct nstack *ns;
	u8 type;                        /* NS_STREAM / NS_DGRAM, 0 = free slot */
	u8 state;                       /* enum tcp_state */
	bool reuseaddr, nodelay, keepalive, bcast;
	bool connected;                 /* UDP: default peer set */
	bool shut_rd, shut_wr;
	bool released;                  /* application closed it; the stack frees it */
	bool fin_rcvd, fin_wanted, fin_sent, fin_out, fin_acked;
	bool persist, timing, ack_now;
	u32 lip, rip;
	u16 lport, rport;
	int err;                        /* pending -MERR_* style error, stored positive */
	void *user;                     /* the socket layer's handle */

	/* UDP receive queue */
	struct udp_dgram *uq, *uq_tail;
	u32 uq_n;

	/* TCP */
	u32 iss, snd_una, snd_nxt, snd_max, fin_seq, snd_wnd, snd_wl1, snd_wl2;
	u32 irs, rcv_nxt;
	u32 cwnd, ssthresh, dupacks, mss;
	u32 rto, srtt, rttvar, retries;
	u32 rtt_seq;
	u64 rtt_time, deadline, tw_deadline;    /* deadline 0 = no timer */
	u32 adv_edge;                   /* right window edge last advertised */
	u8 *sbuf, *rbuf;
	u32 scap, shead, slen;
	u32 rcap, rhead, rlen;
	struct ooo_seg *ooo;
	u32 ooo_n;
	struct nsock *parent, *accq, *acc_next;
	u32 backlog, nacc;
};

struct arp_ent {
	u32 ip;
	u8 mac[6];
	bool valid;
	u8 tries;
	u64 stamp;                      /* last request (pending) / last use */
	u8 *pend;                       /* one queued frame while resolving */
	u16 pend_len;
};

enum { DHCP_OFF, DHCP_DISCOVER, DHCP_REQUEST, DHCP_BOUND };

struct nstack {
	struct netdev *dev;
	u32 ip, mask, gw, dns[2], dhcp_server, lease_s;
	u64 now;                        /* ms; set by the caller before every entry */
	u32 seed;
	u16 ephem;
	u32 ip_id;
	struct arp_ent arp[NET_ARP_N];
	struct nsock socks[NET_NSOCK];
	u8 *lo[NET_LO_N];
	u16 lo_len[NET_LO_N];
	u32 lo_head, lo_n;
	bool in_drain;
	/* ping */
	u16 ping_id, ping_seq_seen;
	u32 ping_replies;
	u64 ping_sent, ping_rtt;
	/* DHCP */
	u8 dhcp_state;
	u32 dhcp_xid, dhcp_offer_ip, dhcp_tries;
	u64 dhcp_deadline;
	/* hooks */
	void (*wake)(struct nstack *ns, struct nsock *s);
	void (*configured)(struct nstack *ns);
	/* statistics */
	u64 rx_frames, tx_frames, rx_bad, tcp_rexmit;
};

/* netcore.c */
u16  net_csum(const void *data, u32 len, u32 init);
u16  net_l4_csum(u32 src, u32 dst, u8 proto, const void *seg, u32 len);
void ns_init(struct nstack *ns, struct netdev *dev);
void ns_destroy(struct nstack *ns);
void ns_input(struct nstack *ns, const u8 *frame, u32 len);
void ns_pump(struct nstack *ns, u64 now_ms);
void ns_set_addr(struct nstack *ns, u32 ip, u32 mask, u32 gw);
void ns_dhcp_start(struct nstack *ns);
int  ns_ping(struct nstack *ns, u32 dst, u16 id, u16 seq);
u32  ns_rand(struct nstack *ns);

/* Layer-4 entry points used by netcore.c and tcp.c */
int  ip_out(struct nstack *ns, u8 *frame, u8 proto, u32 src, u32 dst, u32 l4len);
u32  ip_src_for(struct nstack *ns, u32 dst);
void ns_lo_drain(struct nstack *ns);
void ns_wake(struct nstack *ns, struct nsock *s);

/* Sockets (netcore.c for UDP and shared parts, tcp.c for streams) */
struct nsock *ns_sock_new(struct nstack *ns, int type);
void ns_sock_close(struct nsock *s);
int  ns_bind(struct nsock *s, u32 ip, u16 port);
int  ns_listen(struct nsock *s, int backlog);
int  ns_connect(struct nsock *s, u32 ip, u16 port);       /* 0, or -MERR_INPROGRESS */
struct nsock *ns_accept(struct nsock *s);
long ns_send(struct nsock *s, const void *buf, u32 len);
long ns_recv(struct nsock *s, void *buf, u32 len, bool peek);
long ns_sendto(struct nsock *s, const void *buf, u32 len, u32 ip, u16 port);
long ns_recvfrom(struct nsock *s, void *buf, u32 len, bool peek, u32 *ip, u16 *port);
int  ns_shutdown(struct nsock *s, int how);                /* 0 RD, 1 WR, 2 both */
u32  ns_poll(struct nsock *s);
int  ns_take_error(struct nsock *s);                       /* positive MERR_*, clears */
u32  ns_rx_avail(struct nsock *s);
int  ns_set_buf(struct nsock *s, bool send, u32 size);
bool ns_port_in_use(struct nstack *ns, u8 type, u32 ip, u16 port, const struct nsock *except);
u16  ns_ephemeral(struct nstack *ns, u8 type);

/* tcp.c */
void tcp_input(struct nstack *ns, u32 src, u32 dst, const u8 *seg, u32 len);
void tcp_timers(struct nstack *ns);
int  tcp_connect(struct nsock *s);
int  tcp_listen(struct nsock *s, int backlog);
long tcp_send(struct nsock *s, const u8 *buf, u32 len);
long tcp_recv(struct nsock *s, u8 *buf, u32 len, bool peek);
void tcp_close(struct nsock *s);
int  tcp_shutdown(struct nsock *s, int how);
void tcp_free(struct nsock *s);
u32  tcp_poll(struct nsock *s);

#endif /* MERGEDOS_NET_H */
