/* SPDX-License-Identifier: BSD-2-Clause */
/* Link-time stand-in for Foundation: never installed, so a program that needs it
 * demonstrates the loader's "missing library / symbols" report. */
void NSLog(const char *fmt, ...) { (void)fmt; }
int NSApplicationMain(int argc, const char **argv) { (void)argc; (void)argv; return 0; }
