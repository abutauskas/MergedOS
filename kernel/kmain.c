/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS kernel entry point.
 *
 * Boot order:
 *   console -> boot info -> CPU tables -> physical + virtual memory ->
 *   ramdisk -> hardware probe -> ABI personality self-check ->
 *   desktop selector -> shell
 *
 * Kernel command line:
 *   selftest          run the scripted self-test and power off
 *   de=<name>         force a desktop profile
 *   de_timeout=<s>    desktop menu timeout
 *   gfx=off           stay in VGA text mode
 *   setup=off         skip the graphical setup wizard
 *   splash=<ms>       how long the boot splash stays up (default 500, 0 = no hold)
 *   selftest_install  with selftest: install onto IDE disk 0 without asking (test rigs only)
 */
#include "kernel/net/netdrv.h"
#include <mergedos/kernel.h>
#include <mergedos/compat.h>
#include <mergedos/boot.h>
#include <mergedos/console.h>
#include <mergedos/cpu.h>
#include <mergedos/gfx.h>
#include <mergedos/initrd.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include <mergedos/monitor.h>
#include <mergedos/sched.h>
#include <mergedos/setup.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>
#include "de_selector/de_selector.h"

void kmain(u32 mb_magic, u32 mb_info);
u32 pmm_ref_entries(void);
int wine_boot_check(void);
int lxtest_boot_check(void);              /* kernel/winetest.c */

static struct hw_profile hw;
static struct install_config install = { .desktop = DESKTOP_AUTO };

const char *system_user(void)
{
	return install.name[0] ? install.name : "guest";
}

const struct hw_profile *system_hw(void)
{
	return &hw;
}

void machine_reboot(void)
{
	struct { u16 limit; u64 base; } MOS_PACKED no_idt = { 0, 0 };

	outb(0x64, 0xFE);               /* keyboard controller: pulse the CPU reset line */
	time_delay_ms(50);
	/* Still here: force a triple fault, which resets every PC. */
	__asm__ volatile("lidt %0; int3" : : "m"(no_idt));
	for (;;)
		cpu_halt();
}

/* The desktop the saved setup asks for; "auto" follows the hardware at every boot. */
static enum de_id configured_desktop(void)
{
	struct de_recommendation rec;

	if (install.desktop < DE_COUNT)
		return (enum de_id)install.desktop;
	de_recommend(&hw, &rec);
	return rec.best;
}

void machine_poweroff(int status)
{
	outb(0xF4, (u8)status);         /* QEMU isa-debug-exit: process exit = status*2+1 */
	outw(0x604, 0x2000);            /* QEMU ACPI PM1a: S5 soft-off                    */
	outw(0xB004, 0x2000);           /* older QEMU / Bochs                             */
	__asm__ volatile("cli");
	for (;;)
		cpu_halt();
}

void system_mem_report(void)
{
	struct pmm_stats st;
	u64 image = (u64)(__kernel_end - __kernel_start);

	pmm_get_stats(&st);
	kprintf("[mem] kernel image: text %llu KiB, rodata %llu KiB, data %llu KiB, bss %llu KiB\n",
		(unsigned long long)(__text_end - __text_start) / 1024,
		(unsigned long long)(__rodata_end - __rodata_start) / 1024,
		(unsigned long long)(__data_end - __data_start) / 1024,
		(unsigned long long)(__bss_end - __bss_start) / 1024);
	kprintf("[mem] kernel footprint: %llu KiB resident (image + boot tables %llu KiB, "
		"frame bitmap %llu KiB), peak %llu KiB\n",
		(unsigned long long)(st.usable_frames - st.free_frames - st.module_frames) * 4,
		(unsigned long long)image / 1024, (unsigned long long)st.bitmap_frames * 4,
		(unsigned long long)st.peak_used_frames * 4);
	if (st.module_frames)
		kprintf("[mem] boot module (user files, not counted above): %llu KiB\n",
			(unsigned long long)st.module_frames * 4);
	kprintf("[mem] RAM: %llu KiB usable, %llu KiB free (%llu%% of the machine is free)\n",
		(unsigned long long)st.usable_frames * 4, (unsigned long long)st.free_frames * 4,
		(unsigned long long)(st.free_frames * 100 / st.usable_frames));
}

/*
 * Run one program per binary format and check that each personality behaved
 * the way that program's own operating system would have: every guest
 * verifies its syscall numbers, argument passing and error convention and
 * exits non-zero at the first mismatch.
 */
