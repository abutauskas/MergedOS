/* SPDX-License-Identifier: BSD-2-Clause */
/* A trivial shared library, loaded at runtime by dyntest through dlopen(). */
int dyntest_add(int a, int b)
{
	return a + b;
}
