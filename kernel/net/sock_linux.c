/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * TEMPORARY seam: Linux socket system calls for AF_INET, on a F_SOCKET file.
 *
 * When the shared socket layer (kernel/socket.c, per-family ops table) lands,
 * delete this file and the F_SOCKET cases in vfs.c, and register
 * inet_socket_ops for AF_INET there. Everything protocol-specific is behind
 * inet_socket_ops; this file only does user-memory marshalling, descriptors,
 * and Linux constants. Hooked into the Linux table by net_linux_lookup() and
 * net_linux_ioctl(), which personality_linux.c reaches through weak symbols.
 */
#include <mergedos/compat.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>
#include "abi_dispatch/abi.h"
#include "kernel/net/inet.h"
#include "kernel/net/net.h"

#define AF_INET_       2
#define SOCK_TYPE_MASK 0xF
#define SOCK_NONBLOCK_ 0x800
#define SOCK_CLOEXEC_  0x80000
#define FIONBIO_       0x5421
#define FIONREAD_      0x541B
#define BOUNCE         4000


static const struct inet_socket_ops *const ops = &inet_socket_ops;

static struct file *sock_of(struct task *t, int fd, long *err)
{
	struct file *f = fd_file(t, fd);

	if (!f) {
		*err = -MERR_BADF;
		return NULL;
	}
	if (f->type != F_SOCKET) {
		*err = -MERR_NOTSOCK;
		return NULL;
	}
	return f;
}

static long get_addr(struct task *t, u64 uaddr, u64 ulen, u32 *ip, u16 *port)
{
	u8 sa[16];

	if (ulen < 8 || !uaddr)
		return -MERR_INVAL;
	if (uaccess_copy_in(t, sa, uaddr, MOS_MIN(ulen, (u64)16) < 16 ? 8 : 16))
		return -MERR_FAULT;
	if (sa[0] != AF_INET_ || sa[1] != 0)
		return -MERR_AFNOSUPPORT;
	if (ulen < 16)
		return -MERR_INVAL;
	*port = net_get16(sa + 2);
	*ip = net_get32(sa + 4);
	return 0;
}

static long put_addr(struct task *t, u64 uaddr, u64 ulenp, u32 ip, u16 port)
{
	u8 sa[16] = { AF_INET_, 0 };
	u32 have, n;

	if (!uaddr)
		return 0;
	if (uaccess_copy_in(t, &have, ulenp, 4))
		return -MERR_FAULT;
	net_put16(sa + 2, port);
	net_put32(sa + 4, ip);
	n = MOS_MIN(have, 16u);
	have = 16;
	if (uaccess_copy_out(t, uaddr, sa, n) || uaccess_copy_out(t, ulenp, &have, 4))
		return -MERR_FAULT;
	return 0;
}

/* ---- data transfer on a flat user buffer ------------------------------------------ */

static long send_user(struct task *t, struct file *f, u64 ubuf, u64 len, u32 flags,
		      const u32 *dip, u16 dport, bool dgram_hint)
{
	bool nb = f->flags & VFS_O_NONBLOCK;
	u8 *bounce = kzalloc(BOUNCE);
	u64 done = 0;
	long rc = 0;

	(void)dgram_hint;
	if (!bounce)
		return -MERR_NOMEM;
	file_get(f);
	do {
		u32 n = (u32)MOS_MIN(len - done, (u64)BOUNCE);

		if (uaccess_copy_in(t, bounce, ubuf + done, n)) {
			rc = done ? (long)done : -MERR_FAULT;
			break;
		}
		rc = ops->sendmsg(f->sock, bounce, n, flags, nb && !done ? true : nb, dip, dport);
		if (rc < 0) {
			if (done && rc != -MERR_PIPE)
				rc = (long)done;
			break;
		}
		done += (u64)rc;
		if ((u64)rc < n)
			break;                          /* datagram or short queue: stop */
		rc = (long)done;
	} while (done < len);
	if (rc >= 0)
		rc = (long)done;
	if (rc == -MERR_PIPE && !(flags & INET_MSG_NOSIGNAL))
		task_signal(t, 13);                     /* SIGPIPE */
	file_put(f);
	kfree(bounce);
	return rc;
}

