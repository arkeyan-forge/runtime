#include "platform/platform.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <chrono>
#include <thread>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

uint64_t PlatformTimeBaseFrequency()
{
#if defined(__aarch64__)
    // mftb reads cntvct_el0, whose rate the system publishes in cntfrq_el0.
    uint64_t frequency;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(frequency));
    return frequency;
#elif defined(__x86_64__) || defined(_M_X64)
    // mftb reads the TSC (invariant on Haswell, Zen and later CPUs), whose rate no instruction reports:
    // measure it once against the OS monotonic clock.
    static const uint64_t frequency = [] {
        auto clockStart = std::chrono::steady_clock::now();
        uint64_t counterStart = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t counterEnd = __rdtsc();
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - clockStart).count();
        return uint64_t(double(counterEnd - counterStart) / seconds);
    }();
    return frequency;
#else
#error "Unsupported host CPU"
#endif
}
