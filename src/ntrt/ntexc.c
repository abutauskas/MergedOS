/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS ntdll, structured exceptions on x64 .pdata / UNWIND_INFO
 * (documented format). Contexts, unwinding, RtlRaiseException's two-phase
 * dispatch and __C_specific_handler (the language handler of __try).
 * Faults raised by the CPU are not dispatched yet (they end the process).
 */
#include "src/ntrt/nt.h"

#define CX_FLAGS   0x30
#define CX_RSP     0x98
#define CX_RAX     0x78
#define CX_RIP     0xF8
#define CX_XMM0    0x1A0
#define CX_SIZE    0x4D0
#define CX_FULL    0x10000Fu

#define EXC_NONCONTINUABLE 1
#define EXC_UNWINDING      2
#define EXC_EXIT_UNWIND    4
#define EXC_TARGET_UNWIND  0x20
#define STATUS_UNWIND      0xC0000027u
#define STATUS_NONCONTINUABLE_EXCEPTION 0xC0000025u

typedef struct { u32 begin, end, unwind; } RTF;

struct exc_record {
	u32 code, flags;
	struct exc_record *next;
	void *address;
	u32 nparams;
	u64 info[15];
};

struct disp_ctx {
	u64 control_pc, image_base;
	const RTF *func;
	u64 frame, target_ip;
	u8 *ctx;
	void *language_handler, *handler_data, *history;
	u32 scope_index, fill0;
};

#define R64(c, off) (*(u64 *)((u8 *)(c) + (off)))
#define REG(c, i)   R64(c, CX_RAX + 8 * (i))

/* ---- context capture / restore (assembly) -------------------------------------- */

