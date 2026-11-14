/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * nettest.linux - AF_INET sockets through raw Linux system calls. Freestanding,
 * a genuine static Linux binary.
 *
 * usage: nettest <http-host-ip> <port> <dns-ip>
 * 1. TCP over loopback: bind/listen, non-blocking connect (EINPROGRESS),
 *    accept4, echo, getsockname/getpeername, FIONREAD, shutdown.
 * 2. UDP over loopback.
 * 3. HTTP GET http://<host>:<port>/ and look for MERGEDOS-NET-OK in the body.
 * 4. A DNS query to <dns-ip>:53 gets an answer with the same id.
 * Exits with the failed step number, or 0 after printing "nettest: OK".
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f;
	long ret;

	__asm__ volatile("syscall" : "=a"(ret)
			 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}
#define sys(n, a, b, c, d, e, f) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

#define SYS_write 1
#define SYS_close 3
#define SYS_ioctl 16
#define SYS_yield 24
#define SYS_socket 41
#define SYS_connect 42
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_shutdown 48
#define SYS_bind 49
#define SYS_listen 50
#define SYS_getsockname 51
#define SYS_getpeername 52
#define SYS_setsockopt 54
#define SYS_getsockopt 55
#define SYS_exit 60
#define SYS_accept4 288
#define EINPROGRESS 115
#define EISCONN 106
#define EAGAIN 11

struct sa_in {
	u16 family, port;
	u32 addr;
	u8 zero[8];
};

static u16 be16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static u32 be32(u32 v) { return __builtin_bswap32(v); }
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

static void say(const char *s) { sys(SYS_write, 1, s, slen(s), 0, 0, 0); }

static void say_num(long v)
{
	char n[24];
	int i = 23;
	int neg = v < 0;
	u64 u = neg ? (u64)-v : (u64)v;

	n[i] = 0;
	do { n[--i] = (char)('0' + u % 10); u /= 10; } while (u);
	if (neg)
		n[--i] = '-';
	say(n + i);
}

static int fail(int step, const char *why)
{
	say("nettest: FAIL ");
	say(why);
	say("\n");
	return step;
}

static u32 parse_ip(const char *s)
{
	u32 ip = 0, v = 0;

	for (;; s++) {
		if (*s >= '0' && *s <= '9') {
			v = v * 10 + (u32)(*s - '0');
		} else {
			ip = (ip << 8) | v;
			v = 0;
			if (!*s)
				break;
		}
	}
	return ip;
}

static u32 parse_num(const char *s)
{
	u32 v = 0;

	while (*s >= '0' && *s <= '9')
		v = v * 10 + (u32)(*s++ - '0');
	return v;
}

static void mkaddr(struct sa_in *a, u32 ip, u16 port)
{
	char *p = (char *)a;

	for (unsigned i = 0; i < sizeof(*a); i++)
		p[i] = 0;
	a->family = 2;
	a->port = be16(port);
	a->addr = be32(ip);
}

static int has(const char *hay, u32 n, const char *needle)
{
	u32 m = slen(needle);

	for (u32 i = 0; i + m <= n; i++) {
		u32 j = 0;

		while (j < m && hay[i + j] == needle[j])
			j++;
		if (j == m)
			return 1;
	}
	return 0;
}

