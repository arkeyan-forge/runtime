// Checks when a guest yield backs off, with a fake clock. usage: ArkeyanYieldBackOffTest
#include "kernel/yield_backoff.h"
#include <cstdio>

namespace
{
    int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    constexpr uint64_t Us = 1000;

    // `count` yields `gapNs` apart (each returns at once); how many of them sleep.
    uint32_t Sleeps(kernel::YieldBackOff& backOff, uint64_t& clock, uint32_t count, uint64_t gapNs)
    {
        uint32_t sleeps = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            clock += gapNs;
            sleeps += backOff.ShouldSleep(clock, kernel::ThreadIsWorking());
            backOff.Done(clock);
        }
        return sleeps;
    }
}

int main()
{
    uint64_t clock = 1'000'000 * Us;

    // A thread that polls (a yield every microsecond) backs off after 32 yields.
    kernel::YieldBackOff spinner;
    CHECK(Sleeps(spinner, clock, 32, 1 * Us) == 0);
    CHECK(Sleeps(spinner, clock, 1, 1 * Us) == 1);
    CHECK(Sleeps(spinner, clock, 100, 1 * Us) == 100);
    // Real work (a gap of 100 us or more) yields at once again.
    CHECK(Sleeps(spinner, clock, 1, 100 * Us) == 0);
    CHECK(Sleeps(spinner, clock, 31, 1 * Us) == 0);

    // SSA's light tile job: Sleep(0) after each tile, a few microseconds apart. Without a scope it backs off from the
    // 33rd tile on (the bug: 200 us per tile); inside one it never does, and the scopes nest.
    kernel::YieldBackOff job;
    CHECK(Sleeps(job, clock, 385, 3 * Us) == 385 - 32);
    CHECK(!kernel::ThreadIsWorking());
    {
        kernel::WorkingThreadScope working;
        CHECK(kernel::ThreadIsWorking());
        CHECK(Sleeps(job, clock, 385, 3 * Us) == 0);
        {
            kernel::WorkingThreadScope inner;
            CHECK(Sleeps(job, clock, 10, 3 * Us) == 0);
        }
        CHECK(kernel::ThreadIsWorking());
        CHECK(Sleeps(job, clock, 10, 3 * Us) == 0);
    }
    CHECK(!kernel::ThreadIsWorking());
    // Out of the scope, the run of close yields goes on counting: the next one backs off.
    CHECK(Sleeps(job, clock, 1, 3 * Us) == 1);

    if (g_failures == 0)
        printf("yield back-off: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