__asm__(
	".text\n"
	".macro SAVEREGS base, rspoff, ripoff\n"
	"	movq %rax, 0x78(\\base)\n"
	"	movq %rcx, 0x80(\\base)\n"
	"	movq %rdx, 0x88(\\base)\n"
	"	movq %rbx, 0x90(\\base)\n"
	"	movq %rbp, 0xA0(\\base)\n"
	"	movq %rsi, 0xA8(\\base)\n"
	"	movq %rdi, 0xB0(\\base)\n"
	"	movq %r8,  0xB8(\\base)\n"
	"	movq %r9,  0xC0(\\base)\n"
	"	movq %r10, 0xC8(\\base)\n"
	"	movq %r11, 0xD0(\\base)\n"
	"	movq %r12, 0xD8(\\base)\n"
	"	movq %r13, 0xE0(\\base)\n"
	"	movq %r14, 0xE8(\\base)\n"
	"	movq %r15, 0xF0(\\base)\n"
	"	movups %xmm0,  0x1A0(\\base)\n"
	"	movups %xmm1,  0x1B0(\\base)\n"
	"	movups %xmm2,  0x1C0(\\base)\n"
	"	movups %xmm3,  0x1D0(\\base)\n"
	"	movups %xmm4,  0x1E0(\\base)\n"
	"	movups %xmm5,  0x1F0(\\base)\n"
	"	movups %xmm6,  0x200(\\base)\n"
	"	movups %xmm7,  0x210(\\base)\n"
	"	movups %xmm8,  0x220(\\base)\n"
	"	movups %xmm9,  0x230(\\base)\n"
	"	movups %xmm10, 0x240(\\base)\n"
	"	movups %xmm11, 0x250(\\base)\n"
	"	movups %xmm12, 0x260(\\base)\n"
	"	movups %xmm13, 0x270(\\base)\n"
	"	movups %xmm14, 0x280(\\base)\n"
	"	movups %xmm15, 0x290(\\base)\n"
	"	stmxcsr 0x34(\\base)\n"
	"	movl $0x10000F, 0x30(\\base)\n"
	".endm\n"

	/* void RtlCaptureContext(CONTEXT *c): the caller's state at the call. */
	".globl RtlCaptureContext\n"
	"RtlCaptureContext:\n"
	"	SAVEREGS %rcx\n"
	"	leaq 8(%rsp), %rax\n"
	"	movq %rax, 0x98(%rcx)\n"
	"	movq (%rsp), %rax\n"
	"	movq %rax, 0xF8(%rcx)\n"
	"	pushfq\n"
	"	popq %rax\n"
	"	movl %eax, 0x44(%rcx)\n"
	"	movq 0x78(%rcx), %rax\n"
	"	ret\n"

	/* void RtlRestoreContext(CONTEXT *c, EXCEPTION_RECORD *r): does not return. */
	".globl RtlRestoreContext\n"
	"RtlRestoreContext:\n"
	"	movq %rcx, %r11\n"
	"	movups 0x200(%r11), %xmm6\n"
	"	movups 0x210(%r11), %xmm7\n"
	"	movups 0x220(%r11), %xmm8\n"
	"	movups 0x230(%r11), %xmm9\n"
	"	movups 0x240(%r11), %xmm10\n"
	"	movups 0x250(%r11), %xmm11\n"
	"	movups 0x260(%r11), %xmm12\n"
	"	movups 0x270(%r11), %xmm13\n"
	"	movups 0x280(%r11), %xmm14\n"
	"	movups 0x290(%r11), %xmm15\n"
	"	movq 0x80(%r11), %rcx\n"
	"	movq 0x88(%r11), %rdx\n"
	"	movq 0x90(%r11), %rbx\n"
	"	movq 0xA0(%r11), %rbp\n"
	"	movq 0xA8(%r11), %rsi\n"
	"	movq 0xB0(%r11), %rdi\n"
	"	movq 0xB8(%r11), %r8\n"
	"	movq 0xC0(%r11), %r9\n"
	"	movq 0xD8(%r11), %r12\n"
	"	movq 0xE0(%r11), %r13\n"
	"	movq 0xE8(%r11), %r14\n"
	"	movq 0xF0(%r11), %r15\n"
	"	movq 0x98(%r11), %rax\n"
	"	movq 0xF8(%r11), %r10\n"
	"	movq %r10, -8(%rax)\n"
	"	leaq -8(%rax), %rsp\n"
	"	movq 0x78(%r11), %rax\n"
	"	movq 0xC8(%r11), %r10\n"
	"	movq 0xD0(%r11), %r11\n"
	"	ret\n"

	/*
	 * Entry points that need the caller's context: capture it on the stack
	 * and call worker(a, b, c, d, CONTEXT *) with the caller's arguments.
	 */
	".macro CAPSTUB name, worker\n"
	".globl \\name\n"
	"\\name:\n"
	"	subq $0x508, %rsp\n"
	"	movq %r11, 0x28(%rsp)\n"
	"	leaq 0x30(%rsp), %r11\n"
	"	SAVEREGS %r11\n"
	"	movq 0x28(%rsp), %rax\n"
	"	movq %rax, 0x30+0xD0(%rsp)\n"
	"	leaq 0x510(%rsp), %rax\n"
	"	movq %rax, 0x30+0x98(%rsp)\n"
	"	movq 0x508(%rsp), %rax\n"
	"	movq %rax, 0x30+0xF8(%rsp)\n"
	"	movq %r11, 0x20(%rsp)\n"
	"	call \\worker\n"
	"	addq $0x508, %rsp\n"
	"	ret\n"
	".endm\n"
	"	CAPSTUB RtlRaiseException, exc_raise\n"
	"	CAPSTUB RtlUnwind, exc_unwind\n"
	"	CAPSTUB RtlRaiseStatus, exc_raise_status\n"
	".text\n"
);

API void RtlCaptureContext(void *ctx);
API void RtlRestoreContext(void *ctx, struct exc_record *rec) __attribute__((noreturn));
API void RtlRaiseException(struct exc_record *rec);
API void RtlRaiseStatus(NTSTATUS status);
API void exc_raise(struct exc_record *rec, u64 b, u64 c, u64 d, u8 *ctx);
API void exc_unwind(u64 target, void *ip, struct exc_record *rec, void *retval, u8 *ctx);
API void exc_raise_status(u64 status, u64 b, u64 c, u64 d, u8 *ctx);
API void RtlUnwind(u64 frame, void *ip, struct exc_record *rec, void *retval);
EXPORT_AS("RtlCaptureContext", RtlCaptureContext);
EXPORT_AS("RtlRestoreContext", RtlRestoreContext);
EXPORT_AS("RtlRaiseException", RtlRaiseException);
EXPORT_AS("RtlUnwind", RtlUnwind);
EXPORT_AS("RtlRaiseStatus", RtlRaiseStatus);

static API void w_RtlCopyContext(void *dst, u32 flags, void *src)
{
	(void)flags;
	memcpy(dst, src, CX_SIZE);
}
EXPORT(RtlCopyContext);

/* ---- function table lookup and unwinding ---------------------------------------- */

