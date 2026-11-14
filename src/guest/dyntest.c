/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dyntest - an ordinary dynamically linked C program. Built by the host gcc
 * (glibc, PIE) and by musl-gcc (dynamic), it runs under the program's own,
 * unmodified dynamic linker on MergedOS. It prints a banner, its argv and a
 * couple of environment variables, resolves its own path through
 * /proc/self/exe, and dlopen()s a helper library to prove runtime loading
 * works. It returns the number of the first step that failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#ifndef LIBC_NAME
#define LIBC_NAME "libc"
#endif
#ifndef DLLIB
#define DLLIB "libdyntest.so"
#endif

int main(int argc, char **argv)
{
	char exe[256];
	void *h;
	int (*add)(int, int);
	const char *path = getenv("PATH"), *home = getenv("HOME");
	ssize_t n;

	printf("dyntest: hello from dynamic %s, argc=%d %s\n",
	       LIBC_NAME, argc, argv[0]);
	for (int i = 1; i < argc; i++)
		printf("dyntest: arg%d=%s\n", i, argv[i]);
	printf("dyntest: PATH=%s HOME=%s\n", path ? path : "(unset)", home ? home : "(unset)");

	n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n <= 0)
		return 2;
	exe[n] = '\0';
	printf("dyntest: exe=%s\n", exe);

	h = dlopen(DLLIB, RTLD_NOW);
	if (!h) {
		printf("dyntest: dlopen(%s) failed: %s\n", DLLIB, dlerror());
		return 3;
	}
	add = (int (*)(int, int))dlsym(h, "dyntest_add");
	if (!add)
	{ printf("dyntest: dlsym failed: %s\n", dlerror()); return 4; }
	printf("dyntest: dlopen add=%d\n", add(40, 2));
	dlclose(h);

	printf("dyntest: OK\n");
	return 0;
}
