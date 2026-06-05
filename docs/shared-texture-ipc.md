# Shared Texture IPC

The injector process and the overlay DLL (inside explorer.exe) communicate through two IPC mechanisms: a named shared memory region for control data, and a shared D3D11 texture for frame data. No sockets, pipes, or file I/O connect the two processes.

**Source:** `src/injector/payload_shared.h`, `src/injector/payload_dll.cpp`

## PayloadSharedData

The control channel is a `PAGE_READWRITE` file mapping shared between the injector and explorer.exe:

```cpp
#pragma pack(push, 1)
typedef struct {
    volatile uint32_t signal;           // 0=idle, 1=frame ready, 2=shutdown
    volatile uint32_t generation;       // frame counter (monotonically increasing)
    volatile int32_t  screen_w;         // screen width
    volatile int32_t  screen_h;         // screen height
    volatile uint32_t ready;            // init state machine
    uint32_t          _pad0;
    uint64_t hwnd;                      // window handle
    uint64_t shared_texture_handle;     // NT handle to shared D3D11 texture
    volatile int32_t  error_hr;         // HRESULT from failed init
} PayloadSharedData;
#pragma pack(pop)
```

### Named Mapping Creation

The shared memory name is constructed at runtime using XOR encoding + FNV-1a hash of the explorer PID:

```cpp
build_shm_name(shm_buf, 64, explorer_pid);
// Result: "Local\<XOR_decoded_base>_<FNV1A_PID_hex>"
```

This ensures:
1. The name is not a plaintext string in the binary
2. Each injection uses a unique name (PID-dependent)
3. The name cannot be predicted without knowing the target PID

The name buffer is zeroed with `SecureZeroMemory` after use.

### Initialization State Machine

The `ready` field tracks the overlay DLL's initialization progress:

| Value | Meaning |
|---|---|
| `0xAA000001` | Shared memory opened |
| `0xAA000002` | COM initialized |
| `0xAA000003` | Window class registered |
| `1` | Window created, ready for texture handle |
| `0xAA000100` | Texture handle received |
| `0xAA000101`–`0xAA000108` | D3D11/DComp setup stages |
| `2` | Fully initialized, frame loop running |
| `0xEE000001`–`0xEE00000E` | Error at specific stage (lower byte = stage) |
| `0xFFFFFFFF` | Window creation failed |

The injector polls this field after injection, waiting up to 20 seconds for `ready == 2`.

## Shared D3D11 Texture

### Creation (Injector Side)

The injector creates a D3D11 texture with the `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` flag:

```cpp
D3D11_TEXTURE2D_DESC texDesc = {};
texDesc.Width = screen_w;
texDesc.Height = screen_h;
texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
texDesc.Usage = D3D11_USAGE_DEFAULT;
texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

device->CreateTexture2D(&texDesc, nullptr, &sharedTex);
```

The `SHARED_KEYEDMUTEX` flag creates a texture that:
1. Can be shared between processes via an NT handle
2. Has a built-in synchronization primitive (keyed mutex)

### Handle Passing

The shared handle is passed through the `PayloadSharedData` struct:

```cpp
// Injector side:
octx.shared_mem->shared_texture_handle = (uint64_t)(uintptr_t)d3.sharedHandle;

// Overlay side:
HANDLE sharedHandle = (HANDLE)(uintptr_t)g_data->shared_texture_handle;
device->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), &sharedTex);
```

`OpenSharedResource` opens the same GPU texture in the overlay's D3D11 device. Both processes now have access to the same physical GPU memory.

### Keyed Mutex Protocol

The keyed mutex uses two keys:
- **Key 0**: Writer (injector) holds the lock
- **Key 1**: Reader (overlay) holds the lock

```
Injector:                        Overlay:
AcquireSync(0, 200)              AcquireSync(1, 0)
  ↓ (locked for writing)           ↓ (waits for key 1)
Write frame to texture
ReleaseSync(1)                   ← unblocked (key 1 acquired)
                                    CopyResource(backBuf, sharedTex)
                                    ReleaseSync(0)
                                    Present(0, 0)
```

The injector acquires key 0 (write lock), writes frame data, then releases with key 1 (signals overlay). The overlay acquires key 1 (read lock), copies to its back buffer, releases with key 0 (signals injector), and presents.

The 200ms timeout on the injector's acquire prevents deadlock if the overlay crashes while holding the lock.

## Frame Synchronization

The `generation` counter provides frame-level synchronization:

1. Injector writes a frame to the shared texture
2. Injector increments `generation`
3. Overlay polls `generation` — if unchanged, sleeps
4. Overlay detects new generation → acquires mutex → copies → presents

This avoids the overlay presenting stale frames and the injector overwriting a frame the overlay is still reading.

## Shutdown Protocol

The injector signals shutdown by setting `signal = 2`:

```cpp
ctx.shared_mem->signal = 2;
Sleep(500);  // give overlay time to notice
```

The overlay's frame loop checks `g_data->signal != 2` and exits when it sees the shutdown signal. The overlay then releases all D3D11/DComp resources and unmaps the shared memory.
