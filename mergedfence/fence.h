/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_FENCE_H
#define MERGEDOS_FENCE_H

/*
 * Mergedfence: a domain blocking engine.
 *
 * Rules come from hosts files ("0.0.0.0 domain", "127.0.0.1 domain") and
 * adblock-style lists ("||domain^", exceptions "@@||domain^"). A rule for
 * example.com also covers every name below it (ads.example.com), but never
 * a different name that merely ends in the same letters (badexample.com).
 * Exceptions always win over blocks. Matching ignores case.
 *
 * Storage is a fixed table inside struct fence: no allocation, so the same
 * code runs in the kernel, in tests and in host tools. The engine only
 * answers "would this name be blocked?"; it is meant to sit in the
 * resolver path once there is one.
 */
#include <mergedos/compat.h>

#define FENCE_MAX_RULES  512
#define FENCE_DOMAIN_MAX 64             /* bytes, including the NUL */

enum fence_verdict {
	FENCE_ALLOWED = 0,
	FENCE_BLOCKED,
	FENCE_EXCEPTED,                 /* a block matched, an exception overrode it */
};

struct fence_rule {
	char domain[FENCE_DOMAIN_MAX];  /* lower case, no trailing dot */
	bool exception;
};

struct fence_stats {
	u32 rules;                      /* block + exception rules loaded       */
	u32 exceptions;                 /* of which exceptions                  */
	u32 ignored;                    /* lines not understood (cosmetic, ...) */
	u32 dropped;                    /* valid rules lost to a full table     */
	u32 checks;                     /* fence_check() calls                  */
	u32 blocked;                    /* of which answered FENCE_BLOCKED      */
	u32 excepted;                   /* of which answered FENCE_EXCEPTED     */
};

struct fence {
	struct fence_rule rules[FENCE_MAX_RULES];
	u32 nrules;
	bool enabled;
	struct fence_stats st;
};

void fence_init(struct fence *f);
/*
 * Add the rules in a text buffer (any mix of the supported syntaxes).
 * Returns the number of new rules, or -MERR_RANGE when the table filled up
 * (rules that fit are kept; stats.dropped counts the rest).
 */
int fence_load(struct fence *f, const char *buf, size_t len);
enum fence_verdict fence_check(struct fence *f, const char *domain);
void fence_enable(struct fence *f, bool on);
const struct fence_stats *fence_stats(const struct fence *f);
const char *fence_verdict_name(enum fence_verdict v);

#endif /* MERGEDOS_FENCE_H */
