#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <cstdint>
#include "payload_shared.h"
#include "stealth_strings.h"

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

static HANDLE g_stop_event = NULL;
static HANDLE g_overlay_thread = NULL;
static PayloadSharedData* g_data = NULL;
static HANDLE g_map = NULL;
static HWND g_hwnd = NULL;
static HCURSOR g_blank_cursor = NULL;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETCURSOR) {
        SetCursor(NULL);
        return TRUE;
    }
    if (msg == WM_NCHITTEST) {
        return HTCLIENT;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp2 = (WINDOWPOS*)lp;
        wp2->flags |= SWP_NOACTIVATE;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI OverlayThread(LPVOID param) {
    wchar_t shm_buf[64];
    build_shm_name(shm_buf, 64, GetCurrentProcessId());
    g_map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, shm_buf);
    SecureZeroMemory(shm_buf, sizeof(shm_buf));
    if (!g_map) return 1;

    g_data = (PayloadSharedData*)MapViewOfFile(g_map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(PayloadSharedData));
    if (!g_data) { CloseHandle(g_map); g_map = NULL; return 1; }

    wchar_t evt_buf[72];
    build_shm_name(evt_buf, 64, GetCurrentProcessId());
    int el = 0; while (el < 64 && evt_buf[el]) el++;
    evt_buf[el] = L'_'; evt_buf[el+1] = L'e'; evt_buf[el+2] = L'v'; evt_buf[el+3] = L't'; evt_buf[el+4] = 0;
    HANDLE frame_event = OpenEventW(SYNCHRONIZE, FALSE, evt_buf);
    SecureZeroMemory(evt_buf, sizeof(evt_buf));

    g_data->ready = 0xAA000001;

    BYTE andMask = 0xFF;
    BYTE xorMask = 0x00;
    g_blank_cursor = CreateCursor(NULL, 0, 0, 1, 1, &andMask, &xorMask);

    wchar_t u32[32];
    build_user32(u32, 32);
    typedef BOOL(WINAPI* pfnSetThreadDpiAwarenessContext)(void*);
    pfnSetThreadDpiAwarenessContext pSetThreadCtx = (pfnSetThreadDpiAwarenessContext)GetProcAddress(GetModuleHandleW(u32), "SetThreadDpiAwarenessContext");
    SecureZeroMemory(u32, sizeof(u32));
    if (pSetThreadCtx) {
        pSetThreadCtx((void*)(intptr_t)-4);
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        g_data->ready = 0xEE000001;
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 0xAA000002;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = NULL;
    wchar_t cls[64];
    build_wnd_class(cls, 64);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    typedef HWND(WINAPI* pfnCreateWindowInBand)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID, DWORD);
    wchar_t u32b[32];
    build_user32(u32b, 32);
    pfnCreateWindowInBand pCreateWindowInBand = (pfnCreateWindowInBand)GetProcAddress(GetModuleHandleW(u32b), "CreateWindowInBand");
    SecureZeroMemory(u32b, sizeof(u32b));

    g_data->ready = 0xAA000003;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    DWORD ex_style = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;

    HWND hwnd = NULL;
    if (pCreateWindowInBand) {
        hwnd = pCreateWindowInBand(ex_style, wc.lpszClassName, L"", WS_POPUP, 0, 0, sw, sh, NULL, NULL, wc.hInstance, NULL, 4);
    }
    if (!hwnd) {
        hwnd = CreateWindowExW(ex_style, wc.lpszClassName, L"", WS_POPUP, 0, 0, sw, sh, NULL, NULL, wc.hInstance, NULL);
    }
    if (!hwnd) {
        g_data->ready = 0xEE000002;
        CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_hwnd = hwnd;
    SetWindowDisplayAffinity(hwnd, 0x00000002);
    ShowWindow(hwnd, SW_SHOW);
    ShowCursor(FALSE);

    g_data->hwnd = (uint64_t)(uintptr_t)hwnd;
    g_data->screen_w = sw;
    g_data->screen_h = sh;

    g_data->ready = 1;

    while ((g_data->shared_texture_handle == 0 || g_data->screen_w == 0) && WaitForSingleObject(g_stop_event, 10) != WAIT_OBJECT_0) {
        Sleep(1);
    }
    if (g_data->shared_texture_handle == 0 || g_data->screen_w == 0) {
        g_data->ready = 0xEE000003;
        DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    int texW = g_data->screen_w;
    int texH = g_data->screen_h;
    MoveWindow(hwnd, 0, 0, texW, texH, TRUE);
    HANDLE sharedHandle = (HANDLE)(uintptr_t)g_data->shared_texture_handle;

    g_data->ready = 0xAA000100;

    ID3D11Device* device = NULL;
    ID3D11DeviceContext* ctx = NULL;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fls, 3, D3D11_SDK_VERSION, &device, &fl, &ctx);
    if (FAILED(hr) || !device) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE000004;
        DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 0xAA000101;

    IDXGIDevice* dxgiDev = NULL;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    if (FAILED(hr) || !dxgiDev) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE000005;
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 0xAA000102;

    IDCompositionDevice* dcompDevice = NULL;
    hr = DCompositionCreateDevice(dxgiDev, __uuidof(IDCompositionDevice), (void**)&dcompDevice);
    if (FAILED(hr) || !dcompDevice) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE000006;
        dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 0xAA000103;

    IDCompositionTarget* dcompTarget = NULL;
    hr = dcompDevice->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget);
    if (FAILED(hr) || !dcompTarget) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE000007;
        dcompDevice->Release();
        dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 0xAA000104;

    IDCompositionVisual* dcompVisual = NULL;
    hr = dcompDevice->CreateVisual(&dcompVisual);
    if (FAILED(hr) || !dcompVisual) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE000008;
        dcompTarget->Release(); dcompDevice->Release(); dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 0xAA000105;

    IDXGIAdapter* adapter = NULL;
    dxgiDev->GetAdapter(&adapter);
    IDXGIFactory2* factory = NULL;
    hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    adapter->Release();
    if (FAILED(hr) || !factory) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE000009;
        dcompVisual->Release(); dcompTarget->Release(); dcompDevice->Release(); dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = texW;
    scDesc.Height = texH;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    IDXGISwapChain1* swapChain = NULL;
    hr = factory->CreateSwapChainForComposition(device, &scDesc, NULL, &swapChain);
    if (FAILED(hr)) {
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scDesc.Scaling = DXGI_SCALING_NONE;
        hr = factory->CreateSwapChainForComposition(device, &scDesc, NULL, &swapChain);
    }
    factory->Release();
    if (FAILED(hr) || !swapChain) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE00000A;
        dcompVisual->Release(); dcompTarget->Release(); dcompDevice->Release(); dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 0xAA000106;

    dcompVisual->SetContent(swapChain);
    dcompTarget->SetRoot(dcompVisual);
    dcompDevice->Commit();

    g_data->ready = 0xAA000107;

    ID3D11Texture2D* backBuf = NULL;
    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuf);
    if (FAILED(hr) || !backBuf) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE00000B;
        swapChain->Release(); dcompVisual->Release(); dcompTarget->Release(); dcompDevice->Release(); dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    ID3D11RenderTargetView* rtv = NULL;
    hr = device->CreateRenderTargetView(backBuf, NULL, &rtv);
    if (FAILED(hr) || !rtv) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE00000C;
        backBuf->Release(); swapChain->Release(); dcompVisual->Release(); dcompTarget->Release(); dcompDevice->Release(); dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 0xAA000108;

    ID3D11Texture2D* sharedTex = NULL;
    hr = device->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void**)&sharedTex);
    if (FAILED(hr) || !sharedTex) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE00000D;
        rtv->Release(); backBuf->Release(); swapChain->Release(); dcompVisual->Release(); dcompTarget->Release(); dcompDevice->Release(); dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    IDXGIKeyedMutex* mutex = NULL;
    hr = sharedTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&mutex);
    if (FAILED(hr) || !mutex) {
        g_data->error_hr = (int32_t)hr; g_data->ready = 0xEE00000E;
        sharedTex->Release(); rtv->Release(); backBuf->Release(); swapChain->Release(); dcompVisual->Release(); dcompTarget->Release(); dcompDevice->Release(); dxgiDev->Release();
        if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
        device->Release(); DestroyWindow(hwnd); CoUninitialize();
        UnmapViewOfFile(g_data); CloseHandle(g_map); g_data = NULL; g_map = NULL;
        return 1;
    }

    g_data->ready = 2;

    float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t last_gen = g_data->generation;

    while (g_data->signal != 2 && WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        uint32_t cur_gen = g_data->generation;
        if (cur_gen == last_gen) {
            if (frame_event) WaitForSingleObject(frame_event, 1);
            continue;
        }
        last_gen = cur_gen;

        hr = mutex->AcquireSync(1, 0);
        if (hr == S_OK) {
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ctx->CopyResource(backBuf, sharedTex);
            mutex->ReleaseSync(0);
            swapChain->Present(0, 0);
        } else {
            if (frame_event) WaitForSingleObject(frame_event, 1);
        }
    }

