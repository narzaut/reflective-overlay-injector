#include "injector.h"
#include "syscalls.h"
#include "stealth_strings.h"
#include "reflective_loader.h"

DllOverlayContext* g_overlay_ctx = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Explorer reflective DLL injection
// ─────────────────────────────────────────────────────────────────────────────

DWORD find_explorer_pid() {
    char exp[32];
    build_explorer_name(exp, sizeof(exp));
    DWORD pid = find_pid(exp);
    SecureZeroMemory(exp, sizeof(exp));
    return pid;
}

void overlay_cleanup(DllOverlayContext& ctx) {
    if (ctx.shared_mem) {
        ctx.shared_mem->signal = 2;
    }
    Sleep(500);
    if (ctx.process_handle && ctx.remote_dll_base && ctx.remote_dll_base != 0) {
        sys_free(ctx.process_handle, ctx.remote_dll_base);
    }
    if (ctx.shared_mem) {
        UnmapViewOfFile(ctx.shared_mem);
        ctx.shared_mem = nullptr;
    }
    if (ctx.shared_map) {
        CloseHandle(ctx.shared_map);
        ctx.shared_map = nullptr;
    }
    if (ctx.process_handle) {
        CloseHandle(ctx.process_handle);
        ctx.process_handle = nullptr;
    }
}

static std::vector<uint8_t> make_loader_stub(uint32_t entry_rva) {
    std::vector<uint8_t> stub = {
        0x48, 0x83, 0xEC, 0x28,
        0x48, 0x89, 0xC8,
        0xBA, 0x01, 0x00, 0x00, 0x00,
        0x45, 0x31, 0xC0,
        0x48, 0x05,
    };
    stub.push_back((uint8_t)(entry_rva & 0xFF));
    stub.push_back((uint8_t)((entry_rva >> 8) & 0xFF));
    stub.push_back((uint8_t)((entry_rva >> 16) & 0xFF));
    stub.push_back((uint8_t)((entry_rva >> 24) & 0xFF));
    stub.push_back(0xFF); stub.push_back(0xD0);
    stub.push_back(0x48); stub.push_back(0x83); stub.push_back(0xC4); stub.push_back(0x28);
    stub.push_back(0xC3);
    return stub;
}

