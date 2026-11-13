/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Windows NT x64 personality.
 *
 *   number : eax = service index (bits 0..11) | table (bits 12..13)
 *   args   : r10, rdx, r8, r9, then the caller's stack. The stub in ntdll is
 *            reached by an ordinary CALL, so at the SYSCALL instruction
 *            [rsp] is a return address, [rsp+8..rsp+0x27] is the home
 *            ("shadow") space and the fifth argument sits at [rsp+0x28].
 *   result : NTSTATUS in eax. No errno, no carry flag.
 *
 * Services: NtReadFile 0x06, NtWriteFile 0x08, NtAllocateVirtualMemory 0x18,
 * NtFreeVirtualMemory 0x1E, NtTerminateProcess 0x2C, NtQueryPerformanceCounter
 * 0x31, NtProtectVirtualMemory 0x50, NtQuerySystemTime 0x5A; threads:
 * NtWaitForSingleObject 0x04, NtClose 0x0F, NtTerminateThread 0x53 and
 * NtCreateThreadEx 0xC2. The built-in
 * Win32 runtime (src/ntrt/start.S) is the ntdll that calls them.
 *
 * Unlike the UNIX personalities, NT service numbers are NOT a stable ABI:
 * Microsoft renumbers them between builds, and applications only ever call
 * ntdll.dll. The stable contract is therefore "the numbers MergedOS's own
 * ntdll uses". We pin the handful below to their long-standing Windows
 * 10/11 x64 values; everything above this layer (kernel32, user32, ...)
 * must come from a userland reimplementation - see docs/ABI_TRANSLATION.md.
 */
#include "abi_dispatch/abi.h"

#define NT_SVC_READ_FILE                0x06
#define NT_SVC_WRITE_FILE               0x08
#define NT_SVC_ALLOCATE_VIRTUAL_MEMORY  0x18
#define NT_SVC_FREE_VIRTUAL_MEMORY      0x1E
#define NT_SVC_TERMINATE_PROCESS        0x2C
#define NT_SVC_QUERY_PERFORMANCE_COUNTER 0x31
#define NT_SVC_PROTECT_VIRTUAL_MEMORY   0x50
#define NT_SVC_QUERY_SYSTEM_TIME        0x5A
#define NT_SVC_WAIT_FOR_SINGLE_OBJECT   0x04
#define NT_SVC_CLOSE                    0x0F
#define NT_SVC_TERMINATE_THREAD         0x53
#define NT_SVC_CREATE_THREAD_EX         0xC2

#define NT_FILETIME_UNIX_EPOCH   116444736000000000ULL  /* 1970-01-01 in 100 ns units */

#define NT_STACK_ARGS_OFFSET     0x28   /* return address + 32-byte home space */

/*
 * NT handles are opaque multiples of four. The PoC handle table maps the
 * three standard handles straight onto native descriptors:
 *   0x4 -> fd 0 (stdin), 0x8 -> fd 1 (stdout), 0xC -> fd 2 (stderr)
 */
static int nt_handle_to_fd(u64 handle)
{
	if (handle == 0 || (handle & 3) || handle > 0xC)
		return -1;
	return (int)(handle >> 2) - 1;
}

/*
 * NTSTATUS NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
 *                      PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
 *                      PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
 *                      PULONG Key);
 *
 * Nine arguments against POSIX write()'s three: four in registers, five
 * fetched from the caller's stack, and the byte count goes back through an
 * out-structure instead of the return value.
 */
static long nt_write_file(struct abi_ctx *ctx, const struct abi_call *c)
{
	struct {
		u64 io_status_block, buffer, length, byte_offset, key;
	} stk;
	struct {
		u64 status, information;
	} iosb;
	long n;
	int fd;

	if (uaccess_copy_in(ctx->task, &stk, c->usp + NT_STACK_ARGS_OFFSET, sizeof(stk)))
		return -MERR_FAULT;
	fd = nt_handle_to_fd(c->arg[0]);
	if (fd < 0)
		return -MERR_BADF;

	n = msys_write(ctx->task, fd, stk.buffer, (u32)stk.length);
	if (n < 0)
		return n;

	iosb.status = NT_STATUS_SUCCESS;
	iosb.information = (u64)n;
	if (stk.io_status_block &&
	    uaccess_copy_out(ctx->task, stk.io_status_block, &iosb, sizeof(iosb)))
		return -MERR_FAULT;
	return 0;
}

