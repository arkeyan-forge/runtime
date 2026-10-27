// Replacements for Win32 functions that the static C++ runtime imports but that are not part of the UWP API
// surface (WindowsApp umbrella), so the executable only depends on DLLs every Windows 10 device family has.
// Defining __imp_<name> here takes precedence over the import libraries.
#include <windows.h>

extern "C"
{
    // libc++ std::thread::hardware_concurrency(). Xbox apps run in a single processor group.
    static DWORD WINAPI UwpGetActiveProcessorCount(WORD)
    {
        SYSTEM_INFO info;
        GetNativeSystemInfo(&info);
        return info.dwNumberOfProcessors;
    }
    DWORD(WINAPI* __imp_GetActiveProcessorCount)(WORD) = UwpGetActiveProcessorCount;

    // The translated game code uses setjmp and longjmp. mingw's headers import _setjmp, which the CRT import
    // libraries bind to api-ms-win-crt-private (outside the UWP surface). libucrtapp carries its own _setjmp and
    // longjmp (and already points __imp_longjmp at the latter): point the setjmp import at its _setjmp too.
    int __cdecl _setjmp(void* buffer, void* frame);
    int(__cdecl* __imp__setjmp)(void*, void*) = _setjmp;
}
