/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Boot-time desktop menu: show the scored profiles, mark the recommended
 * one, and take a choice from the keyboard or the serial console.
 *
 * Kernel command line:
 *   de=<taskbar|dock|tiling|retro>   skip the menu, use this desktop
 *   de_timeout=<seconds>             menu timeout (default 5, 0 = no wait)
 */
#include <mergedos/kernel.h>
#include <mergedos/boot.h>
#include <mergedos/console.h>
#include "de_selector/de_selector.h"

#define DE_DEFAULT_TIMEOUT_S 5
#define POLL_MS 50

void de_print_table(const struct hw_profile *hw, const struct de_recommendation *rec)
{
	kprintf("\n== MergedOS desktop selector ==\n");
	kprintf("   hardware: %u CPU%s, %u MiB RAM, GPU: %s\n", hw->cpus, hw->cpus == 1 ? "" : "s",
		hw->ram_mib, hw->ngpu ? hw->gpu[0].name : "none");
	for (int i = 0; i < DE_COUNT; i++) {
		const struct de_profile *p = de_profile((enum de_id)i);
		const struct de_score *s = &rec->score[i];

		kprintf(" %c [%d] %-17s %-15s ", (enum de_id)i == rec->best ? '*' : ' ', i + 1,
			p->title, p->style);
		if (s->eligible)
			kprintf("score %3u  %s\n", s->score, s->reason);
		else
			kprintf("   --      %s\n", s->reason);
	}
	kprintf("   * recommended for this machine: %s (%s, ~%u MiB resident)\n",
		de_profile(rec->best)->title, de_profile(rec->best)->compositor,
		de_profile(rec->best)->resident_mib);
}

static u32 parse_u32(const char *s, u32 fallback)
{
	u32 v = 0;

	if (*s < '0' || *s > '9')
		return fallback;
	for (; *s >= '0' && *s <= '9'; s++)
		v = v * 10 + (u32)(*s - '0');
	return v;
}

enum de_id de_boot_select(const struct hw_profile *hw, bool interactive)
{
	struct de_recommendation rec;
	enum de_id choice;
	char val[32];
	u32 timeout_s = DE_DEFAULT_TIMEOUT_S;

	de_recommend(hw, &rec);
	de_print_table(hw, &rec);
	choice = rec.best;

	if (boot_cmdline_get("de", val, sizeof(val))) {
		int forced = de_lookup(val);

		if (forced >= 0) {
			kprintf("   command line override: de=%s\n", val);
			choice = (enum de_id)forced;
			goto done;
		}
		kprintf("   ignoring unknown de=%s\n", val);
	}
	if (boot_cmdline_get("de_timeout", val, sizeof(val)))
		timeout_s = parse_u32(val, DE_DEFAULT_TIMEOUT_S);
	if (!interactive)
		timeout_s = 0;

	if (timeout_s) {
		kprintf("   Press 1-%d to choose, Enter for the recommended desktop (%us) ",
			DE_COUNT, timeout_s);
		for (u32 ms = 0; ms < timeout_s * 1000; ms += POLL_MS) {
			int c = console_poll();

			if (c >= '1' && c < '1' + DE_COUNT) {
				choice = (enum de_id)(c - '1');
				break;
			}
			if (c == '\n')
				break;
			if (ms % 1000 == 0)
				kprintf(".");
			time_delay_ms(POLL_MS);
		}
		kprintf("\n");
	}
done:
	if (!rec.score[choice].eligible)
		kprintf("   warning: %s is below its minimum spec here (%s)\n",
			de_profile(choice)->title, rec.score[choice].reason);
	kprintf("   selected: %s%s\n", de_profile(choice)->title,
		choice == rec.best ? " (recommended)" : "");
	return choice;
}
