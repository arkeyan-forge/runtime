// Checks the pacer of per-frame simulation steps with a fake clock. usage: SSAStepPacerTest
#include "d3d/step_pacer.h"
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

    constexpr uint64_t Ms = 1'000'000;

    // Steps run in `seconds` of frames `period` apart (plus a jitter that alternates by +-`jitter`), at the limit `limit`.
    uint32_t Steps(uint32_t limit, uint64_t period, uint64_t jitter, uint32_t seconds)
    {
        StepPacer pacer;
        uint64_t clock = 1000 * Ms;
        uint32_t steps = 0;
        const uint32_t frames = uint32_t(uint64_t(seconds) * 1000 * Ms / period);
        for (uint32_t i = 0; i < frames; i++)
        {
            clock += period + (i % 2 == 0 ? jitter : 0) - (i % 2 == 1 ? jitter : 0);
            steps += pacer.Run(clock, limit);
        }
        return steps;
    }
}

int main()
{
    // 30 or below: every frame, whatever the jitter.
    CHECK(Steps(30, 33'333'333, 0, 10) == 300);
    CHECK(Steps(30, 33'333'333, 3 * Ms, 10) == 300);
    CHECK(Steps(30, 50 * Ms, 0, 10) == 200);
    // 60: the same 30 steps a second as at 30, one frame in two.
    CHECK(Steps(60, 16'666'667, 0, 10) == 300);
    CHECK(Steps(60, 16'666'667, 2 * Ms, 10) == 300);
    // 60 asked, 30 shown (the automatic drop): every frame runs.
    CHECK(Steps(60, 33'333'333, 0, 10) == 300);
    // A machine at 45 FPS or 120 or without a limit: still 30 a second, never more.
    CHECK(Steps(60, 22'222'222, 0, 10) >= 295 && Steps(60, 22'222'222, 0, 10) <= 305);
    CHECK(Steps(120, 8'333'333, 0, 10) == 300);
    CHECK(Steps(0, 4'166'667, 0, 10) >= 295 && Steps(0, 4'166'667, 0, 10) <= 305);
    // Slower than 30 (a heavy scene): every frame, since a step cannot be made up.
    CHECK(Steps(60, 50 * Ms, 0, 10) == 200);
    // A pause (a load) is not caught up on afterwards.
    {
        StepPacer pacer;
        uint64_t clock = 1000 * Ms;
        for (int i = 0; i < 10; i++)
            pacer.Run(clock += 16'666'667, 60);
        clock += 5000 * Ms;
        CHECK(pacer.Run(clock, 60));
        CHECK(!pacer.Run(clock += 16'666'667, 60));
    }
    if (g_failures == 0)
        printf("step pacer: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
