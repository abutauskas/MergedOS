/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * demo.musl - an ordinary C program, statically linked against the
 * unmodified musl C library with the host's musl-gcc. Nothing in it knows
 * about MergedOS: it is a Linux program. It exercises what a libc needs
 * from a kernel to start, print, read a file and allocate memory, and
 * returns the number of the first step that failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct utsname u;
	struct timespec ts;
	char line[128], *big, *small[200];
	FILE *f;
	long rest = 0;

	if (argc != 1 || !argv[0] || argv[1])
		return 1;
	if (uname(&u) || strcmp(u.sysname, "Linux"))
		return 2;
	printf("musl demo: kernel says %s %s %s; argv[0]=%s\n", u.sysname, u.release, u.machine,
	       argv[0]);

	f = fopen("/etc/motd", "r");                    /* open, fstat, read */
	if (!f)
		return 3;
	if (!fgets(line, sizeof(line), f))
		return 4;
	while (fgetc(f) != EOF)
		rest++;
	if (fseek(f, 0, SEEK_SET) || fgetc(f) != line[0] || fclose(f))
		return 5;
	printf("musl demo: /etc/motd: \"%.*s\" + %ld more bytes\n", (int)strcspn(line, "\n"), line,
	       rest);

	big = malloc(1 << 20);                          /* large: anonymous mmap */
	if (!big)
		return 6;
	memset(big, 0x5A, 1 << 20);
	for (int i = 0; i < 200; i++) {                 /* small: brk / pooled */
		small[i] = malloc(64 + i);
		if (!small[i])
			return 7;
		memset(small[i], i, 64 + i);
	}
	for (int i = 0; i < 200; i++)
		if ((unsigned char)small[i][63 + i] != (unsigned char)i || big[(i * 5003) % (1 << 20)] != 0x5A)
			return 8;
	for (int i = 0; i < 200; i++)
		free(small[i]);
	free(big);

	if (clock_gettime(CLOCK_MONOTONIC, &ts) || getpid() <= 0)
		return 9;
	if (fopen("/etc/motd", "w"))                    /* the ramdisk is read-only */
		return 10;
	if (fopen("/no/such/file", "r"))
		return 11;
	printf("musl demo: malloc ok, pid %d, pi is about %.4f\n", getpid(), 3.14159265);
	return 0;
}
