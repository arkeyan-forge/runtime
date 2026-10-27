#include "host/log.h"
#include "host/settings.h"
#include "collection/datadir.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "env.h"
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#endif

#ifndef SSA_BUILD
#define SSA_BUILD "unknown"
#endif

namespace
{
    std::string g_path;
    constexpr int StderrFile = 2;

#ifdef _WIN32
    std::wstring Wide(const std::string& text)
    {
        std::wstring wide(text.size() + 1, L'\0');
        int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), int(wide.size()));
        wide.resize(length > 0 ? size_t(length - 1) : 0);
        return wide;
    }

    // Shared with readers (the launcher copies the log while the game runs).
    int OpenFile(const std::string& path, bool write)
    {
        int fd = -1;
        int flags = write ? _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY : _O_RDONLY | _O_BINARY;
        _wsopen_s(&fd, Wide(path).c_str(), flags, _SH_DENYNO, _S_IREAD | _S_IWRITE);
        return fd;
    }
#else
    int OpenFile(const std::string& path, bool write)
    {
        return write ? open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
#endif

    // The terminal (or whatever stderr was) gets the lines from the file, so that the file itself is written directly
    // by every print: nothing is lost if the process dies. Raw OS reads: the C runtime's _read stops at the first end
    // of file on Windows.
    void Mirror(int terminal, int file)
    {
#ifdef _WIN32
        HANDLE input = reinterpret_cast<HANDLE>(_get_osfhandle(file)), output = reinterpret_cast<HANDLE>(_get_osfhandle(terminal));
#endif
        char buffer[4096];
        while (true)
        {
#ifdef _WIN32
            DWORD size = 0, written = 0;
            if (ReadFile(input, buffer, sizeof(buffer), &size, nullptr) && size > 0)
                WriteFile(output, buffer, size, &written, nullptr);
#else
            ssize_t size = read(file, buffer, sizeof(buffer));
            if (size > 0)
                write(terminal, buffer, size_t(size));
#endif
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::string OsName()
    {
#ifdef _WIN32
        std::string name = "Windows";
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
        if (auto get = ntdll != nullptr ? reinterpret_cast<RtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr)
        {
            OSVERSIONINFOW info{ sizeof(info) };
            if (get(&info) == 0)
                name += " " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + " build " +
                    std::to_string(info.dwBuildNumber);
        }
        using WineGetVersion = const char*(CDECL*)();
        if (auto wine = ntdll != nullptr ? reinterpret_cast<WineGetVersion>(GetProcAddress(ntdll, "wine_get_version")) : nullptr)
            name += std::string(" (Wine ") + wine() + ")";
        return name;
#else
        std::string name = SDL_GetPlatform();
#ifdef __APPLE__
        char version[64] = {};
        size_t size = sizeof(version);
        if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) == 0)
            name += std::string(" ") + version;
#endif
        utsname system{};
        if (uname(&system) == 0)
            name += std::string(" (") + system.sysname + " " + system.release + ", " + system.machine + ")";
        return name;
#endif
    }

    std::string CpuName()
    {
        std::string name;
#if defined(__x86_64__) || defined(_M_X64)
        unsigned int registers[12] = {};
        unsigned int highest = __get_cpuid_max(0x80000000, nullptr);
        for (unsigned int i = 0; i < 3 && highest >= 0x80000004; i++)
            __get_cpuid(0x80000002 + i, &registers[i * 4], &registers[i * 4 + 1], &registers[i * 4 + 2], &registers[i * 4 + 3]);
        name.assign(reinterpret_cast<const char*>(registers), sizeof(registers));
        name = name.c_str();
#elif defined(__APPLE__)
        char brand[128] = {};
        size_t size = sizeof(brand);
        if (sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0)
            name = brand;
#endif
        size_t begin = name.find_first_not_of(' ');
        return begin == std::string::npos ? "unknown" : name.substr(begin);
    }
}

void LogInit()
{
    if (const char* chosen = skyenv::Get("SKY_LOG"))
        g_path = chosen;
    else if (std::filesystem::path folder = datadir::Game(SSA_GAME_ID); !folder.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        g_path = datadir::Text(folder / "SSARecomp.log");
    }
    if (g_path.empty())
        return;

    std::string previous = g_path.size() > 4 && g_path.substr(g_path.size() - 4) == ".log"
        ? g_path.substr(0, g_path.size() - 4) + ".prev.log" : g_path + ".prev";
    SDL_PathInfo info;
    if (SDL_GetPathInfo(g_path.c_str(), &info))
        SDL_RenamePath(g_path.c_str(), previous.c_str());

    int file = OpenFile(g_path, true);
    if (file < 0)
    {
        fprintf(stderr, "[log] cannot write %s\n", g_path.c_str());
        g_path.clear();
        return;
    }
    fflush(stderr);
    int terminal = dup(StderrFile);
    int reader = terminal >= 0 ? OpenFile(g_path, false) : -1;
    dup2(file, StderrFile);
#ifdef _WIN32
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(StderrFile)));
#endif
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (reader >= 0)
        std::thread(Mirror, terminal, reader).detach();
}

void LogHeader(const RuntimeSettings& settings)
{
    fprintf(stderr, "[log] %s\n", g_path.empty() ? "no log file" : g_path.c_str());
    fprintf(stderr, "[version] SSARecomp %s, build %s\n", SSA_VERSION, SSA_BUILD);
    fprintf(stderr, "[system] %s\n", OsName().c_str());
    fprintf(stderr, "[system] CPU %s, %d logical cores, AVX %s, AVX2 %s; %d MB RAM\n", CpuName().c_str(),
        SDL_GetNumLogicalCPUCores(), SDL_HasAVX() ? "yes" : "no", SDL_HasAVX2() ? "yes" : "no", SDL_GetSystemRAM());
    fprintf(stderr, "[settings] %s\n", DescribeSettings(settings).c_str());
}