static long recv_user(struct task *t, struct file *f, u64 ubuf, u64 len, u32 flags,
		      u32 *sip, u16 *sport)
{
	bool nb = f->flags & VFS_O_NONBLOCK;
	u8 *bounce = kzalloc(BOUNCE);
	u64 done = 0;
	long rc;

	if (!bounce)
		return -MERR_NOMEM;
	file_get(f);
	for (;;) {
		u32 n = (u32)MOS_MIN(len - done, (u64)BOUNCE);

		/* After the first chunk, take only what is already there. */
		rc = ops->recvmsg(f->sock, bounce, n, flags, done ? true : nb, sip, sport);
		if (rc <= 0) {
			if (done)
				rc = (long)done;
			break;
		}
		if (uaccess_copy_out(t, ubuf + done, bounce, (size_t)rc)) {
			rc = done ? (long)done : -MERR_FAULT;
			break;
		}
		done += (u64)rc;
		rc = (long)done;
		if ((u64)rc >= len || (u32)rc < n || (flags & INET_MSG_PEEK))
			break;
		if (!(flags & INET_MSG_WAITALL))
			flags |= INET_MSG_DONTWAIT;
	}
	file_put(f);
	kfree(bounce);
	return rc;
}

long sock_file_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	return recv_user(t, f, ubuf, len, 0, NULL, NULL);
}

long sock_file_write(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	return send_user(t, f, ubuf, len, 0, NULL, 0, false);
}

void sock_file_release(struct file *f)
{
	if (f->sock)
		ops->release(f->sock);
	f->sock = NULL;
}

/* ---- system calls ---------------------------------------------------------------- */

static long lx_socket(struct abi_ctx *ctx, const struct abi_call *c)
{
	int domain = (int)c->arg[0], type = (int)c->arg[1] & SOCK_TYPE_MASK;
	u32 fl = (u32)c->arg[1];
	void *priv = NULL;
	struct file *f;
	long rc;

	if (domain != AF_INET_)
		return -MERR_AFNOSUPPORT;                   /* AF_UNIX/AF_INET6: other layers */
	rc = ops->create(type, (int)c->arg[2], &priv);
	if (rc)
		return rc;
	f = socket_file_new(priv, (fl & SOCK_NONBLOCK_) ? VFS_O_NONBLOCK : 0);
	if (!f) {
		ops->release(priv);
		return -MERR_NOMEM;
	}
	rc = fd_install(ctx->task, f, 0, fl & SOCK_CLOEXEC_);
	return rc;
}

static long lx_bind(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	u32 ip;
	u16 port;

	if (!f)
		return err;
	err = get_addr(ctx->task, c->arg[1], c->arg[2], &ip, &port);
	return err ? err : ops->bind(f->sock, ip, port);
}

static long lx_connect(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0, rc;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	u32 ip;
	u16 port;

	if (!f)
		return err;
	err = get_addr(ctx->task, c->arg[1], c->arg[2], &ip, &port);
	if (err)
		return err;
	file_get(f);
	rc = ops->connect(f->sock, ip, port, f->flags & VFS_O_NONBLOCK);
	file_put(f);
	return rc;
}

static long lx_listen(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);

	return f ? ops->listen(f->sock, (int)c->arg[1]) : err;
}

static long do_accept(struct abi_ctx *ctx, const struct abi_call *c, u32 flags)
{
	long err = 0, rc;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err), *nf;
	void *np = NULL;
	u32 ip = 0;
	u16 port = 0;

	if (!f)
		return err;
	file_get(f);
	rc = ops->accept(f->sock, f->flags & VFS_O_NONBLOCK, &np, &ip, &port);
	file_put(f);
	if (rc)
		return rc;
	nf = socket_file_new(np, (flags & SOCK_NONBLOCK_) ? VFS_O_NONBLOCK : 0);
	if (!nf) {
		ops->release(np);
		return -MERR_NOMEM;
	}
	rc = fd_install(ctx->task, nf, 0, flags & SOCK_CLOEXEC_);
	if (rc >= 0 && c->arg[1])
		put_addr(ctx->task, c->arg[1], c->arg[2], ip, port);
	return rc;
}

