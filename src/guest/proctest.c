/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * proctest.linux - processes, threads, pipes, signals, ramfs and demand
 * paging, exercised through raw Linux x86-64 system calls. Freestanding (no
 * libc), so it builds with the host gcc alone; it is a genuine static Linux
 * binary and runs unmodified on Linux too.
 *
 * Exits with the number of the first failed check, or 0 after printing
 * "proctest: OK". With argv[1] == "child" it is the program the test execs.
 */
typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;

#define SIGINT   2
#define SIGUSR1  10
#define SIGPIPE  13
#define SIGTERM  15
#define SIGCHLD  17
#define SA_RESTORER 0x04000000UL

#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f;
	long ret;

	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}
#define sys3(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define sys4(n, a, b, c, d) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

static long xwrite(int fd, const void *p, u64 n) { return sys3(1, fd, p, n); }
static long xread(int fd, void *p, u64 n) { return sys3(0, fd, p, n); }
static long xclose(int fd) { return sys3(3, fd, 0, 0); }
static void xexit(int code) { for (;;) sys3(60, code, 0, 0); }
static long xgetpid(void) { return sys3(39, 0, 0, 0); }
static long xfork(void) { return sys3(57, 0, 0, 0); }
static long xwait(long pid, int *st) { return sys4(61, pid, st, 0, 0); }
static long xkill(long pid, int sig) { return sys3(62, pid, sig, 0); }

