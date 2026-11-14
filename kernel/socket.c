/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Generic socket layer: the socket file object and the socket system calls,
 * on top of a per-family ops table (include/mergedos/socket.h).
 */
#include <mergedos/kernel.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>
#include <mergedos/socket.h>

#define AF_UNIX 1
#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define SOCK_IO_MAX (256u * 1024)
#define IOV_MAX_N 64
#define A(i) (c->arg[i])
#define TASK (ctx->task)
#define LXFN(name) long name(struct abi_ctx *ctx, const struct abi_call *c)

static const struct sock_ops *families[8];

void sock_register(const struct sock_ops *ops)
{
	for (unsigned i = 0; i < ARRAY_SIZE(families); i++)
		if (!families[i]) {
			families[i] = ops;
			return;
		}
}

static const struct sock_ops *family_ops(int family)
{
	for (unsigned i = 0; i < ARRAY_SIZE(families); i++)
		if (families[i] && families[i]->family == family)
			return families[i];
	return NULL;
}

/* ---- the socket file --------------------------------------------------------------- */

static long sock_recv_to(struct task *t, struct socket *s, u32 fflags, u64 ubuf, u64 len, u32 flags);

static long sf_read(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	return sock_recv_to(t, f->priv, f->flags, ubuf, len, 0);
}

static long sf_write(struct task *t, struct file *f, u64 ubuf, u64 len)
{
	struct socket *s = f->priv;
	u64 done = 0;

	while (done < len) {
		u64 n = MOS_MIN(len - done, (u64)SOCK_IO_MAX);
		u8 *buf = kzalloc((size_t)n);
		long rc;

		if (!buf)
			return done ? (long)done : -MERR_NOMEM;
		if (uaccess_copy_in(t, buf, ubuf + done, (size_t)n)) {
			kfree(buf);
			return done ? (long)done : -MERR_FAULT;
		}
		rc = s->ops->send(t, s, buf, (size_t)n, NULL, 0, (f->flags & VFS_O_NONBLOCK) ? MSG_DONTWAIT : 0,
				  NULL, 0);
		kfree(buf);
		if (rc < 0)
			return done ? (long)done : rc;
		done += (u64)rc;
		if ((u64)rc < n || s->type != SOCK_STREAM)
			break;
	}
	return (long)done;
}

static u32 sf_poll(struct file *f, u32 events)
{
	struct socket *s = f->priv;

	return s->ops->poll(s, events);
}

static void sf_release(struct file *f)
{
	struct socket *s = f->priv;

	s->ops->release(s);
	kfree(s);
}

static long sf_ioctl(struct task *t, struct file *f, u32 cmd, u64 arg)
{
	struct socket *s = f->priv;
	int n;

	if (cmd != 0x541B || !s->ops->avail)
		return -MERR_NOTTY;
	n = (int)s->ops->avail(s);
	return uaccess_copy_out(t, arg, &n, 4) ? -MERR_FAULT : 0;
}

static const struct fops sock_fops = {
	.read = sf_read, .write = sf_write, .poll = sf_poll, .release = sf_release, .ioctl = sf_ioctl,
	.mode = S_IFSOCK | 0777,
};

static struct socket *sock_alloc(const struct sock_ops *ops, int type)
{
	struct socket *s = kzalloc(sizeof(*s));

	if (s) {
		s->ops = ops;
		s->type = type & 0xff;
	}
	return s;
}

static long sock_file(struct task *t, struct socket *s, u32 sflags)
{
	struct file *f = file_new_ops(&sock_fops, s, VFS_O_RDWR | ((sflags & 0x800) ? VFS_O_NONBLOCK : 0));

	if (!f) {
		s->ops->release(s);
		kfree(s);
		return -MERR_NOMEM;
	}
	s->file = f;
	return fd_install(t, f, 0, sflags & 0x80000);
}

static struct socket *sock_of(struct task *t, int fd, struct file **fp, long *err)
{
	struct file *f = fd_file(t, fd);

	if (!f) {
		*err = -MERR_BADF;
		return NULL;
	}
	if (f->type != F_OPS || f->ops != &sock_fops) {
		*err = -MERR_NOTSOCK;
		return NULL;
	}
	*fp = f;
	return f->priv;
}

#define GETSOCK(fd) struct file *f = NULL; long err = 0; struct socket *s = sock_of(TASK, (int)(fd), &f, &err); \
	if (!s) return err

