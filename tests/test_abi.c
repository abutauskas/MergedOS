/* SPDX-License-Identifier: BSD-2-Clause */
#include "tests/test.h"
#include "abi_dispatch/abi.h"
#include "abi_dispatch/binfmt.h"

/* Every prefix and every single-byte corruption must be handled without a crash. */
static void torture(const u8 *img, size_t len)
{
	struct exec_plan plan;
	u8 *copy = malloc(len);

	for (size_t n = 0; n <= len; n++) {
		u8 *exact = malloc(n ? n : 1);  /* exact-size buffer: ASan sees any overread */

		memcpy(exact, img, n);
		(void)binfmt_load_plan(exact, n, &plan);
		free(exact);
	}
	for (size_t i = 0; i < len; i++) {
		for (int bit = 0; bit < 8; bit += 3) {
			memcpy(copy, img, len);
			copy[i] ^= (u8)(1u << bit);
			if (!binfmt_load_plan(copy, len, &plan)) {
				for (u32 s = 0; s < plan.nsegs; s++)
					CHECK(mos_in_range(len, plan.seg[s].fileoff, plan.seg[s].filesz));
			}
		}
	}
	free(copy);
}

void test_binfmt(void)
{
	struct exec_plan plan;
	struct binfmt_info info;
	size_t len;
	u8 *img;

	/* Linux ELF: unbranded System V image -> Linux by fallback brand. */
	img = load_fixture("bin/hello.linux", &len);
	CHECK_EQ(binfmt_load_plan(img, len, &plan), 0);
	CHECK_EQ(plan.info.format, BINFMT_ELF64);
	CHECK_EQ(plan.info.arch, BIN_ARCH_X86_64);
	CHECK_EQ(plan.info.personality, ABI_LINUX);
	CHECK_EQ(plan.nsegs, 2);
	CHECK_EQ(plan.seg[0].vaddr, 0x400000);
	CHECK_EQ(plan.seg[0].prot, EXEC_PROT_R | EXEC_PROT_X);
	CHECK_EQ(plan.seg[1].prot, EXEC_PROT_R | EXEC_PROT_W);
	CHECK(plan.seg[1].memsz > plan.seg[1].filesz);          /* has .bss */
	CHECK(plan.entry > 0x400000 && plan.entry < 0x401000);

	/* Same container, different brand byte: the personality must follow it. */
	img[7] = 9;
	CHECK_EQ(binfmt_probe(img, len, &info), 0);
	CHECK_EQ(info.personality, ABI_FREEBSD);
	img[7] = 3;
	CHECK_EQ(binfmt_probe(img, len, &info), 0);
	CHECK_EQ(info.personality, ABI_LINUX);
	img[7] = 0;

	/* Turn the data segment into PT_INTERP: now it needs a dynamic linker,
	 * and its contents are no NUL-terminated path, so it is refused. */
	img[64 + 56] = 3;
	CHECK_EQ(binfmt_load_plan(img, len, &plan), -MERR_NOEXEC);
	CHECK(plan.info.flags & BINFMT_F_DYNAMIC);
	img[64 + 56] = 1;
	/* ET_DYN: planned with relative addresses (the kernel adds a bias). */
	img[16] = 3;
	CHECK_EQ(binfmt_load_plan(img, len, &plan), 0);
	CHECK(plan.relative && plan.info.flags & BINFMT_F_PIE);
	CHECK_EQ(plan.interp_len, 0);
	img[16] = 2;
	torture(img, len);
	free(img);

	img = load_fixture("bin/hello.freebsd", &len);
	CHECK_EQ(binfmt_load_plan(img, len, &plan), 0);
	CHECK_EQ(plan.info.personality, ABI_FREEBSD);
	CHECK_EQ(plan.nsegs, 1);
	torture(img, len);
	free(img);

	/* Mach-O: __PAGEZERO is a reservation and must not become a mapping. */
	img = load_fixture("bin/hello.macho", &len);
	CHECK_EQ(binfmt_load_plan(img, len, &plan), 0);
	CHECK_EQ(plan.info.format, BINFMT_MACHO64);
	CHECK_EQ(plan.info.personality, ABI_XNU);
	CHECK_EQ(plan.nsegs, 1);
	CHECK_EQ(plan.seg[0].vaddr, 0x100000000ULL);
	CHECK(plan.entry > 0x100000000ULL);
	torture(img, len);
	free(img);

	/* PE32+: headers mapped read-only at the image base, then one section. */
	img = load_fixture("bin/hello.exe", &len);
	CHECK_EQ(binfmt_load_plan(img, len, &plan), 0);
	CHECK_EQ(plan.info.format, BINFMT_PE32PLUS);
	CHECK_EQ(plan.info.personality, ABI_NT);
	CHECK(!(plan.info.flags & (BINFMT_F_GUI | BINFMT_F_DYNAMIC)));
	CHECK_EQ(plan.nsegs, 2);
	CHECK_EQ(plan.seg[0].vaddr, 0x140000000ULL);
	CHECK_EQ(plan.seg[0].prot, EXEC_PROT_R);
	CHECK_EQ(plan.seg[1].vaddr, 0x140001000ULL);
	CHECK_EQ(plan.seg[1].prot, EXEC_PROT_R | EXEC_PROT_X);
	/* Give it an import directory: flagged dynamic, and still planned - the
	 * kernel's NT loader binds imports (tests/test_pe.c covers the walk). */
	img[0x40 + 4 + 20 + 112 + 8 + 4] = 0x28;
	CHECK_EQ(binfmt_load_plan(img, len, &plan), 0);
	CHECK(plan.info.flags & BINFMT_F_DYNAMIC);
	img[0x40 + 4 + 20 + 112 + 8 + 4] = 0;
	/* A DLL is not a program. */
	img[0x40 + 4 + 18 + 1] |= 0x20;
	CHECK_EQ(binfmt_load_plan(img, len, &plan), -MERR_NOTSUP);
	CHECK(plan.info.flags & BINFMT_F_LIBRARY);
	img[0x40 + 4 + 18 + 1] &= (u8)~0x20;
	/* GUI subsystem flag. */
	img[0x40 + 4 + 20 + 68] = 2;
	CHECK_EQ(binfmt_probe(img, len, &info), 0);
	CHECK(info.flags & BINFMT_F_GUI);
	img[0x40 + 4 + 20 + 68] = 3;
	torture(img, len);
	free(img);

	/* Things that are not ours. */
	{
		static const u8 java[] = { 0xCA, 0xFE, 0xBA, 0xBE, 0, 0, 0, 52, 0, 0, 0, 0 };
		static const u8 script[] = "#!/bin/sh\necho hi\n";
		static const u8 text[] = "plain text";
		static const u8 dos[0x40] = { 'M', 'Z' };

		CHECK_EQ(binfmt_probe(java, sizeof(java), &info), -MERR_NOEXEC);
		CHECK_EQ(binfmt_probe(script, sizeof(script), &info), 0);
		CHECK_EQ(info.format, BINFMT_SCRIPT);
		CHECK_EQ(binfmt_probe(text, sizeof(text), &info), -MERR_NOEXEC);
		CHECK_EQ(binfmt_probe(dos, sizeof(dos), &info), -MERR_NOEXEC);
		CHECK_EQ(binfmt_probe(text, 0, &info), -MERR_NOEXEC);
	}
}

