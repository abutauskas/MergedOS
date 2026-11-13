/* SPDX-License-Identifier: BSD-2-Clause */
/* Two-level namespace through a re-exporting dylib, weak imports, data binds. */
#include "mini.h"

int greet(const char *);
extern int greet_calls;
int wrap_twice(int);
extern int optional_symbol(void) __attribute__((weak_import));
extern int optional_data __attribute__((weak_import));

static int (*const fn_table[])(const char *) = { greet };

int main(void)
{
	int n = fn_table[0]("dyn");

	printf("darwin dyn: n=%d calls=%d twice=%d weak=%s/%s\n", n, greet_calls, wrap_twice(21),
	       &optional_symbol ? "present" : "absent", &optional_data ? "present" : "absent");
	return 0;
}
