# NT user space: DLL loader, ntdll, Wine's DLLs

Short specs written before the code; the code follows them. Everything here
runs in ring 3 inside the NT process, in MergedOS's own runtime
(`src/ntrt/`, "ntdll"). The kernel only maps the program image and the
runtime and offers a handful of descriptor-level services.

## 0. Delivery of Wine's DLLs

- `make wine` runs `apt-get download libwine`, unpacks it with `dpkg-deb -x`
  into its own empty directory, and copies a fixed list of x86_64-windows
  PE files (debug sections stripped) and NLS tables into
  `build/wine/wine.tar` as `windows/system32/*`. Nothing is compiled in,
  committed or executed on the host.
- QEMU gets the archive as a Multiboot module (`-initrd`). The kernel keeps
  it where the loader put it, reserves its frames (reported separately, not
  in the kernel footprint) and searches it after the ramdisk, read-only.
- `C:\` is the VFS root, so `C:\windows\system32\kernel32.dll` is the module
  file `windows/system32/kernel32.dll`.
- Tests that need Wine skip with a note when the archive is missing.

## 1. DLL loader (`src/ntrt/ldr.c`)

- The kernel no longer binds imports. It maps the image (relocated if
  needed), the runtime and the parameters page, then starts the runtime.
- Module list: the program, MergedOS ntdll (the runtime itself, no PE
  headers: lookups go to its export table), and every DLL loaded from disk.
  Each entry carries an `LDR_DATA_TABLE_ENTRY`-compatible head linked into
  `PEB->Ldr` (load, memory and init order lists).
- Name resolution for an import of `X.dll`:
  1. `api-ms-win-*` / `ext-ms-*` API sets are redirected by prefix
     (`-crt-` to ucrtbase, `-core-` and most others to kernelbase,
     security/registry/eventing ones to advapi32), then resolved like a
     plain name; unknown sets fall back to the built-in table.
  2. `ntdll` is always the runtime.
  3. A module already loaded under that base name is reused (refcount).
  4. `C:\windows\system32\X.dll` (or the program's own directory) when the
     file exists: load it.
  5. Otherwise the runtime's built-in table (kernel32, msvcrt, user32
     surrogates, ...); a name it lacks gets a stub that prints
     `missing import DLL!Name` and exits with 0xC0000139 when called.
- Loading a file: read the headers, check PE32+/AMD64, reserve
  SizeOfImage anywhere (`NtAllocateVirtualMemory`), read each section to
  its RVA, apply base relocations (DIR64, HIGHLOW, ABSOLUTE), make the
  image RWX (protections are not narrowed yet).
- Imports: by name (hint first, then binary search of the export name
  table) and by ordinal. Exports that point inside the export directory are
  forwarders `DLL.Name` or `DLL.#ord`, resolved recursively (depth limit).
- Delay-load: `LdrResolveDelayLoadedAPI` and `__delayLoadHelper2` load the
  DLL on first call and patch the IAT slot.
- Static TLS: every module with a TLS directory loaded before the program
  starts gets the next index in `ThreadLocalStoragePointer`; its template is
  copied per thread. TLS callbacks run before the entry point.
- Initialisation: dependencies first (post-order of the import graph);
  for each DLL, TLS callbacks then `DllMain(DLL_PROCESS_ATTACH)`. A DllMain
  returning FALSE fails the load (STATUS_DLL_INIT_FAILED). Later
  `LdrLoadDll` calls initialise the newly loaded set the same way.
- API: `LdrLoadDll`, `LdrGetDllHandle(Ex)`, `LdrGetProcedureAddress`,
  `LdrUnloadDll` (refcount only, never unmaps), `LdrFindResource_U`,
  `LdrAccessResource`, `LdrResolveDelayLoadedAPI`, `LdrGetDllFullName`,
  `RtlImageNtHeader`, `RtlImageDirectoryEntryToData`, `RtlPcToFileHeader`.
  kernel32's `LoadLibrary`/`GetProcAddress`/`GetModuleHandle` in the
  built-in table use the same code.

## 2. MergedOS ntdll (`src/ntrt/ntdll*.c`)

- Kernel services (table 2, `eax = 0x2000 | n`, see
  `abi_dispatch/personality_nt.c`): open, close, read, write, seek, fstat,
  stat, readdir, mkdir, unlink, rename, futex, sleep, dup, gettid. They
  return a value or an NTSTATUS error.
- Handles: a user-space table; a handle is `4 * (slot + 1)` above the three
  standard handles (4, 8, 0xC = descriptors 0, 1, 2). Kinds: file,
  directory, registry key, event, mutant, semaphore, thread, process
  pseudo-handle. `NtClose`, `NtDuplicateObject` work on all kinds.
