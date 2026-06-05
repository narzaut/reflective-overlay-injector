#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

struct PEMappingInfo {
    uint64_t base_addr;
    size_t   image_size;
    uint64_t entry_point_rva;
    DWORD    nt_headers_size;
};

static bool pe_parse(const uint8_t* data, size_t data_size, PEMappingInfo& info) {
    if (data_size < 0x40) return false;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0 || (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > data_size) return false;

    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(data + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

    info.image_size = nt->OptionalHeader.SizeOfImage;
    info.entry_point_rva = nt->OptionalHeader.AddressOfEntryPoint;
    info.nt_headers_size = sizeof(IMAGE_NT_HEADERS64) + nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    info.base_addr = nt->OptionalHeader.ImageBase;
    return true;
}

static const IMAGE_SECTION_HEADER* pe_sections(const uint8_t* data, size_t* count) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)data;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(data + dos->e_lfanew);
    *count = nt->FileHeader.NumberOfSections;
    return IMAGE_FIRST_SECTION(nt);
}

static bool pe_fix_relocations(uint8_t* mapped, uint64_t old_base, uint64_t new_base) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mapped;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(mapped + dos->e_lfanew);
    DWORD reloc_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    DWORD reloc_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
    if (reloc_rva == 0 || reloc_size == 0) return true;

    uint8_t* reloc_data = mapped + reloc_rva;
    uint8_t* reloc_end = reloc_data + reloc_size;
    int64_t delta = (int64_t)new_base - (int64_t)old_base;

    while (reloc_data < reloc_end) {
        const IMAGE_BASE_RELOCATION* block = (const IMAGE_BASE_RELOCATION*)reloc_data;
        if (block->SizeOfBlock == 0) break;
        DWORD block_rva = block->VirtualAddress;
        DWORD entry_count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        const WORD* entries = (const WORD*)(reloc_data + sizeof(IMAGE_BASE_RELOCATION));
        for (DWORD i = 0; i < entry_count; i++) {
            WORD entry = entries[i];
            int type = (entry >> 12) & 0xF;
            WORD offset = entry & 0xFFF;
            if (type == IMAGE_REL_BASED_DIR64) {
                uint64_t* target = (uint64_t*)(mapped + block_rva + offset);
                *target += delta;
            } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                uint32_t* target = (uint32_t*)(mapped + block_rva + offset);
                *target += (uint32_t)delta;
            }
        }
        reloc_data += block->SizeOfBlock;
    }
    return true;
}

static bool pe_resolve_imports(uint8_t* mapped, char* fail_name, size_t fail_name_sz) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mapped;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(mapped + dos->e_lfanew);
    DWORD import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    DWORD import_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (import_rva == 0 || import_size == 0) return true;

    IMAGE_IMPORT_DESCRIPTOR* imports = (IMAGE_IMPORT_DESCRIPTOR*)(mapped + import_rva);
    while (imports->Name != 0) {
        const char* dll_name = (const char*)(mapped + imports->Name);
        HMODULE hmod = GetModuleHandleA(dll_name);
        if (!hmod) hmod = LoadLibraryA(dll_name);
        if (!hmod) {
            if (fail_name && fail_name_sz > 0) {
                size_t len = strlen(dll_name);
                if (len >= fail_name_sz) len = fail_name_sz - 1;
                memcpy(fail_name, dll_name, len);
                fail_name[len] = '\0';
            }
            return false;
        }

        DWORD oft_rva = imports->OriginalFirstThunk;
        DWORD ft_rva = imports->FirstThunk;

        IMAGE_THUNK_DATA64* orig_thunk = oft_rva ? (IMAGE_THUNK_DATA64*)(mapped + oft_rva) : (IMAGE_THUNK_DATA64*)(mapped + ft_rva);
        IMAGE_THUNK_DATA64* iat_thunk = (IMAGE_THUNK_DATA64*)(mapped + ft_rva);

        while (orig_thunk->u1.AddressOfData != 0) {
            if (IMAGE_SNAP_BY_ORDINAL64(orig_thunk->u1.Ordinal)) {
                WORD ordinal = (WORD)IMAGE_ORDINAL64(orig_thunk->u1.Ordinal);
                FARPROC proc = GetProcAddress(hmod, (LPCSTR)(uintptr_t)ordinal);
                if (!proc) return false;
                iat_thunk->u1.Function = (uint64_t)(uintptr_t)proc;
            } else {
                IMAGE_IMPORT_BY_NAME* hint = (IMAGE_IMPORT_BY_NAME*)(mapped + orig_thunk->u1.AddressOfData);
                FARPROC proc = GetProcAddress(hmod, (LPCSTR)hint->Name);
                if (!proc) return false;
                iat_thunk->u1.Function = (uint64_t)(uintptr_t)proc;
            }
            orig_thunk++;
            iat_thunk++;
        }
        imports++;
    }
    return true;
}