/*
 * NTSTATUS NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
 *                     PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
 *                     PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
 *                     PULONG Key);
 * Same shape as NtWriteFile.
 */
static long nt_read_file(struct abi_ctx *ctx, const struct abi_call *c)
{
	struct {
		u64 io_status_block, buffer, length, byte_offset, key;
	} stk;
	u64 iosb[2];
	long n;
	int fd;

	if (uaccess_copy_in(ctx->task, &stk, c->usp + NT_STACK_ARGS_OFFSET, sizeof(stk)))
		return -MERR_FAULT;
	fd = nt_handle_to_fd(c->arg[0]);
	if (fd < 0)
		return -MERR_BADF;
	n = msys_read(ctx->task, fd, stk.buffer, (u32)stk.length);
	if (n < 0)
		return n;
	iosb[0] = NT_STATUS_SUCCESS;
	iosb[1] = (u64)n;
	if (stk.io_status_block && uaccess_copy_out(ctx->task, stk.io_status_block, iosb, sizeof(iosb)))
		return -MERR_FAULT;
	return 0;
}

static bool nt_self(u64 process)
{
	return process == 0 || process == ~0ULL;       /* NtCurrentProcess() */
}

/* PAGE_* protection constant -> native rights. */
static u32 nt_prot(u64 prot)
{
	return ((prot & 0xCC) ? MSYS_PROT_W : 0) | ((prot & 0xF0) ? MSYS_PROT_X : 0);
}

/*
 * NTSTATUS NtAllocateVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
 *                                  ULONG_PTR ZeroBits, PSIZE_T RegionSize,
 *                                  ULONG AllocationType, ULONG Protect);
 * Reserving also commits (pages are mapped at once); a later MEM_COMMIT
 * inside such a region therefore just succeeds.
 */
static long nt_allocate_virtual_memory(struct abi_ctx *ctx, const struct abi_call *c)
{
	struct {
		u64 type, protect;
	} stk;
	u64 base, size;
	long addr;

	if (!nt_self(c->arg[0]))
		return -MERR_BADF;
	if (uaccess_copy_in(ctx->task, &stk, c->usp + NT_STACK_ARGS_OFFSET, sizeof(stk)) ||
	    uaccess_copy_in(ctx->task, &base, c->arg[1], 8) ||
	    uaccess_copy_in(ctx->task, &size, c->arg[3], 8))
		return -MERR_FAULT;
	if (size == 0)
		return -MERR_INVAL;
	if (base)
		return msys_protect(ctx->task, base, size, nt_prot(stk.protect | 0x04));
	addr = msys_mmap_anon(ctx->task, size);
	if (addr < 0)
		return addr;
	base = (u64)addr;
	size = ALIGN_UP(size, PAGE_SIZE);
	if (uaccess_copy_out(ctx->task, c->arg[1], &base, 8) ||
	    uaccess_copy_out(ctx->task, c->arg[3], &size, 8))
		return -MERR_FAULT;
	return 0;
}

/* Pages stay mapped until the process exits: freeing is accepted, not done. */
static long nt_free_virtual_memory(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	return nt_self(c->arg[0]) ? 0 : -MERR_BADF;
}

/*
 * NTSTATUS NtProtectVirtualMemory(HANDLE ProcessHandle, PVOID *BaseAddress,
 *                                 PSIZE_T RegionSize, ULONG NewProtect,
 *                                 PULONG OldProtect);
 * Rights are only ever added, so "old" reports the new protection.
 */
static long nt_protect_virtual_memory(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 base, size, old_ptr;
	u32 old = (u32)c->arg[3];
	long rc;

	if (!nt_self(c->arg[0]))
		return -MERR_BADF;
	if (uaccess_copy_in(ctx->task, &old_ptr, c->usp + NT_STACK_ARGS_OFFSET, 8) ||
	    uaccess_copy_in(ctx->task, &base, c->arg[1], 8) ||
	    uaccess_copy_in(ctx->task, &size, c->arg[2], 8))
		return -MERR_FAULT;
	rc = msys_protect(ctx->task, base, size, nt_prot(c->arg[3]));
	if (rc)
		return rc;
	if (old_ptr && uaccess_copy_out(ctx->task, old_ptr, &old, 4))
		return -MERR_FAULT;
	return 0;
}

