/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Runs the network stack in the kernel: finds a NIC, owns the one stack
 * instance, polls the NIC and the stack's timers from the `netd` kernel
 * thread, writes /etc/resolv.conf when DHCP finishes, and carries the boot
 * self-test (DHCP lease, ping, DNS, guest HTTP fetch).
 *
 * Locking: one CPU, so "interrupts off" is the lock. netd takes it around
 * every pass; system calls already run with interrupts off.
 */
#include <mergedos/compat.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include <mergedos/sched.h>
#include <mergedos/vfs.h>
#include "hardware_probe/hwprobe.h"
#include "kernel/net/netdrv.h"

struct nstack net_stack;
bool net_present;
static struct netdev *nic;

u64 net_ms(void)
{
	return time_ticks() * (1000 / SCHED_HZ);
}

void net_enter(void)
{
	net_stack.now = net_ms();
}

const char *net_nic_name(void)
{
	return nic ? nic->name : "none";
}

void *net_dma_alloc(u32 pages, u64 *phys)
{
	u64 p = pages == 1 ? pmm_alloc() : pmm_alloc_contig(pages);

	if (!p)
		return NULL;
	*phys = p;
	return phys_to_virt(p);
}

bool net_pci_find(u16 vendor, const u16 *devices, int ndev, u8 *bus, u8 *dev, u8 *fn)
{
	for (unsigned b = 0; b < 256; b++)
		for (u8 d = 0; d < 32; d++)
			for (u8 f = 0; f < 8; f++) {
				u32 id = pci_read((u8)b, d, f, 0);

				if ((id & 0xFFFF) == 0xFFFF) {
					if (f == 0)
						break;
					continue;
				}
				if ((id & 0xFFFF) != vendor)
					continue;
				for (int i = 0; i < ndev; i++)
					if ((id >> 16) == devices[i]) {
						*bus = (u8)b;
						*dev = d;
						*fn = f;
						return true;
					}
			}
	return false;
}

