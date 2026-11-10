#include "host/machine_state.h"
#include "gpu/frame_stats.h"
#include "d3d/render_stats.h"
#include "host/audio.h"
#include "host/glyphs.h"
#include "host/portal.h"
#include "host/texture_pack.h"
#include "memory.h"
#include "thread.h"
#include <SDL3/SDL.h>
#include "platform/platform.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <mach/mach.h>
#include <notify.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

void machine::ReadRuntime(Runtime& runtime)
{
    runtime = {};
    runtime.guestThreads = GuestThreadsRunning();
    runtime.heapUsed = g_physicalHeap.UsedBytes();
    runtime.heapSize = g_physicalHeap.SizeBytes();
    runtime.texturesKnown = d3d::TryCountTextures(runtime.textures);
    runtime.packFiles = texpack::Files();
    runtime.voices = AudioVoicesActive();
    runtime.underruns = AudioUnderruns();
    // The pad's type, not its name (a wireless pad's name can be one its owner gave it).
    int count = 0;
    SDL_JoystickID* pads = SDL_GetGamepads(&count);
    if (pads != nullptr && count > 0)
    {
        const char* type = SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(pads[0]));
        snprintf(
            runtime.input, sizeof(runtime.input), "pad (%s), %d connected", type != nullptr ? type : "unknown", count);
    }
    else
        snprintf(runtime.input, sizeof(runtime.input), "keyboard and mouse");
    SDL_free(pads);
    static constexpr const char* Styles[] = { "Xbox 360", "Xbox One", "PlayStation", "Nintendo", "keyboard",
        "PlayStation 3" };
    size_t style = size_t(glyphs::Current());
    snprintf(runtime.glyphs, sizeof(runtime.glyphs), "%s", style < std::size(Styles) ? Styles[style] : "n/a");
    // A seat's toy type: block 1's first two bytes, once the title read it (the rest is encrypted).
    for (int slot = 0; slot < PortalSlotCount && runtime.seats < std::size(runtime.seatToys); slot++)
    {
        PortalSlotSeen seen = PortalSeen(slot);
        if (seen.present)
            runtime.seatToys[runtime.seats++] =
                (seen.blocks & 2) != 0 ? uint32_t(seen.data[16]) | uint32_t(seen.data[17]) << 8 : 0;
    }
}

void machine::ReadCpu(Cpu& cpu)
{
    cpu = {};
    cpu.processKnown = PlatformProcessCpuNs(cpu.processNs);
    uint64_t clocks[gpu::framestats::MaxThreads];
    gpu::framestats::Thread kinds[gpu::framestats::MaxThreads];
    size_t count = gpu::framestats::Threads(clocks, kinds);
    for (size_t i = 0; i < count; i++)
    {
        uint64_t ns = 0;
        int kind = int(kinds[i]);
        if (kind < ThreadCount && PlatformThreadCpuNs(clocks[i], ns))
        {
            cpu.threadKnown[kind] = true;
            cpu.threadNs[kind] += ns;
            cpu.threads[kind]++;
        }
    }
}

#if !defined(__APPLE__) && !defined(_WIN32)
namespace
{
    // A small text file of /sys, into `text`; false when it cannot be read.
    bool ReadSys(const char* path, char* text, size_t capacity)
    {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        ssize_t size = read(fd, text, capacity - 1);
        close(fd);
        if (size <= 0)
            return false;
        text[size] = 0;
        text[strcspn(text, "\n")] = 0;
        return true;
    }
}
#endif

machine::System machine::ReadSystem()
{
    System system;
#if defined(__APPLE__)
    host_cpu_load_info_data_t load{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&load), &count) ==
        KERN_SUCCESS)
    {
        uint64_t busy = uint64_t(load.cpu_ticks[CPU_STATE_USER]) + load.cpu_ticks[CPU_STATE_SYSTEM] +
                        load.cpu_ticks[CPU_STATE_NICE];
        system.cpuBusy = busy;
        system.cpuTotal = busy + load.cpu_ticks[CPU_STATE_IDLE];
        system.cpuKnown = true;
    }
    vm_statistics64_data_t vm{};
    count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    int64_t total = 0;
    size_t size = sizeof(total);
    if (host_page_size(mach_host_self(), &page) == KERN_SUCCESS &&
        host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) ==
            KERN_SUCCESS &&
        sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0)
    {
        // As Activity Monitor's "memory used": app memory (active less purgeable), wired and compressed.
        uint64_t pages = uint64_t(vm.active_count) - std::min<uint64_t>(vm.purgeable_count, vm.active_count) +
                         vm.wire_count + vm.compressor_page_count;
        system.ramUsed = pages * page;
        system.ramTotal = uint64_t(total);
        system.ramKnown = true;
    }
#elif defined(_WIN32)
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user))
    {
        auto ticks = [](const FILETIME& t) { return uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime; };
        system.cpuTotal = ticks(kernel) + ticks(user); // kernel time includes the idle time
        system.cpuBusy = system.cpuTotal - ticks(idle);
        system.cpuKnown = true;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        system.ramTotal = memory.ullTotalPhys;
        system.ramUsed = memory.ullTotalPhys - memory.ullAvailPhys;
        system.ramKnown = true;
    }