/* NTSTATUS NtQueryPerformanceCounter(PLARGE_INTEGER Counter, PLARGE_INTEGER Frequency); */
static long nt_query_performance_counter(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 counter = msys_clock_ns() / 100, freq = 10000000;

	if (uaccess_copy_out(ctx->task, c->arg[0], &counter, 8) ||
	    (c->arg[1] && uaccess_copy_out(ctx->task, c->arg[1], &freq, 8)))
		return -MERR_FAULT;
	return 0;
}

/*
 * NTSTATUS NtQuerySystemTime(PLARGE_INTEGER SystemTime);
 * There is no wall clock yet: like the UNIX personalities, time starts at
 * the epoch when the machine boots.
 */
static long nt_query_system_time(struct abi_ctx *ctx, const struct abi_call *c)
{
	u64 t = NT_FILETIME_UNIX_EPOCH + msys_clock_ns() / 100;

	return uaccess_copy_out(ctx->task, c->arg[0], &t, 8) ? -MERR_FAULT : 0;
}

/* NTSTATUS NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus); */
static long nt_terminate_process(struct abi_ctx *ctx, const struct abi_call *c)
{
	/* NULL and NtCurrentProcess() (-1) both mean "this process". */
	if (c->arg[0] != 0 && c->arg[0] != ~0ULL)
		return -MERR_BADF;
	msys_exit(ctx->task, (int)(u32)c->arg[1]);
	return 0;
}

/*
 * Threads. A thread handle is NT_THREAD_HANDLE + 4 * thread id; the thread
 * starts at StartRoutine(Argument) and, when it returns, lands on the
 * kernel-installed thunk that calls NtTerminateThread with its result.
 */
#define NT_THREAD_HANDLE      0x40000ULL
#define NT_THREAD_EXIT_THUNK  0x7FFE1020ULL     /* second thunk in the NT thunk page */
#define NT_CREATE_SUSPENDED   0x1

static int nt_thread_id(u64 handle)
{
	if (handle <= NT_THREAD_HANDLE || (handle & 3) || handle > NT_THREAD_HANDLE + 0x7FFFFFFCULL)
		return -1;
	return (int)((handle - NT_THREAD_HANDLE) >> 2);
}

/*
 * NTSTATUS NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess,
 *     POBJECT_ATTRIBUTES ObjectAttributes, HANDLE ProcessHandle, PVOID StartRoutine,
 *     PVOID Argument, ULONG CreateFlags, SIZE_T ZeroBits, SIZE_T StackSize,
 *     SIZE_T MaximumStackSize, PPS_ATTRIBUTE_LIST AttributeList);
 */
static long nt_create_thread_ex(struct abi_ctx *ctx, const struct abi_call *c)
{
	struct {
		u64 start, arg, flags, zero_bits, stack_size, max_stack, attrs;
	} stk;
	u64 handle;
	long tid;

	if (uaccess_copy_in(ctx->task, &stk, c->usp + NT_STACK_ARGS_OFFSET, sizeof(stk)))
		return -MERR_FAULT;
	if (!nt_self(c->arg[3]))
		return -MERR_BADF;
	if (stk.flags & NT_CREATE_SUSPENDED)
		return -MERR_NOTSUP;
	tid = msys_thread_spawn(ctx->task, stk.start, stk.arg,
				MOS_MAX(stk.stack_size, stk.max_stack), NT_THREAD_EXIT_THUNK);
	if (tid < 0)
		return tid;
	handle = NT_THREAD_HANDLE + ((u64)tid << 2);
	return uaccess_copy_out(ctx->task, c->arg[0], &handle, sizeof(handle));
}

/* NTSTATUS NtTerminateThread(HANDLE ThreadHandle, NTSTATUS ExitStatus); */
static long nt_terminate_thread(struct abi_ctx *ctx, const struct abi_call *c)
{
	if (c->arg[0] != 0 && c->arg[0] != (u64)-2)    /* NtCurrentThread() */
		return -MERR_NOTSUP;
	return msys_thread_exit(ctx->task, (int)(u32)c->arg[1]);
}