static const RTF *lookup(u64 pc, u64 *base)
{
	u64 b = ldr_module_of(pc);
	u8 *nt;
	u32 rva, size, lo = 0, hi;
	const RTF *tab;

	*base = 0;
	if (!b)
		return NULL;
	nt = (u8 *)(b + *(u32 *)(b + 0x3C));
	rva = *(u32 *)(nt + 0x88 + 3 * 8);
	size = *(u32 *)(nt + 0x88 + 3 * 8 + 4);
	if (!rva)
		return NULL;
	*base = b;
	tab = (const RTF *)(b + rva);
	hi = size / sizeof(RTF);
	while (lo < hi) {
		u32 mid = (lo + hi) / 2;
		u32 off = (u32)(pc - b);

		if (off < tab[mid].begin)
			hi = mid;
		else if (off >= tab[mid].end)
			lo = mid + 1;
		else
			return &tab[mid];
	}
	return NULL;
}

/* Undo one frame in ctx. Returns the handler of kind `want`, if any. */
static void *unwind_frame(u32 want, u64 base, u64 pc, const RTF *f, u8 *ctx, void **hdata,
			  u64 *frame)
{
	void *handler = NULL;
	const RTF *cur = f;
	bool first = true, mach = false;
	u64 off = pc - (base + f->begin);

	*hdata = NULL;
	*frame = REG(ctx, 4);
	for (;;) {
		const u8 *ui = (const u8 *)(base + cur->unwind);
		u8 flags = ui[0] >> 3, prolog = ui[1], count = ui[2], fr = ui[3] & 15, fo = ui[3] >> 4;
		const u16 *codes = (const u16 *)(ui + 4);
		bool partial = first && off < prolog;

		for (u32 i = 0; i < count; ) {
			u8 co = (u8)codes[i], op = (codes[i] >> 8) & 15, info = codes[i] >> 12;
			u32 slots = 1;

			if (op == 1)
				slots = info ? 3 : 2;
			else if (op == 4 || op == 8)
				slots = 2;
			else if (op == 5 || op == 9)
				slots = 3;
			if (partial && co > off) {
				i += slots;
				continue;
			}
			switch (op) {
			case 0:                 /* PUSH_NONVOL */
				REG(ctx, info) = *(u64 *)REG(ctx, 4);
				REG(ctx, 4) += 8;
				break;
			case 1: {               /* ALLOC_LARGE */
				u64 sz = info ? (u64)codes[i + 1] | (u64)codes[i + 2] << 16
					      : (u64)codes[i + 1] * 8;

				REG(ctx, 4) += sz;
				break;
			}
			case 2:                 /* ALLOC_SMALL */
				REG(ctx, 4) += (u64)info * 8 + 8;
				break;
			case 3:                 /* SET_FPREG */
				*frame = REG(ctx, fr) - (u64)fo * 16;
				REG(ctx, 4) = *frame;
				break;
			case 4:                 /* SAVE_NONVOL */
				REG(ctx, info) = *(u64 *)(REG(ctx, 4) + (u64)codes[i + 1] * 8);
				break;
			case 5:                 /* SAVE_NONVOL_FAR */
				REG(ctx, info) = *(u64 *)(REG(ctx, 4) +
					((u64)codes[i + 1] | (u64)codes[i + 2] << 16));
				break;
			case 8:                 /* SAVE_XMM128 */
				memcpy(ctx + CX_XMM0 + 16 * info,
				       (void *)(REG(ctx, 4) + (u64)codes[i + 1] * 16), 16);
				break;
			case 9:                 /* SAVE_XMM128_FAR */
				memcpy(ctx + CX_XMM0 + 16 * info, (void *)(REG(ctx, 4) +
				       ((u64)codes[i + 1] | (u64)codes[i + 2] << 16)), 16);
				break;
			case 10: {              /* PUSH_MACHFRAME */
				u64 sp = REG(ctx, 4) + (info ? 8 : 0);

				R64(ctx, CX_RIP) = *(u64 *)sp;
				REG(ctx, 4) = *(u64 *)(sp + 24);
				mach = true;
				break;
			}
			default:
				break;
			}
			i += slots;
		}
		if (flags & 4) {                /* UNW_FLAG_CHAININFO */
			cur = (const RTF *)(ui + 4 + ((count + 1) & ~1u) * 2);
			first = false;
			continue;
		}
		if (flags & want) {
			const u32 *h = (const u32 *)(ui + 4 + ((count + 1) & ~1u) * 2);

			handler = (void *)(base + h[0]);
			*hdata = (void *)(h + 1);
		}
		break;
	}
	if (!mach) {
		R64(ctx, CX_RIP) = *(u64 *)REG(ctx, 4);
		REG(ctx, 4) += 8;
	}
	return handler;
}

