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

	.section .note.GNU-stack, "", @progbits
