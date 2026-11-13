/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Native kernel services ("msys") - the single implementation every ABI
 * personality is translated onto - plus checked user-memory access.
 */
#include <mergedos/kernel.h>
#include <mergedos/console.h>
#include <mergedos/cpu.h>
#include <mergedos/initrd.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/mm.h>
#include <mergedos/task.h>

#define MSYS_WRITE_MAX (1u << 20)

/* C side of SYSCALL: called from entry.S with the saved register frame. */
void syscall_handler(struct syscall_frame *f)
{
	struct task *t = task_current();

	if (!t)
		panic("SYSCALL from ring 3 with no current task");
	abi_dispatch(&t->abi, f);
	if (t->exited)
		user_exit_resume(t->exit_code);         /* abandon the user context */
}

/*
 * User pointers are validated against the task's own page tables before
 * they are touched, so a bad pointer becomes an error return in the
 * caller's dialect (EFAULT / STATUS_ACCESS_VIOLATION), never a kernel fault.
 * The task's address space is the active one while it is in a system call.
 */
int uaccess_copy_in(struct task *t, void *dst, u64 uaddr, size_t len)
{
	if (!vmm_user_ok(t->pml4, uaddr, len, false))
		return -MERR_FAULT;
	memcpy(dst, (const void *)uaddr, len);
	return 0;
}

int uaccess_copy_out(struct task *t, u64 uaddr, const void *src, size_t len)
{
	if (!vmm_user_ok(t->pml4, uaddr, len, true))
		return -MERR_FAULT;
	memcpy((void *)uaddr, src, len);
	return 0;
}

long msys_write(struct task *t, int fd, u64 ubuf, u64 len)
{
	char chunk[128];
	u64 done = 0;

	if (fd != 1 && fd != 2)
		return -MERR_BADF;
	if (len > MSYS_WRITE_MAX)
		len = MSYS_WRITE_MAX;

	while (done < len) {
		size_t n = (size_t)MOS_MIN(len - done, sizeof(chunk));

		if (uaccess_copy_in(t, chunk, ubuf + done, n))
			return done ? (long)done : -MERR_FAULT;
		console_write(chunk, n);
		if (t->out_len < sizeof(t->out)) {
			size_t keep = MOS_MIN(n, sizeof(t->out) - t->out_len);

			memcpy(t->out + t->out_len, chunk, keep);
			t->out_len += (u32)keep;
		}
		done += n;
	}
	return (long)done;
}

long msys_getpid(struct task *t)
{
	return t->pid;
}

void msys_exit(struct task *t, int status)
{
	t->exited = true;
	t->exit_code = status;
}

/* ---- files: descriptors over the read-only ramdisk --------------------------- */

static long copy_path(struct task *t, u64 upath, char *buf, size_t cap)
{
	for (size_t i = 0; i < cap; i++) {
		if (uaccess_copy_in(t, &buf[i], upath + i, 1))
			return -MERR_FAULT;
		if (!buf[i])
			return 0;
	}
	return -MERR_RANGE;
}

static struct task_file *file_of(struct task *t, int fd)
{
	return fd >= 3 && fd < TASK_FD_MAX && t->file[fd].used ? &t->file[fd] : NULL;
}

long msys_open(struct task *t, u64 upath, bool for_writing)
{
	char path[128];
	const u8 *data;
	size_t size;
	long rc = copy_path(t, upath, path, sizeof(path));

	if (rc)
		return rc;
	if (initrd_find(path, &data, &size))
		return -MERR_NOENT;
	if (for_writing)
		return -MERR_ACCES;             /* the ramdisk cannot be written */
	for (int fd = 3; fd < TASK_FD_MAX; fd++) {
		if (!t->file[fd].used) {
			t->file[fd] = (struct task_file){ data, size, 0, true };
			return fd;
		}
	}
	return -MERR_AGAIN;                     /* descriptor table full */
}

long msys_read(struct task *t, int fd, u64 ubuf, u64 len)
{
	struct task_file *f = file_of(t, fd);
	u64 n;

	if (fd == 0)
		return 0;                       /* no interactive stdin yet: end of file */
	if (!f)
		return -MERR_BADF;
	n = MOS_MIN(len, f->size - f->pos);
	if (n && uaccess_copy_out(t, ubuf, f->data + f->pos, (size_t)n))
		return -MERR_FAULT;
	f->pos += n;
	return (long)n;
}

