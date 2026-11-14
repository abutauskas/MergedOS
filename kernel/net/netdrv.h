/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_NETDRV_H
#define MERGEDOS_NETDRV_H
/* NIC drivers (polled) and the glue that runs the stack in the kernel. */
#include "kernel/net/net.h"

struct netdev *virtio_net_probe(void);          /* legacy (transitional) PCI device */
struct netdev *e1000_probe(void);               /* Intel 8254x family, QEMU's default NIC */
bool net_pci_find(u16 vendor, const u16 *devices, int ndev, u8 *bus, u8 *dev, u8 *fn);
void *net_dma_alloc(u32 pages, u64 *phys);      /* zeroed contiguous frames, direct-mapped */
void net_dma_free(u64 phys, u32 pages);
bool virtio_net_present(void);
bool e1000_present(void);
void net_shutdown(void);                        /* NIC reset, memory and netd given back */

extern struct nstack net_stack;
extern bool net_present;

void net_init(void);                            /* probe, start netd, begin DHCP */
void net_enter(void);                           /* refresh the stack's clock; call with irqs off */
u64  net_ms(void);
int  net_selftest(void);                        /* DHCP lease, ping, DNS, guest HTTP fetch */
const char *net_nic_name(void);
#endif
