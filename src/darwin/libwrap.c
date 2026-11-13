/* SPDX-License-Identifier: BSD-2-Clause */
/* Re-exports libgreet.dylib (see the Makefile) and adds one symbol of its own. */
int wrap_twice(int x)
{
	return 2 * x;
}
