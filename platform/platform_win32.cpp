// Windows 10+ desktop. The UWP build (Xbox Dev Mode) compiles this file with WINAPI_FAMILY=WINAPI_FAMILY_APP, so
// it only uses calls of the SDK's APP partition, except where marked desktop-only.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/platform.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <windows.h>
#include <timeapi.h>

extern "C" IMAGE_DOS_HEADER __ImageBase; // this executable, provided by the linker

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
// In the UWP API surface (windowsapp), but declared for the desktop partition only.
extern "C" WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
#endif

namespace
{
    void (*g_onFault)(const char* description, const void* address);

    // Set when the guest space could only be reserved: its pages are then committed on first access.
    uint8_t* g_lazyBase;
    size_t g_lazySize;

    void* Commit(void* address, size_t size, ULONG protection)
    {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        return VirtualAlloc(address, size, MEM_COMMIT, protection);
#else
        return VirtualAllocFromApp(address, size, MEM_COMMIT, protection);
#endif
    }

    // Commits the 64 KB granule holding `address` if it lies in the lazily committed guest space and is only reserved.
    bool CommitOnDemand(const void* address)
    {
        auto* byte = static_cast<const uint8_t*>(address);
        if (g_lazyBase == nullptr || byte < g_lazyBase || byte >= g_lazyBase + g_lazySize)
            return false;
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(byte, &info, sizeof(info)) == 0)
            return false;
        // Another thread may have committed it since this access faulted: then retrying is enough.
        if (info.State == MEM_COMMIT)
            return info.Protect == PAGE_READWRITE;
        if (info.State != MEM_RESERVE)
            return false;
        auto* granule = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(byte) & ~uintptr_t(0xFFFF));
        return Commit(granule, 0x10000, PAGE_READWRITE) != nullptr;
    }

    // First access to a reserved guest page. Runs before the unhandled-exception filter; anything else is passed on.
    LONG WINAPI OnFirstAccess(EXCEPTION_POINTERS* pointers)
    {
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2 &&
            CommitOnDemand(reinterpret_cast<const void*>(record.ExceptionInformation[1])))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    std::wstring Widen(const char* utf8)
    {
        int length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (length <= 1)
            return {};
        std::wstring wide(length - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), length);
        return wide;
    }

    // Each frame as module+offset. The loader rewrites the in-memory ImageBase to where ASLR put the executable, so the
    // executable's frames are printed at the linker's base (0x140000000) for llvm-symbolizer and addr2line.
    void PrintFrame(const void* address)
    {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH];
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(address), &module) ||
            GetModuleFileNameW(module, path, MAX_PATH) == 0)
        {
            fprintf(stderr, "  %p\n", address);
            return;
        }
        auto offset = static_cast<unsigned long long>(
            static_cast<const uint8_t*>(address) - reinterpret_cast<const uint8_t*>(module));
        const wchar_t* name = wcsrchr(path, L'\\') != nullptr ? wcsrchr(path, L'\\') + 1 : path;
        if (module == reinterpret_cast<HMODULE>(&__ImageBase))
            fprintf(stderr, "  0x%llX (executable +0x%llX)\n", 0x140000000ull + offset, offset);
        else
            fprintf(stderr, "  %p %ls+0x%llX\n", address, name, offset);
    }

    // An unhandled-exception filter rather than a vectored handler: vectored handlers also see first-chance faults
    // that DLLs (GPU drivers) handle themselves, and AddVectoredExceptionHandler is desktop-only in the SDK headers.
    LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers)
    {
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        const void* address = record.ExceptionAddress;
        if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            record.NumberParameters >= 2)
        {
            address = reinterpret_cast<const void*>(record.ExceptionInformation[1]);
        }

        char description[32];
        snprintf(description, sizeof(description), "exception 0x%08lX", record.ExceptionCode);
        g_onFault(description, address);

        fprintf(stderr, "Executable loaded at %p (frames below at the linker's base 0x140000000)\n",
            static_cast<void*>(&__ImageBase));
        fprintf(stderr, "Faulting instruction:\n");
        PrintFrame(record.ExceptionAddress);
        fprintf(stderr, "Backtrace:\n");
        void* frames[64];
        USHORT count = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
        for (USHORT i = 0; i < count; i++)
            PrintFrame(frames[i]);
        _Exit(1);
    }

    // abort() (the runtime's own fatal errors) does not reach the unhandled-exception filter: same report from SIGABRT.
    void OnAbort(int)
    {
        g_onFault("abort", nullptr);
        fprintf(stderr, "Backtrace:\n");
        void* frames[64];
        USHORT count = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
        for (USHORT i = 0; i < count; i++)
            PrintFrame(frames[i]);
        _Exit(1);
    }

    DWORD WINAPI ThreadMain(void* argument)
    {
        std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()>*>(argument));
        (*body)();
        return 0;
    }
}

void PlatformInit()
{
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    // Sleeps and timed waits (the 1 ms KeTimeStampBundle tick, VBlank pacing, KeDelayExecutionThread) otherwise
    // round up to the 15.6 ms system tick. Desktop-only: UWP has no equivalent.
    timeBeginPeriod(1);
#endif
}

