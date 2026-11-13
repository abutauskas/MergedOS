/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_SETUP_H
#define MERGEDOS_SETUP_H

#include "installer/installer.h"
#include "storage/storage.h"

/* ata.c: legacy IDE disks as block devices. */
#define ATA_MAX_DISKS 4
int ata_init(void);                             /* returns the number of disks found */
struct block_dev *ata_disk(int index);

/* ahci.c: SATA disks behind an AHCI controller. */
#define AHCI_MAX_DISKS 4
int ahci_init(void);
struct block_dev *ahci_disk(int index);

#define SETUP_MAX_DISKS (AHCI_MAX_DISKS + ATA_MAX_DISKS)

/* Probe disks, read their partition tables and log what is on them. */
void storage_init(void);
/* True if a disk holds a saved setup; fills `out`. */
bool setup_load_config(struct install_config *out);
/* Non-interactive install onto the first disk (kernel command line: selftest_install). */
int storage_selftest(void);

/*
 * Run the graphical setup wizard until the user finishes it. Needs a
 * framebuffer. Returns true if the setup was saved to a disk.
 */
bool setup_run(struct install_config *out);

/* Ask for the password until it is right. Returns at once if password login is off. */
void login_run(const struct install_config *cfg);

/* Boot splash: logo and progress bar (0..100). No-ops without a framebuffer. */
void splash_show(u32 percent);
void splash_done(void);

/* Scripted wizard self-check for the boot self-test; returns failed checks. */
int setup_selftest(void);

#endif /* MERGEDOS_SETUP_H */