static int abi_boot_check(void)
{
	static const struct {
		const char *path;
		enum abi_personality abi;
	} guests[] = {
		{ "bin/hello.linux",   ABI_LINUX },
		{ "bin/hello.freebsd", ABI_FREEBSD },
		{ "bin/hello.macho",   ABI_XNU },
		{ "bin/hello.exe",     ABI_NT },
	};
	struct pmm_stats before, after;
	int failures = 0;

	kprintf("\n== ABI personalities: four binary formats, one kernel ==\n");
	pmm_get_stats(&before);
	for (unsigned i = 0; i < ARRAY_SIZE(guests); i++) {
		struct exec_report rep = { 0 };
		const u8 *img;
		size_t len;
		int rc = initrd_find(guests[i].path, &img, &len);
		bool ok;

		if (!rc)
			rc = task_exec(guests[i].path, img, len, &rep);
		ok = !rc && rep.exit_code == 0 && !rep.faulted &&
		     rep.personality == guests[i].abi && rep.out_len > 11 &&
		     !memcmp(rep.out, "Hello from ", 11);
		kprintf("[abi] %-18s %-9s -> %-7s %llu syscalls, %u pages, exit %d  %s\n",
			guests[i].path, binfmt_format_name(rep.format),
			abi_personality_name(rep.personality), (unsigned long long)rep.syscalls,
			rep.pages, rc ? rc : rep.exit_code, ok ? "OK" : "FAILED");
		failures += !ok;
	}
	/* An unmodified C library on the Linux personality: files, heap, mmap, TLS, printf. */
	{
		struct exec_report rep = { 0 };
		const u8 *img;
		size_t len;

		if (initrd_find("bin/demo.musl", &img, &len)) {
			kprintf("[abi] bin/demo.musl      skipped (image built without musl-gcc)\n");
		} else {
			int rc = task_exec("bin/demo.musl", img, len, &rep);
			bool ok = !rc && rep.exit_code == 0 && !rep.faulted &&
				  mos_memstr(rep.out, rep.out_len, "kernel says Linux 6.6.0");

			kprintf("[abi] %-18s %-9s -> %-7s %llu syscalls, %u pages, exit %d  %s\n",
				"bin/demo.musl", binfmt_format_name(rep.format),
				abi_personality_name(rep.personality),
				(unsigned long long)rep.syscalls, rep.pages, rc ? rc : rep.exit_code,
				ok ? "OK" : "FAILED");
			failures += !ok;
		}
	}
	/*
	 * Dynamically linked Linux programs: run natively under their own,
	 * unmodified dynamic linker (glibc ld-linux, musl ld-musl). These live in
	 * the sysroot module (qemu -initrd build/sysroot.tar); without it, skipped.
	 */
	{
		static const char *const env[] = {
			"LD_LIBRARY_PATH=/lib:/lib64:/lib/x86_64-linux-gnu:/usr/lib",
			"PATH=/bin", "HOME=/home/user", "TERM=linux", NULL,
		};
		static const struct {
			const char *path;
			const char *argv[4];
			const char *expect;
			const char *note;
		} dyn[] = {
			{ "/bin/dyntest.gnu",  { "dyntest.gnu", "alpha", "beta", NULL },
			  "dyntest: dlopen add=42", "glibc dyntest (built without gcc)" },
			{ "/bin/dyntest.musl", { "dyntest.musl", "one", "two", NULL },
			  "dyntest: dlopen add=42", "musl dyntest (built without musl-gcc)" },
			{ "/bin/busybox",      { "busybox", "echo", "busybox-says-hi", NULL },
			  "busybox-says-hi", "busybox (no distribution busybox)" },
			{ "/bin/busybox",      { "busybox", "ls", "/bin", NULL },
			  "busybox", "busybox (no distribution busybox)" },
		};
		const u8 *img;
		size_t len;

		if (initrd_find("lib64/ld-linux-x86-64.so.2", &img, &len) &&
		    initrd_find("lib/ld-musl-x86_64.so.1", &img, &len)) {
			kprintf("[dyn] dynamic linking    skipped (no sysroot module; "
				"use qemu -initrd build/sysroot.tar)\n");
		} else {
			for (unsigned i = 0; i < ARRAY_SIZE(dyn); i++) {
				struct exec_report rep = { 0 };
				int rc;
				bool ok;

				if (initrd_find(dyn[i].path + 1, &img, &len)) {
					kprintf("[dyn] %-18s skipped (%s)\n", dyn[i].path, dyn[i].note);
					continue;
				}
				rc = task_exec_argv(dyn[i].path, dyn[i].argv, env, &rep);
				ok = !rc && rep.exit_code == 0 && !rep.faulted &&
				     rep.personality == ABI_LINUX &&
				     mos_memstr(rep.out, rep.out_len, dyn[i].expect);
				kprintf("[dyn] %-18s %-9s -> %-7s %llu syscalls, %u pages, exit %d  %s\n",
					dyn[i].argv[1] ? dyn[i].argv[1] : dyn[i].path,
					binfmt_format_name(rep.format),
					abi_personality_name(rep.personality),
					(unsigned long long)rep.syscalls, rep.pages,
					rc ? rc : rep.exit_code, ok ? "OK" : "FAILED");
				failures += !ok;
			}
		}
	}
	/*
	 * Dynamically linked Mach-O programs on the XNU personality: MergedOS's own
	 * dyld and libSystem.B.dylib from the sysroot module. The last program needs
	 * Foundation, which does not exist here: dyld must name it and the symbols.
	 */
	{
		static const char *const env[] = { "PATH=/bin", "HOME=/home/user", NULL };
		static const struct {
			const char *path;
			const char *argv[4];
			int exit_code;
			const char *expect, *expect2;
		} mach[] = {
			{ "/bin/darwin-hello", { "darwin-hello", NULL }, 0,
			  "dyld + libSystem OK", "" },
			{ "/bin/darwin-argv", { "darwin-argv", "alpha", "beta", NULL }, 0,
			  "argc=3 a1=alpha a2=beta", "argv:42: 3.14:0xff HOME=/home/user" },
			{ "/bin/darwin-fileio", { "darwin-fileio", NULL }, 0,
			  "read: line 2", "size=16 unlink=0 missing=1" },
			{ "/bin/darwin-dyn", { "darwin-dyn", NULL }, 0,
			  "hello dyn (init=1, calls=8)", "twice=42 weak=absent/absent" },
			{ "/bin/darwin-needs-foundation", { "darwin-needs-foundation", NULL }, 1,
			  "Library not loaded: /System/Library/Frameworks/Foundation.framework",
			  "_NSLog" },
		};
		const u8 *img;
		size_t len;

		if (initrd_find("usr/lib/dyld", &img, &len) ||
		    initrd_find("usr/lib/libSystem.B.dylib", &img, &len)) {
			kprintf("[mach] Darwin user space  skipped (no sysroot module or built "
				"without clang/ld64.lld)\n");
		} else {
			for (unsigned i = 0; i < ARRAY_SIZE(mach); i++) {
				struct exec_report rep = { 0 };
				int rc = task_exec_argv(mach[i].path, mach[i].argv, env, &rep);
				bool ok = !rc && rep.exit_code == mach[i].exit_code && !rep.faulted &&
					  rep.personality == ABI_XNU &&
					  mos_memstr(rep.out, rep.out_len, mach[i].expect) &&
					  mos_memstr(rep.out, rep.out_len, mach[i].expect2);

				kprintf("[mach] %-18s %-9s -> %-7s %llu syscalls, %u pages, exit %d  %s\n",
					mach[i].argv[0], binfmt_format_name(rep.format),
					abi_personality_name(rep.personality),
					(unsigned long long)rep.syscalls, rep.pages,
					rc ? rc : rep.exit_code, ok ? "OK" : "FAILED");
				if (!ok)
					kprintf("[mach]   output: %s\n", rep.out);
				failures += !ok;
			}
		}
	}
	/*
	 * Real Windows x64 console programs on the NT personality: imports bound
	 * to the built-in Win32 runtime. win_hello.exe uses the unmodified
	 * MinGW-w64 C runtime; the Wine builtins are only in the ramdisk when the
	 * image was built with WINE_EXE_DIR (see Makefile).
	 */
	pmm_get_stats(&after);
	if (after.free_frames != before.free_frames) {
		kprintf("[abi] FAILED: %lld frames leaked by exec/exit\n",
			(long long)(before.free_frames - after.free_frames));
		failures++;
	}
	{
		static const char *const cmd_args[] = { "/c", "echo", "cmd-says-hi", NULL };
		static const char *const reg_add[] = { "add", "HKCU\\Software\\MergedOS", "/v", "Greeting",
						       "/t", "REG_SZ", "/d", "reg-says-hi", "/f", NULL };
		static const char *const reg_query[] = { "query", "HKCU\\Software\\MergedOS", NULL };
		static const struct {
			const char *path, *expect, *missing_note;
			const char *const *argv;
		} nt_progs[] = {
			{ "bin/win_hello.exe", "RtlGetVersion 10.0.22631", "built without MinGW-w64" },
			{ "bin/wine/hostname.exe", "MERGEDOS", NULL },
			{ "bin/wine/whoami.exe", "MERGEDOS\\user", NULL },
			{ "windows/system32/cmd.exe", "cmd-says-hi", NULL,
			  cmd_args },
			{ "windows/system32/reg.exe", "completed successfully", NULL, reg_add },
			{ "windows/system32/reg.exe", "reg-says-hi", NULL, reg_query },
		};

		for (unsigned i = 0; i < ARRAY_SIZE(nt_progs); i++) {
			struct exec_report rep = { 0 };
			const u8 *img;
			size_t len;
			int rc;
			bool ok;

			if (initrd_find(nt_progs[i].path, &img, &len)) {
				if (nt_progs[i].missing_note)
					kprintf("[abi] %-18s skipped (image %s)\n", nt_progs[i].path,
						nt_progs[i].missing_note);
				continue;
			}
			rc = task_exec_args(nt_progs[i].path, img, len, nt_progs[i].argv, &rep);
			ok = !rc && rep.exit_code == 0 && !rep.faulted && rep.personality == ABI_NT &&
			     mos_memstr(rep.out, rep.out_len, nt_progs[i].expect);
			kprintf("[abi] %-18s %-9s -> %-7s %llu syscalls, %u pages, exit %d  %s\n",
				nt_progs[i].path, binfmt_format_name(rep.format),
				abi_personality_name(rep.personality), (unsigned long long)rep.syscalls,
				rep.pages, rc ? rc : rep.exit_code, ok ? "OK" : "FAILED");
			failures += !ok;
			/*
			 * NT programs leave state in the ramfs (the registry hive under
			 * /tmp/.reg grows, user files): re-baseline after each.
			 */
			pmm_get_stats(&before);
		}
	}
	/* Several processes at once: fork, exec, wait, pipes, threads, signals, ramfs. */
	{
		struct exec_report rep = { 0 };
		const u8 *img;
		size_t len;
		int rc = initrd_find("bin/proctest.linux", &img, &len);
		bool ok;

		if (!rc)
			rc = task_exec("bin/proctest.linux", img, len, &rep);
		ok = !rc && rep.exit_code == 0 && !rep.faulted &&
		     mos_memstr(rep.out, rep.out_len, "proctest: OK");
		kprintf("[proc] %-18s %-9s -> %-7s %llu syscalls, %u pages, exit %d  %s\n",
			"bin/proctest.linux", binfmt_format_name(rep.format),
			abi_personality_name(rep.personality), (unsigned long long)rep.syscalls,
			rep.pages, rc ? rc : rep.exit_code, ok ? "OK" : "FAILED");
		failures += !ok;
	}
	pmm_get_stats(&after);
	if (after.free_frames != before.free_frames) {
		kprintf("[abi] FAILED: %lld frames leaked by exec/exit (%u shared-frame entries)\n",
			(long long)(before.free_frames - after.free_frames), pmm_ref_entries());
		failures++;
	}
	failures += lxtest_boot_check();
	/* Wine's own Linux stack (needs its module and RAM; see docs/WINE_STACK.md). */
	if (after.usable_frames * 4 >= 1000 * 1024)
		failures += wine_boot_check();
	else
		kprintf("[wine] wine64 loader     skipped (needs 1 GiB of RAM)\n");
	return failures;
}

