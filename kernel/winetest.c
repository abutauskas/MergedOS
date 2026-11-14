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
#include <mergedos/task.h>

int wine_boot_check(void)
{
	static const char *const env[] = {
		"PATH=/usr/lib/wine:/bin", "HOME=/home/user", "TERM=dumb",
		"WINEPREFIX=/home/user/.wine", "WINESERVER=/usr/lib/wine/wineserver64",
		"LD_LIBRARY_PATH=/lib/x86_64-linux-gnu:/lib:/lib64",
		"WINEDEBUG=err+all,fixme-all", "WINEDLLOVERRIDES=winemenubuilder.exe=d",
		NULL,
	};
	static const struct {
		const char *argv[8];
		const char *expect;
	} runs[] = {
		{ { "wine64", "--version", NULL }, "wine-9" },
		{ { "wine64", "cmd.exe", "/c", "echo", "hi", NULL }, "hi" },
	};
	const u8 *img;
	size_t len;
	int failures = 0;

	if (initrd_find("usr/lib/wine/wine64", &img, &len)) {
		kprintf("[wine] wine64 loader     skipped (no Wine module; make wine-linux)\n");
		return 0;
	}
	for (unsigned i = 0; i < ARRAY_SIZE(runs); i++) {
		struct exec_report rep = { 0 };
		int rc = task_exec_argv("/usr/lib/wine/wine64", runs[i].argv, env, &rep);
		bool ok = !rc && rep.exit_code == 0 && !rep.faulted &&
			  mos_memstr(rep.out, rep.out_len, runs[i].expect);

		kprintf("[wine] %-10s %-12s %llu syscalls (%llu unknown), %u pages, exit %d  %s\n",
			runs[i].argv[0], runs[i].argv[1], (unsigned long long)rep.syscalls,
			(unsigned long long)rep.unknown_syscalls, rep.pages, rc ? rc : rep.exit_code,
			ok ? "OK" : "FAILED");
		if (!ok)
			kprintf("[wine]   output: %s\n", rep.out);
		failures += !ok;
		if (!ok)
			break;
	}
	return failures;
}