static API void *w_RtlVirtualUnwind(u32 type, u64 base, u64 pc, const RTF *f, u8 *ctx,
				    void **hdata, u64 *frame, void *ptrs)
{
	void *h;
	void *dummy;
	u64 fr;

	(void)ptrs;
	if (!f) {
		R64(ctx, CX_RIP) = *(u64 *)REG(ctx, 4);
		REG(ctx, 4) += 8;
		if (frame)
			*frame = REG(ctx, 4);
		return NULL;
	}
	h = unwind_frame(type, base, pc, f, ctx, hdata ? hdata : &dummy, frame ? frame : &fr);
	return h;
}
EXPORT(RtlVirtualUnwind);

static API const RTF *w_RtlLookupFunctionEntry(u64 pc, u64 *base, void *hist)
{
	const RTF *f = lookup(pc, base);

	(void)hist;
	return f;
}
EXPORT(RtlLookupFunctionEntry);

/* ---- dispatch ---------------------------------------------------------------------- */

static void *unhandled_filter;

static API void *w_RtlSetUnhandledExceptionFilter(void *f)
{
	void *old = unhandled_filter;

	unhandled_filter = f;
	return old;
}
EXPORT(RtlSetUnhandledExceptionFilter);

static void hex(char *out, u64 v)
{
	static const char d[] = "0123456789abcdef";
	int n = 0;

	for (u64 t = v; t >> 4; t >>= 4)
		n++;
	for (int i = n; i >= 0; i--, v >>= 4)
		out[i] = d[v & 15];
	out[n + 1] = 0;
}

static void unhandled(struct exc_record *rec, u8 *ctx)
{
	char h[24];
	struct { struct exc_record *rec; u8 *ctx; } ep = { rec, ctx };

	if (unhandled_filter) {
		long r = ((API long (*)(void *))unhandled_filter)(&ep);

		if (r < 0) {
			RtlRestoreContext(ctx, rec);
		}
	}
	rt_write(2, "unhandled exception 0x", 22);
	hex(h, rec->code);
	rt_write(2, h, rt_strlen(h));
	rt_write(2, " at 0x", 6);
	hex(h, (u64)rec->address);
	rt_write(2, h, rt_strlen(h));
	rt_write(2, "\n", 1);
	rt_exit(rec->code);
}

/* Handlers see the stack of the frame being examined, so walk a copy. */
API void exc_raise(struct exc_record *rec, u64 b, u64 c, u64 d, u8 *ctx)
{
	u8 cur[CX_SIZE] __attribute__((aligned(16)));
	u8 pre[CX_SIZE] __attribute__((aligned(16)));
	u64 stack_lo = *(u64 *)(nt_teb() + 0x10), stack_hi = *(u64 *)(nt_teb() + 8);
	bool first = true;

	(void)b; (void)c; (void)d;
	if (!rec->address)
		rec->address = (void *)R64(ctx, CX_RIP);
	memcpy(cur, ctx, CX_SIZE);
	for (int depth = 0; depth < 1024; depth++) {
		u64 pc = R64(cur, CX_RIP), base = 0, frame;
		const RTF *f;
		void *hdata, *handler;

		if (pc < 0x1000 || REG(cur, 4) < stack_lo || REG(cur, 4) >= stack_hi)
			break;
		memcpy(pre, cur, CX_SIZE);
		f = lookup(first ? pc : pc - 1, &base);
		if (!f) {
			R64(cur, CX_RIP) = *(u64 *)REG(cur, 4);
			REG(cur, 4) += 8;
			first = false;
			continue;
		}
		handler = unwind_frame(1, base, pc, f, cur, &hdata, &frame);
		if (handler) {
			struct disp_ctx dc = { pc, base, f, frame, 0, pre, handler, hdata, NULL, 0, 0 };
			int r = ((API int (*)(void *, u64, void *, void *))handler)(rec, frame, pre, &dc);

			if (r == 0) {
				if (rec->flags & EXC_NONCONTINUABLE) {
					struct exc_record nr = { STATUS_NONCONTINUABLE_EXCEPTION,
								 EXC_NONCONTINUABLE, rec, rec->address, 0, { 0 } };

					exc_raise(&nr, 0, 0, 0, ctx);
				}
				RtlRestoreContext(ctx, rec);
			}
		}
		first = false;
	}
	unhandled(rec, ctx);
}

