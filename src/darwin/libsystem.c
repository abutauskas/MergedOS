/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS libSystem.B.dylib: the libc subset small command-line programs
 * import, written from scratch for the XNU personality (BSD system calls
 * through SYSCALL, class 2). Built with the host clang as an x86-64 Mach-O
 * dylib; see docs/DARWIN_USERSPACE.md.
 *
 * Not provided: floating point in strtod/scanf, locale, threads, signals,
 * networking, Mach IPC beyond a few constants. Errors are reported through
 * errno (a per-thread slot reached through %gs, as Darwin does it).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef long off_t;
typedef long time_t;

/* ---- raw system calls -------------------------------------------------------- */

/* Darwin returns a positive errno with the carry flag set; fold it to -errno. */
static inline long sc(long n, long a, long b, long c, long d, long e, long f)
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

/* Per-thread data block: gs:[0] points at itself (Darwin TSD convention). */
struct tsd {
	void *self;
	int err;
	void *slots[6];
};
static struct tsd main_tsd;

static int tsd_ready;

int *__error(void)
{
	struct tsd *t;

	if (!tsd_ready)
		return &main_tsd.err;
	__asm__ volatile("movq %%gs:0, %0" : "=r"(t));
	return &t->err;
}
#define errno (*__error())

static long chk(long r)
{
	if (r < 0) {
		errno = (int)-r;
		return -1;
	}
	return r;
}

/* ---- stack protector, dyld glue ------------------------------------------------ */

uintptr_t __stack_chk_guard = 0x2f1d00a5c3e9b77dUL;

void __stack_chk_fail(void)
{
	static const char m[] = "*** stack smashing detected ***\n";

	sc(4, 2, (long)m, sizeof(m) - 1, 0, 0, 0);
	sc(1, 134, 0, 0, 0, 0, 0);
	for (;;)
		;
}

/* Lazy-binding helper: dyld binds everything up front, so this is never reached. */
void dyld_stub_binder_impl(void) __asm__("dyld_stub_binder");
void dyld_stub_binder_impl(void)
{
	__stack_chk_fail();
}

char **environ;
const char *__progname = "";
static int g_argc;
static char **g_argv;
static char **g_environ;
char ***_NSGetArgv(void) { return &g_argv; }
int *_NSGetArgc(void) { return &g_argc; }
char ***_NSGetEnviron(void) { return &g_environ; }
char **_NSGetProgname(void) { return (char **)&__progname; }
unsigned int mach_task_self_ = 0x103;

/* ---- string.h ------------------------------------------------------------------ */

size_t strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t n)
{
	size_t i = 0;

	while (i < n && s[i])
		i++;
	return i;
}

void *memcpy(void *d, const void *s, size_t n)
{
	void *r = d;

	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return r;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;

	if (a < b || a >= b + n)
		return memcpy(d, s, n);
	while (n--)
		a[n] = b[n];
	return d;
}

void *memset(void *d, int c, size_t n)
{
	void *r = d;

	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return r;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return 0;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			break;
	}
	return 0;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;

	while ((*d++ = *s++))
		;
	return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;

	for (; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0;
	return d;
}

char *strcat(char *d, const char *s)
{
	strcpy(d + strlen(d), s);
	return d;
}

char *strncat(char *d, const char *s, size_t n)
{
	char *e = d + strlen(d);

	for (; n && *s; n--)
		*e++ = *s++;
	*e = 0;
	return d;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return 0;
	}
}

char *strrchr(const char *s, int c)
{
	const char *r = 0;

	for (;; s++) {
		if (*s == (char)c)
			r = s;
		if (!*s)
			return (char *)r;
	}
}

char *strstr(const char *h, const char *n)
{
	size_t l = strlen(n);

	if (!l)
		return (char *)h;
	for (; *h; h++)
		if (*h == *n && !strncmp(h, n, l))
			return (char *)h;
	return 0;
}

/* ---- ctype ------------------------------------------------------------------------ */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ---- heap ------------------------------------------------------------------------- */