static int loopback_tcp(void)
{
	struct sa_in a, b;
	char buf[64];
	u32 one = 1, len = sizeof(a), avail = 0, nb = 1;
	int ls, cs, as, err = 0;
	long rc;

	ls = (int)sys(SYS_socket, 2, 1, 0, 0, 0, 0);
	if (ls < 0)
		return fail(1, "socket");
	sys(SYS_setsockopt, ls, 1, 2, &one, 4, 0);                      /* SO_REUSEADDR */
	mkaddr(&a, 0x7F000001, 4567);
	if (sys(SYS_bind, ls, &a, 16, 0, 0, 0) || sys(SYS_listen, ls, 4, 0, 0, 0, 0))
		return fail(1, "bind/listen");
	cs = (int)sys(SYS_socket, 2, 1 | 0x800, 0, 0, 0, 0);            /* SOCK_NONBLOCK */
	rc = sys(SYS_connect, cs, &a, 16, 0, 0, 0);
	if (rc != 0 && rc != -EINPROGRESS)
		return fail(1, "nonblocking connect");
	for (int i = 0; i < 1000; i++) {
		u32 el = 4;

		err = -1;
		sys(SYS_getsockopt, cs, 1, 4, &err, &el, 0);            /* SO_ERROR */
		rc = sys(SYS_connect, cs, &a, 16, 0, 0, 0);
		if (rc == -EISCONN || rc == 0)
			break;
		sys(SYS_yield, 0, 0, 0, 0, 0, 0);
	}
	if (rc != -EISCONN && rc != 0)
		return fail(1, "connect did not complete");
	as = (int)sys(SYS_accept4, ls, &b, &len, 0, 0, 0);
	if (as < 0 || len != 16 || b.family != 2 || be32(b.addr) != 0x7F000001)
		return fail(1, "accept4");
	if (sys(SYS_sendto, cs, "ping", 4, 0, 0, 0) != 4)
		return fail(1, "send");
	rc = sys(SYS_recvfrom, as, buf, sizeof(buf), 0, 0, 0);
	if (rc != 4 || buf[0] != 'p')
		return fail(1, "recv");
	{
		struct { int fd; short events, revents; } pf = { cs, 1, 0 };

		if (sys(7, &pf, 1, 0, 0, 0, 0) != 0)                    /* poll: nothing to read yet */
			return fail(1, "poll idle");
		sys(SYS_write, as, "pong!", 5, 0, 0, 0);
		if (sys(7, &pf, 1, 2000, 0, 0, 0) != 1 || !(pf.revents & 1))
			return fail(1, "poll POLLIN");
	}
	for (int i = 0; i < 100 && sys(SYS_ioctl, cs, 0x541B, &avail, 0, 0, 0) == 0 && !avail; i++)
		sys(SYS_yield, 0, 0, 0, 0, 0, 0);
	if (avail != 5)
		return fail(1, "FIONREAD");
	if (sys(SYS_recvfrom, cs, buf, 3, 0x2, 0, 0) != 3)             /* MSG_PEEK */
		return fail(1, "peek");
	if (sys(SYS_recvfrom, cs, buf, sizeof(buf), 0, 0, 0) != 5)
		return fail(1, "recv2");
	len = 16;
	if (sys(SYS_getpeername, cs, &b, &len, 0, 0, 0) || be16(b.port) != 4567)
		return fail(1, "getpeername");
	len = 16;
	if (sys(SYS_getsockname, cs, &b, &len, 0, 0, 0) || be16(b.port) < 49152)
		return fail(1, "getsockname");
	rc = sys(SYS_recvfrom, cs, buf, 1, 0, 0, 0);                    /* nonblocking: nothing yet */
	if (rc != -EAGAIN)
		return fail(1, "EAGAIN");
	sys(SYS_shutdown, as, 1, 0, 0, 0, 0);
	for (int i = 0; i < 1000; i++) {
		rc = sys(SYS_recvfrom, cs, buf, 1, 0, 0, 0);
		if (rc != -EAGAIN)
			break;
		sys(SYS_yield, 0, 0, 0, 0, 0, 0);
	}
	if (rc != 0)
		return fail(1, "EOF after shutdown");
	(void)nb;
	sys(SYS_close, as, 0, 0, 0, 0, 0);
	sys(SYS_close, cs, 0, 0, 0, 0, 0);
	sys(SYS_close, ls, 0, 0, 0, 0, 0);
	return 0;
}

