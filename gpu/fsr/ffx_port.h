// Forced into every file that includes the FidelityFX SDK (gpu/fsr/fsr.cmake): the few Windows functions its host code
// calls, for the other compilers. It builds with two-byte wchar_t (-fshort-wchar), so the C library's wide string
// functions, made for four bytes, are replaced by these.
#pragma once
#include <stddef.h>
#include <string.h>
#if defined(_WIN32) // MinGW: _countof, wcscpy_s and swprintf_s come with these, which the SDK does not include itself
#ifndef MINGW_HAS_SECURE_API
#define MINGW_HAS_SECURE_API 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#endif

#if defined(__cplusplus)
#include <bit> // ffx_util.h uses std::popcount in C++20 without including it
#endif

#if defined(__cplusplus) && !defined(_WIN32) // Windows has them, with a two-byte wchar_t
#include <FidelityFX/host/ffx_util.h>
#include <cwchar> // before the wcscmp below: the C library's own declarations stay as they are (glibc's wchar.h)

template <typename T, size_t N>
constexpr size_t _countof(T (&)[N])
{
    return N;
}

inline int ffx_port_wcscmp(const wchar_t* a, const wchar_t* b)
{
    while (*a != 0 && *a == *b)
        ++a, ++b;
    return int(*a) - int(*b);
}

template <size_t N>
inline int wcscpy_s(wchar_t (&destination)[N], const wchar_t* source)
{
    size_t i = 0;
    for (; i + 1 < N && source[i] != 0; ++i)
        destination[i] = source[i];
    destination[i] = 0;
    return 0;
}

#define wcscmp ffx_port_wcscmp

// The optical flow names its passes with std::wstring and std::to_wstring (ffx_opticalflow.cpp, the only SDK file that
// uses them: FFX_PORT_WSTRING, framegen.cmake). The C++ library is built for a four-byte wchar_t: once its string code
// is not inlined (the kits' -O2 build), a two-byte string corrupts the heap at the first frame generated. A two-byte
// string of our own takes their names in that file (the headers it includes come first, so theirs stay as they are).
#if defined(FFX_PORT_WSTRING)
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
namespace std
{
    struct ffx_port_wstring
    {
        wchar_t text[64] = {};
        size_t length = 0;
        ffx_port_wstring() = default;
        ffx_port_wstring(const wchar_t* source) { append(source); }
        void append(const wchar_t* source)
        {
            while (*source != 0 && length + 1 < _countof(text))
                text[length++] = *source++;
            text[length] = 0;
        }
        const wchar_t* c_str() const { return text; }
        friend ffx_port_wstring operator+(ffx_port_wstring a, const ffx_port_wstring& b)
        {
            a.append(b.text);
            return a;
        }
    };

    inline ffx_port_wstring ffx_port_to_wstring(unsigned long long value)
    {
        wchar_t digits[24];
        size_t count = 0;
        do
            digits[count++] = wchar_t(L'0' + value % 10);
        while ((value /= 10) != 0);
        ffx_port_wstring text;
        while (count > 0)
        {
            const wchar_t digit[2] = { digits[--count], 0 };
            text.append(digit);
        }
        return text;
    }
} // namespace std
#define wstring ffx_port_wstring
#define to_wstring ffx_port_to_wstring
#endif // FFX_PORT_WSTRING
#endif
