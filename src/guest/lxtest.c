/* SPDX-License-Identifier: BSD-2-Clause */
/* Linux kernel interfaces Wine needs: mappings, COW fork, shared memory, sockets, epoll, files. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("lxtest: FAIL line %d: %s (errno %d)\n", __LINE__, #c, errno); fails++; } } while (0)

static void test_mmap_file(void)
{
	static unsigned char a[0x3000], *m;
	int fd = open("/lib/ld-musl-x86_64.so.1", O_RDONLY);

	if (fd < 0)
		fd = open("/lib64/ld-linux-x86-64.so.2", O_RDONLY);
	CHECK(fd >= 0);
	CHECK(pread(fd, a, sizeof(a), 0x1000) == sizeof(a));
	m = mmap(NULL, 0x3000, PROT_READ, MAP_PRIVATE, fd, 0x1000);
	CHECK(m != MAP_FAILED);
	CHECK(!memcmp(m, a, sizeof(a)));
	CHECK(mprotect(m + 0x1000, 0x1000, PROT_NONE) == 0);
	CHECK(!memcmp(m, a, 0x1000) && !memcmp(m + 0x2000, a + 0x2000, 0x1000));
	CHECK(mprotect(m + 0x1000, 0x1000, PROT_READ) == 0);
	CHECK(!memcmp(m + 0x1000, a + 0x1000, 0x1000));
	close(fd);
}

static volatile int got_segv;
static void on_segv(int s, siginfo_t *si, void *uc)
{
	(void)s; (void)uc;
	got_segv = (si->si_addr != NULL) + 1;
	mprotect((void *)((unsigned long)si->si_addr & ~0xfffUL), 4096, PROT_READ | PROT_WRITE);
}

static void test_protnone_signal(void)
{
	char *p = mmap(NULL, 1 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	struct sigaction sa = { .sa_sigaction = on_segv, .sa_flags = SA_SIGINFO };

	CHECK(p != MAP_FAILED);
	sigaction(SIGSEGV, &sa, NULL);
	p[0x2345] = 7;                          /* faults, handler unprotects, retried */
	CHECK(got_segv == 2 && p[0x2345] == 7);
	CHECK(munmap(p, 1 << 20) == 0);
	p = mmap(NULL, 1UL << 40, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	CHECK(p != MAP_FAILED);
}

static void test_cow_fork(void)
{
	char *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int st, pid;

	memset(p, 'a', 0x4000);
	pid = fork();
	if (pid == 0) {
		p[0] = 'b';
		_exit(p[0] == 'b' && p[0x3000] == 'a' ? 0 : 1);
	}
	CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	CHECK(p[0] == 'a');
}

static void test_shared(void)
{
	int fd = memfd_create("t", 0), st, pid;
	char *p;

	CHECK(fd >= 0 && ftruncate(fd, 0x2000) == 0);
	p = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(p != MAP_FAILED);
	pid = fork();
	if (pid == 0) {
		p[5] = 'x';
		_exit(0);
	}
	waitpid(pid, &st, 0);
	CHECK(p[5] == 'x');
	{
		char c = 0;
		CHECK(pread(fd, &c, 1, 5) == 1 && c == 'x');
	}
	p = mremap(p, 0x2000, 0x4000, MREMAP_MAYMOVE);
	CHECK(p != MAP_FAILED && p[5] == 'x');
}

