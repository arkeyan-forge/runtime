// Checks the vsync mode chosen for a refresh rate and the display cadence count (gpu/present_mode.h).
// usage: ArkeyanPresentModeTest
#include "gpu/present_mode.h"
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

    using gpu::presentmode::Choose;
    using gpu::presentmode::Mode;
}

int main()
{
    // Multiples keep FIFO, including the NTSC rates; the others show the newest frame at each refresh.
    for (double hz : { 60.0, 59.94, 120.0, 119.88, 240.0 })
        CHECK(Choose(true, hz, 60) == Mode::Fifo);
    for (double hz : { 75.0, 100.0, 143.98, 144.0, 165.0, 170.0 })
        CHECK(Choose(true, hz, 60) == Mode::Latest);
    CHECK(Choose(true, 60, 30) == Mode::Fifo && Choose(true, 90, 30) == Mode::Fifo &&
          Choose(true, 144, 30) == Mode::Latest);
    CHECK(Choose(true, 144, 120) == Mode::Latest && Choose(true, 240, 120) == Mode::Fifo);
    // More frames than refreshes, no limit, an unknown display: FIFO; vsync off: off.
    CHECK(
        Choose(true, 60, 120) == Mode::Fifo && Choose(true, 144, 0) == Mode::Fifo && Choose(true, 0, 60) == Mode::Fifo);
    CHECK(Choose(false, 144, 60) == Mode::Immediate && Choose(false, 60, 60) == Mode::Immediate);

    // D3D12 statistics: refresh counts. 60 on 144: 2, 2, 3, 2, 3 refreshes (12 in 5 frames); a skipped poll (two frames
    // in one step) is not counted, a repeated one changes nothing.
    {
        gpu::presentmode::Cadence cadence;
        const uint64_t refreshes[] = { 100, 102, 104, 107, 109, 112 };
        for (uint64_t i = 0; i < 6; i++)
        {
            cadence.Add(10 + i, refreshes[i], 0, 0);
            cadence.Add(10 + i, refreshes[i], 0, 0);
        }
        CHECK(cadence.Counted() == 5 && cadence.held[2] == 3 && cadence.held[3] == 2);
        cadence.Add(18, 117, 0, 0);
        CHECK(cadence.Counted() == 5);
        cadence.Add(19, 119, 0, 0);
        CHECK(cadence.held[2] == 4);
        cadence.Clear();
        CHECK(cadence.Counted() == 0);
        cadence.Add(20, 121, 0, 0);
        CHECK(cadence.held[2] == 1);
    }
    // Metal: presented times, 60 on a 120 Hz ProMotion screen: 2 refreshes each; a frame held 4 refreshes or more.
    {
        gpu::presentmode::Cadence cadence;
        const uint64_t refreshNs = 8'333'333;
        for (uint64_t i = 0; i < 10; i++)
            cadence.Add(i, 0, 1'000'000'000 + i * 16'666'667, refreshNs);
        cadence.Add(10, 0, 1'000'000'000 + 9 * 16'666'667 + 50'000'000, refreshNs);
        CHECK(cadence.held[2] == 9 && cadence.held[4] == 1 && cadence.Counted() == 10);
    }
    if (g_failures == 0)
        printf("present mode: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