/* First-fit free list over mmap'd chunks; blocks carry a 16-byte header. */
struct blk {
	size_t size;            /* payload bytes */
	struct blk *next;       /* free list link while free */
};
static struct blk *freelist;

static void *vm_alloc(size_t n)
{
	long r = sc(197, 0, (long)n, 3, 0x1002, -1, 0);         /* PROT_RW, MAP_PRIVATE|MAP_ANON */

	return r < 0 ? 0 : (void *)r;
}

void *malloc(size_t n)
{
	struct blk **pp, *b;

	n = (n + 15) & ~(size_t)15;
	if (!n)
		n = 16;
	for (pp = &freelist; (b = *pp); pp = &b->next) {
		if (b->size < n)
			continue;
		if (b->size >= n + 32) {                        /* split */
			struct blk *rest = (struct blk *)((char *)b + 16 + n);

			rest->size = b->size - n - 16;
			rest->next = b->next;
			b->size = n;
			*pp = rest;
		} else {
			*pp = b->next;
		}
		return (char *)b + 16;
	}
	{
		size_t chunk = n + 16 > 0x40000 ? (n + 16 + 0xfff) & ~(size_t)0xfff : 0x40000;
		char *m = vm_alloc(chunk);

		if (!m) {
			errno = 12;
			return 0;
		}
		b = (struct blk *)m;
		b->size = n;
		if (chunk - n - 16 >= 32) {                     /* the tail becomes a free block */
			struct blk *rest = (struct blk *)(m + 16 + n);

			rest->size = chunk - n - 32;
			rest->next = freelist;
			freelist = rest;
		} else {
			b->size = chunk - 16;
		}
		return m + 16;
	}
}

void free(void *p)
{
	struct blk *b;

	if (!p)
		return;
	b = (struct blk *)((char *)p - 16);
	b->next = freelist;
	freelist = b;
}

void *calloc(size_t a, size_t b)
{
	size_t n = a * b;
	void *p;

	if (a && n / a != b) {
		errno = 12;
		return 0;
	}
	p = malloc(n);
	return p ? memset(p, 0, n) : 0;
}

void *realloc(void *p, size_t n)
{
	struct blk *b;
	void *q;

	if (!p)
		return malloc(n);
	b = (struct blk *)((char *)p - 16);
	if (b->size >= n)
		return p;
	q = malloc(n);
	if (q) {
		memcpy(q, p, b->size);
		free(p);
	}
	return q;
}

char *strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	return d ? memcpy(d, s, n) : 0;
}

/* ---- conversions -------------------------------------------------------------------- */

unsigned long strtoul(const char *s, char **end, int base)
{
	unsigned long v = 0;
	int neg = 0, any = 0;

	while (isspace((unsigned char)*s))
		s++;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = s[0] == '0' ? 8 : 10;
	}
	for (;; s++, any = 1) {
		int d = isdigit(*s) ? *s - '0' : isalpha(*s) ? tolower(*s) - 'a' + 10 : 99;

		if (d >= base)
			break;
		v = v * (unsigned long)base + (unsigned long)d;
	}
	if (end)
		*end = (char *)(any ? s : 0);
	return neg ? -v : v;
}

long strtol(const char *s, char **end, int base)
{
	return (long)strtoul(s, end, base);
}

