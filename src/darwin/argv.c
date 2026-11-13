/* SPDX-License-Identifier: BSD-2-Clause */
/* argv, environment, heap and formatting; linked with chained fixups. */
#include "mini.h"

static const char *const greeting[] = { "argv", "env" };    /* pointers in data: rebases */
static int counter = 40;

int main(int argc, char **argv)
{
	char buf[64];
	char *copy = malloc(32);
	const char *home = getenv("HOME");

	counter += 2;
	snprintf(buf, sizeof(buf), "%s:%d:%5.2f:%#x", greeting[0], counter, 3.14159, 255);
	printf("darwin %s: argc=%d a1=%s a2=%s\n", greeting[1], argc, argc > 1 ? argv[1] : "-",
	       argc > 2 ? argv[2] : "-");
	printf("darwin %s HOME=%s\n", buf, home ? home : "(unset)");
	strcpy(copy, "heap-ok");
	printf("darwin %s %s\n", copy, strcmp(copy, "heap-ok") ? "BAD" : "OK");
	free(copy);
	return 0;
}
