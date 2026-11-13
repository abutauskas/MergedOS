/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Polled delays using PIT channel 2 (the "speaker" channel), whose output
 * can be read back through port 0x61 without any interrupt plumbing.
 */
#include <mergedos/kernel.h>
#include <mergedos/io.h>

#define PIT_HZ 1193182u

void time_delay_ms(u32 ms)
{
	while (ms) {
		u32 slice = ms > 50 ? 50 : ms;          /* 50 ms fits the 16-bit counter */
		u32 count = PIT_HZ / 1000 * slice;

		outb(0x61, inb(0x61) & 0xFC);           /* gate low, speaker off */
		outb(0x43, 0xB0);                       /* channel 2, lo/hi, mode 0 */
		outb(0x42, (u8)count);
		outb(0x42, (u8)(count >> 8));
		outb(0x61, (inb(0x61) & 0xFC) | 0x01);  /* gate high: start counting */

		/* Bounded wait so a machine without a legacy PIT cannot hang the boot. */
		for (u32 spin = 0; spin < 50000000u && !(inb(0x61) & 0x20); spin++)
			cpu_relax();
		ms -= slice;
	}
}
