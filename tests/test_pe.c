/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * PE32+ import-table and base-relocation walkers (abi_dispatch/binfmt.c),
 * as used by the kernel's NT loader. A small image is built in memory so
 * every field can be corrupted on purpose.
 */
#include "tests/test.h"
#include "abi_dispatch/abi.h"
#include "abi_dispatch/binfmt.h"

/* File layout: headers in [0, 0x200), one 0x600-byte section at RVA 0x1000 / file 0x200. */
#define IMG_LEN    0x800
#define SECT_RVA   0x1000
#define SECT_OFF   0x200
#define SECT_SIZE  0x600
#define SECT_END   (SECT_RVA + SECT_SIZE)
#define OPT        0x58                 /* optional header */
#define DIRS       (OPT + 112)
#define AT(rva)    (SECT_OFF + (rva) - SECT_RVA)

static void w16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void w32(u8 *p, u32 v) { w16(p, (u16)v); w16(p + 2, (u16)(v >> 16)); }
static void w64(u8 *p, u64 v) { w32(p, (u32)v); w32(p + 4, (u32)(v >> 32)); }

static void build(u8 *img)
{
	memset(img, 0, IMG_LEN);
	img[0] = 'M';
	img[1] = 'Z';
	w32(img + 0x3C, 0x40);
	memcpy(img + 0x40, "PE\0\0", 4);
	w16(img + 0x44, 0x8664);                /* AMD64 */
	w16(img + 0x46, 1);                     /* one section */
	w16(img + 0x54, 240);                   /* optional header size */
	w16(img + 0x56, 0x22);                  /* executable, large address aware */
	w16(img + OPT, 0x20B);
	w32(img + OPT + 16, SECT_RVA + 0x300);  /* entry */
	w64(img + OPT + 24, 0x140000000ULL);
	w32(img + OPT + 32, 0x1000);
	w32(img + OPT + 36, 0x200);
	w32(img + OPT + 56, 0x2000);            /* SizeOfImage */
	w32(img + OPT + 60, 0x200);             /* SizeOfHeaders */
	w16(img + OPT + 68, 3);                 /* console */
	w16(img + OPT + 70, 0x0160);            /* dynamic base, NX, high-entropy VA */
	w32(img + OPT + 108, 16);
	w32(img + DIRS + 1 * 8, 0x1000);        /* import directory */
	w32(img + DIRS + 1 * 8 + 4, 60);
	w32(img + DIRS + 5 * 8, 0x1400);        /* base relocations */
	w32(img + DIRS + 5 * 8 + 4, 16);

	memcpy(img + 0x148, ".data\0\0\0", 8);
	w32(img + 0x148 + 8, SECT_SIZE);
	w32(img + 0x148 + 12, SECT_RVA);
	w32(img + 0x148 + 16, SECT_SIZE);
	w32(img + 0x148 + 20, SECT_OFF);
	w32(img + 0x148 + 36, 0xE0000060u);     /* code, data, RWX */

	/* Two import descriptors and the all-zero terminator. */
	w32(img + AT(0x1000), 0x1100);          /* ILT  */
	w32(img + AT(0x1000) + 12, 0x1300);     /* name */
	w32(img + AT(0x1000) + 16, 0x1200);     /* IAT  */
	w32(img + AT(0x1014), 0);               /* no ILT: the IAT is the lookup table */
	w32(img + AT(0x1014) + 12, 0x1310);
	w32(img + AT(0x1014) + 16, 0x1240);
	w64(img + AT(0x1100), 0x1380);                  /* by name: ExitProcess */
	w64(img + AT(0x1108), 0x8000000000000005ULL);   /* by ordinal 5 */
	w64(img + AT(0x1200), 0x1380);
	w64(img + AT(0x1208), 0x8000000000000005ULL);
	w64(img + AT(0x1240), 0x13A0);                  /* printf */
	strcpy((char *)img + AT(0x1300), "KERNEL32.dll");
	strcpy((char *)img + AT(0x1310), "msvcrt.dll");
	strcpy((char *)img + AT(0x1382), "ExitProcess");
	strcpy((char *)img + AT(0x13A2), "printf");

	/* One relocation block: three DIR64 entries and one ABSOLUTE pad. */
	w32(img + AT(0x1400), 0x1000);
	w32(img + AT(0x1404), 16);
	w16(img + AT(0x1408), 0xA500);
	w16(img + AT(0x140A), 0xA508);
	w16(img + AT(0x140C), 0xA510);
	w16(img + AT(0x140E), 0x0000);
	w64(img + AT(0x1500), 0x140001300ULL);  /* the words being relocated */
	w64(img + AT(0x1508), 0x140001310ULL);
	w64(img + AT(0x1510), 0x140000000ULL);
}

struct seen {
	int n;
	char text[8][48];
	u32 iat[8];
	u32 rva[8];
};