static int loopback_udp(void)
{
	struct sa_in a, from;
	char buf[32];
	u32 len = 16;
	int r = (int)sys(SYS_socket, 2, 2, 0, 0, 0, 0), s = (int)sys(SYS_socket, 2, 2, 0, 0, 0, 0);

	mkaddr(&a, 0x7F000001, 5353);
	if (r < 0 || s < 0 || sys(SYS_bind, r, &a, 16, 0, 0, 0))
		return fail(2, "udp bind");
	if (sys(SYS_sendto, s, "dgram", 5, 0, &a, 16) != 5)
		return fail(2, "udp sendto");
	if (sys(SYS_recvfrom, r, buf, sizeof(buf), 0, &from, &len) != 5 || buf[0] != 'd' ||
	    be32(from.addr) != 0x7F000001)
		return fail(2, "udp recvfrom");
	sys(SYS_close, r, 0, 0, 0, 0, 0);
	sys(SYS_close, s, 0, 0, 0, 0, 0);
	return 0;
}

static int http_get(u32 ip, u16 port)
{
	static char resp[4096];
	const char *req = "GET /index.html HTTP/1.0\r\nHost: mergedos\r\nConnection: close\r\n\r\n";
	struct sa_in a;
	u32 total = 0;
	int s = (int)sys(SYS_socket, 2, 1, 0, 0, 0, 0);
	long rc;

	mkaddr(&a, ip, port);
	rc = s < 0 ? s : sys(SYS_connect, s, &a, 16, 0, 0, 0);
	if (rc) {
		say("nettest: connect rc ");
		say_num(rc);
		say(" ip ");
		say_num(ip);
		say(" port ");
		say_num(port);
		say("\n");
		return fail(3, "http connect");
	}
	if (sys(SYS_write, s, req, slen(req), 0, 0, 0) != (long)slen(req))
		return fail(3, "http send");
	while ((rc = sys(SYS_recvfrom, s, resp + total, sizeof(resp) - total, 0, 0, 0)) > 0)
		total += (u32)rc;
	sys(SYS_close, s, 0, 0, 0, 0, 0);
	if (!has(resp, total, "200 OK"))
		return fail(3, "http status");
	if (!has(resp, total, "MERGEDOS-NET-OK"))
		return fail(3, "http body");
	say("nettest: http fetched ");
	{
		char n[12];
		int i = 11;
		u32 v = total;

		n[i] = 0;
		do { n[--i] = (char)('0' + v % 10); v /= 10; } while (v);
		say(n + i);
	}
	say(" bytes\n");
	return 0;
}

static int dns(u32 server)
{
	u8 q[] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 9, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't',
		   0, 0, 1, 0, 1 };
	u8 r[512];
	struct sa_in a;
	int s = (int)sys(SYS_socket, 2, 2, 0, 0, 0, 0);
	long n = -1;

	mkaddr(&a, server, 53);
	for (int attempt = 0; attempt < 3 && n < 12; attempt++) {
		sys(SYS_sendto, s, q, sizeof(q), 0, &a, 16);
		for (int i = 0; i < 100000 && n < 12; i++) {
			n = sys(SYS_recvfrom, s, r, sizeof(r), 0x40, 0, 0);     /* MSG_DONTWAIT */
			if (n < 12)
				sys(SYS_yield, 0, 0, 0, 0, 0, 0);
		}
	}
	sys(SYS_close, s, 0, 0, 0, 0, 0);
	if (n < 12 || r[0] != 0x12 || r[1] != 0x34 || !(r[2] & 0x80))
		return fail(4, "dns");
	return 0;
}

int main(int argc, char **argv)
{
	int rc;

	if (argc < 4)
		return fail(9, "usage");
	if ((rc = loopback_tcp()) || (rc = loopback_udp()))
		return rc;
	if ((rc = http_get(parse_ip(argv[1]), (u16)parse_num(argv[2]))))
		return rc;
	if ((rc = dns(parse_ip(argv[3]))))
		return rc;
	say("nettest: OK\n");
	return 0;
}

__asm__(".globl _start\n_start:\n\tmov %rsp, %rdi\n\tcall start_c\n");

void start_c(long *sp)
{
	int rc = main((int)sp[0], (char **)(sp + 1));

	sys(SYS_exit, rc, 0, 0, 0, 0, 0);
	for (;;)
		;
}