long long strtoll(const char *s, char **end, int base) { return (long long)strtoul(s, end, base); }
unsigned long long strtoull(const char *s, char **end, int base) { return strtoul(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

static unsigned long rand_state = 1;
void srand(unsigned s) { rand_state = s; }
int rand(void)
{
	rand_state = rand_state * 6364136223846793005UL + 1442695040888963407UL;
	return (int)((rand_state >> 33) & 0x7fffffff);
}

void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
	char *a = base, tmp[64];

	if (sz > sizeof(tmp))
		return;
	for (size_t gap = n / 2; gap; gap /= 2)         /* shell sort */
		for (size_t i = gap; i < n; i++) {
			size_t j = i;

			memcpy(tmp, a + i * sz, sz);
			for (; j >= gap && cmp(a + (j - gap) * sz, tmp) > 0; j -= gap)
				memcpy(a + j * sz, a + (j - gap) * sz, sz);
			memcpy(a + j * sz, tmp, sz);
		}
}

/* ---- process, environment, exit --------------------------------------------------------- */

static void (*atexit_fns[16])(void);
static int atexit_n;
int atexit(void (*fn)(void))
{
	if (atexit_n == 16)
		return -1;
	atexit_fns[atexit_n++] = fn;
	return 0;
}

int fflush(void *fp);

void exit(int status)
{
	while (atexit_n)
		atexit_fns[--atexit_n]();
	fflush(0);
	sc(1, status, 0, 0, 0, 0, 0);
	for (;;)
		;
}

void _exit(int status)
{
	sc(1, status, 0, 0, 0, 0, 0);
	for (;;)
		;
}

void abort(void)
{
	fflush(0);
	sc(1, 134, 0, 0, 0, 0, 0);
	for (;;)
		;
}

char *getenv(const char *name)
{
	size_t l = strlen(name);

	for (char **e = g_environ; e && *e; e++)
		if (!strncmp(*e, name, l) && (*e)[l] == '=')
			return *e + l + 1;
	return 0;
}

int getpid(void) { return (int)sc(20, 0, 0, 0, 0, 0, 0); }

int open(const char *path, int flags, ...)
{
	return (int)chk(sc(5, (long)path, flags, 0644, 0, 0, 0));
}

int close(int fd) { return (int)chk(sc(6, fd, 0, 0, 0, 0, 0)); }
ssize_t read(int fd, void *b, size_t n) { return chk(sc(3, fd, (long)b, (long)n, 0, 0, 0)); }
ssize_t write(int fd, const void *b, size_t n) { return chk(sc(4, fd, (long)b, (long)n, 0, 0, 0)); }
off_t lseek(int fd, off_t o, int w) { return chk(sc(199, fd, o, w, 0, 0, 0)); }
int unlink(const char *p) { return (int)chk(sc(10, (long)p, 0, 0, 0, 0, 0)); }
int mkdir(const char *p, int mode) { return (int)chk(sc(136, (long)p, mode, 0, 0, 0, 0)); }
int rmdir(const char *p) { return (int)chk(sc(137, (long)p, 0, 0, 0, 0, 0)); }
int rename(const char *a, const char *b) { return (int)chk(sc(128, (long)a, (long)b, 0, 0, 0, 0)); }
int access(const char *p, int m) { return (int)chk(sc(33, (long)p, m, 0, 0, 0, 0)); }
int stat(const char *p, void *st) { return (int)chk(sc(338, (long)p, (long)st, 0, 0, 0, 0)); }
int fstat(int fd, void *st) { return (int)chk(sc(339, fd, (long)st, 0, 0, 0, 0)); }
int isatty(int fd) { (void)fd; return 0; }

int gettimeofday(void *tv, void *tz)
{
	return (int)chk(sc(116, (long)tv, (long)tz, 0, 0, 0, 0));
}

time_t time(time_t *t)
{
	long tv[2] = { 0, 0 };

	gettimeofday(tv, 0);
	if (t)
		*t = tv[0];
	return tv[0];
}

int sysctlbyname(const char *name, void *old, size_t *oldlen, void *new, size_t newlen)
{
	return (int)chk(sc(274, (long)name, (long)strlen(name), (long)old, (long)oldlen,
			   (long)new, (long)newlen));
}

/* Mach: the traps libSystem's own clients commonly call. */
uint64_t mach_absolute_time(void)
{
	long r;

	__asm__ volatile("syscall" : "=a"(r) : "a"(0x1000000L | 91) : "rcx", "r11", "memory");
	return (uint64_t)r;
}

int mach_timebase_info(uint32_t *tb)
{
	tb[0] = tb[1] = 1;
	return 0;
}

unsigned int mach_host_self(void) { return 0x303; }

/* ---- errno strings ---------------------------------------------------------------------- */

char *strerror(int e)
{
	static char buf[24];
	static const char *const names[] = {
		[0] = "Undefined error: 0", [1] = "Operation not permitted",
		[2] = "No such file or directory", [3] = "No such process",
		[4] = "Interrupted system call", [5] = "Input/output error",
		[9] = "Bad file descriptor", [12] = "Cannot allocate memory",
		[13] = "Permission denied", [14] = "Bad address", [17] = "File exists",
		[20] = "Not a directory", [21] = "Is a directory", [22] = "Invalid argument",
		[24] = "Too many open files", [78] = "Function not implemented",
	};
	char *p = buf + sizeof(buf) - 1;
	unsigned v = (unsigned)e;

	if (e >= 0 && e < (int)(sizeof(names) / sizeof(names[0])) && names[e])
		return (char *)names[e];
	strcpy(buf, "Unknown error: ");
	*p = 0;
	do {
		*--p = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	strcpy(buf + 15, p);
	return buf;
}

/* ---- stdio ------------------------------------------------------------------------------- */

#define F_READ   1
#define F_WRITE  2
#define F_EOF    4
#define F_ERR    8
#define F_LINE   16
#define F_UNBUF  32
#define F_OWNED  64
#define BUFSZ    1024

typedef struct sfile {
	int fd, flags;
	int bpos, blen;                 /* read: consumed/valid; write: blen = pending */
	struct sfile *next;
	unsigned char buf[BUFSZ];
} FILE;

static FILE f_in = { .fd = 0, .flags = F_READ };
static FILE f_out = { .fd = 1, .flags = F_WRITE | F_LINE };
static FILE f_err = { .fd = 2, .flags = F_WRITE | F_UNBUF };
FILE *__stdinp = &f_in, *__stdoutp = &f_out, *__stderrp = &f_err;
static FILE *open_files;

static int flush_one(FILE *f)
{
	int off = 0;

	if (!(f->flags & F_WRITE) || !f->blen)
		return 0;
	while (off < f->blen) {
		long r = sc(4, f->fd, (long)f->buf + off, f->blen - off, 0, 0, 0);

		if (r <= 0) {
			f->flags |= F_ERR;
			errno = (int)-r;
			f->blen = 0;
			return -1;
		}
		off += (int)r;
	}
	f->blen = 0;
	return 0;
}

int fflush(void *fp)
{
	int rc = 0;

	if (fp)
		return flush_one(fp);
	rc |= flush_one(&f_out);
	rc |= flush_one(&f_err);
	for (FILE *f = open_files; f; f = f->next)
		rc |= flush_one(f);
	return rc;
}

static int out_byte(FILE *f, int c)
{
	if (!(f->flags & F_WRITE)) {
		errno = 9;
		f->flags |= F_ERR;
		return -1;
	}
	f->buf[f->blen++] = (unsigned char)c;
	if (f->blen == BUFSZ || (f->flags & F_UNBUF) || ((f->flags & F_LINE) && c == '\n'))
		return flush_one(f);
	return 0;
}

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
	const unsigned char *b = p;
	size_t total = sz * n;

	for (size_t i = 0; i < total; i++)
		if (out_byte(f, b[i]))
			return i / (sz ? sz : 1);
	return n;
}

int fputc(int c, FILE *f) { return out_byte(f, c) ? -1 : (unsigned char)c; }
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, __stdoutp); }

