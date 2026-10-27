#include "kernel/objects.h"
#include "platform/platform.h"
#include <chrono>
#include <thread>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t TitleProcess = 1;
}

void StartTimeStampBundle(uint32_t bundle)
{
    // KeTimeStampBundle: u64 InterruptTime (100 ns since boot), u64 SystemTime, u32 TickCount (ms since boot).
    std::thread([bundle]
    {
        auto boot = std::chrono::steady_clock::now();
        auto next = boot;
        while (true)
        {
            auto sinceBoot = std::chrono::steady_clock::now() - boot;
            g_memory.Write<uint64_t>(bundle + 0x0, std::chrono::duration_cast<std::chrono::nanoseconds>(sinceBoot).count() / 100);
            g_memory.Write<uint64_t>(bundle + 0x8, GuestSystemTime());
            g_memory.Write<uint32_t>(bundle + 0x10, uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(sinceBoot).count()));
            next += std::chrono::milliseconds(1);
            std::this_thread::sleep_until(next);
        }
    }).detach();
}

PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    ctx.r3.u64 = TitleProcess;
}

PPC_FUNC(__imp__KeSetCurrentProcessType) {}

PPC_FUNC(__imp__KeQuerySystemTime)
{
    g_memory.Write<uint64_t>(ctx.r3.u32, GuestSystemTime());
}

// mftb is translated to the host CPU counter (cntvct_el0 on arm64, the TSC on x64), so report that counter's frequency.
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    ctx.r3.u64 = PlatformTimeBaseFrequency();
}