static long lx_accept(struct abi_ctx *ctx, const struct abi_call *c)
{
	return do_accept(ctx, c, 0);
}

static long lx_accept4(struct abi_ctx *ctx, const struct abi_call *c)
{
	return do_accept(ctx, c, (u32)c->arg[3]);
}

static long lx_sendto(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	u32 ip = 0;
	u16 port = 0;

	if (!f)
		return err;
	if (c->arg[4]) {
		err = get_addr(ctx->task, c->arg[4], c->arg[5], &ip, &port);
		if (err)
			return err;
	}
	return send_user(ctx->task, f, c->arg[1], c->arg[2], (u32)c->arg[3], c->arg[4] ? &ip : NULL,
			 port, false);
}

static long lx_recvfrom(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0, rc;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	u32 ip = 0;
	u16 port = 0;

	if (!f)
		return err;
	rc = recv_user(ctx->task, f, c->arg[1], c->arg[2], (u32)c->arg[3], &ip, &port);
	if (rc >= 0 && c->arg[4])
		put_addr(ctx->task, c->arg[4], c->arg[5], ip, port);
	return rc;
}

struct lx_msghdr {
	u64 name;
	u32 namelen, pad;
	u64 iov, iovlen, control, controllen;
	u32 flags, pad2;
};

#define IOV_MAX_ 16

static long lx_sendmsg(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0, total = 0;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	struct lx_msghdr m;
	u32 ip = 0, flags = (u32)c->arg[2];
	u16 port = 0;
	bool have_addr = false;

	if (!f)
		return err;
	if (uaccess_copy_in(ctx->task, &m, c->arg[1], sizeof(m)))
		return -MERR_FAULT;
	if (m.iovlen > IOV_MAX_)
		return -MERR_INVAL;
	if (m.name) {
		err = get_addr(ctx->task, m.name, m.namelen, &ip, &port);
		if (err)
			return err;
		have_addr = true;
	}
	for (u64 i = 0; i < m.iovlen; i++) {
		u64 iov[2];
		long n;

		if (uaccess_copy_in(ctx->task, iov, m.iov + i * 16, 16))
			return total ? total : -MERR_FAULT;
		if (!iov[1])
			continue;
		n = send_user(ctx->task, f, iov[0], iov[1], flags, have_addr ? &ip : NULL, port, false);
		if (n < 0)
			return total ? total : n;
		total += n;
		if ((u64)n < iov[1])
			break;
	}
	return total;
}

static long lx_recvmsg(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0, total = 0;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	struct lx_msghdr m;
	u32 ip = 0, flags = (u32)c->arg[2];
	u16 port = 0;

	if (!f)
		return err;
	if (uaccess_copy_in(ctx->task, &m, c->arg[1], sizeof(m)))
		return -MERR_FAULT;
	if (m.iovlen > IOV_MAX_)
		return -MERR_INVAL;
	for (u64 i = 0; i < m.iovlen; i++) {
		u64 iov[2];
		long n;

		if (uaccess_copy_in(ctx->task, iov, m.iov + i * 16, 16))
			return total ? total : -MERR_FAULT;
		if (!iov[1])
			continue;
		n = recv_user(ctx->task, f, iov[0], iov[1], flags, &ip, &port);
		if (n < 0)
			return total ? total : n;
		total += n;
		flags |= INET_MSG_DONTWAIT;             /* only the first iovec may block */
		if ((u64)n < iov[1])
			break;
	}
	if (m.name) {
		u32 have = m.namelen, n = MOS_MIN(have, 16u);
		u8 sa[16] = { AF_INET_, 0 };

		net_put16(sa + 2, port);
		net_put32(sa + 4, ip);
		if (n)
			uaccess_copy_out(ctx->task, m.name, sa, n);
		m.namelen = 16;
	}
	m.controllen = 0;
	m.flags = 0;
	uaccess_copy_out(ctx->task, c->arg[1], &m, sizeof(m));
	return total;
}

static long lx_shutdown(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);

	return f ? ops->shutdown(f->sock, (int)c->arg[1]) : err;
}

static long names(struct abi_ctx *ctx, const struct abi_call *c, bool peer)
{
	long err = 0;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	u32 ip = 0;
	u16 port = 0;

	if (!f)
		return err;
	err = ops->getname(f->sock, peer, &ip, &port);
	return err ? err : put_addr(ctx->task, c->arg[1], c->arg[2], ip, port);
}

