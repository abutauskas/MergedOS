/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Intel 8254x ("e1000") driver: polled, MMIO. QEMU's default NIC (82540EM).
 * Written from Intel's 8254x software developer's manual: legacy receive and
 * transmit descriptors, 32 receive buffers and 8 transmit slots of 2 KiB.
 * No interrupts, no offloads, no VLANs. Registers are reached through the
 * boot direct map (cacheable), fine under emulation; real hardware needs an
 * uncached mapping first (same caveat as the AHCI driver).
 */
#include <mergedos/compat.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include "hardware_probe/hwprobe.h"
#include "kernel/net/netdrv.h"

#define R_CTRL   0x0000
#define R_STATUS 0x0008
#define R_IMC    0x00D8
#define R_RCTL   0x0100
#define R_TCTL   0x0400
#define R_TIPG   0x0410
#define R_RDBAL  0x2800
#define R_RDBAH  0x2804
#define R_RDLEN  0x2808
#define R_RDH    0x2810
#define R_RDT    0x2818
#define R_TDBAL  0x3800
#define R_TDBAH  0x3804
#define R_TDLEN  0x3808
#define R_TDH    0x3810
#define R_TDT    0x3818
#define R_MTA    0x5200
#define R_RAL0   0x5400
#define R_RAH0   0x5404

#define CTRL_SLU (1u << 6)
#define CTRL_RST (1u << 26)
#define RCTL_EN  (1u << 1)
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN  (1u << 1)
#define TCTL_PSP (1u << 3)
#define DD       1
#define EOP      1
#define IFCS     2
#define RS       8

#define BUF      2048
#define RX_N     32
#define TX_N     8

struct rxd {
	u64 addr;
	u16 len, csum;
	u8 status, errors;
	u16 special;
};

struct txd {
	u64 addr;
	u16 len;
	u8 cso, cmd, status, css;
	u16 special;
};

static struct e1000 {
	volatile u8 *mmio;
	struct rxd *rx;
	struct txd *tx;
	u8 *rxbuf, *txbuf;
	u64 rxphys, txphys;
	u32 rx_next, tx_tail;
	struct netdev dev;
} e;

static inline u32 rd(u32 r) { return *(volatile u32 *)(e.mmio + r); }
static inline void wr(u32 r, u32 v) { *(volatile u32 *)(e.mmio + r) = v; }

static int e1000_send(struct netdev *d, const void *frame, u32 len)
{
	struct txd *t = &e.tx[e.tx_tail];

	(void)d;
	if (len > BUF || len < 14)
		return -MERR_MSGSIZE;
	for (int spin = 0; spin < 100000 && !(t->status & DD); spin++)
		cpu_relax();
	if (!(t->status & DD))
		return -MERR_AGAIN;
	memcpy(e.txbuf + (u64)e.tx_tail * BUF, frame, len);
	t->addr = e.txphys + (u64)e.tx_tail * BUF;
	t->len = (u16)(len < 60 ? 60 : len);                    /* pad to the Ethernet minimum */
	t->cmd = EOP | IFCS | RS;
	t->status = 0;
	__asm__ volatile("mfence" : : : "memory");
	e.tx_tail = (e.tx_tail + 1) % TX_N;
	wr(R_TDT, e.tx_tail);
	return 0;
}

static int e1000_poll(struct netdev *d, struct nstack *ns)
{
	int n = 0;

	(void)d;
	for (;;) {
		struct rxd *r = &e.rx[e.rx_next];

		if (!(r->status & DD))
			break;
		if (r->len > 0 && r->len <= BUF && !r->errors)
			ns_input(ns, e.rxbuf + (u64)e.rx_next * BUF, r->len);
		r->status = 0;
		wr(R_RDT, e.rx_next);
		e.rx_next = (e.rx_next + 1) % RX_N;
		n++;
	}
	return n;
}

struct netdev *e1000_probe(void)
{
	/* 82540EM, 82545EM (copper), 82543GC, 82541, 82544 */
	static const u16 ids[] = { 0x100E, 0x100F, 0x1004, 0x1019, 0x1010 };
	u8 bus, dev, fn;
	u32 bar0, ral, rah;
	u64 ringphys;
	u8 *ring;

	if (!net_pci_find(0x8086, ids, ARRAY_SIZE(ids), &bus, &dev, &fn))
		return NULL;
	bar0 = pci_read(bus, dev, fn, 0x10);
	if ((bar0 & 1) || !(bar0 & ~0xFu) || (bar0 & ~0xFu) >= DMAP_LIMIT - 0x20000)
		return NULL;
	pci_write(bus, dev, fn, 0x04, (pci_read(bus, dev, fn, 0x04) & 0xFFFF) | 0x6);  /* MMIO + bus master */
	e.mmio = phys_to_virt(bar0 & ~0xFu);

	wr(R_IMC, 0xFFFFFFFFu);
	wr(R_CTRL, rd(R_CTRL) | CTRL_RST);
	for (int i = 0; i < 100000 && (rd(R_CTRL) & CTRL_RST); i++)
		cpu_relax();
	wr(R_IMC, 0xFFFFFFFFu);
	wr(R_CTRL, (rd(R_CTRL) | CTRL_SLU) & ~(1u << 3 | 1u << 31 | 1u << 30));   /* link up; no VLAN/PHY reset */

	ring = net_dma_alloc(1, &ringphys);
	e.rxbuf = net_dma_alloc(RX_N * BUF / 4096, &e.rxphys);
	e.txbuf = net_dma_alloc(TX_N * BUF / 4096, &e.txphys);
	if (!ring || !e.rxbuf || !e.txbuf)
		return NULL;
	e.rx = (struct rxd *)ring;
	e.tx = (struct txd *)(ring + 1024);
	for (int i = 0; i < RX_N; i++)
		e.rx[i].addr = e.rxphys + (u64)i * BUF;
	for (int i = 0; i < TX_N; i++)
		e.tx[i].status = DD;                            /* free */
	for (u32 i = 0; i < 128; i++)
		wr(R_MTA + i * 4, 0);

	ral = rd(R_RAL0);
	rah = rd(R_RAH0);
	e.dev.mac[0] = (u8)ral;
	e.dev.mac[1] = (u8)(ral >> 8);
	e.dev.mac[2] = (u8)(ral >> 16);
	e.dev.mac[3] = (u8)(ral >> 24);
	e.dev.mac[4] = (u8)rah;
	e.dev.mac[5] = (u8)(rah >> 8);
	wr(R_RAH0, rah | (1u << 31));                           /* address valid */

	wr(R_RDBAL, (u32)ringphys);
	wr(R_RDBAH, (u32)(ringphys >> 32));
	wr(R_RDLEN, RX_N * sizeof(struct rxd));
	wr(R_RDH, 0);
	wr(R_RDT, RX_N - 1);
	wr(R_TDBAL, (u32)(ringphys + 1024));
	wr(R_TDBAH, (u32)((ringphys + 1024) >> 32));
	wr(R_TDLEN, TX_N * sizeof(struct txd));
	wr(R_TDH, 0);
	wr(R_TDT, 0);
	wr(R_TIPG, 0x0060200A);
	wr(R_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << 4) | (0x40u << 12));
	wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);            /* 2048-byte buffers */

	mos_strlcpy(e.dev.name, "e1000", sizeof(e.dev.name));
	e.dev.send = e1000_send;
	e.dev.poll = e1000_poll;
	e.dev.priv = &e;
	return &e.dev;
}