void PlatformInstallFaultHandler(void (*onFault)(const char* description, const void* address))
{
    g_onFault = onFault;
    SetUnhandledExceptionFilter(OnUnhandledException);
    signal(SIGABRT, OnAbort);
}

uint8_t* PlatformReserveMemory(size_t size)
{
    // Committed up front, as the guest may touch any of it. Unlike mmap's MAP_NORESERVE, this counts against the
    // system commit limit (RAM + page file) immediately, though pages still take memory only once touched.
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    void* memory = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* memory = VirtualAllocFromApp(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#endif
    if (memory != nullptr)
        return static_cast<uint8_t*>(memory);

    // Over the commit limit (no page file, or a console's per-title budget: about 5 GB for an Xbox One game): reserve
    // only, and commit 64 KB at a time where the guest first touches it (PlatformReadFile covers kernel-mode writes).
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    memory = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
#else
    memory = VirtualAllocFromApp(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
#endif
    if (memory == nullptr || AddVectoredExceptionHandler(1, OnFirstAccess) == nullptr)
        return nullptr;
    g_lazyBase = static_cast<uint8_t*>(memory);
    g_lazySize = size;
    fprintf(
        stderr, "[platform] cannot commit %zu MB up front: guest memory is committed on first access\n", size >> 20);
    return g_lazyBase;
}

bool PlatformProtectNoAccess(void* address, size_t size)
{
    ULONG previous;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    if (VirtualProtect(address, size, PAGE_NOACCESS, &previous))
        return true;
#else
    if (VirtualProtectFromApp(address, size, PAGE_NOACCESS, &previous))
        return true;
#endif
    // Only reserved (lazy commit): committing it inaccessible keeps OnFirstAccess away from it.
    return Commit(address, size, PAGE_NOACCESS) != nullptr;
}

bool PlatformProtectReadOnly(void* address, size_t size, bool readOnly)
{
    ULONG previous;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    return VirtualProtect(address, size, readOnly ? PAGE_READONLY : PAGE_READWRITE, &previous);
#else
    return VirtualProtectFromApp(address, size, readOnly ? PAGE_READONLY : PAGE_READWRITE, &previous);
#endif
}

size_t PlatformPageSize()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}

namespace
{
    bool (*volatile g_writeFaultFilter)(void* address);

    // A write to a page write_watch.cpp made read-only; anything else goes on to the other handlers. ExceptionInformation:
    // [0] 1 for a write, [1] the address.
    LONG WINAPI OnWriteFault(EXCEPTION_POINTERS* pointers)
    {
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        bool (*filter)(void*) = g_writeFaultFilter;
        if (filter != nullptr && record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2 &&
            record.ExceptionInformation[0] == 1 && filter(reinterpret_cast<void*>(record.ExceptionInformation[1])))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

bool PlatformInstallWriteFaultFilter(bool (*handled)(void* address))
{
    g_writeFaultFilter = handled;
    static void* const installed = AddVectoredExceptionHandler(1, OnWriteFault);
    return installed != nullptr;
}

void PlatformStartThread(size_t stackSize, std::function<void()> body)
{
    // The stack size is a reservation: pages are committed as the stack grows.
    HANDLE thread = CreateThread(nullptr, stackSize, ThreadMain, new std::function<void()>(std::move(body)),
        STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
}

void PlatformSetThreadName(const char* name) { SetThreadDescription(GetCurrentThread(), Widen(name).c_str()); }

void PlatformSetThreadTimingCritical(uint32_t)
{
    // timeBeginPeriod(1) (PlatformInit) already makes timed waits precise to about 1 ms.
}

PlatformFile PlatformOpenFile(const char* path)
{
    // CreateFile2 is the call both desktop and UWP have. A UWP app can only open paths it has access to
    // (CreateFile2FromAppW with the broadFileSystemAccess capability for a user-chosen ISO).
    HANDLE file = CreateFile2(Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    return reinterpret_cast<PlatformFile>(file); // INVALID_HANDLE_VALUE is -1
}

int64_t PlatformReadFile(PlatformFile file, void* buffer, uint32_t size, uint64_t offset)
{
    // The kernel writing into a page that is only reserved fails the read instead of raising an access violation.
    if (g_lazyBase != nullptr)
    {
        auto* end = static_cast<uint8_t*>(buffer) + size;
        for (auto* page = static_cast<uint8_t*>(buffer); page < end; page += 0x10000)
            CommitOnDemand(page);
        if (size != 0)
            CommitOnDemand(end - 1);
    }

    // On a synchronous handle, ReadFile reads at the OVERLAPPED offset, so concurrent reads don't share a file pointer.
    OVERLAPPED overlapped{};
    overlapped.Offset = uint32_t(offset);
    overlapped.OffsetHigh = uint32_t(offset >> 32);
    DWORD read = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(file), buffer, size, &read, &overlapped))
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return read;
}

void PlatformCloseFile(PlatformFile file) { CloseHandle(reinterpret_cast<HANDLE>(file)); }
