/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Desktop recommendation policy. Pure integer arithmetic, no I/O.
 *
 * For every profile whose minimum spec is met:
 *
 *   ram_fit  = how far RAM is between the minimum and the comfortable amount
 *   gpu_fit  = 100% at the ideal GPU tier, 40% at the bare minimum tier
 *   cpu_fit  = 100% with the ideal core count, 50% below it
 *   fit      = 50% ram_fit + 30% gpu_fit + 20% cpu_fit
 *   score    = fit * weight
 *
 * `weight` is the richness of the desktop. A rich desktop therefore wins
 * only when the machine runs it comfortably; on a marginal machine a
 * lighter desktop at 100% fit beats it. Ties go to the lighter profile.
 */
#include "de_selector/de_selector.h"

static const struct de_profile profiles[DE_COUNT] = {
	[DE_TASKBAR] = {
		.key = "taskbar", .title = "Taskbar shell", .style = "Windows-style",
		.compositor = "stacking, software or GPU",
		.min_ram_mib = 192, .comfort_ram_mib = 768,
		.min_gpu = HW_GPU_BASIC, .ideal_gpu = HW_GPU_PARAVIRT,
		.min_cpus = 1, .ideal_cpus = 2, .resident_mib = 48, .weight = 80,
	},
	[DE_DOCK] = {
		.key = "dock", .title = "Dock layout", .style = "Mac-like layout",
		.compositor = "GPU-accelerated compositing",
		.min_ram_mib = 1024, .comfort_ram_mib = 4096,
		.min_gpu = HW_GPU_PARAVIRT, .ideal_gpu = HW_GPU_DISCRETE,
		.min_cpus = 2, .ideal_cpus = 4, .resident_mib = 128, .weight = 100,
	},
	[DE_TILING] = {
		.key = "tiling", .title = "Tiling WM", .style = "Linux-style",
		.compositor = "none, direct framebuffer",
		.min_ram_mib = 32, .comfort_ram_mib = 64,
		.min_gpu = HW_GPU_BASIC, .ideal_gpu = HW_GPU_BASIC,
		.min_cpus = 1, .ideal_cpus = 1, .resident_mib = 8, .weight = 60,
	},
	[DE_RETRO] = {
		.key = "retro", .title = "Retro 640x480x16", .style = "TempleOS-style",
		.compositor = "none, planar VGA",
		.min_ram_mib = 16, .comfort_ram_mib = 32,
		.min_gpu = HW_GPU_NONE, .ideal_gpu = HW_GPU_NONE,
		.min_cpus = 1, .ideal_cpus = 1, .resident_mib = 2, .weight = 40,
	},
};

const struct de_profile *de_profile(enum de_id id)
{
	return (unsigned)id < DE_COUNT ? &profiles[id] : NULL;
}

int de_lookup(const char *key)
{
	for (int i = 0; i < DE_COUNT; i++)
		if (!mos_strcasecmp(key, profiles[i].key))
			return i;
	return -1;
}

static void score_one(const struct de_profile *p, const struct hw_profile *hw, struct de_score *s)
{
	u32 ram_fit, gpu_fit, cpu_fit, fit;

	memset(s, 0, sizeof(*s));
	if (hw->ram_mib < p->min_ram_mib) {
		mos_snprintf(s->reason, sizeof(s->reason), "needs >= %u MiB RAM (have %u)",
			     p->min_ram_mib, hw->ram_mib);
		return;
	}
	if (hw->gpu_tier < p->min_gpu) {
		mos_snprintf(s->reason, sizeof(s->reason), "needs a %s GPU or better",
			     hw_gpu_tier_name(p->min_gpu));
		return;
	}
	if (hw->cpus < p->min_cpus) {
		mos_snprintf(s->reason, sizeof(s->reason), "needs >= %u CPUs (have %u)",
			     p->min_cpus, hw->cpus);
		return;
	}

	if (hw->ram_mib >= p->comfort_ram_mib)
		ram_fit = 1000;
	else
		ram_fit = (hw->ram_mib - p->min_ram_mib) * 1000 /
			  (p->comfort_ram_mib - p->min_ram_mib);

	if (hw->gpu_tier >= p->ideal_gpu)
		gpu_fit = 1000;
	else
		gpu_fit = 400 + 600 * (u32)(hw->gpu_tier - p->min_gpu) /
				(u32)(p->ideal_gpu - p->min_gpu);

	cpu_fit = hw->cpus >= p->ideal_cpus ? 1000 : 500;

	fit = (ram_fit * 5 + gpu_fit * 3 + cpu_fit * 2) / 10;
	s->eligible = true;
	s->fit = fit / 10;
	s->score = p->weight * fit / 1000;
	mos_snprintf(s->reason, sizeof(s->reason), "fit %u%% (ram %u%%, gpu %u%%, cpu %u%%)",
		     s->fit, ram_fit / 10, gpu_fit / 10, cpu_fit / 10);
}

void de_recommend(const struct hw_profile *hw, struct de_recommendation *out)
{
	int best = -1;

	for (int i = 0; i < DE_COUNT; i++) {
		score_one(&profiles[i], hw, &out->score[i]);
		if (!out->score[i].eligible)
			continue;
		if (best < 0 || out->score[i].score > out->score[best].score ||
		    (out->score[i].score == out->score[best].score &&
		     profiles[i].resident_mib < profiles[best].resident_mib))
			best = i;
	}
	/* Below every minimum spec: the retro profile is the fallback of last resort. */
	out->best = best < 0 ? DE_RETRO : (enum de_id)best;
}
