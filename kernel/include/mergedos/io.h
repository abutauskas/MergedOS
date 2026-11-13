/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_IO_H
#define MERGEDOS_IO_H

#include <mergedos/types.h>

/* x86 port I/O */
static inline void outb(u16 port, u8 v)  { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8  inb(u16 port) { u8 v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* Model-specific and control registers */
static inline u64 rdmsr(u32 msr)
{
	u32 lo, hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((u64)hi << 32) | lo;
}

static inline void wrmsr(u32 msr, u64 v)
{
	__asm__ volatile("wrmsr" : : "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}

static inline u64 read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(u64 v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }

static inline void cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

/* Interrupt masking. On one CPU, "interrupts off" is a lock. */
static inline u64 irq_save(void)
{
	u64 flags;

	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));
	return flags;
}

static inline void irq_restore(u64 flags)
{
	if (flags & 0x200)
		__asm__ volatile("sti");
}

static inline void irq_enable(void) { __asm__ volatile("sti"); }

static inline void cpu_relax(void) { __asm__ volatile("pause"); }
static inline void cpu_halt(void)  { __asm__ volatile("hlt"); }

#endif /* MERGEDOS_IO_H */