int fputs(const char *s, FILE *f)
{
	return fwrite(s, 1, strlen(s), f) == strlen(s) ? 0 : -1;
}

int puts(const char *s)
{
	if (fputs(s, __stdoutp) < 0)
		return -1;
	return out_byte(__stdoutp, '\n') ? -1 : 1;
}

static int in_byte(FILE *f)
{
	if (!(f->flags & F_READ)) {
		errno = 9;
		return -1;
	}
	if (f->bpos == f->blen) {
		long r = sc(3, f->fd, (long)f->buf, BUFSZ, 0, 0, 0);

		if (r < 0) {
			f->flags |= F_ERR;
			errno = (int)-r;
			return -1;
		}
		if (r == 0) {
			f->flags |= F_EOF;
			return -1;
		}
		f->bpos = 0;
		f->blen = (int)r;
	}
	return f->buf[f->bpos++];
}

int fgetc(FILE *f) { return in_byte(f); }
int getc(FILE *f) { return in_byte(f); }
int getchar(void) { return in_byte(__stdinp); }

size_t fread(void *p, size_t sz, size_t n, FILE *f)
{
	unsigned char *b = p;
	size_t total = sz * n, i;

	for (i = 0; i < total; i++) {
		int c = in_byte(f);

		if (c < 0)
			break;
		b[i] = (unsigned char)c;
	}
	return sz ? i / sz : 0;
}

