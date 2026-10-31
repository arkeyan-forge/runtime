// Checks the frame rate limiter's schedule and its wait without sleeping. usage: SSAFrameLimiterTest
#include "kernel/frame_limiter.h"
#include <cstdio>
#include <initializer_list>

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

    constexpr uint64_t Ms = 1'000'000;
}

int main()
{
    for (uint32_t fps : { 30u, 60u, 120u, 144u, 240u })
    {
        FrameLimiter limiter;
        limiter.SetRate(fps);
        CHECK(limiter.Rate() == fps);
        uint64_t clock = 5 * Ms, firstShown = 0, shown = 0;
        constexpr uint32_t Frames = 1000;
        for (uint32_t i = 0; i < Frames; i++)
        {
            clock += 1 * Ms; // a frame takes 1 ms to build, less than the period of every rate here
            uint64_t due = limiter.Due(clock);
            if (due > clock)
                clock = due;
            if (i == 0)
                firstShown = clock;
            shown = clock;
        }
        // Frames built faster than the period are shown one period apart on average.
        double seconds = double(shown - firstShown) / 1e9, rate = (Frames - 1) / seconds;
        CHECK(rate > fps * 0.995 && rate < fps * 1.005);
    }

    // A frame that takes longer than the period is shown at once, and does not make the next ones burst.
    {
        FrameLimiter limiter;
        limiter.SetRate(60);
        uint64_t now = 100 * Ms;
        CHECK(limiter.Due(now) == now);
        now += 50 * Ms; // 3 periods late
        CHECK(limiter.Due(now) == now);
        uint64_t next = limiter.Due(now + 1 * Ms);
        CHECK(next >= now + 16 * Ms); // not 3 frames in a row at once
    }

    // Unlimited: never waits.
    {
        FrameLimiter limiter;
        limiter.SetRate(0);
        CHECK(limiter.Rate() == 0);
        for (uint64_t t = 1; t < 10; t++)
            CHECK(limiter.Due(t) == t);
    }

    // The wait sleeps until `spin` before the due time, then spins on the clock.
    {
        uint64_t clock = 0, slept = 0, reads = 0;
        auto now = [&] {
            reads++;
            clock += 10'000; // each read of the clock takes 10 us, so the spin ends
            return clock;
        };
        auto sleep = [&](uint64_t ns) {
            slept += ns;
            clock += ns + 300'000;
        }; // a sleep ends 0.3 ms late
        FrameLimiter::WaitUntil(10 * Ms, 1 * Ms, now, sleep);
        CHECK(clock >= 10 * Ms);
        CHECK(slept <= 9 * Ms);
        // Already past: no sleep, one read.
        clock = 20 * Ms;
        slept = 0;
        reads = 0;
        FrameLimiter::WaitUntil(10 * Ms, 1 * Ms, now, sleep);
        CHECK(slept == 0 && reads == 1);
    }

    printf(g_failures == 0 ? "PASS\n" : "FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
