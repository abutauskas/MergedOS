/* SPDX-License-Identifier: BSD-2-Clause */
/* A dylib with its own data, an initialiser and imports from libSystem. */
#include "mini.h"

int greet_calls = 7;
static const char *const names[] = { "dylib" };
static int inited;

__attribute__((constructor)) static void greet_init(void)
{
	inited = 1;
}

int greet(const char *who)
{
	greet_calls++;
	return printf("darwin %s: hello %s (init=%d, calls=%d)\n", names[0], who, inited, greet_calls);
}
