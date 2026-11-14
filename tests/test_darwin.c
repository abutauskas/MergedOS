/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Darwin user space (docs/DARWIN_USERSPACE.md): Mach-O dynamic-exec plans,
 * the XNU system-call tables, import reporting and the .app "what is missing"
 * report. The real Mach-O programs are built by the Makefile with the host
 * clang and ld64.lld; without them the image-based checks are skipped.
 */
#include "tests/test.h"
#include "abi_dispatch/abi.h"
#include "abi_dispatch/binfmt.h"
#include "pkg_translate/pkg.h"

#ifndef DARWIN_ROOT
#define DARWIN_ROOT "build/guest/darwin/root"
#endif

static u8 *load_darwin(const char *rel, size_t *len)
{
	char path[512];
	FILE *f;
	u8 *buf;

	snprintf(path, sizeof(path), "%s/%s", DARWIN_ROOT, rel);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(*len ? *len : 1);
	if (fread(buf, 1, *len, f) != *len || !*len) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

/* ---- system calls ------------------------------------------------------------ */

#define BASE 0x10000ULL
static struct task task;
static struct abi_ctx ctx;

static struct syscall_frame call(u64 rax, u64 a0, u64 a1, u64 a2, u64 a3)
{
	struct syscall_frame f = { .rax = rax, .rdi = a0, .rsi = a1, .rdx = a2, .r10 = a3 };

	abi_dispatch(&ctx, &f);
	return f;
}

static void test_calls(void)
{
	struct syscall_frame f;
	u64 len, v64;
	u32 v32, msg[9] = { 0, 24, 0x111, 0x222, 0, 3200 };

	memset(&task, 0, sizeof(task));
	task.base = BASE;
	abi_ctx_init(&ctx, ABI_XNU, &task);

	/* Mach traps: fixed port names, no carry flag. */
	f = call(0x1000000 | 28, 0, 0, 0, 0);
	CHECK(f.rax == 0x103 && !(f.rflags & RFLAGS_CF));
	f = call(0x1000000 | 27, 0, 0, 0, 0);
	CHECK_EQ(f.rax, 0x203);
	f = call(0x1000000 | 29, 0, 0, 0, 0);
	CHECK_EQ(f.rax, 0x303);
	f = call(0x1000000 | 26, 0, 0, 0, 0);
	CHECK_EQ(f.rax, 0x403);

	/* mach_timebase_info: 1/1, i.e. mach_absolute_time counts nanoseconds. */
	f = call(0x1000000 | 89, BASE + 8, 0, 0, 0);
	CHECK_EQ(f.rax, 0);
	memcpy(&v64, task.mem + 8, 8);
	CHECK_EQ(v64, 0x100000001ULL);

	/* mach_msg(send|receive): a MIG error reply in place; send-only: invalid dest. */
	memcpy(task.mem + 256, msg, sizeof(msg));
	f = call(0x1000000 | 31, BASE + 256, 3, 24, 36);
	CHECK_EQ(f.rax, 0);
	memcpy(msg, task.mem + 256, sizeof(msg));
	CHECK_EQ(msg[2], 0x222);                                /* remote = the reply port */
	CHECK_EQ(msg[5], 3300);                                 /* id + 100                */
	CHECK_EQ(msg[8], 46);                                   /* KERN_NOT_SUPPORTED      */
	f = call(0x1000000 | 31, BASE + 256, 1, 24, 0);
	CHECK_EQ(f.rax, 0x10000003u);                           /* MACH_SEND_INVALID_DEST  */
	f = call(0x1000000 | 31, 0xdead0000, 3, 24, 36);        /* unreadable header       */
	CHECK_EQ(f.rax, 0x10000003u);

	/* BSD: shared_region_check_np -> ENOMEM so dyld maps libraries itself. */
	f = call(0x2000000 | 294, BASE, 0, 0, 0);
	CHECK(f.rax == 12 && (f.rflags & RFLAGS_CF));
	f = call(0x2000000 | 327, 0, 0, 0, 0);                  /* issetugid */
	CHECK(f.rax == 0 && !(f.rflags & RFLAGS_CF));
	f = call(0x2000000 | 4999, 0, 0, 0, 0);                 /* unknown: ENOSYS */
	CHECK(f.rax == 78 && (f.rflags & RFLAGS_CF));
	f = call(0x2000000 | 220, 0, 0, 0, 0);                  /* getattrlist: ENOTSUP */
	CHECK(f.rax == 45 && (f.rflags & RFLAGS_CF));

	/* sysctlbyname: ints, strings, size query, short buffer, unknown name. */
	memcpy(task.mem, "hw.pagesize", 11);
	len = 64;
	memcpy(task.mem + 64, &len, 8);
	f = call(0x2000000 | 274, BASE, 11, BASE + 128, BASE + 64);
	CHECK(f.rax == 0 && !(f.rflags & RFLAGS_CF));
	memcpy(&v32, task.mem + 128, 4);
	memcpy(&len, task.mem + 64, 8);
	CHECK(v32 == 4096 && len == 4);
	memcpy(task.mem, "kern.ostype", 11);
	len = 64;
	memcpy(task.mem + 64, &len, 8);
	f = call(0x2000000 | 274, BASE, 11, BASE + 128, BASE + 64);
	CHECK_EQ(f.rax, 0);
	CHECK_STR((const char *)task.mem + 128, "Darwin");
	len = 3;                                                /* too small: ENOMEM + needed size */
	memcpy(task.mem + 64, &len, 8);
	f = call(0x2000000 | 274, BASE, 11, BASE + 128, BASE + 64);
	CHECK(f.rax == 12 && (f.rflags & RFLAGS_CF));
	memcpy(&len, task.mem + 64, 8);
	CHECK_EQ(len, 7);
	f = call(0x2000000 | 274, BASE, 11, 0, BASE + 64);      /* size query only */
	CHECK_EQ(f.rax, 0);
	memcpy(task.mem, "no.such.key", 11);
	f = call(0x2000000 | 274, BASE, 11, BASE + 128, BASE + 64);
	CHECK(f.rax == 2 && (f.rflags & RFLAGS_CF));

	/* sysctl(mib): { CTL_HW, HW_NCPU }. */
	{
		int mib[2] = { 6, 3 };

		memcpy(task.mem, mib, sizeof(mib));
		len = 64;
		memcpy(task.mem + 64, &len, 8);
		f = call(0x2000000 | 202, BASE, 2, BASE + 128, BASE + 64);
		CHECK_EQ(f.rax, 0);
		memcpy(&v32, task.mem + 128, 4);
		CHECK_EQ(v32, 1);
	}
}

/* ---- Mach-O images -------------------------------------------------------------- */

struct import_log {
	char text[1024];
};

static void log_import(const char *lib, const char *sym, bool weak, void *arg)
{
	struct import_log *l = arg;
	size_t n = strlen(l->text);

	snprintf(l->text + n, sizeof(l->text) - n, "[%s|%s%s]", lib ? lib : "-", sym,
		 weak ? "?" : "");
}

static void test_images(void)
{
	struct exec_plan plan;
	struct import_log log;
	size_t len;
	u8 *img = load_darwin("bin/darwin-hello", &len);

	if (!img) {
		printf("  (Darwin test programs not built: image checks skipped)\n");
		return;
	}
	/* A dynamic program: planned with its dylinker, entry = LC_MAIN, header address kept. */
	CHECK_EQ(binfmt_load_plan(img, len, &plan), 0);
	CHECK_EQ(plan.info.personality, ABI_XNU);
	CHECK(plan.info.flags & BINFMT_F_DYNAMIC);
	CHECK(plan.lc_main);
	CHECK_EQ(plan.image_base, 0x100000000ULL);
	CHECK(plan.entry > plan.image_base && plan.entry < plan.image_base + 0x2000);
	CHECK_EQ(plan.interp_len, 13);
	CHECK(!memcmp(img + plan.interp_off, "/usr/lib/dyld", 14));
	CHECK(plan.nsegs >= 3);
	/* Truncation and header corruption: the dylinker path must stay inside the image. */
	for (size_t n = 0; n <= len; n += 5) {
		u8 *cut = malloc(n ? n : 1);

		memcpy(cut, img, n);
		if (!binfmt_load_plan(cut, n, &plan) && plan.interp_len)
			CHECK(mos_in_range(n, plan.interp_off, (u64)plan.interp_len + 1));
		free(cut);
	}
	for (size_t i = 0; i < 1400 && i < len; i++) {
		u8 *copy = malloc(len);

		memcpy(copy, img, len);
		copy[i] ^= 0x5A;
		if (!binfmt_load_plan(copy, len, &plan) && plan.interp_len)
			CHECK(mos_in_range(len, plan.interp_off, (u64)plan.interp_len + 1));
		free(copy);
	}
	free(img);

	/* The dynamic linker itself: planned at its own high link address. */
	img = load_darwin("usr/lib/dyld", &len);
	CHECK(img != NULL);
	if (img) {
		CHECK_EQ(binfmt_load_plan(img, len, &plan), 0);
		CHECK(plan.image_base >= 0x7ff000000000ULL && plan.entry >= plan.image_base);
		free(img);
	}
	/* A dylib is not a program. */
	img = load_darwin("usr/lib/libSystem.B.dylib", &len);
	CHECK(img != NULL);
	if (img) {
		CHECK_EQ(binfmt_load_plan(img, len, &plan), -MERR_NOTSUP);
		free(img);
	}

	/* Imports through classic bind opcodes, and the framework nobody provides. */
	img = load_darwin("bin/darwin-needs-foundation", &len);
	CHECK(img != NULL);
	if (img) {
		const char *fw = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
		char want[256];

		memset(&log, 0, sizeof(log));
		CHECK_EQ(binfmt_macho_imports(img, len, log_import, &log), 0);
		snprintf(want, sizeof(want), "[%s|_NSLog]", fw);
		CHECK(strstr(log.text, want) != NULL);
		snprintf(want, sizeof(want), "[%s|_NSApplicationMain]", fw);
		CHECK(strstr(log.text, want) != NULL);
		CHECK(strstr(log.text, "[/usr/lib/libSystem.B.dylib|_printf]") != NULL);
		for (size_t n = 0; n <= len; n += 7) {          /* truncation never overreads */
			u8 *cut = malloc(n ? n : 1);

			memcpy(cut, img, n);
			memset(&log, 0, sizeof(log));
			(void)binfmt_macho_imports(cut, n, log_import, &log);
			free(cut);
		}
		free(img);
	}

	/* Imports through LC_DYLD_CHAINED_FIXUPS. */
	img = load_darwin("bin/darwin-argv", &len);
	CHECK(img != NULL);
	if (img) {
		memset(&log, 0, sizeof(log));
		CHECK_EQ(binfmt_macho_imports(img, len, log_import, &log), 0);
		CHECK(strstr(log.text, "[/usr/lib/libSystem.B.dylib|_getenv]") != NULL);
		CHECK(strstr(log.text, "[/usr/lib/libSystem.B.dylib|_malloc]") != NULL);
		for (size_t n = 0; n <= len; n += 7) {
			u8 *cut = malloc(n ? n : 1);

			memcpy(cut, img, n);
			(void)binfmt_macho_imports(cut, n, log_import, &log);
			free(cut);
		}
		free(img);
	}

	/* .app: the translator names the missing framework and the symbols bound from it. */
	{
		size_t plen, elen;
		u8 *plist = load_fixture("Applications/Hello.app/Contents/Info.plist", &plen);
		u8 *exe = load_darwin("bin/darwin-needs-foundation", &elen);
		struct pkg_manifest m;
		char text[2048];

		CHECK(exe != NULL);
		if (exe) {
			CHECK_EQ(pkg_translate_app(plist, plen, exe, elen, &m), 0);
			CHECK_EQ(m.ndeps, 2);
			for (u32 i = 0; i < m.ndeps; i++) {
				if (strstr(m.deps[i].capability, "framework:Foundation")) {
					CHECK(!m.deps[i].base);
					CHECK_EQ(m.deps[i].nimports, 2);
					CHECK(strstr(m.deps[i].imports, "_NSLog") != NULL);
					CHECK(strstr(m.deps[i].imports, "_NSApplicationMain") != NULL);
				} else {
					CHECK(m.deps[i].base);          /* libSystem is provided */
					CHECK_EQ(m.deps[i].nimports, 0);
				}
			}
			CHECK(pkg_manifest_print(&m, text, sizeof(text)) > 0);
			CHECK(strstr(text, "missing      /System/Library/Frameworks/Foundation") != NULL);
			CHECK(strstr(text, "2 imports: ") != NULL);
			CHECK(strstr(text, "_NSLog") != NULL);
			free(exe);
		}
		free(plist);
	}
}

void test_darwin(void)
{
	test_calls();
	test_images();
}
