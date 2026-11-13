/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS dyld: a minimal dynamic linker for x86-64 Mach-O programs, written
 * from scratch (no code from Apple's dyld). Freestanding, no libraries, no
 * heap: every table is a fixed array. The kernel maps this image at its
 * link-time address and enters at _dyld_start with the Darwin initial stack
 *
 *     rsp -> [mach_header *][argc][argv..][0][envp..][0][apple..][0]
 *
 * It loads LC_LOAD_DYLIB dependencies, applies rebases and binds (classic
 * LC_DYLD_INFO opcodes and LC_DYLD_CHAINED_FIXUPS), runs initialisers, then
 * calls main(argc, argv, envp, apple) and exit(). Missing libraries and
 * symbols are all collected and reported together. See
 * docs/DARWIN_USERSPACE.md.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef long i64;
typedef unsigned long size_t;

__asm__(".globl _dyld_start\n_dyld_start:\n"
	"	movq %rsp, %rdi\n"
	"	andq $-16, %rsp\n"
	"	call _dyld_main\n"
	"	hlt\n");

/* ---- system calls -------------------------------------------------------------- */

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	long r;
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f;

	__asm__ volatile("syscall\n\tjnc 1f\n\tnegq %%rax\n1:"
			 : "=a"(r)
			 : "a"(0x2000000L | n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory", "cc");
	return r;
}

#define PROT_RWX_BITS(p) (p)
#define MAP_PRIVATE 2
#define MAP_FIXED   0x10
#define MAP_ANON    0x1000

void *memcpy(void *d, const void *s, size_t n) __attribute__((used));
void *memset(void *d, int c, size_t n) __attribute__((used));
void *memcpy(void *d, const void *s, size_t n)
{
	u8 *a = d;
	const u8 *b = s;

	while (n--)
		*a++ = *b++;
	return d;
}

void *memset(void *d, int c, size_t n)
{
	u8 *a = d;

	while (n--)
		*a++ = (u8)c;
	return d;
}

static size_t slen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

static int seq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int sprefix(const char *s, const char *p)
{
	while (*p && *s == *p)
		s++, p++;
	return !*p;
}

static void out(const char *s)
{
	sc(4, 2, (long)s, (long)slen(s), 0, 0, 0);
}

static void out_num(u64 v)
{
	char b[24], *p = b + sizeof(b) - 1;

	*p = 0;
	do {
		*--p = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	out(p);
}

static void die(const char *msg)
{
	out("dyld: ");
	out(msg);
	out("\n");
	sc(1, 1, 0, 0, 0, 0, 0);
	for (;;)
		;
}

/* ---- images ------------------------------------------------------------------------ */

#define MAXIMG 24
#define MAXDEP 24
#define MAXSEG 12
#define MH_MAGIC_64 0xFEEDFACFu
#define CPU_X86_64  0x01000007u

#define LC_SEGMENT_64      0x19
#define LC_LOAD_DYLIB      0x0C
#define LC_ID_DYLIB        0x0D
#define LC_LOAD_WEAK_DYLIB 0x80000018u
#define LC_REEXPORT_DYLIB  0x8000001Fu
#define LC_LOAD_UPWARD     0x80000023u
#define LC_DYLD_INFO       0x22
#define LC_DYLD_INFO_ONLY  0x80000022u
#define LC_DYLD_EXPORTS    0x80000033u
#define LC_DYLD_CHAINED    0x80000034u
#define LC_RPATH           0x8000001Cu
#define LC_MAIN            0x80000028u
#define LC_UNIXTHREAD      0x05

struct seg {
	u64 vmaddr, vmsize;
};

struct img {
	const char *path;       /* where it was loaded from */
	const char *name;       /* install name it was asked for */
	u8 *mh;
	u64 slide;
	u32 ncmds;
	int nseg;
	struct seg seg[MAXSEG];
	int ndeps;
	int dep[MAXDEP];        /* image index, or -1 when the library is missing */
	u8 dep_reexport[MAXDEP];
	u8 dep_weak[MAXDEP];
	const char *dep_name[MAXDEP];
	u32 rebase_off, rebase_size, bind_off, bind_size, lazy_off, lazy_size;
	u32 exp_off, exp_size, chain_off, chain_size;
	u8 *linkedit;           /* memory address of file offset linkedit_fileoff */
	u64 linkedit_fileoff;
	u8 inited;
};

static struct img imgs[MAXIMG];
static int nimg;
static const char *exe_path = "";
static char pool[8192];
static size_t pool_used;
static u64 next_base = 0x7FF010000000UL;
static int g_argc;
static char **g_argv, **g_envp, **g_apple;

static const char *pdup(const char *s, size_t n)
{
	char *r = pool + pool_used;

	if (pool_used + n + 1 > sizeof(pool))
		die("out of string space");
	memcpy(r, s, n);
	r[n] = 0;
	pool_used += n + 1;
	return r;
}

/* ---- what went wrong, collected so it can be reported all at once ------------------------ */

struct misslib {
	const char *name, *from, *why;
};
struct misssym {
	const char *sym, *from, *lib;
};
static struct misslib mlibs[16];
static int nmlibs;
static struct misssym msyms[96];
static int nmsyms;

static void note_missing_lib(const char *name, const char *from, const char *why)
{
	for (int i = 0; i < nmlibs; i++)
		if (seq(mlibs[i].name, name))
			return;
	if (nmlibs < 16)
		mlibs[nmlibs++] = (struct misslib){ name, from, why };
}

static void note_missing_sym(const char *sym, const char *from, const char *lib)
{
	for (int i = 0; i < nmsyms; i++)
		if (seq(msyms[i].sym, sym) && seq(msyms[i].lib, lib))
			return;
	if (nmsyms < 96)
		msyms[nmsyms++] = (struct misssym){ sym, from, lib };
}

static void list_syms(const char *lib)
{
	out("  Missing symbols:");
	for (int i = 0; i < nmsyms; i++)
		if (seq(msyms[i].lib, lib)) {
			out(" ");
			out(msyms[i].sym);
		}
	out("\n");
}

/* Everything that could not be resolved, grouped by library, then exit(1). */
static void report_and_exit(void)
{
	for (int i = 0; i < nmlibs; i++) {
		out("dyld: Library not loaded: ");
		out(mlibs[i].name);
		out(" (");
		out(mlibs[i].why);
		out(")\n  Referenced from: ");
		out(mlibs[i].from);
		out("\n");
		list_syms(mlibs[i].name);
	}
	for (int i = 0; i < nmsyms; i++) {
		int seen = 0;

		for (int j = 0; j < nmlibs; j++)
			seen |= seq(mlibs[j].name, msyms[i].lib);
		for (int j = 0; j < i; j++)
			seen |= seq(msyms[j].lib, msyms[i].lib);
		if (seen)
			continue;
		out("dyld: Symbol not found in ");
		out(msyms[i].lib);
		out("\n  Referenced from: ");
		out(msyms[i].from);
		out("\n");
		list_syms(msyms[i].lib);
	}
	out("dyld: ");
	out_num((u64)nmlibs);
	out(nmlibs == 1 ? " library, " : " libraries, ");
	out_num((u64)nmsyms);
	out(nmsyms == 1 ? " symbol missing\n" : " symbols missing\n");
	sc(1, 1, 0, 0, 0, 0, 0);
	for (;;)
		;
}

/* ---- LEB128 and load-command helpers ------------------------------------------------------- */

static u64 uleb(const u8 **pp)
{
	const u8 *p = *pp;
	u64 v = 0;
	int sh = 0;

	do {
		v |= (u64)(*p & 0x7F) << sh;
		sh += 7;
	} while (*p++ & 0x80);
	*pp = p;
	return v;
}

static i64 sleb(const u8 **pp)
{
	const u8 *p = *pp;
	i64 v = 0;
	int sh = 0;
	u8 b;

	do {
		b = *p++;
		v |= (i64)(b & 0x7F) << sh;
		sh += 7;
	} while (b & 0x80);
	if (sh < 64 && (b & 0x40))
		v |= -((i64)1 << sh);
	*pp = p;
	return v;
}

static u32 rd32(const u8 *p) { return *(const u32 *)p; }
static u64 rd64(const u8 *p) { return *(const u64 *)p; }

/* Fill the dependency, segment and fixup tables of a mapped image. */
static void parse_image(struct img *im)
{
	const u8 *lc = im->mh + 32;

	im->ndeps = 0;
	im->nseg = 0;
	for (u32 i = 0; i < im->ncmds; i++, lc += rd32(lc + 4)) {
		u32 cmd = rd32(lc);

		if (cmd == LC_SEGMENT_64 && im->nseg < MAXSEG) {
			im->seg[im->nseg].vmaddr = rd64(lc + 24);
			im->seg[im->nseg].vmsize = rd64(lc + 32);
			if (lc[8] == '_' && lc[9] == '_' && lc[10] == 'L' && lc[11] == 'I' &&
			    lc[12] == 'N') {
				im->linkedit = (u8 *)(rd64(lc + 24) + im->slide);
				im->linkedit_fileoff = rd64(lc + 40);
			}
			im->nseg++;
		} else if ((cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB ||
			    cmd == LC_REEXPORT_DYLIB || cmd == LC_LOAD_UPWARD) && im->ndeps < MAXDEP) {
			im->dep_name[im->ndeps] = (const char *)lc + rd32(lc + 8);
			im->dep_weak[im->ndeps] = cmd == LC_LOAD_WEAK_DYLIB;
			im->dep_reexport[im->ndeps] = cmd == LC_REEXPORT_DYLIB;
			im->dep[im->ndeps++] = -1;
		} else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
			im->rebase_off = rd32(lc + 8);
			im->rebase_size = rd32(lc + 12);
			im->bind_off = rd32(lc + 16);
			im->bind_size = rd32(lc + 20);
			im->lazy_off = rd32(lc + 32);
			im->lazy_size = rd32(lc + 36);
			im->exp_off = rd32(lc + 40);
			im->exp_size = rd32(lc + 44);
		} else if (cmd == LC_DYLD_EXPORTS) {
			im->exp_off = rd32(lc + 8);
			im->exp_size = rd32(lc + 12);
		} else if (cmd == LC_DYLD_CHAINED) {
			im->chain_off = rd32(lc + 8);
			im->chain_size = rd32(lc + 12);
		}
	}
}

static const u8 *le(struct img *im, u32 off)
{
	return im->linkedit + (off - im->linkedit_fileoff);
}

/* ---- finding and mapping libraries --------------------------------------------------------- */

static int find_loaded(const char *name)
{
	for (int i = 0; i < nimg; i++)
		if (seq(imgs[i].name, name) || seq(imgs[i].path, name))
			return i;
	return -1;
}

static int load_image(const char *name, int from);

/* Expand @executable_path / @loader_path / @rpath into `buf`; returns 0 if impossible. */
static int expand_path(const char *name, int from, char *buf, size_t cap, int rpath_try,
		       const char *rpath)
{
	const char *dir_of, *tail = name;
	size_t dl;

	buf[0] = 0;
	if (sprefix(name, "@executable_path/")) {
		dir_of = exe_path;
		tail = name + 17;
	} else if (sprefix(name, "@loader_path/")) {
		dir_of = from >= 0 ? imgs[from].path : exe_path;
		tail = name + 13;
	} else if (sprefix(name, "@rpath/")) {
		if (!rpath_try)
			return 0;
		tail = name + 7;
		if (sprefix(rpath, "@executable_path")) {
			dir_of = exe_path;
			rpath += 16;
		} else if (sprefix(rpath, "@loader_path")) {
			dir_of = from >= 0 ? imgs[from].path : exe_path;
			rpath += 12;
		} else {
			dir_of = 0;
		}
		dl = 0;
		if (dir_of) {
			dl = slen(dir_of);
			while (dl && dir_of[dl - 1] != '/')
				dl--;
			if (dl + slen(rpath) + slen(tail) + 2 > cap)
				return 0;
			memcpy(buf, dir_of, dl);
		}
		memcpy(buf + dl, rpath, slen(rpath));
		dl += slen(rpath);
		if (!dl || buf[dl - 1] != '/')
			buf[dl++] = '/';
		memcpy(buf + dl, tail, slen(tail) + 1);
		return 1;
	} else {
		if (slen(name) >= cap)
			return 0;
		memcpy(buf, name, slen(name) + 1);
		return 1;
	}
	dl = slen(dir_of);
	while (dl && dir_of[dl - 1] != '/')
		dl--;
	if (dl + slen(tail) + 1 > cap)
		return 0;
	memcpy(buf, dir_of, dl);
	memcpy(buf + dl, tail, slen(tail) + 1);
	return 1;
}

/* Collect LC_RPATH entries of image `i` into out[]; returns the count. */
static int rpaths_of(int i, const char **out_paths, int max)
{
	const u8 *lc = imgs[i].mh + 32;
	int n = 0;

	for (u32 k = 0; k < imgs[i].ncmds; k++, lc += rd32(lc + 4))
		if (rd32(lc) == LC_RPATH && n < max)
			out_paths[n++] = (const char *)lc + rd32(lc + 8);
	return n;
}

static void map_fail(const char *name, int from, const char *why)
{
	note_missing_lib(name, from >= 0 ? imgs[from].path : exe_path, why);
}

static int map_file(const char *name, const char *path, int from)
{
	static u8 hdr[16384];
	struct img *im;
	int fd = (int)sc(5, (long)path, 0, 0, 0, 0, 0);
	long n;
	u64 lo = ~0UL, hi = 0, text_va = 0;
	const u8 *lc;
	u32 ncmds;
	u64 base;

	if (fd < 0)
		return -1;
	n = sc(153, fd, (long)hdr, sizeof(hdr), 0, 0, 0);
	if (n < 32 || rd32(hdr) != MH_MAGIC_64 || rd32(hdr + 4) != CPU_X86_64 ||
	    (rd32(hdr + 12) != 6 && rd32(hdr + 12) != 8)) {
		sc(6, fd, 0, 0, 0, 0, 0);
		map_fail(name, from, "not an x86-64 Mach-O dylib");
		return -2;
	}
	ncmds = rd32(hdr + 16);
	lc = hdr + 32;
	for (u32 i = 0; i < ncmds; i++, lc += rd32(lc + 4)) {
		u64 va, sz;

		if (rd32(lc) != LC_SEGMENT_64 || rd32(lc + 60) == 0)
			continue;
		va = rd64(lc + 24);
		sz = rd64(lc + 32);
		if (va < lo)
			lo = va;
		if (va + sz > hi)
			hi = va + sz;
		if (rd64(lc + 40) == 0 && rd64(lc + 48))
			text_va = va;
	}
	if (nimg == MAXIMG || lo == ~0UL)
		die("too many images");
	base = next_base;
	next_base += ((hi - lo + 0xFFF) & ~0xFFFUL) + 0x10000;
	lc = hdr + 32;
	for (u32 i = 0; i < ncmds; i++, lc += rd32(lc + 4)) {
		u64 va, sz, foff, fsz;
		u32 prot;
		long r;

		if (rd32(lc) != LC_SEGMENT_64 || (prot = rd32(lc + 60)) == 0)
			continue;
		va = rd64(lc + 24) - lo + base;
		sz = (rd64(lc + 32) + 0xFFF) & ~0xFFFUL;
		foff = rd64(lc + 40);
		fsz = (rd64(lc + 48) + 0xFFF) & ~0xFFFUL;
		if (fsz > sz)
			fsz = sz;
		if (fsz) {
			r = sc(197, (long)va, (long)fsz, prot, MAP_PRIVATE | MAP_FIXED, fd, (long)foff);
			if (r < 0)
				die("cannot map segment");
		}
		if (sz > fsz) {
			r = sc(197, (long)(va + fsz), (long)(sz - fsz), prot,
			       MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);
			if (r < 0)
				die("cannot map zero-fill");
		}
	}
	sc(6, fd, 0, 0, 0, 0, 0);
	im = &imgs[nimg];
	memset(im, 0, sizeof(*im));
	im->path = pdup(path, slen(path));
	im->name = name;
	im->slide = base - lo;
	im->mh = (u8 *)(text_va + im->slide);
	im->ncmds = ncmds;
	parse_image(im);
	return nimg++;
}

static int load_image(const char *name, int from)
{
	char buf[256];
	const char *rps[8];
	int idx = find_loaded(name), rc = -1, nrp, lastrc = -1;

	if (idx >= 0)
		return idx;
	if (sprefix(name, "@rpath/")) {
		nrp = rpaths_of(0, rps, 8);
		for (int i = 0; i < nrp && rc < 0; i++)
			if (expand_path(name, 0, buf, sizeof(buf), 1, rps[i])) {
				idx = find_loaded(buf);
				if (idx >= 0)
					return idx;
				rc = map_file(name, buf, from);
				lastrc = rc == -2 ? -2 : lastrc;
			}
		if (rc < 0 && lastrc != -2) {
			buf[0] = 0;
			map_fail(name, from, "image not found");
		}
		return rc;
	}
	if (!expand_path(name, from, buf, sizeof(buf), 0, 0)) {
		map_fail(name, from, "path too long");
		return -1;
	}
	idx = find_loaded(buf);
	if (idx >= 0)
		return idx;
	rc = map_file(name, buf, from);
	if (rc == -1)
		map_fail(name, from, "image not found");
	return rc;
}

static void load_deps(int i)
{
	/* Recursion over a growing table: nimg may increase while we walk. */
	for (int d = 0; d < imgs[i].ndeps; d++) {
		int before = nimg, j = load_image(imgs[i].dep_name[d], i);

		imgs[i].dep[d] = j >= 0 ? j : -1;
		if (j < 0 && imgs[i].dep_weak[d]) {     /* a missing weak library is not an error */
			for (int k = 0; k < nmlibs; k++)
				if (seq(mlibs[k].name, imgs[i].dep_name[d]))
					mlibs[k] = mlibs[--nmlibs];
			imgs[i].dep[d] = -2;
		}
		if (j >= 0 && nimg > before)
			load_deps(j);
	}
}

/* ---- symbol lookup ------------------------------------------------------------------------- */

static int find_sym(int ii, const char *sym, u64 *val, int depth);

/* Walk one export trie. Returns 1 and sets *val when `sym` is exported by image ii. */
static int trie_find(int ii, const char *sym, u64 *val, int depth)
{
	struct img *im = &imgs[ii];
	const char *full = sym;
	const u8 *start, *p;

	if (!im->exp_size || !im->linkedit)
		return 0;
	start = le(im, im->exp_off);
	p = start;
	for (;;) {
		u64 term = uleb(&p);
		const u8 *kids = p + term;
		u32 n;
		int hit = 0;

		if (!*sym) {
			u64 flags, addr;

			if (!term)
				return 0;
			flags = uleb(&p);
			if (flags & 0x8) {              /* EXPORT_SYMBOL_FLAGS_REEXPORT */
				u64 ord = uleb(&p);
				const char *nm = (const char *)p;

				if (!*nm)
					nm = full;      /* re-exported under the same name */
				if (ord == 0 || ord > (u64)im->ndeps || im->dep[ord - 1] < 0)
					return 0;
				return find_sym(im->dep[ord - 1], nm, val, depth + 1);
			}
			addr = uleb(&p);
			*val = (flags & 0x2) ? addr : (u64)im->mh + addr;   /* ABSOLUTE or image-relative */
			return 1;
		}
		n = *kids++;
		for (u32 i = 0; i < n && !hit; i++) {
			const char *lab = (const char *)kids;
			u64 off;

			kids += slen(lab) + 1;
			off = uleb(&kids);
			if (sprefix(sym, lab)) {
				sym += slen(lab);
				p = start + off;
				hit = 1;
			}
		}
		if (!hit)
			return 0;
	}
}

/* An image exports what its trie lists plus everything its LC_REEXPORT_DYLIBs export. */
static int find_sym(int ii, const char *sym, u64 *val, int depth)
{
	struct img *im = &imgs[ii];

	if (depth > 8)
		return 0;
	if (trie_find(ii, sym, val, depth))
		return 1;
	for (int d = 0; d < im->ndeps; d++)     /* LC_REEXPORT_DYLIB re-exports everything */
		if (im->dep_reexport[d] && im->dep[d] >= 0 && find_sym(im->dep[d], sym, val, depth + 1))
			return 1;
	return 0;
}

/*
 * Resolve one import of image `ii`. ordinal: >0 library (1-based), 0 self,
 * -1 main executable, -2 flat lookup, -3 weak (any image).
 */
static u64 resolve(int ii, int ordinal, const char *sym, int weak)
{
	struct img *im = &imgs[ii];
	const char *lib = "(flat namespace)";
	u64 v = 0;

	if (ordinal > 0 && ordinal <= im->ndeps) {
		int t = im->dep[ordinal - 1];

		lib = im->dep_name[ordinal - 1];
		if (t == -2)                    /* weak library that is not there */
			return 0;
		if (t >= 0 && find_sym(t, sym, &v, 0))
			return v;
		if (t < 0 && weak)
			return 0;
	} else if (ordinal == 0) {
		lib = im->path;
		if (find_sym(ii, sym, &v, 0))
			return v;
	} else if (ordinal == -1) {
		lib = exe_path;
		if (find_sym(0, sym, &v, 0))
			return v;
	} else {
		for (int i = 0; i < nimg; i++)
			if (find_sym(i, sym, &v, 0))
				return v;
	}
	if (weak)
		return 0;
	note_missing_sym(sym, im->path, lib);
	return 0;
}

/* ---- rebasing and binding ---------------------------------------------------------------------- */

static u64 seg_addr(struct img *im, int s, u64 off)
{
	return im->seg[s < MAXSEG ? s : 0].vmaddr + im->slide + off;
}

static void rebase_stream(struct img *im)
{
	const u8 *p = le(im, im->rebase_off), *end = p + im->rebase_size;
	u64 addr = 0;
	int type = 1;

	if (!im->rebase_size)
		return;
	while (p < end) {
		u8 b = *p++, op = b & 0xF0, imm = b & 0x0F;
		u64 count = 1, skip = 0;

		switch (op) {
		case 0x00: return;
		case 0x10: type = imm; continue;
		case 0x20: addr = seg_addr(im, imm, uleb(&p)); continue;
		case 0x30: addr += uleb(&p); continue;
		case 0x40: addr += (u64)imm * 8; continue;
		case 0x50: count = imm; break;
		case 0x60: count = uleb(&p); break;
		case 0x70: skip = uleb(&p); break;      /* rebase here, then skip ahead */
		case 0x80: count = uleb(&p); skip = uleb(&p); break;
		default: die("bad rebase opcode");
		}
		for (u64 i = 0; i < count; i++) {
			if (type == 1)
				*(u64 *)addr += im->slide;
			addr += 8 + skip;
		}
	}
}

static void bind_stream(int ii, u32 off, u32 size)
{
	struct img *im = &imgs[ii];
	const u8 *p, *end;
	const char *sym = "";
	int ordinal = 0, weak = 0, type = 1;
	i64 addend = 0;
	u64 addr = 0;

	if (!size)
		return;
	p = le(im, off);
	end = p + size;
	while (p < end) {
		u8 b = *p++, op = b & 0xF0, imm = b & 0x0F;
		u64 count = 1, skip = 0;
		int doit = 0;

		switch (op) {
		case 0x00: continue;                    /* DONE (separator in the lazy stream) */
		case 0x10: ordinal = imm; continue;
		case 0x20: ordinal = (int)uleb(&p); continue;
		case 0x30: ordinal = imm ? (int)(signed char)(0xF0 | imm) : 0; continue;
		case 0x40: sym = (const char *)p; weak = imm & 1; p += slen(sym) + 1; continue;
		case 0x50: type = imm; continue;
		case 0x60: addend = sleb(&p); continue;
		case 0x70: addr = seg_addr(im, imm, uleb(&p)); continue;
		case 0x80: addr += uleb(&p); continue;
		case 0x90: doit = 1; break;
		case 0xA0: doit = 1; skip = uleb(&p); break;
		case 0xB0: doit = 1; skip = (u64)imm * 8; break;
		case 0xC0: doit = 1; count = uleb(&p); skip = uleb(&p); break;
		default: die("unsupported bind opcode");
		}
		for (u64 i = 0; doit && i < count; i++) {
			if (type == 1)
				*(u64 *)addr = resolve(ii, ordinal, sym, weak) + (u64)addend;
			addr += 8 + skip;
		}
	}
}

static void chained_fixups(int ii)
{
	struct img *im = &imgs[ii];
	const u8 *cf = le(im, im->chain_off);
	const u8 *starts = cf + rd32(cf + 4), *imports = cf + rd32(cf + 8);
	const char *names = (const char *)cf + rd32(cf + 12);
	u32 nseg = rd32(starts), ifmt = rd32(cf + 20);

	for (u32 s = 0; s < nseg; s++) {
		u32 so = rd32(starts + 4 + s * 4);
		const u8 *ss;
		u32 psize, pages;
		u16 fmt;
		u64 segoff;

		if (!so)
			continue;
		ss = starts + so;
		psize = *(const u16 *)(ss + 4);
		fmt = *(const u16 *)(ss + 6);
		segoff = rd64(ss + 8);
		pages = *(const u16 *)(ss + 20);
		if (fmt != 2 && fmt != 6)
			die("unsupported chained fixup pointer format");
		for (u32 pg = 0; pg < pages; pg++) {
			u16 first = *(const u16 *)(ss + 22 + pg * 2);
			u64 *a;

			if (first == 0xFFFF || (first & 0x8000))
				continue;
			a = (u64 *)(im->mh + segoff + (u64)pg * psize + first);
			for (;;) {
				u64 v = *a;
				u64 next = (v >> 51) & 0xFFF;

				if (v >> 63) {
					u32 oi = (u32)(v & 0xFFFFFF);
					int ord, weak;
					const char *nm;
					i64 add = (i64)((v >> 24) & 0xFF);

					if (ifmt == 1) {
						u32 e = rd32(imports + oi * 4);

						ord = (int)(e & 0xFF);
						ord = ord >= 0xF0 ? ord - 256 : ord;
						weak = (e >> 8) & 1;
						nm = names + (e >> 9);
					} else if (ifmt == 2) {
						u32 e = rd32(imports + oi * 8);

						ord = (int)(e & 0xFF);
						ord = ord >= 0xF0 ? ord - 256 : ord;
						weak = (e >> 8) & 1;
						nm = names + (e >> 9);
						add += (i64)(int)rd32(imports + oi * 8 + 4);
					} else {
						u64 e = rd64(imports + oi * 16);

						ord = (int)(e & 0xFFFF);
						ord = ord >= 0xFFF0 ? ord - 65536 : ord;
						weak = (int)((e >> 16) & 1);
						nm = names + (e >> 32);
						add += (i64)rd64(imports + oi * 16 + 8);
					}
					*a = resolve(ii, ord, nm, weak) + (u64)add;
				} else {
					u64 target = v & 0xFFFFFFFFFULL, high8 = (v >> 36) & 0xFF;

					target = fmt == 2 ? target + im->slide : (u64)im->mh + target;
					*a = target | (high8 << 56);
				}
				if (!next)
					break;
				a = (u64 *)((u8 *)a + next * 4);
			}
		}
	}
}

static void fixup_image(int ii)
{
	struct img *im = &imgs[ii];

	if (im->chain_size) {
		chained_fixups(ii);
		return;
	}
	rebase_stream(im);
	bind_stream(ii, im->bind_off, im->bind_size);
	bind_stream(ii, im->lazy_off, im->lazy_size);   /* lazy binds are done eagerly */
}

/* ---- initialisers and start ----------------------------------------------------------------------- */

typedef void (*initfn)(int, char **, char **, char **);

static void run_inits(int ii)
{
	struct img *im = &imgs[ii];
	const u8 *lc = im->mh + 32;

	if (im->inited)
		return;
	im->inited = 1;
	for (int d = 0; d < im->ndeps; d++)
		if (im->dep[d] >= 0)
			run_inits(im->dep[d]);
	for (u32 i = 0; i < im->ncmds; i++, lc += rd32(lc + 4)) {
		u32 nsect;
		const u8 *sec;

		if (rd32(lc) != LC_SEGMENT_64)
			continue;
		nsect = rd32(lc + 64);
		sec = lc + 72;
		for (u32 s = 0; s < nsect; s++, sec += 80) {
			u32 type = rd32(sec + 64) & 0xFF;
			u64 addr = rd64(sec + 32) + im->slide, size = rd64(sec + 40);

			if (type == 9)                  /* S_MOD_INIT_FUNC_POINTERS */
				for (u64 k = 0; k < size / 8; k++)
					((initfn *)addr)[k](g_argc, g_argv, g_envp, g_apple);
			else if (type == 0x16)          /* S_INIT_FUNC_OFFSETS */
				for (u64 k = 0; k < size / 4; k++)
					((initfn)(im->mh + ((u32 *)addr)[k]))(g_argc, g_argv, g_envp,
									      g_apple);
		}
	}
}

__attribute__((noreturn, used)) void dyld_main(u64 *sp)
{
	struct img *im;
	const u8 *lc;
	u64 entry = 0, text_va = 0;
	int unixthread = 0, nenv = 0;
	char **ap;
	u64 v;

	g_argc = (int)sp[1];
	g_argv = (char **)&sp[2];
	g_envp = g_argv + g_argc + 1;
	while (g_envp[nenv])
		nenv++;
	g_apple = g_envp + nenv + 1;
	for (ap = g_apple; *ap; ap++)
		if (sprefix(*ap, "executable_path="))
			exe_path = *ap + 16;

	/* The kernel already mapped the program; register it as image 0. */
	im = &imgs[nimg++];
	im->path = exe_path;
	im->name = exe_path;
	im->mh = (u8 *)sp[0];
	im->ncmds = rd32(im->mh + 16);
	lc = im->mh + 32;
	for (u32 i = 0; i < im->ncmds; i++, lc += rd32(lc + 4))
		if (rd32(lc) == LC_SEGMENT_64 && rd64(lc + 40) == 0 && rd64(lc + 48))
			text_va = rd64(lc + 24);
	im->slide = (u64)im->mh - text_va;
	parse_image(im);

	load_deps(0);
	for (int i = 0; i < nimg; i++)
		fixup_image(i);
	if (nmlibs || nmsyms)
		report_and_exit();

	/* libSystem first (it gets argc/argv/envp for getenv, errno, the stack guard). */
	for (int i = 1; i < nimg; i++)
		if (find_sym(i, "_mos_libsystem_init", &v, 0)) {
			((initfn)v)(g_argc, g_argv, g_envp, g_apple);
			break;
		}
	for (int i = 0; i < nimg; i++)
		run_inits(i);

	lc = im->mh + 32;
	for (u32 i = 0; i < im->ncmds; i++, lc += rd32(lc + 4)) {
		if (rd32(lc) == LC_MAIN) {
			entry = (u64)im->mh + rd64(lc + 8);
		} else if (rd32(lc) == LC_UNIXTHREAD) {
			entry = rd64(lc + 16 + 16 * 8) + im->slide;
			unixthread = 1;
		}
	}
	if (!entry)
		die("program has no entry point");
	if (unixthread) {       /* old-style crt1 start: expects the original stack at argc */
		__asm__ volatile("movq %0, %%rsp\n\tjmp *%1" : : "r"(&sp[1]), "r"(entry) : "memory");
		__builtin_unreachable();
	}
	{
		int rc = ((int (*)(int, char **, char **, char **))entry)(g_argc, g_argv, g_envp, g_apple);

		for (int i = 1; i < nimg; i++)
			if (find_sym(i, "_exit", &v, 0))
				((void (*)(int))v)(rc);
		sc(1, rc, 0, 0, 0, 0, 0);
	}
	for (;;)
		;
}
