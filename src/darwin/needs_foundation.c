/* SPDX-License-Identifier: BSD-2-Clause */
#include "mini.h"

void NSLog(const char *, ...);
int NSApplicationMain(int, const char **);

int main(int argc, const char **argv)
{
	printf("not reached\n");
	NSLog("hi");
	return NSApplicationMain(argc, argv);
}