static bool reflective_load_dll(HANDLE process, const uint8_t* dll_data, size_t dll_size, uint64_t& out_base, uint32_t& out_entry_rva, size_t& out_image_size, size_t& out_headers_size) {
    PEMappingInfo pei = {};
    if (!pe_parse(dll_data, dll_size, pei)) {
        LOG("  reflective: PE parse failed\n");
        return false;
    }
    LOG("  reflective: image_size=%zu entry_rva=0x%llx preferred_base=0x%llx\n",
        pei.image_size, (unsigned long long)pei.entry_point_rva, (unsigned long long)pei.base_addr);

    uint64_t alloc_base = sys_alloc_remote_backed(process, pei.image_size,
        L"C:\\Windows\\System32\\d3d11.dll");
    if (!alloc_base) {
        LOG("  reflective: section-backed allocation failed, falling back to private\n");
        alloc_base = sys_alloc_remote(process, pei.image_size, PAGE_READWRITE);
    }
    if (alloc_base == 0) {
        LOG("  reflective: NtAllocateVirtualMemory failed\n");
        return false;
    }
    LOG("  reflective: allocated at 0x%llx (size=%zu)\n", (unsigned long long)alloc_base, pei.image_size);

    std::vector<uint8_t> mapped(pei.image_size, 0);
    // Copy PE headers
    memcpy(mapped.data(), dll_data, ((const IMAGE_DOS_HEADER*)dll_data)->e_lfanew + pei.nt_headers_size);
    // Map each section from raw offset to virtual offset
    size_t sec_count = 0;
    const IMAGE_SECTION_HEADER* secs = pe_sections(dll_data, &sec_count);
    for (size_t i = 0; i < sec_count; i++) {
        if (secs[i].SizeOfRawData == 0) continue;
        if (secs[i].VirtualAddress + secs[i].SizeOfRawData > pei.image_size) continue;
        memcpy(mapped.data() + secs[i].VirtualAddress,
               dll_data + secs[i].PointerToRawData,
               secs[i].SizeOfRawData);
    }

    LOG("  reflective: resolving imports (pre-relocation, alloc_base=0x%llx)\n", (unsigned long long)alloc_base);
    char fail_dll[256] = {};
    if (!pe_resolve_imports(mapped.data(), fail_dll, sizeof(fail_dll))) {
        LOG("  reflective: import resolution failed (dll=%s)\n", fail_dll);
        return false;
    }
    LOG("  reflective: imports resolved OK, saving IAT entries\n");

    // Save IAT entries (GetProcAddress results) before relocation corrupts them
    const IMAGE_NT_HEADERS64* nt_before = (const IMAGE_NT_HEADERS64*)(mapped.data() + ((const IMAGE_DOS_HEADER*)mapped.data())->e_lfanew);
    DWORD iat_rva = nt_before->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress;
    DWORD iat_size = nt_before->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size;
    std::vector<uint8_t> iat_saved;
    if (iat_rva != 0 && iat_size != 0) {
        iat_saved.assign(mapped.data() + iat_rva, mapped.data() + iat_rva + iat_size);
    }

    LOG("  reflective: fixing relocations delta=0x%llx\n", (unsigned long long)((int64_t)alloc_base - (int64_t)pei.base_addr));
    pe_fix_relocations(mapped.data(), pei.base_addr, alloc_base);

    // Restore IAT entries (relocation would corrupt the resolved addresses)
    if (!iat_saved.empty()) {
        memcpy(mapped.data() + iat_rva, iat_saved.data(), iat_size);
        LOG("  reflective: restored %u bytes of IAT\n", (unsigned)iat_size);
    }

    size_t headers_size = (size_t)((const IMAGE_DOS_HEADER*)mapped.data())->e_lfanew + pei.nt_headers_size;
    LOG("  reflective: writing headers %zu bytes to 0x%llx\n", headers_size, (unsigned long long)alloc_base);
    if (!sys_write(process, alloc_base, mapped.data(), headers_size)) {
        LOG("  reflective: write headers failed\n");
        return false;
    }
    LOG("  reflective: headers written OK\n");

    size_t section_count = 0;
    const IMAGE_SECTION_HEADER* sections = pe_sections(mapped.data(), &section_count);
    LOG("  reflective: writing %zu sections\n", section_count);
    for (size_t i = 0; i < section_count; i++) {
        if (sections[i].SizeOfRawData == 0) continue;
        uint8_t* section_data = mapped.data() + sections[i].VirtualAddress;
        uint64_t section_va = alloc_base + sections[i].VirtualAddress;
        size_t section_sz = sections[i].SizeOfRawData;
        LOG("  reflective: section %zu: VA=0x%llx size=%zu\n", i, (unsigned long long)section_va, section_sz);
        size_t off = 0;
        bool section_ok = true;
        while (off < section_sz) {
            size_t chunk = section_sz - off;
            if (chunk > 65536) chunk = 65536;
            if (!sys_write(process, section_va + off, section_data + off, chunk)) {
                LOG("  reflective: write section %zu chunk failed (off=%zu)\n", i, off);
                section_ok = false;
                break;
            }
            off += chunk;
        }
        if (!section_ok) {
            LOG("  reflective: section %zu write FAILED\n", i);
        }
    }
    LOG("  reflective: all sections written\n");

    for (size_t i = 0; i < section_count; i++) {
        DWORD chars = sections[i].Characteristics;
        DWORD prot = PAGE_EXECUTE_READWRITE;
        bool exec  = (chars & IMAGE_SCN_MEM_EXECUTE) != 0;
        bool read  = (chars & IMAGE_SCN_MEM_READ) != 0;
        bool write = (chars & IMAGE_SCN_MEM_WRITE) != 0;
        if (exec && read && write) prot = PAGE_EXECUTE_READWRITE;
        else if (exec && read)     prot = PAGE_EXECUTE_READ;
        else if (exec)             prot = PAGE_EXECUTE;
        else if (read && write)    prot = PAGE_READWRITE;
        else if (read)             prot = PAGE_READONLY;

        uint64_t sec_base = alloc_base + sections[i].VirtualAddress;
        size_t sec_sz = sections[i].Misc.VirtualSize ? sections[i].Misc.VirtualSize : sections[i].SizeOfRawData;
        ULONG old_prot = 0;
        if (!sys_protect(process, sec_base, sec_sz, prot, &old_prot)) {
            LOG("  reflective: sys_protect section %zu failed (prot=0x%lx)\n", i, prot);
        }
    }
    LOG("  reflective: section protections set\n");

    out_base = alloc_base;
    out_entry_rva = (uint32_t)pei.entry_point_rva;
    out_image_size = pei.image_size;
    out_headers_size = headers_size;
    LOG("  reflective: mapped at 0x%llx, DllMain=0x%llx\n",
        (unsigned long long)out_base, (unsigned long long)(out_base + out_entry_rva));
    return true;
}

