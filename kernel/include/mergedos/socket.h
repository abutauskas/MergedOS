/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_SOCKET_H
#define MERGEDOS_SOCKET_H

/*
 * Generic sockets: a socket is a file (kernel/socket.c) whose behaviour comes
 * from a per-family ops table. AF_UNIX registers one (kernel/unix.c); AF_INET
 * registers its own with sock_register() and gets the whole system-call layer
 * (socket, bind, listen, accept, connect, send/recv, sendmsg/recvmsg with
 * SCM_RIGHTS, getsockopt, ...) for free. Addresses are opaque byte strings.
 */
#include <mergedos/types.h>

struct task;
struct file;
struct socket;

#define SOCK_MAX_FDS  8
#define SOCK_ADDR_MAX 128
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_SEQPACKET 5
#define MSG_PEEK     0x2
#define MSG_TRUNC    0x20
#define MSG_DONTWAIT 0x40
#define MSG_WAITALL  0x100
#define MSG_NOSIGNAL 0x4000
#define MSG_CTRUNC   0x8

struct sock_ops {
	int family;
	long (*create)(struct socket *s, int proto);
	void (*release)(struct socket *s);
	long (*bind)(struct task *t, struct socket *s, const u8 *addr, u32 len);
	long (*listen)(struct socket *s, int backlog);
	/* `news` is a fresh socket of the same family to attach the connection to. */
	long (*accept)(struct task *t, struct socket *s, struct socket *news, bool nonblock);
	long (*connect)(struct task *t, struct socket *s, const u8 *addr, u32 len, bool nonblock);
	long (*send)(struct task *t, struct socket *s, const u8 *data, size_t len, struct file **fds,
		     int nfds, u32 flags, const u8 *to, u32 tolen);
	long (*recv)(struct task *t, struct socket *s, u8 *buf, size_t cap, struct file **fds,
		     int *nfds, u32 flags, u8 *from, u32 *fromlen, u32 *outflags);
	u32  (*poll)(struct socket *s, u32 events);
	long (*shutdown)(struct socket *s, int how);
	long (*getname)(struct socket *s, bool peer, u8 *addr, u32 *len);
};

struct socket {
	const struct sock_ops *ops;
	int type;
	void *priv;                     /* the family's state */
	struct file *file;              /* the descriptor object, not referenced */
};

void sock_register(const struct sock_ops *ops);
/* A connected pair for socketpair() (family-specific, optional). */
long unix_socketpair(struct socket *a, struct socket *b);
void unix_init(void);

#endif /* MERGEDOS_SOCKET_H */
