// Checks the automatic 60 -> 30 policy (host/settings_fallback.h) on recorded and made-up frame sequences.
// usage: ArkeyanFrameRateFallbackTest
#include "host/settings_fallback.h"
#include <cstdio>
#include <cstring>

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

    using Change = FrameRateFallback::Change;

    // Two 10 s performance logs of Trap Team at 60 on Windows (an RTX 3050), where the old rule (20 late frames in 120)
    // dropped to 30 in each: one frame per character, '#' for a present more than 18 ms after the one before.
    // 585 frames, 43 late
    constexpr const char* Steady =
        ".......................#........................................................................#.#."
        "#..........#..........#.........#..................................................................."
        ".......#.......#.##......#....#.....#...#.....#.........#..#.................##....#.....#.....#..#."
        "...........#........#.....#..#.#.......#..#..........#...........#..........#...............#......#"
        "#.....#..#...........#.....#....................................................#.....#............."
        ".....................................................................................";
    // 564 frames, 91 late
    constexpr const char* Busy =
        "...........................................#......................................#................."
        "....................#..........###...........................................................###.###"
        ".#.######.###.####.####..###.###..#...#.#..#.#..##.#..#.#..##.###..###..#.##............#.....#....#"
        "...........#..#...........#...#......##....................#..#######..............................."
        "..........#.#.........#...#.......#......#...#.......................#..............#...........#..."
        "#.........#.....#..#......#.#.................#.................";

    // Feeds `frames` samples (`late` every `lateEvery`-th, 0: never) and returns the first change, with its sample.
    struct Seen
    {
        Change change = Change::None;
        int at = -1;
    };
    Seen Feed(FrameRateFallback& f, int frames, int lateEvery, bool fits = true, bool paused = false)
    {
        Seen seen;
        for (int i = 0; i < frames; i++)
        {
            const Change c = f.Sample(true, lateEvery != 0 && i % lateEvery == 0, fits, paused);
            if (c != Change::None && seen.at < 0)
                seen = { c, i };
        }
        return seen;
    }

    Seen FeedRecorded(FrameRateFallback& f, const char* frames)
    {
        Seen seen;
        for (int i = 0; frames[i] != 0; i++)
        {
            const Change c = f.Sample(true, frames[i] == '#', frames[i] != '#');
            if (c != Change::None && seen.at < 0)
                seen = { c, i };
        }
        return seen;
    }

    // Halves `f` from 60 with late frames (within 5 blocks), and stops there.
    bool Halve(FrameRateFallback& f)
    {
        for (int i = 0; i < 300; i++)
            if (f.Sample(true, true, false) == Change::Halved)
                return true;
        return false;
    }
}

int main()
{
    // The recordings, alone and as a minute of the same scenes: 60 stays.
    {
        FrameRateFallback f;
        CHECK(FeedRecorded(f, Steady).at < 0 && FeedRecorded(f, Busy).at < 0);
        for (int i = 0; i < 3; i++)
            CHECK(FeedRecorded(f, Busy).at < 0 && FeedRecorded(f, Steady).at < 0 && !f.active);
    }
    // A hitch outside the 5 s after a file open (3 s of late frames) keeps 60; so does a 20 s load, and it breaks the
    // run of heavy blocks: 3 before it and 2 after are not 5 in a row.
    {
        FrameRateFallback f;
        CHECK(Feed(f, 180, 1).at < 0);
        CHECK(Feed(f, 1200, 1, false, true).at < 0 && !f.active);
        CHECK(Feed(f, 120, 1).at < 0);
        const Seen seen = Feed(f, 180, 1);
        CHECK(seen.change == Change::Halved && seen.at == 179);
    }
    // A sustained overload: a frame in 2 or in 4 late halves after 5 s; a frame in 5 never does.
    for (int every : { 2, 4 })
    {
        FrameRateFallback f;
        const Seen seen = Feed(f, 600, every);
        CHECK(seen.change == Change::Halved && seen.at == 299 && f.active);
    }
    {
        FrameRateFallback f;
        CHECK(Feed(f, 6000, 5).at < 0 && !f.active);
    }
    // At 30: held 30 s even when every frame fits, then back to 60; not while under 90 % of the frames fit.
    {
        FrameRateFallback f;
        CHECK(Halve(f) && f.holdFrames == FrameRateFallback::HoldFrames);
        Seen seen = Feed(f, 1000, 0);
        CHECK(seen.change == Change::Restored && seen.at == 899 && !f.active);
        f = {};
        CHECK(Halve(f));
        for (int i = 0; i < 1200; i++) // 85 % fit
            CHECK(f.Sample(true, false, i % 20 >= 3) == Change::None);
        CHECK(f.active);
        seen = Feed(f, 300, 0); // 90 % of the last 300 fit within about 100 more
        CHECK(seen.change == Change::Restored && seen.at > 60 && seen.at < 110);
    }
    // Back at 60 for less than a minute: each drop doubles the hold, up to 2 minutes; a minute at 60 resets it.
    {
        FrameRateFallback f;
        CHECK(Halve(f));
        for (uint32_t hold : { 900u, 1800u, 3600u, 3600u })
        {
            CHECK(f.holdFrames == hold);
            const Seen seen = Feed(f, 4000, 0);
            CHECK(seen.change == Change::Restored && seen.at == int(hold) - 1);
            CHECK(Halve(f));
        }
        CHECK(Feed(f, 4000, 0).change == Change::Restored);
        CHECK(Feed(f, 3600, 0).at < 0 && Halve(f) && f.holdFrames == FrameRateFallback::HoldFrames);
    }
    // A movie or a load at 30: no switch during it, the fit count starts again after it.
    {
        FrameRateFallback f;
        CHECK(Halve(f));
        CHECK(Feed(f, 800, 0).at < 0);
        CHECK(Feed(f, 1200, 0, true, true).at < 0 && f.active);
        const Seen seen = Feed(f, 400, 0);
        CHECK(seen.change == Change::Restored && seen.at == 299);
    }
    // Turned off: back to 60 at once, and on again it starts fresh.
    {
        FrameRateFallback f;
        CHECK(Halve(f));
        CHECK(f.Sample(false, true, false) == Change::None && !f.active);
        CHECK(Feed(f, 299, 1).at < 0 && Feed(f, 1, 1).change == Change::Halved);
    }
    if (g_failures == 0)
        printf("frame rate fallback: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
