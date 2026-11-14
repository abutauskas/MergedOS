/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * A Windows x64 console program built with MinGW-w64 against its normal C
 * runtime (msvcrt.dll + kernel32.dll imports). The boot self-test runs it
 * on the NT personality and checks its output and exit status.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef LONG (WINAPI *rtl_get_version_fn)(OSVERSIONINFOW *);

int main(int argc, char **argv)
{
	OSVERSIONINFOW v = { .dwOSVersionInfoSize = sizeof(v) };
	OSVERSIONINFOW r = { .dwOSVersionInfoSize = sizeof(r) };
	rtl_get_version_fn rtl_get_version;
	char *buf, line[96];
	int sum = 0, n;

	/* GetVersionExW is shimmed on real Windows; RtlGetVersion never is. */
	rtl_get_version = (rtl_get_version_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"),
								     "RtlGetVersion");
	if (!GetVersionExW(&v) || !rtl_get_version || rtl_get_version(&r) != 0)
		return 2;

	buf = malloc(4096);
	if (!buf)
		return 3;
	for (int i = 0; i < 4096; i++)
		buf[i] = (char)i;
	for (int i = 0; i < 4096; i++)
		sum += (unsigned char)buf[i];
	free(buf);

	printf("Hello from win_hello.exe (MinGW-w64 CRT)\n");
	printf("windows %lu.%lu.%lu, RtlGetVersion %lu.%lu.%lu\n", v.dwMajorVersion,
	       v.dwMinorVersion, v.dwBuildNumber, r.dwMajorVersion, r.dwMinorVersion,
	       r.dwBuildNumber);
	printf("argc=%d argv[0]=%s cmdline=%s\n", argc, argv[0], GetCommandLineA());
	printf("heap sum=%d hex=%x neg=%d str=[%-5s]\n", sum, 0xC0FFEE, -42, "ok");
	/* printf above is MinGW's own formatter; _snprintf is msvcrt.dll's. */
	n = _snprintf(line, sizeof(line), "%d|%5s|%#x|%I64u|%.3f|%c", -7, "abc", 255,
		      12345678901ULL, 3.14159, 'Z');
	printf("msvcrt _snprintf: %s (%d)\n", line, n);
	/* Wine's kernelbase applies the unmanifested-program version shim (6.2.9200). */
	return ((v.dwBuildNumber == 22631 || v.dwBuildNumber == 9200) && r.dwBuildNumber == 22631 && sum == 522240 &&
		strcmp(line, "-7|  abc|0xff|12345678901|3.142|Z") == 0) ? 0 : 1;
}