static int on_import(const char *dll, const char *name, u16 ord, u32 iat, void *arg)
{
	struct seen *s = arg;

	if (s->n < 8) {
		if (name)
			snprintf(s->text[s->n], sizeof(s->text[0]), "%s!%s", dll, name);
		else
			snprintf(s->text[s->n], sizeof(s->text[0]), "%s!#%u", dll, ord);
		s->iat[s->n] = iat;
	}
	s->n++;
	return 0;
}

static int on_reloc(u32 rva, void *arg)
{
	struct seen *s = arg;

	if (s->n < 8)
		s->rva[s->n] = rva;
	s->n++;
	return 0;
}

static int stop_at_second(const char *dll, const char *name, u16 ord, u32 iat, void *arg)
{
	(void)dll; (void)name; (void)ord; (void)iat;
	return ++*(int *)arg == 2 ? -MERR_RANGE : 0;
}

static int imports(const u8 *img, size_t len, struct seen *s)
{
	memset(s, 0, sizeof(*s));
	return binfmt_pe_imports(img, len, on_import, s);
}

static int relocs(const u8 *img, size_t len, struct seen *s)
{
	memset(s, 0, sizeof(*s));
	return binfmt_pe_relocs(img, len, on_reloc, s);
}

/* Optional: the real MinGW-built program, when the cross compiler was present. */
static void real_mingw_image(void)
{
	char path[512];
	FILE *f;
	long size;
	u8 *buf;
	struct seen s;
	struct exec_plan plan;
	int found = 0;

	snprintf(path, sizeof(path), "%s/bin/win_hello.exe", FIXTURE_DIR);
	f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(size > 0 ? (size_t)size : 1);
	if (size <= 0 || fread(buf, 1, (size_t)size, f) != (size_t)size) {
		fclose(f);
		free(buf);
		return;
	}
	fclose(f);
	CHECK_EQ(binfmt_load_plan(buf, (size_t)size, &plan), 0);
	CHECK(plan.info.flags & BINFMT_F_DYNAMIC);
	CHECK(plan.info.flags & BINFMT_F_RELOC);
	memset(&s, 0, sizeof(s));
	CHECK_EQ(binfmt_pe_imports(buf, (size_t)size, on_import, &s), 0);
	CHECK(s.n > 20);
	CHECK_EQ(relocs(buf, (size_t)size, &s), 0);
	CHECK(s.n > 0);
	for (int i = 0; i < 8 && i < s.n; i++)
		found += s.rva[i] < plan.image_size;
	CHECK_EQ(found, s.n < 8 ? s.n : 8);
	free(buf);
}