/* NTSTATUS NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout); */
static long nt_wait_for_single_object(struct abi_ctx *ctx, const struct abi_call *c)
{
	int tid = nt_thread_id(c->arg[0]);
	u64 timeout_ns = ~0ULL;

	if (tid < 0)
		return -MERR_BADF;
	if (c->arg[2]) {
		s64 t;

		if (uaccess_copy_in(ctx->task, &t, c->arg[2], sizeof(t)))
			return -MERR_FAULT;
		timeout_ns = t < 0 ? (u64)-t * 100 : 0;    /* relative 100 ns units */
	}
	return msys_thread_join(ctx->task, tid, timeout_ns);
}

/* NTSTATUS NtClose(HANDLE Handle); */
static long nt_close(struct abi_ctx *ctx, const struct abi_call *c)
{
	(void)ctx;
	return nt_thread_id(c->arg[0]) >= 0 || nt_handle_to_fd(c->arg[0]) >= 0 ? 0 : -MERR_BADF;
}

static const struct abi_sysent nt_sysent[] = {
	[NT_SVC_WAIT_FOR_SINGLE_OBJECT]    = { nt_wait_for_single_object,  "NtWaitForSingleObject" },
	[NT_SVC_CLOSE]                     = { nt_close,                   "NtClose" },
	[NT_SVC_TERMINATE_THREAD]          = { nt_terminate_thread,        "NtTerminateThread" },
	[NT_SVC_CREATE_THREAD_EX]          = { nt_create_thread_ex,        "NtCreateThreadEx" },
	[NT_SVC_READ_FILE]                 = { nt_read_file,               "NtReadFile" },
	[NT_SVC_WRITE_FILE]                = { nt_write_file,              "NtWriteFile" },
	[NT_SVC_ALLOCATE_VIRTUAL_MEMORY]   = { nt_allocate_virtual_memory, "NtAllocateVirtualMemory" },
	[NT_SVC_FREE_VIRTUAL_MEMORY]       = { nt_free_virtual_memory,     "NtFreeVirtualMemory" },
	[NT_SVC_TERMINATE_PROCESS]         = { nt_terminate_process,       "NtTerminateProcess" },
	[NT_SVC_QUERY_PERFORMANCE_COUNTER] = { nt_query_performance_counter, "NtQueryPerformanceCounter" },
	[NT_SVC_PROTECT_VIRTUAL_MEMORY]    = { nt_protect_virtual_memory,  "NtProtectVirtualMemory" },
	[NT_SVC_QUERY_SYSTEM_TIME]         = { nt_query_system_time,       "NtQuerySystemTime" },
};

static void nt_decode(const struct syscall_frame *f, struct abi_call *c)
{
	c->nr = f->rax & 0xFFF;
	c->klass = (u32)((f->rax >> 12) & 0x3);       /* 0 = executive, 1 = win32k */
	c->arg[0] = f->r10;                             /* rcx is destroyed by SYSCALL */
	c->arg[1] = f->rdx;
	c->arg[2] = f->r8;
	c->arg[3] = f->r9;
	c->arg[4] = 0;                                  /* remaining args live at usp */
	c->arg[5] = 0;
	c->usp = f->rsp;
}

static const struct abi_sysent *nt_lookup(const struct abi_call *c)
{
	if (c->klass == 0 && c->nr < ARRAY_SIZE(nt_sysent) && nt_sysent[c->nr].fn)
		return &nt_sysent[c->nr];
	return NULL;
}

static void nt_encode(struct syscall_frame *f, const struct abi_call *c, long ret)
{
	(void)c;
	/* NTSTATUS is a signed 32-bit value; sign-extend it like a C return would. */
	f->rax = (u64)(s64)(s32)abi_ntstatus(ret < 0 ? (int)-ret : 0);
}

const struct abi_personality_ops abi_nt_ops = {
	.name   = "nt",
	.decode = nt_decode,
	.lookup = nt_lookup,
	.encode = nt_encode,
};
