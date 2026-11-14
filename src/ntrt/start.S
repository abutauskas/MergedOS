/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ntdll-style system-call stubs of the built-in Win32 runtime.
 *
 * Each stub is reached by an ordinary ms_abi CALL, so at the SYSCALL the
 * stack already has the layout the NT personality decodes: return address,
 * 32 bytes of home space, then arguments five and up. rcx is moved to r10
 * because SYSCALL overwrites rcx with the return address.
 */
	.text

.macro NTSTUB name, num
	.globl  \name
	.type   \name, @function
\name:
	movq    %rcx, %r10
	movl    $\num, %eax
	syscall
	ret
.endm

	NTSTUB  NtReadFile,                0x06
	NTSTUB  NtWriteFile,               0x08
	NTSTUB  NtAllocateVirtualMemory,   0x18
	NTSTUB  NtFreeVirtualMemory,       0x1E
	NTSTUB  NtTerminateProcess,        0x2C
	NTSTUB  NtQueryPerformanceCounter, 0x31
	NTSTUB  NtProtectVirtualMemory,    0x50
	NTSTUB  NtQuerySystemTime,         0x5A
	NTSTUB  NtCreateThreadEx,          0xC2
	NTSTUB  NtWaitForSingleObject,     0x04
	NTSTUB  NtTerminateThread,         0x53

/* MergedOS services, table 2 (see abi_dispatch/personality_nt.c). */
	NTSTUB  mos_open,    0x2000
	NTSTUB  mos_close,   0x2001
	NTSTUB  mos_read,    0x2002
	NTSTUB  mos_write,   0x2003
	NTSTUB  mos_seek,    0x2004
	NTSTUB  mos_fstat,   0x2005
	NTSTUB  mos_stat,    0x2006
	NTSTUB  mos_readdir, 0x2007
	NTSTUB  mos_mkdir,   0x2008
	NTSTUB  mos_unlink,  0x2009
	NTSTUB  mos_rename,  0x200A
	NTSTUB  mos_futex,   0x200B
	NTSTUB  mos_sleep,   0x200C
	NTSTUB  mos_dup,     0x200D
	NTSTUB  mos_gettid,  0x200E

	.section .note.GNU-stack, "", @progbits
