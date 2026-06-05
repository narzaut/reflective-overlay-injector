#pragma pack(push, 1)
typedef struct {
    volatile uint32_t signal;
    volatile uint32_t generation;
    volatile int32_t  screen_w;
    volatile int32_t  screen_h;
    volatile uint32_t ready;
    uint32_t          _pad0;
    uint64_t hwnd;
    uint64_t shared_texture_handle;
    volatile int32_t  error_hr;
} PayloadSharedData;
#pragma pack(pop)