- PEB/TEB: documented x64 offsets; `gs` base = TEB. PEB fields set:
  ImageBaseAddress, Ldr, ProcessParameters, ProcessHeap, NumberOfProcessors,
  OS version, Ansi/Oem/UnicodeCaseTable code-page data (from the NLS files
  when present), TlsBitmap. TEB: stack, ClientId, Peb, LastError, TLS slots.
- `RTL_USER_PROCESS_PARAMETERS` (normalised: absolute pointers): image
  path, command line, current directory `C:\`, DllPath, environment block
  (`PATH`, `SystemRoot`, `windir`, `COMSPEC`, `USERNAME`, `COMPUTERNAME`,
  `TEMP`), std handles, ConsoleHandle 0.
- Heap: `RtlCreateHeap`/`RtlAllocateHeap`/`RtlFreeHeap`/`RtlReAllocateHeap`/
  `RtlSizeHeap` over the runtime's size-class allocator; HEAP_ZERO_MEMORY
  honoured; every heap handle shares one allocator.
- Strings: `Rtl*UnicodeString*`, `Rtl*AnsiString*`, UTF-8/multibyte/OEM
  conversion (code pages 1252/437 via the NLS table when loaded, else
  Latin-1), upcase/downcase, integer conversions, the CRT string functions
  ntdll exports.
- Paths: `RtlDosPathNameToNtPathName_U(_WithStatus)`, `RtlGetFullPathName_U`,
  `RtlDetermineDosPathNameType_U`, current directory get/set,
  `RtlDoesFileExists_U`. NT `\??\C:\a\b` maps to VFS `/a/b`.
- Files: `NtCreateFile`/`NtOpenFile` (dispositions over open flags +
  O_CREAT/O_EXCL/O_TRUNC, directories), `NtReadFile`/`NtWriteFile` (with
  ByteOffset), `NtQueryInformationFile` (standard, basic, position,
  network-open, name, all, attribute-tag), `NtSetInformationFile`
  (position, disposition = delete on close, rename, end of file),
  `NtQueryAttributesFile`, `NtQueryFullAttributesFile`,
  `NtQueryDirectoryFile` (both-directory, full, names, with wildcards),
  `NtQueryVolumeInformationFile` (device, attribute), `NtFlushBuffersFile`.
  Console: std handles report FILE_DEVICE_CONSOLE-less character devices;
  `NtDeviceIoControlFile` answers condrv requests with "not a console", so
  Wine falls back to plain file I/O.
- Memory: `NtAllocateVirtualMemory(Ex)`, `NtFreeVirtualMemory`,
  `NtProtectVirtualMemory`, `NtQueryVirtualMemory` (basic info from a
  user-side region list).
- Information: `NtQuerySystemInformation` (basic, cpu, time-of-day,
  performance), `NtQueryInformationProcess` (basic, image file name, ...),
  `NtQueryInformationThread`, tokens (fixed user and groups SIDs).
- NLS: `NtGetNlsSectionPtr`, `NtInitializeNlsFiles`, `NtQueryDefaultLocale`,
  `RtlInitCodePageTable`, read from `C:\windows\system32\*.nls`.
- Exceptions: faults still end the process. `RtlCaptureContext`,
  `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx` on `.pdata`
  unwind data; `RtlRaiseException` dispatches to the frame handlers found by
  unwinding (enough for Wine's `__TRY` and debug-print exceptions).
- Wine debug channels: `__wine_dbg_*` report every channel off.

## 3. Registry, files, threads (`src/ntrt/ntreg.c`, `ntsync.c`)

- Registry: a hive kept as files under the ramfs mount `/registry` so every
  process sees the same tree: one directory per key (lower-case name,
  original spelling in `.name`), one file per value (lower-case name; file
  holds type, original name and data). Roots: `\Registry\Machine` and
  `\Registry\User\S-1-5-21-0-0-0-1000` (`RtlOpenCurrentUser`).
  `NtCreateKey`, `NtOpenKey(Ex)`, `NtQueryKey`, `NtEnumerateKey`,
  `NtSetValueKey`, `NtQueryValueKey`, `NtEnumerateValueKey`,
  `NtDeleteValueKey`, `NtDeleteKey`, `NtFlushKey`; all three information
  classes each.
- Sync objects live in user space (handles of the same process): events
  (manual/auto), mutants (owner + recursion), semaphores. Waits use the
  kernel futex on a per-process generation counter: `NtWaitForSingleObject`,
  `NtWaitForMultipleObjects` (any/all), timeouts, `NtDelayExecution`,
  `NtYieldExecution`. Critical sections: `RtlEnterCriticalSection` etc. with
  an atomic lock count and a futex wait (keyed-event semantics without a
  keyed event). SRW locks, condition variables, `RtlWaitOnAddress`,
  `RtlRunOnce*` on the same futex.
- Threads: `NtCreateThreadEx` in the kernel; the runtime gives each thread
  its own TEB (set through a start trampoline), runs TLS initialisation and
  `DLL_THREAD_ATTACH`.

## Status

See the end of this file once each part has landed.