long msys_close(struct task *t, int fd)
{
	struct task_file *f = file_of(t, fd);

	if (fd >= 0 && fd < 3)
		return 0;
	if (!f)
		return -MERR_BADF;
	f->used = false;
	return 0;
}

long msys_lseek(struct task *t, int fd, s64 offset, int whence)
{
	struct task_file *f = file_of(t, fd);
	s64 base;

	if (!f)
		return -MERR_BADF;
	base = whence == 0 ? 0 : whence == 1 ? (s64)f->pos : whence == 2 ? (s64)f->size : -1;
	if (base < 0 || base + offset < 0 || (u64)(base + offset) > f->size)
		return -MERR_INVAL;
	f->pos = (u64)(base + offset);
	return (long)f->pos;
}

long msys_fstat(struct task *t, int fd, u64 *size, bool *is_tty)
{
	struct task_file *f = file_of(t, fd);

	if (fd >= 0 && fd < 3) {
		*size = 0;
		*is_tty = true;
		return 0;
	}
	if (!f)
		return -MERR_BADF;
	*size = f->size;
	*is_tty = false;
	return 0;
}

long msys_stat(struct task *t, u64 upath, u64 *size, bool *is_dir)
{
	char path[128];
	const u8 *data;
	size_t n;
	long rc = copy_path(t, upath, path, sizeof(path));

	if (rc)
		return rc;
	*is_dir = false;
	if (!initrd_find(path, &data, &n)) {
		*size = n;
		return 0;
	}
	*size = 0;
	*is_dir = initrd_is_dir(path);
	return *is_dir ? 0 : -MERR_NOENT;
}

/* ---- memory -------------------------------------------------------------------- */

/* Map zeroed, writable pages for [from, to) in the calling task. */
static bool grow(struct task *t, u64 from, u64 to)
{
	for (u64 page = ALIGN_UP(from, PAGE_SIZE); page < to; page += PAGE_SIZE) {
		if (t->pages >= TASK_MAX_PAGES || !vmm_map_user(t->pml4, page, VMM_R | VMM_W))
			return false;
		t->pages++;
	}
	return true;
}

long msys_brk(struct task *t, u64 addr)
{
	/* Linux semantics: on any failure the old break is returned unchanged. */
	if (addr > t->brk && addr < t->brk_start + TASK_MAX_PAGES * PAGE_SIZE && grow(t, t->brk, addr))
		t->brk = addr;
	else if (addr >= t->brk_start && addr <= t->brk)
		t->brk = addr;                  /* shrinking keeps the pages until exit */
	return (long)t->brk;
}

long msys_mmap_anon(struct task *t, u64 len)
{
	u64 addr = t->mmap_next;

	len = ALIGN_UP(len, PAGE_SIZE);
	if (!len || len > TASK_MAX_PAGES * PAGE_SIZE || !grow(t, addr, addr + len))
		return -MERR_NOMEM;
	t->mmap_next = addr + len + PAGE_SIZE;  /* leave an unmapped guard page between mappings */
	return (long)addr;
}

long msys_set_fs_base(struct task *t, u64 base)
{
	(void)t;
	if (base >= USER_TOP)
		return -MERR_PERM;
	wrmsr(0xC0000100, base);                /* IA32_FS_BASE */
	return 0;
}

long msys_protect(struct task *t, u64 addr, u64 len, u32 prot)
{
	u64 end = addr + len, page;

	if (!len || end < addr || end > USER_TOP)
		return -MERR_INVAL;
	for (page = ALIGN_DOWN(addr, PAGE_SIZE); page < end; page += PAGE_SIZE)
		if (!vmm_user_to_phys(t->pml4, page))
			return -MERR_FAULT;     /* all or nothing */
	for (page = ALIGN_DOWN(addr, PAGE_SIZE); page < end; page += PAGE_SIZE) {
		vmm_map_user(t->pml4, page, VMM_R | (prot & MSYS_PROT_W ? VMM_W : 0) |
					    (prot & MSYS_PROT_X ? VMM_X : 0));
		__asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
	}
	return 0;
}

u64 msys_clock_ns(void)
{
	return time_ticks() * (1000000000ULL / SCHED_HZ);
}
