/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_MONITOR_H
#define MERGEDOS_MONITOR_H

#include <mergedos/types.h>
#include "hardware_probe/hwprobe.h"

/* kmain.c */
const struct hw_profile *system_hw(void);
void system_mem_report(void);
const char *system_user(void);          /* name from the setup, or "guest" */
struct fence;
/* Mergedfence engine for the desktop: NULL until the shell has loaded the list. */
const struct fence *monitor_fence(void);

/* monitor.c: the in-kernel msh front end */
int  monitor_exec(const char *line);    /* 0 on success */
void monitor_run(void) MOS_NORETURN;
int  monitor_selftest(void);            /* number of failed checks */

#endif /* MERGEDOS_MONITOR_H */
