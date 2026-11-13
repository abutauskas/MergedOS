/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * CPU bring-up: runtime GDT + TSS, exception IDT, and the SYSCALL MSRs that
 * route every foreign system call into syscall_entry.
 */
#include <mergedos/kernel.h>
#include <mergedos/cpu.h>
#include <mergedos/io.h>

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084
#define EFER_SCE   (1ULL << 0)
#define EFER_NXE   (1ULL << 11)

struct desc_ptr {
	u16 limit;
	u64 base;
} MOS_PACKED;

struct tss64 {
	u32 reserved0;
	u64 rsp[3];
	u64 reserved1;
	u64 ist[7];
	u64 reserved2;
	u16 reserved3;
	u16 iomap_base;
} MOS_PACKED;

struct idt_gate {
	u16 offset_lo;
	u16 selector;
	u8  ist;
	u8  type_attr;
	u16 offset_mid;
	u32 offset_hi;
	u32 reserved;
} MOS_PACKED;

extern void gdt_load(const struct desc_ptr *gdtr);
extern void syscall_entry(void);
extern const u64 isr_stub_table[48];
extern u8 ring0_stack_top[];

/*
 * Segment order is dictated by SYSCALL/SYSRET: STAR[47:32] selects kernel
 * CS (SS = +8) and STAR[63:48] selects the base for user SS (+8) / CS (+16).
 */
static u64 gdt[7] = {
	0,
	0x00AF9A000000FFFFULL,  /* 0x08 kernel code (64-bit)   */
	0x00CF92000000FFFFULL,  /* 0x10 kernel data            */
	0x00CFF2000000FFFFULL,  /* 0x18 user data              */
	0x00AFFA000000FFFFULL,  /* 0x20 user code (64-bit)     */
	0, 0                    /* 0x28 TSS (16-byte system descriptor) */
};

static struct tss64 tss;
static struct idt_gate idt[256];
static bool nx_enabled;

bool cpu_nx_enabled(void)
{
	return nx_enabled;
}

static void idt_set_gate(unsigned vec, u64 handler)
{
	idt[vec].offset_lo  = (u16)handler;
	idt[vec].selector   = KERNEL_CS;
	idt[vec].ist        = 0;
	idt[vec].type_attr  = 0x8E;             /* present, DPL 0, interrupt gate */
	idt[vec].offset_mid = (u16)(handler >> 16);
	idt[vec].offset_hi  = (u32)(handler >> 32);
	idt[vec].reserved   = 0;
}

void cpu_init(void)
{
	struct desc_ptr gdtr, idtr;
	u64 tss_base = (u64)&tss;
	u64 efer;
	u32 a, b, c, d;

	/* Ring 3 -> ring 0 transitions (exceptions) land on the ring-0 stack. */
	tss.rsp[0] = (u64)ring0_stack_top;
	tss.iomap_base = sizeof(tss);

	gdt[5] = (sizeof(tss) - 1)
	       | ((tss_base & 0xFFFFFFULL) << 16)
	       | (0x89ULL << 40)                        /* present, 64-bit TSS (available) */
	       | (((tss_base >> 24) & 0xFFULL) << 56);
	gdt[6] = tss_base >> 32;

	gdtr.limit = sizeof(gdt) - 1;
	gdtr.base = (u64)gdt;
	gdt_load(&gdtr);
	__asm__ volatile("ltr %w0" : : "r"((u16)TSS_SEL));

	for (unsigned v = 0; v < 48; v++)
		idt_set_gate(v, isr_stub_table[v]);
	idtr.limit = sizeof(idt) - 1;
	idtr.base = (u64)idt;
	__asm__ volatile("lidt %0" : : "m"(idtr));

	/*
	 * Legacy PIC: move IRQ 0..15 to vectors 32..47 (their power-on vectors
	 * collide with CPU exceptions) and unmask only the timer. Keyboard,
	 * mouse and disks are still polled.
	 */
	outb(0x20, 0x11);
	outb(0xA0, 0x11);
	outb(0x21, 0x20);
	outb(0xA1, 0x28);
	outb(0x21, 0x04);
	outb(0xA1, 0x02);
	outb(0x21, 0x01);
	outb(0xA1, 0x01);
	outb(0x21, 0xFE);
	outb(0xA1, 0xFF);

	/* PIT channel 0: periodic, 100 Hz. (Channel 2 stays free for polled delays.) */
	outb(0x43, 0x34);
	outb(0x40, (u8)(1193182 / 100));
	outb(0x40, (u8)((1193182 / 100) >> 8));

	/* SYSCALL/SYSRET, plus NX when the CPU has it. */
	efer = rdmsr(MSR_EFER) | EFER_SCE;
	cpuid(0x80000001, 0, &a, &b, &c, &d);
	if (d & (1u << 20)) {
		efer |= EFER_NXE;
		nx_enabled = true;
	}
	wrmsr(MSR_EFER, efer);
	wrmsr(MSR_STAR, ((u64)(USER_DS - 8 - 3) << 48) | ((u64)KERNEL_CS << 32));
	wrmsr(MSR_LSTAR, (u64)syscall_entry);
	wrmsr(MSR_SFMASK, 0x00040700);                  /* clear AC, DF, IF, TF on entry */
}
