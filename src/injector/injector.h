#pragma once
#include "common.h"
#include "syscalls.h"
#include "payload_shared.h"

struct DllOverlayContext {
    HANDLE   process_handle;
    uint64_t remote_dll_base;
    HWND     overlay_hwnd;
    PayloadSharedData* shared_mem;
    HANDLE   shared_map;
    HANDLE   frame_event;
    size_t   image_size;
};

extern DllOverlayContext* g_overlay_ctx;

DWORD find_explorer_pid();
void overlay_cleanup(DllOverlayContext& ctx);
DllOverlayContext inject_overlay_reflective();