char *fgets(char *s, int n, FILE *f)
{
	int i = 0;

	while (i < n - 1) {
		int c = in_byte(f);

		if (c < 0)
			break;
		s[i++] = (char)c;
		if (c == '\n')
			break;
	}
	if (!i)
		return 0;
	s[i] = 0;
	return s;
}

int feof(FILE *f) { return (f->flags & F_EOF) != 0; }
int ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f) { return f->fd; }

FILE *fopen(const char *path, const char *mode)
{
	int fl, fd, rw = mode[0] != 'r' ? 1 : 0, plus = 0;
	FILE *f;

	for (const char *m = mode + 1; *m; m++)
		plus |= *m == '+';
	if (mode[0] == 'r')
		fl = plus ? 2 : 0;
	else if (mode[0] == 'w')
		fl = (plus ? 2 : 1) | 0x200 | 0x400;            /* O_CREAT | O_TRUNC */
	else if (mode[0] == 'a')
		fl = (plus ? 2 : 1) | 0x200 | 0x8;              /* O_CREAT | O_APPEND */
	else {
		errno = 22;
		return 0;
	}
	fd = open(path, fl, 0644);
	if (fd < 0)
		return 0;
	f = calloc(1, sizeof(*f));
	if (!f) {
		close(fd);
		return 0;
	}
	f->fd = fd;
	f->flags = (rw || plus ? F_WRITE : 0) | (!rw || plus ? F_READ : 0) | F_OWNED;
	f->next = open_files;
	open_files = f;
	return f;
}

int fclose(FILE *f)
{
	FILE **pp;
	int rc = flush_one(f);

	if (f->flags & F_OWNED) {
		for (pp = &open_files; *pp; pp = &(*pp)->next)
			if (*pp == f) {
				*pp = f->next;
				break;
			}
		rc |= close(f->fd);
		free(f);
	}
	return rc ? -1 : 0;
}

int fseek(FILE *f, long off, int whence)
{
	flush_one(f);
	if (f->flags & F_READ) {
		if (whence == 1)
			off -= f->blen - f->bpos;
		f->bpos = f->blen = 0;
	}
	f->flags &= ~F_EOF;
	return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f)
{
	long p = lseek(f->fd, 0, 1);

	if (p < 0)
		return -1;
	return (f->flags & F_READ) && f->blen ? p - (f->blen - f->bpos) : p + f->blen;
}

void rewind(FILE *f) { fseek(f, 0, 0); clearerr(f); }
int remove(const char *p) { return unlink(p); }
void perror(const char *s)
{
	if (s && *s) {
		fputs(s, __stderrp);
		fputs(": ", __stderrp);
	}
	fputs(strerror(errno), __stderrp);
	fputc('\n', __stderrp);
}

/* ---- printf ------------------------------------------------------------------------------ */

struct sink {
	FILE *fp;
	char *buf;
	size_t cap, len;        /* snprintf: total length keeps counting past cap */
};

static void emit(struct sink *s, char c)
{
	if (s->fp)
		out_byte(s->fp, c);
	else if (s->len + 1 < s->cap)
		s->buf[s->len] = c;
	s->len++;
}

static void pad(struct sink *s, int n, char c)
{
	while (n-- > 0)
		emit(s, c);
}