void test_pe(void)
{
	u8 *img = malloc(IMG_LEN), *copy = malloc(IMG_LEN);
	struct exec_plan plan;
	struct seen s;
	int count = 0;

	build(img);

	/* Probe and plan: dynamic, relocatable, ASLR; one section after the headers. */
	CHECK_EQ(binfmt_load_plan(img, IMG_LEN, &plan), 0);
	CHECK_EQ(plan.info.flags & (BINFMT_F_DYNAMIC | BINFMT_F_RELOC | BINFMT_F_ASLR),
		 BINFMT_F_DYNAMIC | BINFMT_F_RELOC | BINFMT_F_ASLR);
	CHECK_EQ(plan.image_base, 0x140000000ULL);
	CHECK_EQ(plan.image_size, 0x2000);
	CHECK_EQ(plan.nsegs, 2);
	CHECK_EQ(plan.entry, 0x140001300ULL);

	/* Imports, in IAT order; by name and by ordinal; ILT-less descriptor. */
	CHECK_EQ(imports(img, IMG_LEN, &s), 0);
	CHECK_EQ(s.n, 3);
	CHECK_STR(s.text[0], "KERNEL32.dll!ExitProcess");
	CHECK_STR(s.text[1], "KERNEL32.dll!#5");
	CHECK_STR(s.text[2], "msvcrt.dll!printf");
	CHECK_EQ(s.iat[0], 0x1200);
	CHECK_EQ(s.iat[1], 0x1208);
	CHECK_EQ(s.iat[2], 0x1240);
	CHECK_EQ(binfmt_pe_imports(img, IMG_LEN, stop_at_second, &count), -MERR_RANGE);
	CHECK_EQ(count, 2);

	/* Relocations: the pad entry is skipped; applying a delta moves every word. */
	CHECK_EQ(relocs(img, IMG_LEN, &s), 0);
	CHECK_EQ(s.n, 3);
	CHECK_EQ(s.rva[0], 0x1500);
	CHECK_EQ(s.rva[1], 0x1508);
	CHECK_EQ(s.rva[2], 0x1510);
	memcpy(copy, img, IMG_LEN);
	for (int i = 0; i < s.n; i++) {
		u8 *p = copy + AT(s.rva[i]);

		w64(p, rd64le(p) + (0x200000000ULL - 0x140000000ULL));
	}
	CHECK_EQ(rd64le(copy + AT(0x1500)), 0x200001300ULL);
	CHECK_EQ(rd64le(copy + AT(0x1510)), 0x200000000ULL);

	/* No import / reloc directory at all is fine. */
	memcpy(copy, img, IMG_LEN);
	w32(copy + DIRS + 1 * 8 + 4, 0);
	w32(copy + DIRS + 5 * 8 + 4, 0);
	CHECK_EQ(imports(copy, IMG_LEN, &s), 0);
	CHECK_EQ(s.n, 0);
	CHECK_EQ(relocs(copy, IMG_LEN, &s), 0);
	CHECK(!(binfmt_probe(copy, IMG_LEN, &plan.info) || (plan.info.flags & BINFMT_F_DYNAMIC)));

	/* Corrupt import tables. */
	memcpy(copy, img, IMG_LEN);
	w32(copy + AT(0x1000) + 12, 0x9000);            /* DLL name outside the image */
	CHECK_EQ(imports(copy, IMG_LEN, &s), -MERR_NOEXEC);
	memcpy(copy, img, IMG_LEN);
	w32(copy + AT(0x1000), SECT_END - 4);           /* ILT runs off the section */
	CHECK_EQ(imports(copy, IMG_LEN, &s), -MERR_NOEXEC);
	memcpy(copy, img, IMG_LEN);
	w64(copy + AT(0x1100), SECT_END - 2);           /* hint/name past the end */
	CHECK_EQ(imports(copy, IMG_LEN, &s), -MERR_NOEXEC);
	memcpy(copy, img, IMG_LEN);
	memset(copy + AT(0x13A2), 'A', SECT_SIZE - 0x3A2);  /* name never terminated */
	CHECK_EQ(imports(copy, IMG_LEN, &s), -MERR_NOEXEC);
	memcpy(copy, img, IMG_LEN);
	w32(copy + AT(0x1000) + 16, 0);                 /* descriptor without IAT */
	CHECK_EQ(imports(copy, IMG_LEN, &s), -MERR_NOEXEC);
	memcpy(copy, img, IMG_LEN);
	w32(copy + DIRS + 1 * 8, SECT_END - 16);        /* no terminator before the end */
	memset(copy + AT(SECT_END - 16), 0x11, 16);
	CHECK_EQ(imports(copy, IMG_LEN, &s), -MERR_NOEXEC);
	memcpy(copy, img, IMG_LEN);
	w64(copy + AT(0x1100), 0x7FFFFFFFFFFFFFFFULL);  /* absurd hint/name RVA */
	CHECK_EQ(imports(copy, IMG_LEN, &s), -MERR_NOEXEC);

	/* Corrupt relocation tables. */
	memcpy(copy, img, IMG_LEN);
	w32(copy + AT(0x1404), 0);                      /* zero-sized block: no progress */
	CHECK_EQ(relocs(copy, IMG_LEN, &s), -MERR_NOEXEC);
	memcpy(copy, img, IMG_LEN);
	w32(copy + AT(0x1404), 64);                     /* block larger than the directory */
	CHECK_EQ(relocs(copy, IMG_LEN, &s), -MERR_NOEXEC);
	memcpy(copy, img, IMG_LEN);
	w16(copy + AT(0x140A), 0x3508);                 /* HIGHLOW: not for PE32+ */
	CHECK_EQ(relocs(copy, IMG_LEN, &s), -MERR_NOTSUP);
	memcpy(copy, img, IMG_LEN);
	w32(copy + DIRS + 5 * 8 + 4, 0x1000);           /* directory beyond the raw data */
	CHECK_EQ(relocs(copy, IMG_LEN, &s), -MERR_NOEXEC);

	/* Truncation at every length, and bit flips everywhere: no crash, no overread. */
	for (size_t n = 0; n <= IMG_LEN; n++) {
		u8 *exact = malloc(n ? n : 1);
		int ri, rr;

		memcpy(exact, img, n);
		ri = imports(exact, n, &s);
		rr = relocs(exact, n, &s);
		CHECK(n == IMG_LEN ? ri == 0 && rr == 0 : ri <= 0 && rr <= 0);
		free(exact);
	}
	for (size_t i = 0; i < IMG_LEN; i++) {
		for (int bit = 0; bit < 8; bit += 3) {
			memcpy(copy, img, IMG_LEN);
			copy[i] ^= (u8)(1u << bit);
			(void)imports(copy, IMG_LEN, &s);
			(void)relocs(copy, IMG_LEN, &s);
			if (!binfmt_load_plan(copy, IMG_LEN, &plan))
				for (u32 k = 0; k < plan.nsegs; k++)
					CHECK(mos_in_range(IMG_LEN, plan.seg[k].fileoff, plan.seg[k].filesz));
		}
	}

	/* Not a PE32+ image at all. */
	CHECK_EQ(binfmt_pe_imports("\177ELF", 4, on_import, &s), -MERR_NOEXEC);
	CHECK_EQ(binfmt_pe_relocs("MZ", 2, on_reloc, &s), -MERR_NOEXEC);

	real_mingw_image();
	free(img);
	free(copy);
}