static void test_sockets(void)
{
	int sv[2], ep, efd, lfd, cfd, afd, pfd[2];
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct epoll_event ev = { .events = EPOLLIN }, out;
	char buf[16], cm[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { buf, 1 };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cm, .msg_controllen = sizeof(cm) };
	struct cmsghdr *ch;

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	CHECK(write(sv[0], "hi", 2) == 2 && read(sv[1], buf, 16) == 2);
	/* descriptor passing */
	CHECK(pipe(pfd) == 0);
	memset(cm, 0, sizeof(cm));
	buf[0] = 'F';
	ch = CMSG_FIRSTHDR(&mh);
	ch->cmsg_level = SOL_SOCKET;
	ch->cmsg_type = SCM_RIGHTS;
	ch->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(ch), &pfd[1], sizeof(int));
	CHECK(sendmsg(sv[0], &mh, 0) == 1);
	memset(cm, 0, sizeof(cm));
	mh.msg_controllen = sizeof(cm);
	CHECK(recvmsg(sv[1], &mh, 0) == 1);
	ch = CMSG_FIRSTHDR(&mh);
	CHECK(ch && ch->cmsg_type == SCM_RIGHTS);
	if (ch) {
		int nfd;
		memcpy(&nfd, CMSG_DATA(ch), sizeof(int));
		CHECK(write(nfd, "z", 1) == 1 && read(pfd[0], buf, 1) == 1 && buf[0] == 'z');
	}
	/* named socket on ramfs, accept, epoll */
	strcpy(sa.sun_path, "/tmp/lxtest.sock");
	unlink(sa.sun_path);
	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(bind(lfd, (void *)&sa, sizeof(sa)) == 0 && listen(lfd, 4) == 0);
	cfd = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(connect(cfd, (void *)&sa, sizeof(sa)) == 0);
	ep = epoll_create1(0);
	ev.data.fd = lfd;
	CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev) == 0);
	CHECK(epoll_wait(ep, &out, 1, 1000) == 1 && out.data.fd == lfd);
	afd = accept(lfd, NULL, NULL);
	CHECK(afd >= 0);
	CHECK(write(cfd, "q", 1) == 1 && read(afd, buf, 1) == 1);
	close(cfd);
	CHECK(read(afd, buf, 1) == 0);          /* peer closed: end of file */
	efd = eventfd(0, EFD_NONBLOCK);
	CHECK(efd >= 0 && read(efd, buf, 8) < 0 && errno == EAGAIN);
	{
		unsigned long long v = 3;
		struct pollfd p = { efd, POLLIN, 0 };

		CHECK(write(efd, &v, 8) == 8 && poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
	}
	unlink(sa.sun_path);
}

static void test_files(void)
{
	char buf[64];
	struct stat s;
	int fd;

	CHECK(mkdir("/tmp/lxd", 0700) == 0);
	CHECK(chdir("/tmp/lxd") == 0 && getcwd(buf, sizeof(buf)) && !strcmp(buf, "/tmp/lxd"));
	fd = open("f", O_CREAT | O_RDWR, 0600);
	CHECK(fd >= 0 && pwrite(fd, "abc", 3, 2) == 3);
	CHECK(symlink("f", "l") == 0 && readlink("l", buf, sizeof(buf)) == 1);
	CHECK(stat("l", &s) == 0 && S_ISREG(s.st_mode) && s.st_size == 5);
	CHECK(lstat("l", &s) == 0 && S_ISLNK(s.st_mode));
	CHECK(stat("/tmp/lxd", &s) == 0 && S_ISDIR(s.st_mode) && (s.st_mode & 0777) == 0700);
	close(fd);
	fd = open("/dev/null", O_RDWR);
	CHECK(fd >= 0 && write(fd, "x", 1) == 1);
	CHECK(open("/proc/self/maps", O_RDONLY) >= 0);
	chdir("/");
	unlink("/tmp/lxd/l");
	unlink("/tmp/lxd/f");
	CHECK(rmdir("/tmp/lxd") == 0);
}

int main(void)
{
	test_mmap_file();
	test_protnone_signal();
	test_cow_fork();
	test_shared();
	test_sockets();
	test_files();
	if (fails) {
		printf("lxtest: %d failure(s)\n", fails);
		return 1;
	}
	printf("lxtest: OK (mmap mprotect PROT_NONE+SIGSEGV COW memfd+MAP_SHARED mremap AF_UNIX scm_rights epoll eventfd files)\n");
	return 0;
}
