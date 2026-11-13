/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_DE_SELECTOR_H
#define MERGEDOS_DE_SELECTOR_H

/*
 * Boot-time desktop selector.
 *
 * de_recommend() scores four desktop profiles against the probed hardware
 * and names the best fit; the boot menu (de_menu.c) shows that table, marks
 * the recommendation and lets the user override it.
 *
 * The profile names describe layout paradigms (a taskbar, a dock, tiles, a
 * 16-colour retro screen). MergedOS ships no third-party artwork or code.
 */
#include "hardware_probe/hwprobe.h"

enum de_id {
	DE_TASKBAR = 0,         /* taskbar + start menu, stacking windows */
	DE_DOCK,                /* global menu bar + dock, composited     */
	DE_TILING,              /* keyboard-driven tiling, no compositor  */
	DE_RETRO,               /* 640x480, 16 colours                    */
	DE_COUNT
};

struct de_profile {
	const char *key;                /* command-line / config name      */
	const char *title;
	const char *style;
	const char *compositor;
	u32 min_ram_mib, comfort_ram_mib;
	enum hw_gpu_tier min_gpu, ideal_gpu;
	u32 min_cpus, ideal_cpus;
	u32 resident_mib;               /* design budget for the idle session */
	u32 weight;                     /* richness: score ceiling, 0..100 */
};

struct de_score {
	bool eligible;                  /* hardware meets the minimum spec */
	u32 fit;                        /* 0..100: how comfortably         */
	u32 score;                      /* fit scaled by profile weight    */
	char reason[72];
};

struct de_recommendation {
	struct de_score score[DE_COUNT];
	enum de_id best;
};

/* ---- portable (de_policy.c) ---------------------------------------------- */
const struct de_profile *de_profile(enum de_id id);
void de_recommend(const struct hw_profile *hw, struct de_recommendation *out);
int  de_lookup(const char *key);        /* de_id, or -1 */

/* ---- kernel only ------------------------------------------------------------ */
void de_print_table(const struct hw_profile *hw, const struct de_recommendation *rec);
enum de_id de_boot_select(const struct hw_profile *hw, bool interactive);
void de_session_start(enum de_id id, const struct hw_profile *hw);
enum de_id de_session_current(void);
/* kernel/deskrun.c: graphical shell. False without a framebuffer. */
bool desktop_start(enum de_id layout);
int  desktop_selftest(void);

#endif /* MERGEDOS_DE_SELECTOR_H */
