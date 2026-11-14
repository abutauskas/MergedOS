/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * hello.exe - a Windows x64 PE32+ console executable, headers written out
 * by hand (no MinGW or Microsoft toolchain needed to build it).
 *
 * It has no import table: instead of calling kernel32/ntdll it carries its
 * own copies of the two ntdll system-call stubs it needs, exactly as ntdll
 * implements them (mov r10, rcx / mov eax, <service> / syscall / ret), and
 * calls them with the Microsoft x64 calling convention. That exercises the
 * NT personality's argument marshalling - NtWriteFile takes nine arguments,
 * five of them on the stack - without needing a DLL loader yet.
 *
 * Exit status = number of the first failed step:
 *   1  NtWriteFile returns STATUS_SUCCESS
 *   2  IO_STATUS_BLOCK.Information == bytes written
 *   3  bad handle        -> STATUS_INVALID_HANDLE         (0xC0000008)
 *   4  unknown service   -> STATUS_INVALID_SYSTEM_SERVICE (0xC000001C)
 *   5  NtCreateThreadEx starts a thread (eleven arguments, seven on the stack)
 *   6  NtWaitForSingleObject joins it; the thread's write is visible; NtClose
 * On success the entry point *returns* 0, like a real main(); the process
 * then ends through the thread-exit thunk the NT personality installs.
 */
	.set    IMAGE_BASE, 0x140000000
	.set    SECT_ALIGN, 0x1000
	.set    FILE_ALIGN, 0x200
	.set    TEXT_RVA,   0x1000

	.text
	.code64
mz:
	.ascii  "MZ"
	.fill   58, 1, 0
	.long   pe - mz                         /* e_lfanew at 0x3C */
pe:
	.ascii  "PE\0\0"
	.word   0x8664                          /* IMAGE_FILE_MACHINE_AMD64 */
	.word   1                               /* NumberOfSections         */
	.long   0, 0, 0                         /* timestamp, symbols       */
	.word   opt_end - opt                   /* SizeOfOptionalHeader     */
	.word   0x0022                          /* EXECUTABLE | LARGE_ADDRESS_AWARE */
opt:
	.word   0x020B                          /* PE32+                    */
	.byte   14, 0                           /* linker version           */
	.long   text_raw_size, 0, 0             /* SizeOfCode / data / bss  */
	.long   TEXT_RVA + (entry - text)       /* AddressOfEntryPoint      */
	.long   TEXT_RVA                        /* BaseOfCode               */
	.quad   IMAGE_BASE
	.long   SECT_ALIGN, FILE_ALIGN
	.word   6, 0, 0, 0, 6, 0                /* OS / image / subsystem versions */
	.long   0                               /* Win32VersionValue        */
	.long   TEXT_RVA + SECT_ALIGN           /* SizeOfImage              */
	.long   FILE_ALIGN                      /* SizeOfHeaders            */
	.long   0                               /* CheckSum                 */
	.word   3                               /* IMAGE_SUBSYSTEM_WINDOWS_CUI */
	.word   0x0100                          /* NX_COMPAT                */
	.quad   0x100000, 0x1000                /* stack reserve / commit   */
	.quad   0x100000, 0x1000                /* heap reserve / commit    */
	.long   0                               /* LoaderFlags              */
	.long   16                              /* NumberOfRvaAndSizes      */
	.fill   16 * 8, 1, 0                    /* data directories: all empty */
opt_end:
	.ascii  ".text\0\0\0"
	.long   text_end - text                 /* VirtualSize      */
	.long   TEXT_RVA                        /* VirtualAddress   */
	.long   text_raw_size                   /* SizeOfRawData    */
	.long   text - mz                       /* PointerToRawData */
	.long   0, 0
	.word   0, 0
	.long   0x60000020                      /* CODE | EXECUTE | READ */

	.balign FILE_ALIGN, 0
text:

/* ---- minimal "ntdll": system call stubs, Microsoft x64 convention --------- */
NtWriteFile:
	movq    %rcx, %r10
	movl    $0x08, %eax
	syscall
	ret
NtTerminateProcess:
	movq    %rcx, %r10
	movl    $0x2C, %eax
	syscall
	ret
NtBogusService:
	movq    %rcx, %r10
	movl    $0xFFF, %eax
	syscall
	ret
NtCreateThreadEx:
	movq    %rcx, %r10
	movl    $0xC2, %eax
	syscall
	ret