done:
    ShowCursor(TRUE);
    if (mutex) mutex->Release();
    if (sharedTex) sharedTex->Release();
    if (rtv) rtv->Release();
    if (backBuf) backBuf->Release();
    swapChain->Release();
    dcompVisual->Release();
    dcompTarget->Release();
    dcompDevice->Release();
    dxgiDev->Release();
    if (ctx) { ctx->ClearState(); ctx->Flush(); ctx->Release(); }
    device->Release();
    DestroyWindow(hwnd);
    if (g_blank_cursor) DestroyCursor(g_blank_cursor);
    CoUninitialize();
    UnmapViewOfFile(g_data);
    CloseHandle(g_map);
    g_data = NULL; g_map = NULL;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)hModule; (void)lpReserved;
    if (reason == DLL_PROCESS_ATTACH) {
        typedef BOOL(WINAPI* pfnSetProcessDpiAwarenessContext)(void*);
        wchar_t u32c[32];
        build_user32(u32c, 32);
        pfnSetProcessDpiAwarenessContext pSetCtx = (pfnSetProcessDpiAwarenessContext)GetProcAddress(GetModuleHandleW(u32c), "SetProcessDpiAwarenessContext");
        SecureZeroMemory(u32c, sizeof(u32c));
        if (pSetCtx) pSetCtx((void*)(intptr_t)-4);
        g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!g_stop_event) return FALSE;
        g_overlay_thread = CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
        if (!g_overlay_thread) { CloseHandle(g_stop_event); return FALSE; }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_data) g_data->signal = 2;
        if (g_stop_event) SetEvent(g_stop_event);
        if (g_overlay_thread) {
            WaitForSingleObject(g_overlay_thread, 5000);
            CloseHandle(g_overlay_thread);
        }
        if (g_stop_event) CloseHandle(g_stop_event);
    }
    return TRUE;
}