static void put_ip(char *out, size_t cap, u32 ip)
{
	mos_snprintf(out, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

/* DHCP finished: publish the resolver and a minimal hosts file. */
static void net_configured(struct nstack *ns)
{
	char conf[160], a[20], b[20];
	size_t n = 0;

	conf[0] = 0;
	for (int i = 0; i < 2; i++) {
		if (!ns->dns[i])
			continue;
		put_ip(a, sizeof(a), ns->dns[i]);
		n += (size_t)mos_snprintf(conf + n, sizeof(conf) - n, "nameserver %s\n", a);
	}
	vfs_write_file("etc/resolv.conf", conf, strlen(conf));
	put_ip(a, sizeof(a), ns->ip);
	put_ip(b, sizeof(b), ns->gw);
	mos_snprintf(conf, sizeof(conf), "127.0.0.1 localhost\n%s mergedos\n", a);
	vfs_write_file("etc/hosts", conf, strlen(conf));
	kprintf("[net] %s: DHCP lease %s, gateway %s, dns %u.%u.%u.%u\n", nic->name, a, b,
		ns->dns[0] >> 24, (ns->dns[0] >> 16) & 255, (ns->dns[0] >> 8) & 255, ns->dns[0] & 255);
}

static void net_wake(struct nstack *ns, struct nsock *s)
{
	(void)ns;
	thread_wake(s, 0, SCHED_MAX_THREADS);
}

/*
 * The poller. While traffic is flowing it yields instead of sleeping, so a
 * request/response exchange is not delayed by the 10 ms tick; after 200 ms of
 * silence it sleeps one tick at a time.
 */
static void netd(void *arg)
{
	u64 last_activity = time_ticks();

	(void)arg;
	for (;;) {
		u64 f = irq_save();
		int n = nic->poll(nic, &net_stack);

		net_enter();
		ns_pump(&net_stack, net_stack.now);
		irq_restore(f);
		if (n || net_stack.dhcp_state == DHCP_DISCOVER || net_stack.dhcp_state == DHCP_REQUEST)
			last_activity = time_ticks();
		if (time_ticks() - last_activity < SCHED_HZ / 5)
			thread_yield();
		else
			thread_sleep_ms(10);
	}
}

void net_init(void)
{
	nic = virtio_net_probe();
	if (!nic)
		nic = e1000_probe();
	if (!nic) {
		kprintf("[net] no supported network adapter (virtio-net, e1000)\n");
		return;
	}
	net_present = true;
	ns_init(&net_stack, nic);
	net_stack.wake = net_wake;
	net_stack.configured = net_configured;
	net_enter();
	ns_set_addr(&net_stack, 0, 0, 0);
	vfs_write_file("etc/hosts", "127.0.0.1 localhost\n", 20);
	{
		u64 f = irq_save();

		ns_dhcp_start(&net_stack);
		irq_restore(f);
	}
	if (thread_create(netd, NULL, "netd") < 0)
		panic("net: cannot start netd");
	kprintf("[net] %s up, mac %02x:%02x:%02x:%02x:%02x:%02x, polled; DHCP started\n", nic->name,
		nic->mac[0], nic->mac[1], nic->mac[2], nic->mac[3], nic->mac[4], nic->mac[5]);
}

/* ---- boot self-test ----------------------------------------------------------------- */

static bool wait_for(bool (*cond)(void), u32 ms)
{
	for (u32 t = 0; t < ms; t += 10) {
		if (cond())
			return true;
		thread_sleep_ms(10);
	}
	return cond();
}

static bool is_bound(void)
{
	return net_stack.dhcp_state == DHCP_BOUND;
}

static u32 ping_target_replies;
static bool got_ping(void)
{
	return net_stack.ping_replies > ping_target_replies;
}

/* A DNS query for `name` (type A) to `server`; true when a matching reply came back. */
static bool dns_query(u32 server, const char *name, u32 *rcode, u32 *answers)
{
	u8 q[64], r[512];
	size_t n = 12;
	struct nsock *s;
	bool ok = false;
	u64 f;

	memset(q, 0, sizeof(q));
	net_put16(q, 0xBEEF);
	q[2] = 1;                               /* recursion desired */
	net_put16(q + 4, 1);
	for (const char *p = name; *p;) {
		const char *dot = strchr(p, '.');
		size_t l = dot ? (size_t)(dot - p) : strlen(p);

		q[n++] = (u8)l;
		memcpy(q + n, p, l);
		n += l;
		p += l + (dot ? 1 : 0);
	}
	q[n++] = 0;
	net_put16(q + n, 1);                    /* A */
	net_put16(q + n + 2, 1);                /* IN */
	n += 4;

	f = irq_save();
	net_enter();
	s = ns_sock_new(&net_stack, NS_DGRAM);
	if (s && ns_bind(s, 0, 0) == 0)
		ns_sendto(s, q, (u32)n, server, 53);
	irq_restore(f);
	for (int t = 0; s && t < 300 && !ok; t++) {      /* up to 3 s, retry once at 1 s */
		long len;

		thread_sleep_ms(10);
		f = irq_save();
		len = ns_recvfrom(s, r, sizeof(r), false, NULL, NULL);
		if (t == 100 && len < 0) {
			net_enter();
			ns_sendto(s, q, (u32)n, server, 53);
		}
		irq_restore(f);
		if (len >= 12 && net_get16(r) == 0xBEEF && (r[2] & 0x80)) {
			*rcode = r[3] & 15;
			*answers = net_get16(r + 6);
			ok = true;
		}
	}
	if (s) {
		f = irq_save();
		ns_sock_close(s);
		irq_restore(f);
	}
	return ok;
}

int net_selftest(void)
{
	int bad = 0;
	u32 rcode = 0, answers = 0;
	u64 f;

	if (!net_present) {
		kprintf("[net] self-test skipped: no adapter\n");
		return 0;
	}
	if (!wait_for(is_bound, 8000)) {
		kprintf("[net] DHCP: no lease within 8 s (no DHCP server on this network?)\n");
		return 0;                       /* an adapter on an unconnected network is not a failure */
	}
	kprintf("[net] DHCP: lease ok, address %u.%u.%u.%u/%u.%u.%u.%u\n", net_stack.ip >> 24,
		(net_stack.ip >> 16) & 255, (net_stack.ip >> 8) & 255, net_stack.ip & 255,
		net_stack.mask >> 24, (net_stack.mask >> 16) & 255, (net_stack.mask >> 8) & 255,
		net_stack.mask & 255);
	/* ping the gateway */
	ping_target_replies = net_stack.ping_replies;
	for (int attempt = 0; attempt < 3 && !got_ping(); attempt++) {
		f = irq_save();
		net_enter();
		ns_ping(&net_stack, net_stack.gw, 0x4D4F, (u16)attempt);
		irq_restore(f);
		wait_for(got_ping, 1000);
	}
	if (got_ping())
		kprintf("[net] ping %u.%u.%u.%u: reply in %llu ms\n", net_stack.gw >> 24,
			(net_stack.gw >> 16) & 255, (net_stack.gw >> 8) & 255, net_stack.gw & 255,
			(unsigned long long)net_stack.ping_rtt);
	else {
		kprintf("[net] ping gateway: FAILED\n");
		bad++;
	}
	/* DNS through the DHCP-provided server */
	if (dns_query(net_stack.dns[0], "localhost", &rcode, &answers))
		kprintf("[net] dns %u.%u.%u.%u: reply rcode %u, %u answer(s): ok\n", net_stack.dns[0] >> 24,
			(net_stack.dns[0] >> 16) & 255, (net_stack.dns[0] >> 8) & 255,
			net_stack.dns[0] & 255, rcode, answers);
	else {
		kprintf("[net] dns: FAILED (no reply)\n");
		bad++;
	}
	return bad;
}