void *memset(void *d, int c, u64 n);
void *memcpy(void *d, const void *s, u64 n);
void *memset(void *d, int c, u64 n)
{
	unsigned char *p = d;

	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, u64 n)
{
	unsigned char *p = d;
	const unsigned char *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

static u64 slen(const char *s)
{
	u64 n = 0;

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

static void puts1(const char *s)
{
	xwrite(1, s, slen(s));
}

static int step;
#define CHECK(cond) do { step++; if (!(cond)) fail(); } while (0)

static void fail(void)
{
	char msg[] = "proctest: check 00 failed\n";

	msg[16] = (char)('0' + step / 10);
	msg[17] = (char)('0' + step % 10);
	puts1(msg);
	xexit(step);
}

/* ---- signals ---- */

extern void restorer(void);
__asm__(".text\n.globl restorer\nrestorer:\n\tmov $15, %eax\n\tsyscall\n\thlt\n");

static volatile int got_sig;
static volatile double fp_value;

static void on_signal(int sig)
{
	double x = fp_value;

	got_sig = sig;
	fp_value = x * 3.0;             /* uses SSE registers inside the handler */
}

static long sigaction1(int sig, u64 handler)
{
	u64 act[4] = { handler, SA_RESTORER, (u64)restorer, 0 };

	return sys4(13, sig, act, 0, 8);
}

/* ---- threads ---- */

static volatile int shared;
static volatile int child_tid = -1;
static u64 tls_block[8];
static unsigned char thread_stack[16384] __attribute__((aligned(16)));

static int thread_main(void *arg)
{
	u64 fs = 0;

	sys3(158, 0x1003, &fs, 0);                      /* arch_prctl(ARCH_GET_FS) */
	shared = (fs == (u64)tls_block) ? (int)(long)arg : -1;
	return 0;
}

/* clone() with a fresh stack: the child pops fn and arg, runs fn, then exit(). */
static long spawn_thread(int (*fn)(void *), void *arg)
{
	u64 *sp = (u64 *)(thread_stack + sizeof(thread_stack));
	register long r10 __asm__("r10") = (long)&child_tid;
	register long r8 __asm__("r8") = (long)tls_block;
	long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
		     CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
	long ret;

	*--sp = (u64)arg;
	*--sp = (u64)fn;
	__asm__ volatile("syscall\n\t"
			 "test %%rax, %%rax\n\t"
			 "jnz 1f\n\t"
			 "pop %%rax\n\t"
			 "pop %%rdi\n\t"
			 "call *%%rax\n\t"
			 "mov %%eax, %%edi\n\t"
			 "mov $60, %%eax\n\t"
			 "syscall\n\t"
			 "hlt\n"
			 "1:"
			 : "=a"(ret)
			 : "a"(56L), "D"(flags), "S"(sp), "d"(&child_tid), "r"(r10), "r"(r8)
			 : "rcx", "r11", "memory");
	return ret;
}

/* ---- the child half of the exec test ---- */

static int child_main(int argc, char **argv, char **envp)
{
	if (argc != 3 || !seq(argv[2], "arg2") || !envp[0] || !seq(envp[0], "MOS=1"))
		return 99;
	puts1("exec-ok");
	return 7;
}

/* Run argv[0]=path child arg2 with stdout on a pipe; collect output and status. */
static int run_child(const char *path, char *out, u64 cap, int *status)
{
	int p[2];
	long pid, n, got = 0;

	if (sys3(22, p, 0, 0))
		return -1;
	pid = xfork();
	if (pid == 0) {
		const char *argv[] = { "proctest", "child", "arg2", 0 };
		const char *envp[] = { "MOS=1", 0 };

		sys3(33, p[1], 1, 0);                   /* dup2(p[1], 1) */
		xclose(p[0]);
		xclose(p[1]);
		sys3(59, path, argv, envp);
		xexit(98);
	}
	xclose(p[1]);
	while ((n = xread(p[0], out + got, cap - 1 - (u64)got)) > 0)
		got += n;
	out[got] = 0;
	xclose(p[0]);
	return xwait(pid, status) == pid ? 0 : -1;
}

int cmain(long *sp);
int cmain(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1), **envp = argv + argc + 1;
	char buf[128];
	int st = 0, p[2], fd;
	long pid, n;

	if (argc > 1 && seq(argv[1], "child"))
		return child_main(argc, argv, envp);
	if (slen(argv[0]) >= 9 && seq(argv[0] + slen(argv[0]) - 9, "cat.linux")) {
		/* Installed a second time as bin/cat.linux: copy stdin to stdout. */
		while ((n = xread(0, buf, sizeof(buf))) > 0)
			xwrite(1, buf, (u64)n);
		return n < 0 ? 1 : 0;
	}

	/* 1-3: identity */
	pid = xgetpid();
	CHECK(pid > 0);
	CHECK(sys3(110, 0, 0, 0) >= 0);                 /* getppid */
	CHECK(sys3(186, 0, 0, 0) == pid);               /* gettid == pid in the main thread */

	/* 4-6: fork + pipe + dup2 + execve + wait4 */
	CHECK(run_child("/bin/proctest.linux", buf, sizeof(buf), &st) == 0);
	CHECK(seq(buf, "exec-ok"));
	CHECK(st == (7 << 8));

	/* 7: vfork; the child exits at once */
	{
		long r;

		__asm__ volatile("syscall\n\t"
				 "test %%rax, %%rax\n\t"
				 "jnz 1f\n\t"
				 "mov $5, %%edi\n\t"
				 "mov $60, %%eax\n\t"
				 "syscall\n"
				 "1:"
				 : "=a"(r) : "a"(58L) : "rcx", "r11", "rdi", "memory");
		CHECK(r > 0 && xwait(r, &st) == r && st == (5 << 8));
	}

	/* 8-9: a thread with its own TLS; join through CLONE_CHILD_CLEARTID + futex */
	n = spawn_thread(thread_main, (void *)42L);
	CHECK(n > 0);
	while (child_tid != 0)
		sys6(202, (long)&child_tid, 0, child_tid, 0, 0, 0);    /* FUTEX_WAIT */
	CHECK(shared == 42);

	/* 10-12: signal handler, masking, SSE state across the handler */
	fp_value = 1.5;
	CHECK(sigaction1(SIGUSR1, (u64)on_signal) == 0);
	{
		u64 set = 1UL << (SIGUSR1 - 1);
		volatile double keep = 2.25;

		sys4(14, 0, &set, 0, 8);                /* SIG_BLOCK */
		xkill(pid, SIGUSR1);
		CHECK(got_sig == 0);
		sys4(14, 1, &set, 0, 8);                /* SIG_UNBLOCK: delivered on return */
		CHECK(got_sig == SIGUSR1 && fp_value == 4.5 && keep == 2.25);
	}

	/* 13: SIGPIPE ignored -> EPIPE */
	sigaction1(SIGPIPE, 1);                         /* SIG_IGN */
	sys3(22, p, 0, 0);
	xclose(p[0]);
	CHECK(xwrite(p[1], "x", 1) == -32);
	xclose(p[1]);

	/* 14-15: preemption + kill: a child that spins forever is still stopped */
	pid = xfork();
	if (pid == 0)
		for (;;)
			;
	sys3(24, 0, 0, 0);                              /* sched_yield: let it spin */
	CHECK(xkill(pid, SIGTERM) == 0);
	CHECK(xwait(pid, &st) == pid && (st & 0x7F) == SIGTERM);

	/* 16-21: ramfs at /tmp and /home/user */
	fd = (int)sys3(2, "/tmp/pt.txt", 0x42 | 0x200, 0644);  /* O_RDWR|O_CREAT|O_TRUNC */
	CHECK(fd >= 0);
	CHECK(xwrite(fd, "hello tmp", 9) == 9 && sys3(8, fd, 0, 0) == 0);
	n = xread(fd, buf, sizeof(buf) - 1);
	buf[n > 0 ? n : 0] = 0;
	xclose(fd);
	CHECK(seq(buf, "hello tmp"));
	CHECK(sys3(83, "/home/user/ptdir", 0755, 0) == 0 &&
	      sys3(83, "/home/user/ptdir", 0755, 0) == -17);     /* EEXIST */
	CHECK(sys3(82, "/tmp/pt.txt", "/tmp/pt2.txt", 0) == 0 &&
	      sys3(2, "/tmp/pt.txt", 0, 0) == -2);
	CHECK(sys3(87, "/tmp/pt2.txt", 0, 0) == 0 && sys3(84, "/home/user/ptdir", 0, 0) == 0);

	/* 22-23: copy ourselves into /tmp and exec the copy */
	{
		static char img[8192];
		int in = (int)sys3(2, "/bin/proctest.linux", 0, 0), out;

		out = (int)sys3(2, "/tmp/pt.bin", 0x41 | 0x200, 0755);
		while ((n = xread(in, img, sizeof(img))) > 0)
			xwrite(out, img, (u64)n);
		xclose(in);
		xclose(out);
		CHECK(run_child("/tmp/pt.bin", buf, sizeof(buf), &st) == 0 && seq(buf, "exec-ok"));
		CHECK(st == (7 << 8) && sys3(87, "/tmp/pt.bin", 0, 0) == 0);
	}

	/* 24-25: 256 MiB of address space, backed only where touched */
	{
		u64 len = 256UL << 20;
		char *m = (char *)sys6(9, 0, (long)len, 3, 0x22, -1, 0);   /* RW, PRIVATE|ANON */

		CHECK((long)m > 0);
		m[0] = 1;
		m[len / 2] = 2;
		m[len - 1] = 3;
		CHECK(m[0] + m[len / 2] + m[len - 1] == 6 && sys3(11, m, len, 0) == 0);
	}

	puts1("proctest: OK (fork exec wait pipe dup2 vfork thread futex signals tmpfs mmap)\n");
	return 0;
}

__asm__(".text\n.globl _start\n_start:\n\tmov %rsp, %rdi\n\tand $-16, %rsp\n"
	"\tcall cmain\n\tmov %eax, %edi\n\tmov $231, %eax\n\tsyscall\n\thlt\n");