/* ---- dispatcher ------------------------------------------------------------- */

#define BASE 0x10000ULL

static struct task task;
static struct abi_ctx ctx;

static void reset(enum abi_personality p)
{
	memset(&task, 0, sizeof(task));
	task.pid = 42;
	task.base = BASE;
	memcpy(task.mem + 16, "hello", 5);
	abi_ctx_init(&ctx, p, &task);
}

static struct syscall_frame posix_frame(u64 nr, u64 a0, u64 a1, u64 a2)
{
	struct syscall_frame f = { .rax = nr, .rdi = a0, .rsi = a1, .rdx = a2, .rflags = 0x202 };

	return f;
}

/* NtWriteFile frame: four register args, five more on the "stack" at rsp + 0x28. */
static struct syscall_frame nt_write_frame(u64 handle, u64 buf, u64 length)
{
	struct syscall_frame f = { .rax = 0x08, .r10 = handle, .rsp = BASE + 0x100,
				   .rflags = 0x202 };
	u64 stack_args[5] = { BASE + 0x200 /* IoStatusBlock */, buf, length, 0, 0 };

	memcpy(task.mem + 0x100 + 0x28, stack_args, sizeof(stack_args));
	return f;
}

void test_dispatch(void)
{
	struct syscall_frame f;
	u64 info;

	/* Linux: number 1, result or -errno in rax. */
	reset(ABI_LINUX);
	f = posix_frame(1, 1, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ(f.rax, 5);
	CHECK(task.out_len == 5 && !memcmp(task.out, "hello", 5));
	f = posix_frame(1, 99, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ((s64)f.rax, -9);                               /* -EBADF  */
	f = posix_frame(1, 1, 0xdead0000, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ((s64)f.rax, -14);                              /* -EFAULT */
	f = posix_frame(511, 0, 0, 0);
	abi_dispatch(&ctx, &f);
	CHECK_EQ((s64)f.rax, -38);                              /* -ENOSYS */
	CHECK_EQ(ctx.stats.unknown, 1);
	CHECK_EQ(ctx.stats.last_unknown_nr, 511);
	f = posix_frame(39, 0, 0, 0);
	abi_dispatch(&ctx, &f);
	CHECK_EQ(f.rax, 42);                                    /* getpid  */
	f = posix_frame(4, 1, BASE + 16, 5);                    /* 4 is stat on Linux, not write */
	abi_dispatch(&ctx, &f);
	CHECK_EQ((s64)f.rax, -38);
	f = posix_frame(231, 7, 0, 0);
	abi_dispatch(&ctx, &f);
	CHECK(task.exited && task.exit_code == 7);
	CHECK_EQ(ctx.stats.calls, 7);
	CHECK_EQ(ctx.stats.errors, 4);

	/* FreeBSD: number 4, carry flag + positive errno. */
	reset(ABI_FREEBSD);
	f = posix_frame(4, 1, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ(f.rax, 5);
	CHECK(!(f.rflags & RFLAGS_CF));
	f = posix_frame(4, 99, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ(f.rax, 9);
	CHECK(f.rflags & RFLAGS_CF);
	f = posix_frame(999, 0, 0, 0);
	abi_dispatch(&ctx, &f);
	CHECK_EQ(f.rax, 78);                                    /* ENOSYS is 78 on BSD */
	CHECK(f.rflags & RFLAGS_CF);
	f = posix_frame(1, 3, 0, 0);                            /* 1 is exit here, write on Linux */
	abi_dispatch(&ctx, &f);
	CHECK(task.exited && task.exit_code == 3);
	/* A successful call must clear a stale carry flag. */
	f = posix_frame(20, 0, 0, 0);
	f.rflags |= RFLAGS_CF;
	abi_dispatch(&ctx, &f);
	CHECK_EQ(f.rax, 42);
	CHECK(!(f.rflags & RFLAGS_CF));

	/* XNU: class byte selects the table. */
	reset(ABI_XNU);
	f = posix_frame(0x2000004, 1, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ(f.rax, 5);
	CHECK(!(f.rflags & RFLAGS_CF));
	f = posix_frame(0x2000004, 99, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK(f.rax == 9 && (f.rflags & RFLAGS_CF));
	f = posix_frame(4, 1, BASE + 16, 5);                    /* no class: not a BSD call */
	abi_dispatch(&ctx, &f);
	CHECK(f.rax == 78 && (f.rflags & RFLAGS_CF));
	f = posix_frame(0x1000004, 0, 0, 0);                    /* Mach trap: kern_return_t */
	abi_dispatch(&ctx, &f);
	CHECK_EQ(f.rax, 4);                                     /* KERN_INVALID_ARGUMENT */
	CHECK(!(f.rflags & RFLAGS_CF));
	f = posix_frame(0x2000001, 5, 0, 0);
	abi_dispatch(&ctx, &f);
	CHECK(task.exited && task.exit_code == 5);

	/* NT: stack arguments, handle translation, IO_STATUS_BLOCK, NTSTATUS. */
	reset(ABI_NT);
	f = nt_write_frame(8, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_SUCCESS);
	CHECK(task.out_len == 5 && !memcmp(task.out, "hello", 5));
	memcpy(&info, task.mem + 0x200 + 8, 8);
	CHECK_EQ(info, 5);                                      /* IoStatusBlock.Information */
	f = nt_write_frame(0x1234, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_INVALID_HANDLE);
	CHECK_EQ(f.rax >> 32, 0xFFFFFFFFu);                     /* sign-extended NTSTATUS */
	f = nt_write_frame(8, 0xdead0000, 5);
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_ACCESS_VIOLATION);
	f = nt_write_frame(8, BASE + 16, 5);
	f.rsp = 0xdead0000;                                     /* unreadable stack */
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_ACCESS_VIOLATION);
	f = nt_write_frame(8, BASE + 16, 5);
	f.rax = 0xFFF;
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_INVALID_SYSTEM_SERVICE);
	f = nt_write_frame(8, BASE + 16, 5);
	f.rax = 0x1008;                                         /* win32k table: not ours */
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_INVALID_SYSTEM_SERVICE);
	f = (struct syscall_frame){ .rax = 0x2C, .r10 = 0x44, .rdx = 9 };
	abi_dispatch(&ctx, &f);                                 /* someone else's process */
	CHECK_EQ((u32)f.rax, NT_STATUS_INVALID_HANDLE);
	CHECK(!task.exited);
	f = (struct syscall_frame){ .rax = 0x2C, .r10 = ~0ULL, .rdx = 9 };
	abi_dispatch(&ctx, &f);
	CHECK(task.exited && task.exit_code == 9);

	/* NtQuerySystemTime: FILETIME, 100 ns since 1601 (the mock clock is at 1970). */
	reset(ABI_NT);
	f = (struct syscall_frame){ .rax = 0x5A, .r10 = BASE + 64 };
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_SUCCESS);
	CHECK_EQ(rd64le(task.mem + 64), 116444736000000000ULL);
	f = (struct syscall_frame){ .rax = 0x5A, .r10 = 0xdead0000 };
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_ACCESS_VIOLATION);
	/* NtQueryPerformanceCounter reports a 10 MHz frequency. */
	f = (struct syscall_frame){ .rax = 0x31, .r10 = BASE + 64, .rdx = BASE + 72 };
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_SUCCESS);
	CHECK_EQ(rd64le(task.mem + 72), 10000000);
	/* NtAllocateVirtualMemory: zero size and foreign processes are refused. */
	memset(task.mem + 64, 0, 16);
	f = (struct syscall_frame){ .rax = 0x18, .r10 = ~0ULL, .rdx = BASE + 64, .r9 = BASE + 72,
				    .rsp = BASE + 256 };
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_INVALID_PARAMETER);
	f.rax = 0x18;
	f.r10 = 0x44;
	abi_dispatch(&ctx, &f);
	CHECK_EQ((u32)f.rax, NT_STATUS_INVALID_HANDLE);

	/* Error numbering really does differ between the dialects. */
	CHECK_EQ(abi_errno_linux(MERR_AGAIN), 11);
	CHECK_EQ(abi_errno_bsd(MERR_AGAIN), 35);
	CHECK_EQ(abi_errno_linux(MERR_NOSYS), 38);
	CHECK_EQ(abi_errno_bsd(MERR_NOSYS), 78);
	CHECK_EQ(abi_ntstatus(MERR_NOMEM), NT_STATUS_NO_MEMORY);
	CHECK_EQ(abi_ntstatus(0), NT_STATUS_SUCCESS);
	CHECK_EQ(abi_errno_linux(9999), 5);                     /* unknown -> EIO */

	/* A task with no personality cannot be answered: it is terminated. */
	reset(ABI_NONE);
	f = posix_frame(1, 1, BASE + 16, 5);
	abi_dispatch(&ctx, &f);
	CHECK(task.exited && task.exit_code == 127);
}
