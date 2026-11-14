/* SPDX-License-Identifier: BSD-2-Clause */
/* Link-time stand-in for libwrap.dylib: defines everything the real one re-exports. */
int greet_calls;
int greet(const char *who) { (void)who; return 0; }
int wrap_twice(int x) { return x; }
