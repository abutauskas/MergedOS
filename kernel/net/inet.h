/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_INET_H
#define MERGEDOS_INET_H
/*
 * AF_INET socket methods for the kernel's socket layer.
 *
 * `inet_socket_ops` is the table a generic socket file registers for AF_INET
 * (see docs/NETWORKING.md, "Seam"). Methods take kernel buffers and host-byte-
 * order addresses only, never user pointers, and block (thread_block) unless
 * `nonblock` is set. Return values are bytes / 0 or -MERR_*. `priv` is the
 * per-socket handle that create() returns. Option and ioctl numbers are the
 * Linux ones. Readiness masks use Linux POLLIN/POLLOUT/POLLERR/POLLHUP.
 */
#include <mergedos/types.h>

#define INET_SOCK_STREAM 1
#define INET_SOCK_DGRAM  2

struct inet_socket_ops {
	long (*create)(int type, int protocol, void **priv);
	long (*bind)(void *priv, u32 ip, u16 port);
	long (*connect)(void *priv, u32 ip, u16 port, bool nonblock);
	long (*listen)(void *priv, int backlog);
	long (*accept)(void *priv, bool nonblock, void **newpriv, u32 *ip, u16 *port);
	/* dip == NULL: use the connected peer */
	long (*sendmsg)(void *priv, const void *buf, u32 len, u32 flags, bool nonblock,
			const u32 *dip, u16 dport);
	long (*recvmsg)(void *priv, void *buf, u32 len, u32 flags, bool nonblock, u32 *sip,
			u16 *sport);
	long (*shutdown)(void *priv, int how);
	long (*getsockopt)(void *priv, int level, int opt, void *val, u32 *len);
	long (*setsockopt)(void *priv, int level, int opt, const void *val, u32 len);
	u32  (*poll)(void *priv);
	long (*getname)(void *priv, bool peer, u32 *ip, u16 *port);
	long (*ioctl)(void *priv, u32 cmd, u32 *val);           /* FIONREAD */
	void (*release)(void *priv);
};

extern const struct inet_socket_ops inet_socket_ops;

/* Linux numbers used by the methods above. */
#define INET_MSG_PEEK     0x02
#define INET_MSG_DONTWAIT 0x40
#define INET_MSG_WAITALL  0x100
#define INET_MSG_NOSIGNAL 0x4000

void net_start_poller(void);            /* netdev.c: start netd if it is not running */
#endif