/* ---- system calls ------------------------------------------------------------------- */

LXFN(lx_socket)
{
	const struct sock_ops *ops = family_ops((int)A(0));
	struct socket *s;
	long rc;

	if (!ops)
		return -MERR_AFNOSUPPORT;
	s = sock_alloc(ops, (int)A(1));
	if (!s)
		return -MERR_NOMEM;
	rc = ops->create(s, (int)A(2));
	if (rc) {
		kfree(s);
		return rc;
	}
	return sock_file(TASK, s, (u32)A(1));
}

LXFN(lx_socketpair)
{
	const struct sock_ops *ops = family_ops((int)A(0));
	struct socket *a, *b;
	int fds[2];
	long rc;

	if (!ops || (int)A(0) != AF_UNIX)
		return -MERR_AFNOSUPPORT;
	a = sock_alloc(ops, (int)A(1));
	b = sock_alloc(ops, (int)A(1));
	if (!a || !b) {
		kfree(a);
		kfree(b);
		return -MERR_NOMEM;
	}
	rc = unix_socketpair(a, b);
	if (rc) {
		kfree(a);
		kfree(b);
		return rc;
	}
	fds[0] = (int)sock_file(TASK, a, (u32)A(1));
	if (fds[0] < 0) {
		ops->release(b);
		kfree(b);
		return fds[0];
	}
	fds[1] = (int)sock_file(TASK, b, (u32)A(1));
	if (fds[1] < 0) {
		msys_close(TASK, fds[0]);
		return fds[1];
	}
	return uaccess_copy_out(TASK, A(3), fds, sizeof(fds)) ? -MERR_FAULT : 0;
}

static long get_addr(struct task *t, u64 uaddr, u64 len, u8 *buf)
{
	if (len > SOCK_ADDR_MAX)
		return -MERR_INVAL;
	return uaccess_copy_in(t, buf, uaddr, (size_t)len) ? -MERR_FAULT : 0;
}

LXFN(lx_bind)
{
	u8 addr[SOCK_ADDR_MAX];
	long rc;

	GETSOCK(A(0));
	rc = get_addr(TASK, A(1), A(2), addr);
	return rc ? rc : s->ops->bind(TASK, s, addr, (u32)A(2));
}

LXFN(lx_listen)
{
	GETSOCK(A(0));
	return s->ops->listen(s, (int)A(1));
}

static long put_addr(struct task *t, u64 uaddr, u64 ulen, const u8 *addr, u32 len)
{
	u32 cap = 0;

	if (!uaddr || !ulen)
		return 0;
	if (uaccess_copy_in(t, &cap, ulen, 4))
		return -MERR_FAULT;
	if (uaccess_copy_out(t, uaddr, addr, MOS_MIN(cap, len)) || uaccess_copy_out(t, ulen, &len, 4))
		return -MERR_FAULT;
	return 0;
}

static long do_accept(struct abi_ctx *ctx, const struct abi_call *c, u32 sflags)
{
	struct socket *news;
	u8 addr[SOCK_ADDR_MAX];
	u32 alen = sizeof(addr);
	long rc, fd;

	GETSOCK(A(0));
	news = sock_alloc(s->ops, s->type);
	if (!news)
		return -MERR_NOMEM;
	rc = s->ops->accept(TASK, s, news, (f->flags & VFS_O_NONBLOCK) || (sflags & 0x800));
	if (rc) {
		kfree(news);
		return rc;
	}
	if (A(1) && !s->ops->getname(news, true, addr, &alen))
		put_addr(TASK, A(1), A(2), addr, alen);
	fd = sock_file(TASK, news, sflags);
	return fd;
}

LXFN(lx_accept)  { return do_accept(ctx, c, 0); }
LXFN(lx_accept4) { return do_accept(ctx, c, (u32)A(3)); }

LXFN(lx_connect)
{
	u8 addr[SOCK_ADDR_MAX];
	long rc;

	GETSOCK(A(0));
	rc = get_addr(TASK, A(1), A(2), addr);
	return rc ? rc : s->ops->connect(TASK, s, addr, (u32)A(2), f->flags & VFS_O_NONBLOCK);
}

LXFN(lx_shutdown)
{
	GETSOCK(A(0));
	return s->ops->shutdown(s, (int)A(1));
}

