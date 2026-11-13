/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ABI dispatcher core: the hot path every foreign system call takes.
 *
 * Cost per call on top of a native syscall: two indirect calls (decode,
 * encode), one bounds-checked array index (lookup) and one thunk. There is
 * no hashing, no string compare and no allocation on this path.
 */
#include "abi_dispatch/abi.h"

static const struct abi_personality_ops *const personalities[ABI_COUNT] = {
	[ABI_LINUX]   = &abi_linux_ops,
	[ABI_FREEBSD] = &abi_freebsd_ops,
	[ABI_XNU]     = &abi_xnu_ops,
	[ABI_NT]      = &abi_nt_ops,
};

const struct abi_personality_ops *abi_personality_ops(enum abi_personality p)
{
	return (unsigned)p < ABI_COUNT ? personalities[p] : NULL;
}

const char *abi_personality_name(enum abi_personality p)
{
	const struct abi_personality_ops *ops = abi_personality_ops(p);

	return ops ? ops->name : "none";
}

void abi_ctx_init(struct abi_ctx *ctx, enum abi_personality p, struct task *task)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->personality = p;
	ctx->task = task;
}

void abi_dispatch(struct abi_ctx *ctx, struct syscall_frame *f)
{
	const struct abi_personality_ops *ops = abi_personality_ops(ctx->personality);
	const struct abi_sysent *ent;
	struct abi_call call;
	long ret;

	if (!ops) {                     /* no personality: nothing sane to return to */
		msys_exit(ctx->task, 127);
		return;
	}

	ops->decode(f, &call);
	ctx->stats.calls++;

	ent = ops->lookup(&call);
	if (ent) {
		ret = ent->fn(ctx, &call);
	} else {
		ret = -MERR_NOSYS;
		ctx->stats.unknown++;
		ctx->stats.last_unknown_nr = call.nr;
	}
	if (ret < 0)
		ctx->stats.errors++;

	ops->encode(f, &call, ret);
}

/* ---- thunks shared by the POSIX-shaped personalities --------------------- */

/* write(int fd, const void *buf, size_t len): identical layout on all three. */
long abi_posix_write(struct abi_ctx *ctx, const struct abi_call *c)
{
	return msys_write(ctx->task, (int)c->arg[0], c->arg[1], c->arg[2]);
}

long abi_posix_exit(struct abi_ctx *ctx, const struct abi_call *c)
{
	msys_exit(ctx->task, (int)(c->arg[0] & 0xFF));
	return 0;
}

long abi_posix_getpid(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)c;
	return msys_getpid(ctx->task);
}
