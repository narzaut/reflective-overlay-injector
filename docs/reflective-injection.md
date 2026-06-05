# Reflective DLL Injection

The overlay DLL is loaded into explorer.exe without calling `LoadLibrary`. The injector manually maps the PE image, resolves imports, applies relocations, and launches `DllMain` through a small stub. After loading, all traces of the injection are wiped.

**Source:** `src/injector/injector.cpp`, `src/injector/reflective_loader.h`

## Why Reflective Loading

`LoadLibrary` leaves multiple forensic traces:
- Entry in the process's module list (`PEB.Ldr.InLoadOrderModuleList`)
- DLL load notification via `LdrRegisterDllNotification` callbacks
- Event in the loader's activity tracker
- The DLL file remains on disk (can be scanned)

Reflective loading bypasses the Windows loader entirely. The DLL is mapped manually — no module list entry, no load notification, no loader lock contention. After loading, the file is deleted from disk.

## The Injection Flow

### 1. Read DLL and Delete File

```cpp
HANDLE dll_file = CreateFileA(dll_path, GENERIC_READ, ...);
ReadFile(dll_file, dll_buf, dll_size, ...);
CloseHandle(dll_file);
DeleteFileA(dll_path);  // file gone before injection even starts
```

The DLL file is deleted immediately after reading. By the time the injection completes, there is no file on disk to scan.

### 2. Open explorer.exe

```cpp
DWORD explorer_pid = find_explorer_pid();  // via CreateToolhelp32Snapshot
HANDLE explorer_h = sys_open(explorer_pid,
    PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
    PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION);
```

### 3. PE Parse

The DLL's PE headers are parsed to extract:
- `ImageSize` — total virtual memory needed
- `AddressOfEntryPoint` — RVA of `DllMain`
- `ImageBase` — preferred load address
- Section headers — for mapping raw data to virtual offsets

### 4. Allocate Remote Memory

```cpp
uint64_t alloc_base = sys_alloc_remote(explorer_h, image_size, PAGE_READWRITE);
```

A single allocation of `ImageSize` bytes in explorer.exe. Initially `PAGE_READWRITE` — section protections are set later.

### 5. Map Sections Locally

The entire PE image is mapped into a local buffer first. This is critical because import resolution and relocation must happen before writing to remote memory:

```
1. Copy PE headers to local buffer
2. For each section: copy raw data from file offset to virtual offset in buffer
3. Resolve imports in the local buffer (GetProcAddress against local modules)
4. Save IAT entries (resolved function pointers)
5. Apply relocations (adjust all absolute addresses for new base)
6. Restore IAT entries (relocations corrupt them — see below)
```

### 6. The IAT Save/Restore Trick

This is the most subtle part of the reflective loader. The order of operations matters:

**Problem:** Import resolution calls `GetProcAddress` which returns addresses valid in the *injector's* process. These addresses are written into the IAT. Then relocations are applied, which add a delta to every absolute address in the image — including the IAT entries. This corrupts the resolved function pointers.

**Solution:**
1. Resolve imports → IAT contains correct `GetProcAddress` results
2. Save a copy of the IAT
3. Apply relocations → IAT entries are corrupted (delta added)
4. Restore the saved IAT → correct function pointers are back

This works because explorer.exe shares the same system DLLs (kernel32, user32, d3d11, etc.) at the same base addresses as the injector process. The `GetProcAddress` results are valid in both processes.

### 7. Write to Remote Memory

The mapped image is written to explorer.exe in stages:
1. **Headers first** — PE headers written as a single block
2. **Sections individually** — each section written in 64KB chunks (to avoid large write failures)

### 8. Set Section Protections

Each section's memory protection is set to match its PE characteristics:

| Section | Typical Protection |
|---|---|
| `.text` | `PAGE_EXECUTE_READ` |
| `.rdata` | `PAGE_READONLY` |
| `.data` | `PAGE_READWRITE` |
| `.pdata` | `PAGE_READONLY` |

### 9. Create Loader Stub

A small x86-64 stub is allocated and written to explorer.exe:

```asm
sub rsp, 0x28          ; 48 83 EC 28  (shadow space + alignment)
mov rax, rcx           ; 48 89 C8     (rcx = DLL base address)
mov edx, 1             ; BA 01 00 00 00 (fdwReason = DLL_PROCESS_ATTACH)
xor r8d, r8d           ; 45 31 C0     (lpvReserved = NULL)
add rax, <entry_rva>   ; 48 05 <4 bytes> (compute DllMain address)
call rax               ; FF D0
add rsp, 0x28          ; 48 83 C4 28
ret                    ; C3
```

The stub calls `DllMain(dll_base, DLL_PROCESS_ATTACH, NULL)`. The DLL base address is passed as the thread argument to `NtCreateThreadEx`, which places it in `rcx`.

### 10. Launch and Wait

```cpp
HANDLE thread = sys_create_thread(explorer_h,
    (LPTHREAD_START_ROUTINE)stub_addr, (PVOID)dll_base);
WaitForSingleObject(thread, 15000);  // wait up to 15 seconds for DllMain
```

The overlay DLL's `DllMain` creates a thread that initializes DirectComposition and signals readiness via shared memory.

### 11. Wipe Traces

After `DllMain` returns:

1. **Stub wiped**: Zero the stub bytes, then free the allocation
2. **PE headers wiped**: Zero the first `headers_size` bytes of the remote image, then set to `PAGE_READONLY`
3. **Process handle closed**: After overlay initialization, the handle to explorer.exe is closed and nulled

```cpp
sys_write(explorer_h, stub_addr, zeros, stub.size());
sys_free(explorer_h, stub_addr);

sys_write(explorer_h, dll_base, hdr_zeros, headers_size);
sys_protect(explorer_h, dll_base, 4096, PAGE_READONLY, &old);
```

After wiping, the DLL image in explorer.exe has:
- Zeroed PE headers (no MZ/PE signature, no section table, no import table)
- Correct section protections (looks like normal code/data)
- No module list entry
- No loader notification
- No file on disk

The only way to detect it is to scan for executable memory regions that don't correspond to known modules — and the DComp device creation is the only behavioral signal.

## Shared Memory Setup

Before injection, a named file mapping is created for IPC between the injector and the overlay DLL:

```cpp
HANDLE shared_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
    PAGE_READWRITE, 0, sizeof(PayloadSharedData), shm_name);
PayloadSharedData* shared_mem = (PayloadSharedData*)MapViewOfFile(
    shared_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(PayloadSharedData));
```

The shared memory name is XOR-obfuscated with an FNV-1a hash of the explorer PID, ensuring each injection uses a unique name that cannot be predicted.

The overlay DLL opens this same mapping by name and uses it to communicate readiness, screen dimensions, and the shared texture handle.