static long name_call(struct abi_ctx *ctx, const struct abi_call *c, bool peer)
{
	u8 addr[SOCK_ADDR_MAX];
	u32 len = sizeof(addr);
	long rc;

	GETSOCK(A(0));
	rc = s->ops->getname(s, peer, addr, &len);
	return rc ? rc : put_addr(TASK, A(1), A(2), addr, len);
}

LXFN(lx_getsockname) { return name_call(ctx, c, false); }
LXFN(lx_getpeername) { return name_call(ctx, c, true); }

LXFN(lx_setsockopt)
{
	u8 val[16] = { 0 };
	u64 len = MOS_MIN(A(4), (u64)sizeof(val));

	GETSOCK(A(0));
	if (!s->ops->setsockopt)
		return 0;                       /* buffer sizes, reuse, keepalive: accepted */
	if (len && uaccess_copy_in(TASK, val, A(3), (size_t)len))
		return -MERR_FAULT;
	return s->ops->setsockopt(s, (int)A(1), (int)A(2), val, (u32)len);
}

LXFN(lx_getsockopt)
{
	u32 val = 0, len = 4;
	u8 cred[12] = { 0 };

	GETSOCK(A(0));
	if (s->ops->getsockopt) {
		u8 ov[16] = { 0 };
		long rc;

		if (uaccess_copy_in(TASK, &len, A(4), 4))
			return -MERR_FAULT;
		len = MOS_MIN(len, (u32)sizeof(ov));
		rc = s->ops->getsockopt(s, (int)A(1), (int)A(2), ov, &len);
		if (rc)
			return rc;
		return uaccess_copy_out(TASK, A(3), ov, len) || uaccess_copy_out(TASK, A(4), &len, 4) ? -MERR_FAULT : 0;
	}
	if (A(1) == SOL_SOCKET) {
		switch (A(2)) {
		case 3:  val = (u32)s->type; break;                     /* SO_TYPE */
		case 7: case 8: val = 212992; break;                    /* SO_SNDBUF / SO_RCVBUF */
		case 17:                                                /* SO_PEERCRED */
			{ u32 pid = (u32)TASK->pid; memcpy(cred, &pid, 4); }
			len = sizeof(cred);
			return uaccess_copy_out(TASK, A(3), cred, sizeof(cred)) ||
			       uaccess_copy_out(TASK, A(4), &len, 4) ? -MERR_FAULT : 0;
		case 30: val = 0; break;
		default: break;
		}
	}
	return uaccess_copy_out(TASK, A(3), &val, 4) || uaccess_copy_out(TASK, A(4), &len, 4) ? -MERR_FAULT : 0;
}

/* ---- send and receive ------------------------------------------------------------------ */

struct iov { u64 base, len; };

static long load_iov(struct task *t, u64 uiov, u64 n, struct iov *iov, u64 *total)
{
	if (n > IOV_MAX_N)
		return -MERR_INVAL;
	*total = 0;
	if (uaccess_copy_in(t, iov, uiov, (size_t)(n * sizeof(*iov))))
		return -MERR_FAULT;
	for (u64 i = 0; i < n; i++)
		*total += iov[i].len;
	return 0;
}

/* Pull the SCM_RIGHTS descriptors out of msg_control; returns their count or -err. */
static long take_fds(struct task *t, u64 ctl, u64 ctllen, struct file **fds)
{
	u8 buf[256];
	long n = 0;
	u64 off = 0;

	if (!ctl || !ctllen)
		return 0;
	if (ctllen > sizeof(buf))
		ctllen = sizeof(buf);
	if (uaccess_copy_in(t, buf, ctl, (size_t)ctllen))
		return -MERR_FAULT;
	while (off + 16 <= ctllen) {
		u64 clen;
		s32 level, type;

		memcpy(&clen, buf + off, 8);
		memcpy(&level, buf + off + 8, 4);
		memcpy(&type, buf + off + 12, 4);
		if (clen < 16 || off + clen > ctllen)
			break;
		if (level == SOL_SOCKET && type == SCM_RIGHTS) {
			for (u64 i = 16; i + 4 <= clen && n < SOCK_MAX_FDS; i += 4) {
				s32 fd;
				struct file *f;

				memcpy(&fd, buf + off + i, 4);
				f = fd_file(t, fd);
				if (!f) {
					while (n)
						file_put(fds[--n]);
					return -MERR_BADF;
				}
				fds[n++] = file_get(f);
			}
		}
		off += ALIGN_UP(clen, 8);
	}
	return n;
}

