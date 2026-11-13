/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * virtio-net, legacy (transitional) PCI interface through the I/O BAR, polled.
 * Written from the virtio 1.x specification, section 4.1.4.8 / 5.1.
 *
 * Queue 0 receives, queue 1 transmits. 32 receive buffers and 8 transmit
 * slots of 2 KiB (10-byte virtio header + frame) keep the footprint small;
 * the poller runs often enough that this is plenty. No offloads, no
 * mergeable buffers, no interrupts. Modern-only (device 0x1041) devices are
 * not driven yet.
 */
#include <mergedos/compat.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include "hardware_probe/hwprobe.h"
#include "kernel/net/netdrv.h"

#define VIRTIO_VENDOR  0x1AF4
#define VIRTIO_NET_LEGACY 0x1000

#define R_DEV_FEAT 0x00
#define R_GST_FEAT 0x04
#define R_Q_PFN    0x08
#define R_Q_SIZE   0x0C
#define R_Q_SEL    0x0E
#define R_Q_NOTIFY 0x10
#define R_STATUS   0x12
#define R_MAC      0x14
#define F_MAC      (1u << 5)
#define ST_ACK     1
#define ST_DRIVER  2
#define ST_OK      4
#define HDR        10
#define BUF        2048
#define RX_BUFS    32
#define TX_SLOTS   8
#define VRING_WRITE 2

struct vdesc {
	u64 addr;
	u32 len;
	u16 flags, next;
};

struct vq {
	u16 size, last_used, avail_idx;
	struct vdesc *desc;
	volatile u16 *avail;            /* flags, idx, ring[] */
	volatile u8 *used;              /* flags(2), idx(2), {id(4), len(4)}[] */
};

struct vnet {
	u16 io;
	struct vq rx, tx;
	u8 *rxbuf, *txbuf;
	u64 rxphys, txphys;
	u8 tx_busy;                     /* bitmask of slots on the wire */
	struct netdev dev;
};

static struct vnet vn;

static inline void barrier(void)
{
	__asm__ volatile("mfence" : : : "memory");
}

static bool vq_setup(struct vnet *v, struct vq *q, u16 index)
{
	u32 avail_off, used_off, bytes, pages;
	u64 phys;
	u8 *mem;

	outw(v->io + R_Q_SEL, index);
	q->size = inw(v->io + R_Q_SIZE);
	if (!q->size || (q->size & (q->size - 1)))
		return false;
	avail_off = 16u * q->size;
	used_off = (u32)ALIGN_UP(avail_off + 6u + 2u * q->size, 4096);
	bytes = used_off + 6u + 8u * q->size;
	pages = (bytes + 4095) / 4096;
	mem = net_dma_alloc(pages, &phys);
	if (!mem)
		return false;
	q->desc = (struct vdesc *)mem;
	q->avail = (volatile u16 *)(mem + avail_off);
	q->used = mem + used_off;
	outl(v->io + R_Q_PFN, (u32)(phys >> 12));
	return true;
}

static void rx_post(struct vnet *v, u16 id)
{
	struct vq *q = &v->rx;

	q->avail[2 + q->avail_idx % q->size] = id;
	barrier();
	q->avail_idx++;
	q->avail[1] = q->avail_idx;
}

static int vnet_send(struct netdev *d, const void *frame, u32 len)
{
	struct vnet *v = d->priv;
	struct vq *q = &v->tx;
	int slot = -1;

	if (len > BUF - HDR)
		return -MERR_MSGSIZE;
	for (int spin = 0; spin < 20000 && slot < 0; spin++) {
		while (q->last_used != *(volatile u16 *)(q->used + 2)) {
			u32 id = *(volatile u32 *)(q->used + 4 + 8u * (q->last_used % q->size));

			v->tx_busy &= (u8)~(1u << (id % TX_SLOTS));
			q->last_used++;
		}
		for (int i = 0; i < TX_SLOTS; i++)
			if (!(v->tx_busy & (1u << i))) {
				slot = i;
				break;
			}
		if (slot < 0)
			cpu_relax();
	}
	if (slot < 0)
		return -MERR_AGAIN;
	memset(v->txbuf + (u32)slot * BUF, 0, HDR);
	memcpy(v->txbuf + (u32)slot * BUF + HDR, frame, len);
	q->desc[slot].addr = v->txphys + (u64)slot * BUF;
	q->desc[slot].len = HDR + len;
	q->desc[slot].flags = 0;
	v->tx_busy |= (u8)(1u << slot);
	q->avail[2 + q->avail_idx % q->size] = (u16)slot;
	barrier();
	q->avail_idx++;
	q->avail[1] = q->avail_idx;
	barrier();
	outw(v->io + R_Q_NOTIFY, 1);
	return 0;
}