/* Switch to the linear framebuffer when one is available; text mode otherwise. */
static void graphics_init(void)
{
	char val[8] = "";
	const struct gfx_info *g = gfx_get();

	if (boot_cmdline_get("gfx", val, sizeof(val)) && !strcmp(val, "off"))
		return;
	if (!gfx_init_bootfb(boot_info_get()) && !gfx_init_bochs(&hw))
		return;
	console_use_framebuffer();
	mouse_init();
	kprintf("[gfx] %ux%u, %u bpp framebuffer via %s; PS/2 mouse %s\n", g->width, g->height,
		g->bpp, g->source, mouse_get()->present ? "found" : "not found");
}

void kmain(u32 mb_magic, u32 mb_info)
{
	bool selftest;
	char setup_opt[8] = "";
	int failures;
	enum de_id de;

	console_init();
	kprintf("MergedOS %s (x86_64) - hybrid kernel proof of concept\n", MERGEDOS_VERSION);

	boot_info_init(mb_magic, mb_info);
	kprintf("[boot] loader: %s | cmdline: \"%s\"\n",
		boot_info_get()->loader[0] ? boot_info_get()->loader : "multiboot",
		boot_info_get()->cmdline);
	selftest = boot_cmdline_get("selftest", NULL, 0);

	cpu_init();
	pmm_init(boot_info_get());
	vmm_init();
	sched_init();
	irq_enable();                   /* timer ticks and preemption from here on */
	initrd_init();
	vfs_init();

	if (boot_cmdline_get("winetest", NULL, 0)) {    /* development: only Wine, then power off */
		int bad = lxtest_boot_check();

		if (!boot_cmdline_get("nowine", NULL, 0))
			bad += wine_boot_check();

		kprintf("\nMERGEDOS-WINETEST: %s\n", bad ? "FAIL" : "PASS");
		machine_poweroff(bad ? 0x11 : 0x10);
	}
	hwprobe_run(&hw);
	graphics_init();
	splash_show(20);
	hwprobe_print(&hw);
	storage_init();
	net_init();
	splash_show(60);
	if (setup_load_config(&install))
		kprintf("[setup] saved setup found on disk: user \"%s\", language %s, "
			"password login %s\n", install.name, wizard_lang_name(install.lang),
			install.password_enabled ? "on" : "off");
	system_mem_report();

	failures = abi_boot_check();
	splash_show(100);
	if (!selftest && gfx_get()->active) {
		/* Boot takes well under a second: hold the splash long enough to be seen. */
		u32 hold_ms = 500;

		if (boot_cmdline_get("splash", setup_opt, sizeof(setup_opt))) {
			hold_ms = 0;
			for (const char *d = setup_opt; *d >= '0' && *d <= '9'; d++)
				hold_ms = hold_ms * 10 + (u32)(*d - '0');
		}
		time_delay_ms(MOS_MIN(hold_ms, 5000u));
	}

	if (selftest) {
		/* Test rigs: no wizard, no login; the machine powers off when the tests end. */
		splash_done();
		de = de_boot_select(&hw, false);
	} else {
		if (install.name[0]) {
			login_run(&install);            /* a saved setup exists: gate on its password */
		} else if (gfx_get()->active && !(boot_cmdline_get("setup", setup_opt, sizeof(setup_opt)) &&
						  !strcmp(setup_opt, "off"))) {
			/* First boot. Once saved, restart into the installed configuration. */
			if (setup_run(&install)) {
				kprintf("[setup] setup saved: restarting\n");
				machine_reboot();
			}
		}
		splash_done();
		if (install.name[0] && !boot_cmdline_get("de", NULL, 0)) {
			de = configured_desktop();
			kprintf("[de] desktop from saved setup: %s%s\n", de_profile(de)->title,
				install.desktop < DE_COUNT ? "" : " (recommended for this hardware)");
		} else {
			de = de_boot_select(&hw, true);
		}
	}
	if (selftest) {
		/* Keep the log linear: no screen re-layout while testing. */
		kprintf("[de] selftest: session start skipped for %s\n", de_profile(de)->title);
		failures += monitor_selftest();
		failures += sched_selftest();
		failures += net_selftest();
		{
			int bad = desktop_selftest();

			kprintf("[desk] layout self-check (4 layouts: geometry, windows, hit-tests, "
				"tiling): %s\n", bad ? "FAILED" : "ok");
			failures += bad;
		}
		if (boot_cmdline_get("selftest_install", NULL, 0))
			failures += storage_selftest();
		{
			int bad = setup_selftest();

			kprintf("[setup] wizard self-check (widgets, focus, languages, "
				"SHA-256/PBKDF2): %s\n", bad ? "FAILED" : "ok");
			failures += bad;
		}
		if (gfx_get()->active) {
			int bad = gfx_selftest();

			kprintf("[gfx] framebuffer read-back: %s\n", bad ? "FAILED" : "ok");
			failures += bad != 0;
		}
		system_mem_report();
		kprintf("\nMERGEDOS-SELFTEST: %s (%d failure%s)\n", failures ? "FAIL" : "PASS",
			failures, failures == 1 ? "" : "s");
		machine_poweroff(failures ? 0x11 : 0x10);
	}

	de_session_start(de, &hw);
	if (install.name[0])
		kprintf("[setup] user \"%s\", language %s, password login %s\n", install.name,
			wizard_lang_name(install.lang), install.password_enabled ? "on" : "off");
	if (failures)
		kprintf("warning: %d ABI self-check failure(s), see log above\n", failures);
	monitor_run();
}