static long send_all(struct task *t, struct file *f, struct socket *s, const struct iov *iov, u64 niov,
		     u64 total, struct file **fds, int nfds, u32 flags, const u8 *to, u32 tolen)
{
	u64 done = 0, want = s->type == SOCK_STREAM ? total : MOS_MIN(total, (u64)SOCK_IO_MAX);
	u8 *buf;

	if (f->flags & VFS_O_NONBLOCK)
		flags |= MSG_DONTWAIT;
	while (done < want || (!want && !done && nfds)) {
		u64 n = MOS_MIN(want - done, (u64)SOCK_IO_MAX), pos = 0, skip = done;
		long rc;

		buf = kzalloc((size_t)n + 1);
		if (!buf)
			return done ? (long)done : -MERR_NOMEM;
		for (u64 i = 0; i < niov && pos < n; i++) {
			u64 l = iov[i].len, o = 0;

			if (skip >= l) {
				skip -= l;
				continue;
			}
			o = skip;
			skip = 0;
			l = MOS_MIN(l - o, n - pos);
			if (uaccess_copy_in(t, buf + pos, iov[i].base + o, (size_t)l)) {
				kfree(buf);
				return done ? (long)done : -MERR_FAULT;
			}
			pos += l;
		}
		rc = s->ops->send(t, s, buf, (size_t)n, done ? NULL : fds, done ? 0 : nfds, flags, to, tolen);
		kfree(buf);
		if (rc < 0)
			return done ? (long)done : rc;
		done += (u64)rc;
		if (!want || (u64)rc < n)
			break;
	}
	return (long)done;
}

static long sendmsg_common(struct abi_ctx *ctx, int fd, u64 umsg, u32 flags)
{
	struct task *t = TASK;
	u64 hdr[7];
	struct iov iov[IOV_MAX_N];
	struct file *fds[SOCK_MAX_FDS], *f = NULL;
	u8 to[SOCK_ADDR_MAX];
	u64 total;
	long nfds, rc, err = 0;
	struct socket *s = sock_of(t, fd, &f, &err);

	if (!s)
		return err;
	if (uaccess_copy_in(t, hdr, umsg, sizeof(hdr)))
		return -MERR_FAULT;
	rc = load_iov(t, hdr[2], hdr[3], iov, &total);
	if (rc)
		return rc;
	if (hdr[0] && (hdr[1] & 0xffffffffu) && get_addr(t, hdr[0], hdr[1] & 0xffffffffu, to))
		return -MERR_FAULT;
	nfds = take_fds(t, hdr[4], hdr[5], fds);
	if (nfds < 0)
		return nfds;
	rc = send_all(t, f, s, iov, hdr[3], total, fds, (int)nfds, flags,
		      hdr[0] ? to : NULL, (u32)(hdr[1] & 0xffffffffu));
	for (long i = 0; i < nfds; i++)
		file_put(fds[i]);                       /* the queue took its own references */
	return rc;
}

LXFN(lx_sendmsg) { return sendmsg_common(ctx, (int)A(0), A(1), (u32)A(2)); }

LXFN(lx_sendto)
{
	struct iov iov = { A(1), A(2) };
	u8 to[SOCK_ADDR_MAX];
	GETSOCK(A(0));
	if (A(4) && get_addr(TASK, A(4), A(5), to))
		return -MERR_FAULT;
	return send_all(TASK, f, s, &iov, 1, A(2), NULL, 0, (u32)A(3), A(4) ? to : NULL, (u32)A(5));
}

/* Receive into user memory described by an iovec array. */
static long recv_iov(struct task *t, struct socket *s, u32 fflags, const struct iov *iov, u64 niov,
		     u64 total, u32 flags, struct file **fds, int *nfds, u8 *from, u32 *fromlen, u32 *outflags)
{
	u64 cap = MOS_MIN(total, (u64)SOCK_IO_MAX), pos = 0;
	u8 *buf = kzalloc((size_t)cap + 1);
	long rc;

	if (!buf)
		return -MERR_NOMEM;
	if (fflags & VFS_O_NONBLOCK)
		flags |= MSG_DONTWAIT;
	rc = s->ops->recv(t, s, buf, (size_t)cap, fds, nfds, flags, from, fromlen, outflags);
	for (u64 i = 0; rc > 0 && i < niov && pos < (u64)rc; i++) {
		u64 l = MOS_MIN(iov[i].len, (u64)rc - pos);

		if (uaccess_copy_out(t, iov[i].base, buf + pos, (size_t)l)) {
			rc = -MERR_FAULT;
			break;
		}
		pos += l;
	}
	kfree(buf);
	return rc;
}

