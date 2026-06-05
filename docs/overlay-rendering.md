# Window Presentation (DirectComposition)

The injected DLL creates a transparent fullscreen window through DirectComposition — not `WS_EX_LAYERED`. The window is invisible to screen capture, input-passthrough, and composites as a standard visual through DWM.

**Source:** `src/injector/payload_dll.cpp`

## Why DirectComposition

Traditional overlay windows use `CreateWindowEx` + `WS_EX_LAYERED` + a DXGI swap chain with `Present()`. These are detectable:

- `EnumWindows` finds the overlay HWND
- `GetWindowLong` reveals `WS_EX_LAYERED`
- `BitBlt` / `PrintWindow` / DXGI Desktop Duplication captures it
- `CreateSwapChainForHwnd` ties a swap chain to the HWND

DirectComposition composites visuals through the DWM pipeline. The visual tree attaches to a window with `WS_EX_NOREDIRECTIONBITMAP` — no backing surface. The swap chain is a composition swap chain (`CreateSwapChainForComposition`), not a DXGI one. There is no `Present()` to a display adapter. Everything composites through DWM directly.

## Window Creation

### CreateWindowInBand

Uses the undocumented `CreateWindowInBand` API from user32.dll with Z-band 4:

```cpp
typedef HWND(WINAPI* pfnCreateWindowInBand)(DWORD, LPCWSTR, LPCWSTR,
    DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID, DWORD);

hwnd = pCreateWindowInBand(
    WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
    class_name, L"", WS_POPUP,
    0, 0, screen_w, screen_h,
    NULL, NULL, hInstance, NULL,
    4  // Z-band: above desktop, below topmost
);
```

Z-band 4 places the window above the desktop layer but below other topmost windows. Falls back to `CreateWindowExW` if `CreateWindowInBand` is unavailable (older Windows builds).

### Window Styles

| Style | Purpose |
|---|---|
| `WS_EX_NOREDIRECTIONBITMAP` | No backing surface — DWM does not allocate a redirection bitmap. Invisible to `PrintWindow` and `BitBlt`. |
| `WS_EX_TOPMOST` | Stays above non-topmost windows |
| `WS_EX_NOACTIVATE` | Does not steal focus |
| `WS_EX_TOOLWINDOW` | Not in taskbar or Alt+Tab |

### Display Affinity

```cpp
SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE); // 0x00000002
```

Makes the window invisible to: `BitBlt`, `PrintWindow`, DXGI Desktop Duplication, Windows.Graphics.Capture, Game Bar, OBS. Screenshots and recordings will not show the overlay.

### Input Passthrough

```cpp
// WM_NCHITTEST → HTCLIENT makes the window transparent to clicks
// WM_SETCURSOR → SetCursor(NULL) hides the cursor over the overlay
// Blank 1x1 cursor for extra safety
```

### DPI Awareness

Both the injector and the injected DLL set per-monitor DPI awareness via `SetThreadDpiAwarenessContext(-4)` before creating the window, ensuring the overlay covers the correct screen region regardless of DPI scaling.

## DComp Visual Tree

```
IDCompositionDevice (created via DCompositionCreateDevice)
  └── IDCompositionTarget (attached to HWND)
        └── IDCompositionVisual
              └── IDXGISwapChain1 (composition swap chain)
                    └── Back buffer (ID3D11Texture2D)
```

### Swap Chain Configuration

```cpp
DXGI_SWAP_CHAIN_DESC1 scDesc = {};
scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
scDesc.BufferCount = 2;
scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
scDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

factory->CreateSwapChainForComposition(device, &scDesc, NULL, &swapChain);
```

`CreateSwapChainForComposition` creates a swap chain composited by DWM — no `Present()` to a display output, only `Present(0, 0)` to the composition engine. This is the key difference from a traditional DXGI overlay.

### Visual Attachment

```cpp
dcompVisual->SetContent(swapChain);
dcompTarget->SetRoot(dcompVisual);
dcompDevice->Commit();
```

The visual's content is the swap chain. The target attaches it to the overlay HWND. `Commit()` applies changes to the DWM composition tree.

## Frame Loop

The injected DLL runs a message loop waiting for new frames:

```cpp
while (g_data->signal != 2 && WaitForSingleObject(g_stop_event, 0) != WAIT_OBJECT_0) {
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { ... }

    uint32_t cur_gen = g_data->generation;
    if (cur_gen == last_gen) {
        if (frame_event) WaitForSingleObject(frame_event, 1);
        continue;
    }
    last_gen = cur_gen;

    hr = mutex->AcquireSync(1, 0);
    if (hr == S_OK) {
        ctx->CopyResource(backBuf, sharedTex);
        mutex->ReleaseSync(0);
        swapChain->Present(0, 0);
    }
}
```

The generation counter ensures the DLL only presents when a new frame is available. The keyed mutex synchronizes texture access (see `shared-texture-ipc.md`).