static long lx_getsockname(struct abi_ctx *ctx, const struct abi_call *c)
{
	return names(ctx, c, false);
}

static long lx_getpeername(struct abi_ctx *ctx, const struct abi_call *c)
{
	return names(ctx, c, true);
}

static long lx_setsockopt(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	u8 val[16] = { 0 };
	u64 len = c->arg[4];

	if (!f)
		return err;
	if (len > sizeof(val))
		len = sizeof(val);
	if (len && uaccess_copy_in(ctx->task, val, c->arg[3], len))
		return -MERR_FAULT;
	return ops->setsockopt(f->sock, (int)c->arg[1], (int)c->arg[2], val, (u32)len);
}

static long lx_getsockopt(struct abi_ctx *ctx, const struct abi_call *c)
{
	long err = 0, rc;
	struct file *f = sock_of(ctx->task, (int)c->arg[0], &err);
	u8 val[16] = { 0 };
	u32 len;

	if (!f)
		return err;
	if (uaccess_copy_in(ctx->task, &len, c->arg[4], 4))
		return -MERR_FAULT;
	len = MOS_MIN(len, (u32)sizeof(val));
	rc = ops->getsockopt(f->sock, (int)c->arg[1], (int)c->arg[2], val, &len);
	if (rc)
		return rc;
	if (uaccess_copy_out(ctx->task, c->arg[3], val, len) ||
	    uaccess_copy_out(ctx->task, c->arg[4], &len, 4))
		return -MERR_FAULT;
	return 0;
}

static const struct abi_sysent net_sysent[] = {
	[41 - 41] = { lx_socket,      "socket" },
	[42 - 41] = { lx_connect,     "connect" },
	[43 - 41] = { lx_accept,      "accept" },
	[44 - 41] = { lx_sendto,      "sendto" },
	[45 - 41] = { lx_recvfrom,    "recvfrom" },
	[46 - 41] = { lx_sendmsg,     "sendmsg" },
	[47 - 41] = { lx_recvmsg,     "recvmsg" },
	[48 - 41] = { lx_shutdown,    "shutdown" },
	[49 - 41] = { lx_bind,        "bind" },
	[50 - 41] = { lx_listen,      "listen" },
	[51 - 41] = { lx_getsockname, "getsockname" },
	[52 - 41] = { lx_getpeername, "getpeername" },
	[54 - 41] = { lx_setsockopt,  "setsockopt" },
	[55 - 41] = { lx_getsockopt,  "getsockopt" },
};

static const struct abi_sysent accept4_ent = { lx_accept4, "accept4" };

const struct abi_sysent *net_linux_lookup(u64 nr);
long net_linux_ioctl(struct abi_ctx *ctx, const struct abi_call *c);

const struct abi_sysent *net_linux_lookup(u64 nr)
{
	if (nr == 288)
		return &accept4_ent;
	if (nr >= 41 && nr - 41 < ARRAY_SIZE(net_sysent) && net_sysent[nr - 41].fn)
		return &net_sysent[nr - 41];
	return NULL;
}

/* ioctl on a socket descriptor: FIONBIO and FIONREAD; -MERR_NOTSOCK if fd is not one. */
long net_linux_ioctl(struct abi_ctx *ctx, const struct abi_call *c)
{
	struct file *f = fd_file(ctx->task, (int)c->arg[0]);
	u32 v = 0;
	long rc;

	if (!f || f->type != F_SOCKET)
		return -MERR_NOTSOCK;
	if (c->arg[1] == FIONBIO_) {
		if (uaccess_copy_in(ctx->task, &v, c->arg[2], 4))
			return -MERR_FAULT;
		f->flags = v ? f->flags | VFS_O_NONBLOCK : f->flags & ~(u32)VFS_O_NONBLOCK;
		return 0;
	}
	rc = ops->ioctl(f->sock, (u32)c->arg[1], &v);
	if (rc)
		return rc;
	return uaccess_copy_out(ctx->task, c->arg[2], &v, 4) ? -MERR_FAULT : 0;
}