DllOverlayContext inject_overlay_reflective() {
    DWORD explorer_pid = find_explorer_pid();
    if (!explorer_pid) { LOG("Cannot find explorer.exe\n"); exit(1); }
    LOG("Found explorer.exe at pid %lu\n", (unsigned long)explorer_pid);

    char dll_path[MAX_PATH];
    char dll_name[MAX_PATH];
    build_dll_name(dll_name, MAX_PATH);
    // Prepend build output directory
    char dll_path_full[512];
    snprintf(dll_path_full, sizeof(dll_path_full), "build\\%s", dll_name);
    GetFullPathNameA(dll_path_full, MAX_PATH, dll_path, nullptr);
    SecureZeroMemory(dll_name, sizeof(dll_name));
    LOG("  DLL path: %s\n", dll_path);

    HANDLE dll_file = CreateFileA(dll_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dll_file == INVALID_HANDLE_VALUE) { LOG("  Cannot open DLL: %lu\n", GetLastError()); exit(1); }
    DWORD dll_size = GetFileSize(dll_file, nullptr);
    std::vector<uint8_t> dll_buf(dll_size);
    DWORD bytes_read = 0;
    ReadFile(dll_file, dll_buf.data(), dll_size, &bytes_read, nullptr);
    CloseHandle(dll_file);
    if (bytes_read != dll_size) { LOG("  DLL read failed\n"); exit(1); }
    DeleteFileA(dll_path);
    SecureZeroMemory(dll_path, sizeof(dll_path));
    LOG("  DLL size: %u bytes\n", dll_size);

    HANDLE explorer_h = sys_open(explorer_pid,
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION);
    if (!explorer_h) { LOG("  Cannot open explorer: %lu\n", GetLastError()); exit(1); }

    wchar_t shm_buf[64];
    build_shm_name(shm_buf, 64, explorer_pid);
    HANDLE shared_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PayloadSharedData), shm_buf);
    SecureZeroMemory(shm_buf, sizeof(shm_buf));
    if (!shared_map) { LOG("  CreateFileMappingW failed: %lu\n", GetLastError()); CloseHandle(explorer_h); exit(1); }
    PayloadSharedData* shared_mem = (PayloadSharedData*)MapViewOfFile(shared_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(PayloadSharedData));
    if (!shared_mem) { LOG("  MapViewOfFile failed: %lu\n", GetLastError()); CloseHandle(shared_map); CloseHandle(explorer_h); exit(1); }
    memset(shared_mem, 0, sizeof(PayloadSharedData));

    wchar_t evt_name[72];
    build_shm_name(evt_name, 64, explorer_pid);
    int el = 0; while (el < 64 && evt_name[el]) el++;
    evt_name[el] = L'_'; evt_name[el+1] = L'e'; evt_name[el+2] = L'v'; evt_name[el+3] = L't'; evt_name[el+4] = 0;
    HANDLE frame_event = CreateEventW(NULL, FALSE, FALSE, evt_name);
    SecureZeroMemory(evt_name, sizeof(evt_name));

    uint64_t dll_base = 0;
    uint32_t entry_rva = 0;
    size_t image_size = 0;
    size_t headers_size_saved = 0;
    if (!reflective_load_dll(explorer_h, dll_buf.data(), dll_buf.size(), dll_base, entry_rva, image_size, headers_size_saved)) {
        LOG("  Reflective DLL loading failed\n"); UnmapViewOfFile(shared_mem); CloseHandle(shared_map); CloseHandle(explorer_h); exit(1);
    }
    SecureZeroMemory(dll_buf.data(), dll_buf.size());
    dll_buf.clear();
    dll_buf.shrink_to_fit();

    auto stub = make_loader_stub(entry_rva);
    uint64_t stub_addr = sys_alloc_remote(explorer_h, 4096, PAGE_EXECUTE_READWRITE);
    if (stub_addr == 0) { LOG("  Stub alloc failed\n"); exit(1); }
    if (!sys_write(explorer_h, stub_addr, stub.data(), stub.size())) { LOG("  Stub write failed\n"); exit(1); }

    // Set stub to PAGE_EXECUTE_READ after writing (remove write permission)
    ULONG old_prot = 0;
    sys_protect(explorer_h, stub_addr, 4096, PAGE_EXECUTE_READ, &old_prot);
    LOG("  Loader stub at 0x%llx (entry_rva=0x%x)\n", (unsigned long long)stub_addr, entry_rva);

    HANDLE thread = sys_create_thread(explorer_h, (LPTHREAD_START_ROUTINE)stub_addr, (PVOID)(uintptr_t)dll_base);
    if (!thread) { LOG("  NtCreateThreadEx failed\n"); exit(1); }
    LOG("  DllMain thread created, waiting...\n");

    WaitForSingleObject(thread, 15000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);
    CloseHandle(thread);
    // Wipe stub contents before freeing
    std::vector<uint8_t> stub_wipe(stub.size(), 0);
    sys_write(explorer_h, stub_addr, stub_wipe.data(), stub_wipe.size());
    sys_free(explorer_h, stub_addr);

    // Wipe PE headers in remote memory — imports are resolved, no longer needed
    if (headers_size_saved > 0) {
        std::vector<uint8_t> hdr_zero(headers_size_saved, 0);
        sys_write(explorer_h, dll_base, hdr_zero.data(), hdr_zero.size());
        ULONG old_prot = 0;
        sys_protect(explorer_h, dll_base, 4096, PAGE_READONLY, &old_prot);
    }
    LOG("  DllMain returned %lu\n", exit_code);

    HWND overlay_hwnd = nullptr;
    for (int i = 0; i < 200; i++) {
        if (shared_mem->ready == 1) {
            overlay_hwnd = (HWND)(uintptr_t)shared_mem->hwnd;
            LOG("  Overlay window ready! HWND=%p\n", (void*)overlay_hwnd);
            break;
        }
        if ((shared_mem->ready & 0xFF000000) == 0xEE000000) {
            LOG("  Overlay init FAILED: ready=0x%08X error_hr=0x%08X\n", shared_mem->ready, (unsigned)shared_mem->error_hr);
            exit(1);
        }
        if (shared_mem->ready == 0xFFFFFFFF) { LOG("  Window creation failed\n"); exit(1); }
        if (i % 10 == 0) LOG("  ...waiting (ready=0x%08X)\n", shared_mem->ready);
        Sleep(50);
    }
    if (!overlay_hwnd) { LOG("  Overlay timed out (ready=0x%08X)\n", shared_mem->ready); exit(1); }

    return { explorer_h, dll_base, overlay_hwnd, shared_mem, shared_map, frame_event, image_size };
}