static int vfmt(struct sink *s, const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = 0, prec = -1, lng = 0;
		char tmp[72], *p = tmp + sizeof(tmp), sign = 0;
		unsigned long long u = 0;
		const char *str = 0;
		int len = 0, base = 10, upper = 0, isnum = 0;

		if (*fmt != '%') {
			emit(s, *fmt);
			continue;
		}
		for (fmt++;; fmt++) {
			if (*fmt == '-') left = 1;
			else if (*fmt == '+') plus = 1;
			else if (*fmt == ' ') space = 1;
			else if (*fmt == '#') alt = 1;
			else if (*fmt == '0') zero = 1;
			else break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = 1;
				width = -width;
			}
			fmt++;
		} else {
			while (isdigit(*fmt))
				width = width * 10 + *fmt++ - '0';
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (isdigit(*fmt))
					prec = prec * 10 + *fmt++ - '0';
			}
		}
		for (;; fmt++) {
			if (*fmt == 'l') lng++;
			else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'q') lng = 2;
			else if (*fmt == 'h') lng--;
			else break;
		}
		switch (*fmt) {
		case 'd': case 'i': {
			long long v = lng >= 1 ? va_arg(ap, long long) : va_arg(ap, int);

			if (lng < 0)
				v = lng == -1 ? (short)v : (signed char)v;
			if (v < 0) {
				sign = '-';
				u = 0ULL - (unsigned long long)v;
			} else {
				u = (unsigned long long)v;
				sign = plus ? '+' : space ? ' ' : 0;
			}
			isnum = 1;
			break;
		}
		case 'u': case 'x': case 'X': case 'o': case 'p':
			if (*fmt == 'p') {
				u = (unsigned long long)(uintptr_t)va_arg(ap, void *);
				base = 16;
				alt = 1;
			} else {
				u = lng >= 1 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
				if (lng == -1) u = (unsigned short)u;
				if (lng == -2) u = (unsigned char)u;
				base = *fmt == 'o' ? 8 : *fmt == 'u' ? 10 : 16;
			}
			upper = *fmt == 'X';
			isnum = 1;
			break;
		case 'c':
			tmp[0] = (char)va_arg(ap, int);
			str = tmp;
			len = 1;
			break;
		case 's':
			str = va_arg(ap, const char *);
			if (!str)
				str = "(null)";
			len = (int)(prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str));
			break;
		case 'f': case 'F': {
			double d = va_arg(ap, double);
			unsigned long long ip, frac = 0;
			int pr = prec < 0 ? 6 : prec;
			double scale = 1, r = 0.5;

			if (d < 0) {
				sign = '-';
				d = -d;
			} else {
				sign = plus ? '+' : space ? ' ' : 0;
			}
			for (int i = 0; i < pr && i < 17; i++) {
				scale *= 10;
				r /= 10;
			}
			d += r;
			ip = (unsigned long long)d;
			frac = (unsigned long long)((d - (double)ip) * scale);
			for (int i = 0; i < pr && i < 17; i++) {
				*--p = (char)('0' + frac % 10);
				frac /= 10;
			}
			if (pr)
				*--p = '.';
			do {
				*--p = (char)('0' + ip % 10);
				ip /= 10;
			} while (ip);
			str = p;
			len = (int)(tmp + sizeof(tmp) - p);
			break;
		}
		case '%':
			emit(s, '%');
			continue;
		default:
			emit(s, '%');
			if (*fmt)
				emit(s, *fmt);
			else
				fmt--;
			continue;
		}
		if (isnum) {
			const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
			int nd, pfx = *fmt == 'p' || (alt && base == 16 && u != 0);

			do {
				*--p = digs[u % (unsigned)base];
				u /= (unsigned)base;
			} while (u);
			nd = (int)(tmp + sizeof(tmp) - p);
			if (prec >= 0) {
				zero = 0;
				while (nd < prec && p > tmp + 8) {
					*--p = '0';
					nd++;
				}
			}
			if (alt && base == 8 && *p != '0')
				*--p = '0';
			if (pfx && zero && !left) {             /* "0x" goes before the zero padding */
				emit(s, '0');
				emit(s, upper ? 'X' : 'x');
				width -= 2;
			} else if (pfx) {
				*--p = upper ? 'X' : 'x';
				*--p = '0';
			}
			str = p;
			len = (int)(tmp + sizeof(tmp) - p);
		}
		{
			int extra = sign ? 1 : 0, padn = width - len - extra;

			if (!left && !zero)
				pad(s, padn, ' ');
			if (sign)
				emit(s, sign);
			if (!left && zero)
				pad(s, padn, '0');
			for (int i = 0; i < len; i++)
				emit(s, str[i]);
			if (left)
				pad(s, padn, ' ');
		}
	}
	return (int)s->len;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
	struct sink s = { f, 0, 0, 0 };

	return vfmt(&s, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(__stdoutp, fmt, ap); }

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
	struct sink s = { 0, buf, cap, 0 };
	int n = vfmt(&s, fmt, ap);

	if (cap)
		buf[s.len < cap ? s.len : cap - 1] = 0;
	return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap); }

int printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vfprintf(__stdoutp, fmt, ap);
	va_end(ap);
	return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}

int snprintf(char *b, size_t cap, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, cap, fmt, ap);
	va_end(ap);
	return n;
}

int sprintf(char *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, (size_t)-1 >> 1, fmt, ap);
	va_end(ap);
	return n;
}

/* Fortified variants the Apple SDK headers select by default. */
void *__memcpy_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memcpy(d, s, n); }
void *__memmove_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memmove(d, s, n); }
void *__memset_chk(void *d, int c, size_t n, size_t dl) { (void)dl; return memset(d, c, n); }
char *__strcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return strcpy(d, s); }
char *__strcat_chk(char *d, const char *s, size_t dl) { (void)dl; return strcat(d, s); }
char *__strncpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncpy(d, s, n); }

int __vsnprintf_chk(char *b, size_t cap, int fl, size_t dl, const char *fmt, va_list ap)
{
	(void)fl;
	(void)dl;
	return vsnprintf(b, cap, fmt, ap);
}

int __snprintf_chk(char *b, size_t cap, int fl, size_t dl, const char *fmt, ...)
{
	va_list ap;
	int n;

	(void)fl;
	(void)dl;
	va_start(ap, fmt);
	n = vsnprintf(b, cap, fmt, ap);
	va_end(ap);
	return n;
}

int __sprintf_chk(char *b, int fl, size_t dl, const char *fmt, ...)
{
	va_list ap;
	int n;

	(void)fl;
	(void)dl;
	va_start(ap, fmt);
	n = vsnprintf(b, (size_t)-1 >> 1, fmt, ap);
	va_end(ap);
	return n;
}

void __assert_rtn(const char *fn, const char *file, int line, const char *expr)
{
	fprintf(__stderrp, "Assertion failed: (%s), function %s, file %s, line %d.\n", expr, fn, file, line);
	abort();
}

/* ---- start-up: dyld calls this before any initialiser or main() ----------------------------- */

void mos_libsystem_init(int argc, char **argv, char **envp, char **apple)
{
	long seed = 0;
	const char *p;

	main_tsd.self = &main_tsd;
	{
		long r;

		__asm__ volatile("syscall" : "=a"(r) : "a"(0x3000003L), "D"(&main_tsd)
				 : "rcx", "r11", "memory");        /* thread_fast_set_cthread_self */
	}
	tsd_ready = 1;
	g_argc = argc;
	g_argv = argv;
	g_environ = environ = envp;
	(void)apple;
	p = argc > 0 && argv[0] ? argv[0] : "";
	for (const char *q = p; *q; q++)
		if (*q == '/')
			p = q + 1;
	__progname = p;
	if (sc(500, (long)&seed, sizeof(seed), 0, 0, 0, 0) == 0 && seed)
		__stack_chk_guard = (uintptr_t)seed & ~(uintptr_t)0xff;
}
