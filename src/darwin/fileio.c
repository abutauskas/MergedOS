/* SPDX-License-Identifier: BSD-2-Clause */
#include "mini.h"

int main(void)
{
	char line[64];
	unsigned char st[144];
	FILE *f = fopen("/tmp/darwin.txt", "w");
	long size;

	if (!f)
		return 2;
	fprintf(f, "line one\nline %d\n", 2);
	fclose(f);
	f = fopen("/tmp/darwin.txt", "r");
	if (!f)
		return 3;
	fgets(line, sizeof(line), f);
	printf("darwin fileio read: %s", line);
	fgets(line, sizeof(line), f);
	printf("darwin fileio read: %s", line);
	fclose(f);
	if (stat("/tmp/darwin.txt", st))
		return 4;
	size = *(long *)(st + 96);                      /* st_size */
	printf("darwin fileio size=%ld unlink=%d missing=%d\n", size, unlink("/tmp/darwin.txt"),
	       fopen("/tmp/darwin.txt", "r") == NULL);
	return 0;
}