static int vnet_poll(struct netdev *d, struct nstack *ns)
{
	struct vnet *v = d->priv;
	struct vq *q = &v->rx;
	int n = 0;

	barrier();
	while (q->last_used != *(volatile u16 *)(q->used + 2)) {
		u32 slot = q->last_used % q->size;
		u32 id = *(volatile u32 *)(q->used + 4 + 8 * slot);
		u32 len = *(volatile u32 *)(q->used + 8 + 8 * slot);

		q->last_used++;
		if (id < RX_BUFS && len > HDR && len <= BUF)
			ns_input(ns, v->rxbuf + id * BUF + HDR, len - HDR);
		if (id < RX_BUFS)
			rx_post(v, (u16)id);
		n++;
	}
	if (n)
		outw(v->io + R_Q_NOTIFY, 0);
	return n;
}

struct netdev *virtio_net_probe(void)
{
	static const u16 ids[] = { VIRTIO_NET_LEGACY };
	struct vnet *v = &vn;
	u8 bus, dev, fn;
	u32 bar0, feat;

	if (!net_pci_find(VIRTIO_VENDOR, ids, 1, &bus, &dev, &fn))
		return NULL;
	bar0 = pci_read(bus, dev, fn, 0x10);
	if (!(bar0 & 1))
		return NULL;                    /* memory BAR: not the legacy layout */
	v->io = (u16)(bar0 & ~3u);
	pci_write(bus, dev, fn, 0x04, (pci_read(bus, dev, fn, 0x04) & 0xFFFF) | 0x5);  /* I/O + bus master */

	outb(v->io + R_STATUS, 0);                              /* reset */
	outb(v->io + R_STATUS, ST_ACK);
	outb(v->io + R_STATUS, ST_ACK | ST_DRIVER);
	feat = inl(v->io + R_DEV_FEAT);
	outl(v->io + R_GST_FEAT, feat & F_MAC);
	if (!vq_setup(v, &v->rx, 0) || !vq_setup(v, &v->tx, 1))
		return NULL;
	v->rxbuf = net_dma_alloc(RX_BUFS * BUF / 4096, &v->rxphys);
	v->txbuf = net_dma_alloc(TX_SLOTS * BUF / 4096, &v->txphys);
	if (!v->rxbuf || !v->txbuf)
		return NULL;
	v->rx.avail[0] = 1;                                     /* no interrupts */
	v->tx.avail[0] = 1;
	for (u16 i = 0; i < RX_BUFS; i++) {
		v->rx.desc[i].addr = v->rxphys + (u64)i * BUF;
		v->rx.desc[i].len = BUF;
		v->rx.desc[i].flags = VRING_WRITE;
		rx_post(v, i);
	}
	for (int i = 0; i < 6; i++)
		v->dev.mac[i] = (feat & F_MAC) ? inb(v->io + R_MAC + i) : (u8)(0x52 + i);
	outb(v->io + R_STATUS, ST_ACK | ST_DRIVER | ST_OK);
	outw(v->io + R_Q_NOTIFY, 0);
	mos_strlcpy(v->dev.name, "virtio0", sizeof(v->dev.name));
	v->dev.send = vnet_send;
	v->dev.poll = vnet_poll;
	v->dev.priv = v;
	return &v->dev;
}
