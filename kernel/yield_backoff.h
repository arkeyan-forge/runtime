#pragma once

#include <cstdint>

namespace kernel
{
    // A zero-length sleep or a yield hands the hardware thread to another ready thread, or returns at once: the titles
    // poll job queues and spin locks that way (SSA's main thread spins while the render job waits for the vertical
    // blank). On the console that only costs a hardware thread the title owns; here it would keep host cores busy, so a
    // thread that keeps yielding with no work in between (32 times, less than 100 us apart) backs off with short sleeps
    // (kernel/threads.cpp). The clock is the caller's, so the decision is tested without sleeping.
    struct YieldBackOff
    {
        static constexpr uint64_t SpinGapNs = 100'000;
        static constexpr uint32_t SpinsBeforeSleeping = 32;

        uint64_t lastYieldNs = 0;
        uint32_t spins = 0;

        // `nowNs`: when the yield is asked; `working`: the thread is inside a WorkingThreadScope. True: sleep instead
        // of yielding. Call Done when the yield or the sleep returns.
        bool ShouldSleep(uint64_t nowNs, bool working)
        {
            spins = nowNs - lastYieldNs < SpinGapNs ? spins + 1 : 0;
            return !working && spins >= SpinsBeforeSleeping;
        }
        void Done(uint64_t nowNs) { lastYieldNs = nowNs; }
    };

    // While one lives on a thread, its yields never back off. For a guest job known to yield after each item of real
    // work, a few microseconds each, which the back-off cannot tell from spinning: SSA's light tile job calls Sleep(0)
    // after each of its 385 tiles, so it slept 200 us per tile, took ~115 ms instead of ~2 ms, and the sun's tile
    // classes reached the screen 3 to 7 frames late (engines/lux/light_tiles.cpp).
    inline thread_local uint32_t t_workingThreadScopes = 0;

    class WorkingThreadScope
    {
    public:
        WorkingThreadScope() { t_workingThreadScopes++; }
        ~WorkingThreadScope() { t_workingThreadScopes--; }
        WorkingThreadScope(const WorkingThreadScope&) = delete;
        WorkingThreadScope& operator=(const WorkingThreadScope&) = delete;
    };

    inline bool ThreadIsWorking() { return t_workingThreadScopes != 0; }
}
