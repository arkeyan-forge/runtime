#pragma once

#include <chrono>
#include <cstdint>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

// Nanoseconds from an arbitrary start, for durations only (the perf log's timers, several a draw on two threads). On
// Apple silicon the counter mach_absolute_time reads (CNTVCT_EL0, same ticks), read directly: mach_absolute_time puts
// an instruction barrier before it, which drains the pipeline (2 % of SSA's render thread, 4 % of the GPU thread in
// `level_008`'s samples); steady_clock::now costs 20 ns.
inline uint64_t PlatformNowNs()
{
#if defined(__APPLE__)
    static const double nsPerTick = [] {
        mach_timebase_info_data_t info;
        mach_timebase_info(&info);
        return double(info.numer) / double(info.denom);
    }();
#if defined(__aarch64__)
    uint64_t ticks;
    asm volatile("mrs %0, CNTVCT_EL0" : "=r"(ticks));
#else
    const uint64_t ticks = mach_absolute_time();
#endif
    return uint64_t(double(ticks) * nsPerTick);
#else
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}