static void unwind_ex(u64 target, void *target_ip, struct exc_record *rec, void *retval, u8 *ctx)
{
	u8 cur[CX_SIZE] __attribute__((aligned(16)));
	u8 pre[CX_SIZE] __attribute__((aligned(16)));
	struct exc_record local;
	u64 stack_lo = *(u64 *)(nt_teb() + 0x10), stack_hi = *(u64 *)(nt_teb() + 8);
	bool first = true;

	if (!rec) {
		memset(&local, 0, sizeof(local));
		local.code = STATUS_UNWIND;
		local.address = (void *)R64(ctx, CX_RIP);
		rec = &local;
	}
	rec->flags |= EXC_UNWINDING | (target ? 0 : EXC_EXIT_UNWIND);
	memcpy(cur, ctx, CX_SIZE);
	for (int depth = 0; depth < 1024; depth++) {
		u64 pc = R64(cur, CX_RIP), base = 0, frame;
		const RTF *f;
		void *hdata, *handler;

		if (pc < 0x1000 || REG(cur, 4) < stack_lo || REG(cur, 4) >= stack_hi)
			break;
		memcpy(pre, cur, CX_SIZE);
		f = lookup(first ? pc : pc - 1, &base);
		if (!f) {
			R64(cur, CX_RIP) = *(u64 *)REG(cur, 4);
			REG(cur, 4) += 8;
			first = false;
			continue;
		}
		handler = unwind_frame(2, base, pc, f, cur, &hdata, &frame);
		if (target && frame > target)
			break;
		if (handler) {
			struct disp_ctx dc = { pc, base, f, frame, (u64)target_ip, pre, handler, hdata,
					       NULL, 0, 0 };
			u32 saved = rec->flags;

			if (frame == target)
				rec->flags |= EXC_TARGET_UNWIND;
			((API int (*)(void *, u64, void *, void *))handler)(rec, frame, pre, &dc);
			rec->flags = saved;
		}
		if (target && frame == target) {
			R64(pre, CX_RIP) = (u64)target_ip;
			R64(pre, CX_RAX) = (u64)retval;
			RtlRestoreContext(pre, rec);
		}
		first = false;
	}
	if (!target)
		return;
	/* Target frame not found: nothing sane left to do. */
	rt_exit(0xC0000026);        /* STATUS_INVALID_DISPOSITION */
}

API void exc_unwind(u64 target, void *ip, struct exc_record *rec, void *retval, u8 *ctx)
{
	unwind_ex(target, ip, rec, retval, ctx);
}

static API void w_RtlUnwindEx(u64 target, void *ip, struct exc_record *rec, void *retval,
			      u8 *ctx, void *hist)
{
	(void)hist;
	unwind_ex(target, ip, rec, retval, ctx);
}
EXPORT(RtlUnwindEx);

API void exc_raise_status(u64 status, u64 b, u64 c, u64 d, u8 *ctx)
{
	struct exc_record rec;

	(void)b; (void)c; (void)d;
	memset(&rec, 0, sizeof(rec));
	rec.code = (u32)status;
	rec.flags = EXC_NONCONTINUABLE;
	rec.address = (void *)R64(ctx, CX_RIP);
	exc_raise(&rec, 0, 0, 0, ctx);
}

/* Language handler of __try / __except / __finally (scope tables). */
static API int w___C_specific_handler(struct exc_record *rec, u64 frame, u8 *ctx,
				      struct disp_ctx *dc)
{
	const u32 *tab = dc->handler_data;
	u32 count = tab[0];
	u64 base = dc->image_base, rva = dc->control_pc - base;

	for (u32 i = dc->scope_index; i < count; i++) {
		const u32 *sc = tab + 1 + 4 * i;     /* begin, end, handler, target */

		if (rva < sc[0] || rva >= sc[1])
			continue;
		if (rec->flags & (EXC_UNWINDING | EXC_EXIT_UNWIND)) {
			if (sc[3]) {
				if ((rec->flags & EXC_TARGET_UNWIND) && dc->target_ip == base + sc[3])
					return 1;
				continue;
			}
			if (sc[2] > 1)
				((API void (*)(int, u64))(base + sc[2]))(1, frame);
		} else if (sc[3]) {
			struct { struct exc_record *rec; u8 *ctx; } ep = { rec, ctx };
			int r = 1;

			if (sc[2] != 1)
				r = ((API int (*)(void *, u64))(base + sc[2]))(&ep, frame);
			if (r < 0)
				return 0;
			if (r > 0)
				w_RtlUnwindEx(frame, (void *)(base + sc[3]), rec,
					      (void *)(u64)rec->code, dc->ctx, dc->history);
		}
	}
	return 1;
}
EXPORT(__C_specific_handler);
