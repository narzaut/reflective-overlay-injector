#pragma once
#include "common.h"

typedef long NTSTATUS;

struct SyscallStub { uint32_t number; void* code; };

struct OverlaySyscalls {
    SyscallStub NtOpenProcess;
    SyscallStub NtAllocateVirtualMemory;
    SyscallStub NtFreeVirtualMemory;
    SyscallStub NtWriteVirtualMemory;
    SyscallStub NtProtectVirtualMemory;
    SyscallStub NtCreateThreadEx;
    SyscallStub NtCreateSection;
    SyscallStub NtMapViewOfSection;
};

typedef NTSTATUS (*fn_NtOpenProcess)(HANDLE*, ULONG, void*, void*);
typedef NTSTATUS (*fn_NtAllocateVirtualMemory)(HANDLE, void**, ULONG_PTR, SIZE_T*, ULONG, ULONG);
typedef NTSTATUS (*fn_NtFreeVirtualMemory)(HANDLE, void**, SIZE_T*, ULONG);
typedef NTSTATUS (*fn_NtWriteVirtualMemory)(HANDLE, void*, void*, SIZE_T, SIZE_T*);
typedef NTSTATUS (*fn_NtProtectVirtualMemory)(HANDLE, void**, SIZE_T*, ULONG, ULONG*);
typedef NTSTATUS (*fn_NtCreateThreadEx)(HANDLE*, ULONG, void*, HANDLE, void*, void*, ULONG, SIZE_T, SIZE_T, SIZE_T, void*);
typedef NTSTATUS (*fn_NtCreateSection)(HANDLE*, ULONG, void*, LARGE_INTEGER*, ULONG, ULONG, HANDLE);
typedef NTSTATUS (*fn_NtMapViewOfSection)(HANDLE, HANDLE, void**, ULONG_PTR, SIZE_T, LARGE_INTEGER*, SIZE_T*, ULONG, ULONG, ULONG);

extern OverlaySyscalls g_sys;

void find_syscall_gadget();
OverlaySyscalls resolve_overlay_syscalls();
void lock_stubs();

DWORD find_pid(const char* name);

HANDLE sys_open(DWORD pid, ULONG access);
bool   sys_write(HANDLE h, uint64_t addr, const void* buf, size_t sz);
bool   sys_protect(HANDLE h, uint64_t addr, size_t sz, ULONG new_prot, ULONG* old_prot);
bool   sys_free(HANDLE h, uint64_t addr);
uint64_t sys_alloc_remote(HANDLE h, size_t size, ULONG prot);
uint64_t sys_alloc_remote_backed(HANDLE h, size_t size, const wchar_t* backing_file);
HANDLE sys_create_thread(HANDLE process, LPTHREAD_START_ROUTINE start, PVOID param);