static long sock_recv_to(struct task *t, struct socket *s, u32 fflags, u64 ubuf, u64 len, u32 flags)
{
	struct iov iov = { ubuf, len };
	struct file *fds[SOCK_MAX_FDS];
	int nfds = 0;
	long rc = recv_iov(t, s, fflags, &iov, 1, len, flags, fds, &nfds, NULL, NULL, NULL);

	while (nfds > 0)
		file_put(fds[--nfds]);                  /* read() cannot deliver descriptors */
	return rc;
}

LXFN(lx_recvfrom)
{
	struct iov iov = { A(1), A(2) };
	struct file *fds[SOCK_MAX_FDS];
	u8 from[SOCK_ADDR_MAX];
	u32 flen = 0;
	int nfds = 0;
	long rc;

	GETSOCK(A(0));
	rc = recv_iov(TASK, s, f->flags, &iov, 1, A(2), (u32)A(3), fds, &nfds, from, &flen, NULL);
	while (nfds > 0)
		file_put(fds[--nfds]);
	if (rc >= 0 && A(4))
		put_addr(TASK, A(4), A(5), from, flen);
	return rc;
}

LXFN(lx_recvmsg)
{
	struct task *t = TASK;
	u64 hdr[7];
	struct iov iov[IOV_MAX_N];
	struct file *fds[SOCK_MAX_FDS];
	u8 from[SOCK_ADDR_MAX], ctl[16 + 4 * SOCK_MAX_FDS];
	u32 flen = 0, oflags = 0, nl;
	u64 total, ctllen = 0;
	int nfds = 0;
	long rc;

	GETSOCK(A(0));
	if (uaccess_copy_in(t, hdr, A(1), sizeof(hdr)))
		return -MERR_FAULT;
	rc = load_iov(t, hdr[2], hdr[3], iov, &total);
	if (rc)
		return rc;
	rc = recv_iov(t, s, f->flags, iov, hdr[3], total, (u32)A(2), fds, &nfds, from, &flen, &oflags);
	if (rc < 0)
		return rc;
	if (nfds) {
		u64 clen = 16 + 4 * (u64)nfds;
		int n = 0;

		if (hdr[4] && hdr[5] >= clen) {
			s32 lvl = SOL_SOCKET, typ = SCM_RIGHTS;

			memset(ctl, 0, sizeof(ctl));
			memcpy(ctl, &clen, 8);
			memcpy(ctl + 8, &lvl, 4);
			memcpy(ctl + 12, &typ, 4);
			for (; n < nfds; n++) {
				int fd = fd_install(t, fds[n], 0, (A(2) & 0x40000000) != 0);   /* MSG_CMSG_CLOEXEC */

				if (fd < 0)
					break;
				memcpy(ctl + 16 + 4 * n, &fd, 4);
			}
			ctllen = clen;
			uaccess_copy_out(t, hdr[4], ctl, (size_t)clen);
		} else {
			oflags |= MSG_CTRUNC;
		}
		while (n < nfds)
			file_put(fds[n++]);             /* no room: dropped */
	}
	nl = flen;
	if (hdr[0] && flen)
		put_addr(t, hdr[0], A(1) + 8, from, flen), (void)nl;
	hdr[5] = ctllen;
	hdr[6] = oflags;
	uaccess_copy_out(t, A(1) + 40, &hdr[5], 16);
	return rc;
}

/* sendmmsg / recvmmsg: one message per entry (struct mmsghdr = msghdr + 4 bytes length, 64 total). */
LXFN(lx_sendmmsg)
{
	u64 i;

	for (i = 0; i < A(2); i++) {
		long rc = sendmsg_common(ctx, (int)A(0), A(1) + i * 64, (u32)A(3));

		if (rc < 0)
			return i ? (long)i : rc;
		{ u32 n = (u32)rc; uaccess_copy_out(TASK, A(1) + i * 64 + 56, &n, 4); }
	}
	return (long)i;
}

void socket_init(void)
{
	unix_init();
	inet_init();
}
