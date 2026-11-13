/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_TYPES_H
#define MERGEDOS_TYPES_H

/* Freestanding headers only: these are provided by the compiler itself. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define ALIGN_DOWN(x, a)  ((u64)(x) & ~((u64)(a) - 1))
#define ALIGN_UP(x, a)    ALIGN_DOWN((u64)(x) + ((u64)(a) - 1), a)
#define MOS_MIN(a, b)     ((a) < (b) ? (a) : (b))
#define MOS_MAX(a, b)     ((a) > (b) ? (a) : (b))

#define MOS_PACKED        __attribute__((packed))
#define MOS_NORETURN      __attribute__((noreturn))
#define MOS_PRINTF(f, a)  __attribute__((format(printf, f, a)))

#define PAGE_SIZE         4096ULL
#define PAGE_SHIFT        12

#endif /* MERGEDOS_TYPES_H */
