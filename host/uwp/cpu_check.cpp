// The Xbox Series package (SSA_UWP_MARCH=x86-64-v3) uses AVX2, FMA and BMI2, which the Xbox One's Jaguar CPU lacks.
// This file is compiled for the baseline (-march=btver2, CMakeLists.txt) and its check runs as the first static
// constructor, before any code built for the newer CPU: on a CPU without them it writes LocalState\SSARecomp.log and
// exits, instead of crashing on an illegal instruction. Raw WinRT calls only (no templates shared with other files).
#if defined(SSA_REQUIRE_X86_64_V3)
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.storage.h>

namespace
{
    using namespace ABI::Windows::Storage;

    // LocalState\SSARecomp.log, as the host writes it (host/uwp/main.cpp).
    void WriteLog(const char* text)
    {
        RoInitialize(RO_INIT_MULTITHREADED);
        HSTRING className = nullptr, path = nullptr;
        const wchar_t name[] = L"Windows.Storage.ApplicationData";
        WindowsCreateString(name, UINT32(sizeof(name) / sizeof(name[0]) - 1), &className);
        IApplicationDataStatics* statics = nullptr;
        IApplicationData* data = nullptr;
        IStorageFolder* folder = nullptr;
        IStorageItem* item = nullptr;
        if (SUCCEEDED(RoGetActivationFactory(
                className, __uuidof(IApplicationDataStatics), reinterpret_cast<void**>(&statics))) &&
            SUCCEEDED(statics->get_Current(&data)) && SUCCEEDED(data->get_LocalFolder(&folder)) &&
            SUCCEEDED(folder->QueryInterface(__uuidof(IStorageItem), reinterpret_cast<void**>(&item))) &&
            SUCCEEDED(item->get_Path(&path)))
        {
            wchar_t file[MAX_PATH] = {};
            UINT32 length = 0;
            const wchar_t* folderPath = WindowsGetStringRawBuffer(path, &length);
            if (length + 16 < MAX_PATH)
            {
                for (UINT32 i = 0; i < length; i++)
                    file[i] = folderPath[i];
                const wchar_t leaf[] = L"\\SSARecomp.log";
                for (UINT32 i = 0; leaf[i] != 0; i++)
                    file[length + i] = leaf[i];
                HANDLE handle = CreateFile2(file, GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
                if (handle != INVALID_HANDLE_VALUE)
                {
                    DWORD written = 0, size = 0;
                    while (text[size] != 0)
                        size++;
                    WriteFile(handle, text, size, &written, nullptr);
                    CloseHandle(handle);
                }
            }
        }
        if (item != nullptr)
            item->Release();
        if (folder != nullptr)
            folder->Release();
        if (data != nullptr)
            data->Release();
        if (statics != nullptr)
            statics->Release();
        WindowsDeleteString(path);
        WindowsDeleteString(className);
    }

    __attribute__((constructor(101))) void CheckCpu()
    {
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2"))
            return;
        WriteLog(
            "This package is built for the Xbox Series X|S CPU (AVX2, FMA, BMI2), which this console does not have.\r\n"
            "Install the standard package (SSARecomp-<version>.msix, not -series) on an Xbox One.\r\n");
        ExitProcess(1);
    }
}
#endif