#else
    char text[256];
    if (ReadSys("/proc/stat", text, sizeof(text)))
    {
        unsigned long long user = 0, nice = 0, kernel = 0, idle = 0, wait = 0, irq = 0, softirq = 0, steal = 0;
        if (sscanf(text, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &kernel, &idle, &wait, &irq,
                &softirq, &steal) >= 4)
        {
            system.cpuBusy = user + nice + kernel + irq + softirq + steal;
            system.cpuTotal = system.cpuBusy + idle + wait;
            system.cpuKnown = true;
        }
    }
    // MemTotal and MemAvailable are the first and third lines.
    int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        char info[512];
        ssize_t size = read(fd, info, sizeof(info) - 1);
        close(fd);
        if (size > 0)
        {
            info[size] = 0;
            unsigned long long totalKb = 0, availableKb = 0;
            const char* total = strstr(info, "MemTotal:");
            const char* available = strstr(info, "MemAvailable:");
            if (total && available && sscanf(total, "MemTotal: %llu", &totalKb) == 1 &&
                sscanf(available, "MemAvailable: %llu", &availableKb) == 1 && totalKb >= availableKb)
            {
                system.ramTotal = totalKb * 1024;
                system.ramUsed = (totalKb - availableKb) * 1024;
                system.ramKnown = true;
            }
        }
    }
#endif
    return system;
}

machine::Heat machine::ReadHeat()
{
    Heat heat;
#if defined(__APPLE__)
    // The system's thermal pressure (OSThermalNotification.h's levels: 0 nominal, 1 moderate, 2 heavy, 3 trapping, 4
    // sleeping), the value macOS itself throttles by.
    static int token = -1;
    static bool registered = notify_register_check("com.apple.system.thermalpressurelevel", &token) == NOTIFY_STATUS_OK;
    uint64_t level = 0;
    if (registered && notify_get_state(token, &level) == NOTIFY_STATUS_OK)
        heat.level = level == 0   ? Thermal::Nominal
                     : level == 1 ? Thermal::Moderate
                     : level == 2 ? Thermal::Heavy
                                  : Thermal::Critical;
#elif !defined(_WIN32)
    char path[96], text[32];
    int hottest = -1000;
    for (int zone = 0; zone < 32; zone++)
    {
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
        if (!ReadSys(path, text, sizeof(text)))
            break;
        hottest = std::max(hottest, atoi(text) / 1000); // millidegrees
    }
    heat.celsius = hottest;
#endif
    return heat;
}

machine::Power machine::ReadPower()
{
    Power power;
#if defined(__APPLE__)
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info == nullptr)
        return power;
    power.known = true;
    if (CFStringRef type = IOPSGetProvidingPowerSourceType(info))
        power.battery = CFStringCompare(type, CFSTR(kIOPMBatteryPowerKey), 0) == kCFCompareEqualTo;
    if (CFArrayRef list = IOPSCopyPowerSourcesList(info))
    {
        for (CFIndex i = 0; i < CFArrayGetCount(list); i++)
        {
            CFDictionaryRef source = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
            if (source == nullptr)
                continue;
            auto type = static_cast<CFStringRef>(CFDictionaryGetValue(source, CFSTR(kIOPSTypeKey)));
            if (type == nullptr || CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) != kCFCompareEqualTo)
                continue;
            int now = 0, full = 0;
            auto current = static_cast<CFNumberRef>(CFDictionaryGetValue(source, CFSTR(kIOPSCurrentCapacityKey)));
            auto maximum = static_cast<CFNumberRef>(CFDictionaryGetValue(source, CFSTR(kIOPSMaxCapacityKey)));
            if (current != nullptr && maximum != nullptr && CFNumberGetValue(current, kCFNumberIntType, &now) &&
                CFNumberGetValue(maximum, kCFNumberIntType, &full) && full > 0)
                power.percent = std::clamp(now * 100 / full, 0, 100);
            break;
        }
        CFRelease(list);
    }
    CFRelease(info);
#elif defined(_WIN32)
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status) && status.ACLineStatus != 255)
    {
        power.known = true;
        power.battery = status.ACLineStatus == 0;
        if ((status.BatteryFlag & 128) == 0 && status.BatteryLifePercent <= 100)
            power.percent = status.BatteryLifePercent;
    }
#else
    DIR* folder = opendir("/sys/class/power_supply");
    if (folder == nullptr)
        return power;
    char path[320], text[32];
    bool mains = false, mainsOnline = false, discharging = false;
    while (dirent* entry = readdir(folder))
    {
        if (entry->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", entry->d_name);
        if (!ReadSys(path, text, sizeof(text)))
            continue;
        if (strcmp(text, "Mains") == 0)
        {
            mains = true;
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/online", entry->d_name);
            mainsOnline = mainsOnline || (ReadSys(path, text, sizeof(text)) && strcmp(text, "1") == 0);
        }
        else if (strcmp(text, "Battery") == 0 && power.percent < 0)
        {
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", entry->d_name);
            if (ReadSys(path, text, sizeof(text)))
                power.percent = std::clamp(atoi(text), 0, 100);
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", entry->d_name);
            discharging = ReadSys(path, text, sizeof(text)) && strcmp(text, "Discharging") == 0;
        }
    }
    closedir(folder);
    power.known = mains || power.percent >= 0;
    power.battery = mains ? !mainsOnline : discharging;
#endif
    return power;
}
