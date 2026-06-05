#pragma once

#ifdef VERBOSE
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <cmath>

template<size_t N, char K>
struct XorStr {
    char d[N];
    constexpr XorStr(const char (&s)[N]) : d{} { for (size_t i = 0; i < N; i++) d[i] = s[i] ^ K; }
    void dec(char* o) const { for (size_t i = 0; i < N; i++) o[i] = d[i] ^ K; }
    operator std::string() const { char b[N]; dec(b); b[N-1] = '\0'; return std::string(b); }
};

#define XS(s) (XorStr<sizeof(s), (char)(sizeof(s)^0x5A)>(s).operator std::string())

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <dcomp.h>