/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_KERNEL_H
#define MERGEDOS_KERNEL_H

#include <mergedos/types.h>
#include <mergedos/errno.h>

#ifndef MERGEDOS_VERSION
#define MERGEDOS_VERSION "0.0.0-dev"
#endif

/* printk.c */
int  kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t cap, const char *fmt, ...) MOS_PRINTF(3, 4);
void kprintf(const char *fmt, ...) MOS_PRINTF(1, 2);
void panic(const char *fmt, ...) MOS_NORETURN MOS_PRINTF(1, 2);

/* string.c - the subset of <string.h> the kernel and the compiler rely on */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);

/* time.c */
void time_delay_ms(u32 ms);

/* power.c (part of kmain.c) */
void machine_poweroff(int status) MOS_NORETURN;
void machine_reboot(void) MOS_NORETURN;

#endif /* MERGEDOS_KERNEL_H */