NtWaitForSingleObject:
	movq    %rcx, %r10
	movl    $0x04, %eax
	syscall
	ret
NtClose:
	movq    %rcx, %r10
	movl    $0x0F, %eax
	syscall
	ret

/* Thread start routine: store a marker through its argument, return 0. */
thread_fn:
	movq    $0x7E57, (%rcx)
	xorl    %eax, %eax
	ret

/* Fill in the five stack arguments of NtWriteFile(handle=rcx, ...). */
.macro WRITE_ARGS len
	xorl    %edx, %edx                      /* Event          */
	xorl    %r8d, %r8d                      /* ApcRoutine     */
	xorl    %r9d, %r9d                      /* ApcContext     */
	leaq    0x50(%rsp), %rax
	movq    %rax, 0x20(%rsp)                /* IoStatusBlock  */
	leaq    msg(%rip), %rax
	movq    %rax, 0x28(%rsp)                /* Buffer         */
	movq    $\len, 0x30(%rsp)               /* Length         */
	movq    $0, 0x38(%rsp)                  /* ByteOffset     */
	movq    $0, 0x40(%rsp)                  /* Key            */
.endm

/* ---- entry point: int mainCRTStartup(void) -------------------------------- */
entry:
	subq    $0x68, %rsp                     /* home space + 5 stack args + IOSB */

	movl    $1, %ebx                        /* step 1: write to stdout (handle 8) */
	movl    $8, %ecx
	WRITE_ARGS msg_len
	call    NtWriteFile
	testl   %eax, %eax
	jnz     fail

	movl    $2, %ebx                        /* step 2: byte count via IO_STATUS_BLOCK */
	cmpq    $msg_len, 0x58(%rsp)
	jne     fail

	movl    $3, %ebx                        /* step 3: NTSTATUS, not errno */
	movl    $0x1234, %ecx
	WRITE_ARGS 1
	call    NtWriteFile
	cmpl    $0xC0000008, %eax               /* STATUS_INVALID_HANDLE */
	jne     fail

	movl    $4, %ebx                        /* step 4: unknown service index */
	call    NtBogusService
	cmpl    $0xC000001C, %eax               /* STATUS_INVALID_SYSTEM_SERVICE */
	jne     fail

	movl    $5, %ebx                        /* step 5: NtCreateThreadEx, 11 arguments */
	movq    $0, 0x60(%rsp)                  /* marker the thread sets */
	leaq    0x58(%rsp), %rcx                /* ThreadHandle (out)     */
	movl    $0x1FFFFF, %edx                 /* THREAD_ALL_ACCESS      */
	xorl    %r8d, %r8d                      /* ObjectAttributes       */
	movq    $-1, %r9                        /* NtCurrentProcess()     */
	leaq    thread_fn(%rip), %rax
	movq    %rax, 0x20(%rsp)                /* StartRoutine           */
	leaq    0x60(%rsp), %rax
	movq    %rax, 0x28(%rsp)                /* Argument               */
	movq    $0, 0x30(%rsp)                  /* CreateFlags            */
	movq    $0, 0x38(%rsp)                  /* ZeroBits               */
	movq    $0, 0x40(%rsp)                  /* StackSize              */
	movq    $0, 0x48(%rsp)                  /* MaximumStackSize       */
	movq    $0, 0x50(%rsp)                  /* AttributeList          */
	call    NtCreateThreadEx
	testl   %eax, %eax
	jnz     fail

	movl    $6, %ebx                        /* step 6: join it, see its work, close it */
	movq    0x58(%rsp), %rcx
	xorl    %edx, %edx                      /* not alertable */
	xorl    %r8d, %r8d                      /* no timeout    */
	call    NtWaitForSingleObject
	testl   %eax, %eax
	jnz     fail
	cmpq    $0x7E57, 0x60(%rsp)
	jne     fail
	movq    0x58(%rsp), %rcx
	call    NtClose
	testl   %eax, %eax
	jnz     fail

	xorl    %eax, %eax                     /* return 0 from the entry point */
	addq    $0x68, %rsp
	ret
fail:
	movq    $-1, %rcx                       /* NtCurrentProcess() */
	movl    %ebx, %edx
	call    NtTerminateProcess
	ud2

msg:
	.ascii  "Hello from a Windows PE32+ binary: NtWriteFile=0x08, 9 arguments, NTSTATUS result!\n"
	.set    msg_len, . - msg
text_end:
	.balign FILE_ALIGN, 0
file_end:
	.set    text_raw_size, file_end - text
