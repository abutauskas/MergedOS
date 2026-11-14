/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Boot self-test for Wine's own Linux stack (docs/WINE_STACK.md): the
 * unmodified wine64 loader, ntdll.so and wineserver64 from the Wine module,
 * running natively on the Linux personality. Skipped without the module.
 */
#include <mergedos/kernel.h>
#include <mergedos/compat.h>
#include <mergedos/initrd.h>
#include <mergedos/mm.h>
#include <mergedos/boot.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>

extern const char *lx_trace_name;
extern int lx_trace_pid;

int wine_boot_check(void)
{
	static const char *const env[] = {
		"PATH=/usr/lib/wine:/bin", "HOME=/home/user", "TERM=dumb",
		"WINEPREFIX=/home/user/.wine", "WINESERVER=/usr/lib/wine/wineserver64",
		"LD_LIBRARY_PATH=/lib/x86_64-linux-gnu:/lib:/lib64",
		"WINEDEBUG=err+all", "WINEDLLOVERRIDES=winemenubuilder.exe=d",
		NULL,
	};
	static const struct {
		const char *argv[8];
		const char *expect;
	} runs[] = {
		{ { "wine64", "--version", NULL }, "wine-9" },
		{ { "wine64", "wineboot", "--init", NULL }, "" },
		{ { "wine64", "cmd.exe", "/c", "echo", "hi", NULL }, "\nhi" },
	};
	const u8 *img;
	size_t len;
	int failures = 0;

	if (boot_cmdline_get("lxtrace", NULL, 0)) {     /* development: log opens and every syscall of wine64 */
		vfs_debug = true;
		lx_trace_name = "wine64";
	}
	if (boot_cmdline_get("lxtrace2", NULL, 0))
		lx_trace_name = "wineserver";
	task_set_tick_limit(0);                 /* daemons must not be stopped by the CPU budget */
	if (initrd_find("usr/lib/wine/wine64", &img, &len)) {
		kprintf("[wine] wine64 loader     skipped (no Wine module; make wine-linux)\n");
		return 0;
	}
	if (boot_cmdline_get("lxtrace", NULL, 0)) {
		static const char *const eargv[] = { "busybox", "env", NULL };
		struct exec_report er = { 0 };

		task_exec_argv("/bin/busybox", eargv, env, &er);
	}
	for (unsigned i = 0; i < ARRAY_SIZE(runs); i++) {
		struct exec_report rep = { 0 };
		static char tee[4096];
		int rc;
		bool ok;

		vfs_console_tee(tee, sizeof(tee));
		rc = task_exec_argv("/usr/lib/wine/wine64", runs[i].argv, env, &rep);
		vfs_console_tee(NULL, 0);
		ok = !rc && rep.exit_code == 0 && !rep.faulted &&
		     (mos_memstr(rep.out, rep.out_len, runs[i].expect) ||
		      mos_memstr(tee, strlen(tee), runs[i].expect));

		if (i == 1)
			ok = true;      /* prefix creation: only its effect matters */
		kprintf("[wine] %-10s %-12s %llu syscalls (%llu unknown), %u pages, exit %d  %s\n",
			runs[i].argv[0], runs[i].argv[1], (unsigned long long)rep.syscalls,
			(unsigned long long)rep.unknown_syscalls, rep.pages, rc ? rc : rep.exit_code,
			ok ? "OK" : "FAILED");
		if (!ok)
			kprintf("[wine]   output: %s\n", rep.out);
		failures += !ok;
		if (!ok)
			break;
		if (i == 1) {   /* the prefix exists now: zlib1.dll (a Debian dependency of user32) is not in wine.inf's list */
			static const char *const cp[] = { "busybox", "cp",
				"/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/zlib1.dll",
				"/home/user/.wine/drive_c/windows/system32/zlib1.dll", NULL };
			struct exec_report er = { 0 };

			task_exec_argv("/bin/busybox", cp, env, &er);
		}
	}
	if (failures && boot_cmdline_get("lxtrace", NULL, 0)) {      /* show what the prefix looks like */
		static const char *const ls[] = { "busybox", "ls", "-la", "/home/user/.wine/drive_c/windows/system32", NULL };
		struct exec_report er = { 0 };

		task_exec_argv("/bin/busybox", ls, env, &er);
	}
	return failures;
}

/* Kernel interfaces Wine relies on, exercised by a static musl program (src/guest/lxtest.c). */
int lxtest_boot_check(void)
{
	static const char *const env[] = { "PATH=/bin", "HOME=/home/user", NULL };
	static const char *const argv[] = { "lxtest", NULL };
	struct exec_report rep = { 0 };
	const u8 *img;
	size_t len;
	int rc;
	bool ok;

	if (initrd_find("bin/lxtest", &img, &len) || !len) {
		kprintf("[lx] lxtest              skipped (built without musl-gcc, or no sysroot)\n");
		return 0;
	}
	rc = task_exec_argv("/bin/lxtest", argv, env, &rep);
	ok = !rc && rep.exit_code == 0 && !rep.faulted && mos_memstr(rep.out, rep.out_len, "lxtest: OK");
	kprintf("[lx] lxtest              %llu syscalls, exit %d  %s\n", (unsigned long long)rep.syscalls,
		rc ? rc : rep.exit_code, ok ? "OK" : "FAILED");
	if (!ok)
		kprintf("[lx]   output: %s\n", rep.out);
	return !ok;
}
