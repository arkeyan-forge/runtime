// Frame generation's rules (gpu/fsr/framegen_rules.h): when a frame is made, and the pacing. ./ArkeyanFrameGenTest
#include "gpu/fsr/framegen_rules.h"
#include <cmath>
#include <cstdio>
#include <iterator>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool condition, int line)
    {
        g_checks++;
        if (!condition)
        {
            fprintf(stderr, "FAIL line %d\n", line);
            g_failures++;
        }
    }

#define CHECK(condition) Check((condition), __LINE__)

    constexpr uint64_t Ms = 1'000'000;
}

int main()
{
    using framegen::Decide;
    using framegen::Outcome;

    // The reset rules: off, no scene (movie, loading screen, menu), no HUD copy, no history or a camera cut.
    CHECK(Decide({ false, true, true, true, false }) == Outcome::Off);
    CHECK(Decide({ true, false, true, true, false }) == Outcome::NoScene);
    CHECK(Decide({ true, true, false, true, false }) == Outcome::NoHudless);
    CHECK(Decide({ true, true, true, false, false }) == Outcome::Reset);
    CHECK(Decide({ true, true, true, true, true }) == Outcome::Reset);
    CHECK(Decide({ true, true, true, true, false }) == Outcome::Generate);
    // No scene wins over a missing HUD copy (a movie has neither).
    CHECK(Decide({ true, false, false, true, false }) == Outcome::NoScene);

    // A sequence: a movie, then the scene comes back: the first scene frame starts the history, the second generates.
    {
        bool history = false;
        const bool scenes[] = { false, false, true, true, true, false, true, true };
        const Outcome expected[] = { Outcome::NoScene, Outcome::NoScene, Outcome::Reset, Outcome::Generate,
            Outcome::Generate, Outcome::NoScene, Outcome::Reset, Outcome::Generate };
        for (size_t i = 0; i < std::size(scenes); i++)
        {
            const Outcome outcome = Decide({ true, scenes[i], true, history, false });
            CHECK(outcome == expected[i]);
            history = outcome == Outcome::Generate || outcome == Outcome::Reset;
        }
    }

    // The display's room with VSync: 30 real on 60 Hz fits, 60 on 60 does not, 60 on 120 does; without VSync always.
    CHECK(framegen::DisplayHasRoom(true, 60.0, 33.3));
    CHECK(!framegen::DisplayHasRoom(true, 60.0, 16.7));
    CHECK(framegen::DisplayHasRoom(true, 120.0, 16.7));
    CHECK(framegen::DisplayHasRoom(false, 60.0, 16.7));
    CHECK(framegen::DisplayHasRoom(true, 0.0, 16.7)); // refresh unknown
    CHECK(framegen::DisplayHasRoom(true, 59.94, 33.4)); // 59.94 Hz still shows 30 x 2
    CHECK(Decide({ true, true, true, true, false, false }) == Outcome::Display);

    // The uneven guard: one bad window is not enough, two are; a window of few generated frames does not count.
    {
        framegen::UnevenGuard guard;
        CHECK(!guard.Window(6.7, 1.0));
        CHECK(guard.Window(6.8, 1.0));
        guard.Reset();
        CHECK(!guard.Window(6.7, 1.0));
        CHECK(!guard.Window(2.9, 1.0)); // SSA's own pacing
        CHECK(!guard.Window(6.7, 1.0));
        CHECK(!guard.Window(8.0, 0.5)); // a loading window
        CHECK(!guard.Window(0.1, 1.0));
    }

    // Camera cuts: the identity and a small pan are not; a jump of half the picture, a turn behind, are.
    {
        double m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        CHECK(!framegen::CameraCut(m));
        m[3] = 0.05; // a pan of 2.5 % of the picture
        CHECK(!framegen::CameraCut(m));
        m[3] = 0.8;
        CHECK(framegen::CameraCut(m));
        double behind[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1 };
        CHECK(framegen::CameraCut(behind));
        double nan[16] = { NAN, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        CHECK(framegen::CameraCut(nan));
        // A jump: sudden, not a pan that builds up or a still camera.
        CHECK(framegen::CameraJump(0.21, 0.02));
        CHECK(!framegen::CameraJump(0.21, 0.15));
        CHECK(!framegen::CameraJump(0.05, 0.0));
    }

    // Pacing at 30 real frames per second: the real frame goes half an interval (16.7 ms) after the generated one.
    {
        framegen::Pacing pacing;
        uint64_t t = 1000 * Ms;
        CHECK(std::fabs(pacing.RealFrame(t) - 16.7f) < 0.01f); // the first frame: a default
        for (int i = 0; i < 60; i++)
        {
            t += 33'333'333;
            const float ms = pacing.RealFrame(t);
            CHECK(std::fabs(ms - 33.333f) < 0.01f);
        }
        CHECK(pacing.HalfIntervalNs() > 16'600'000 && pacing.HalfIntervalNs() < 16'700'000);
        // At 60: the measure follows within about 30 frames.
        for (int i = 0; i < 40; i++)
            pacing.RealFrame(t += 16'666'667);
        CHECK(pacing.HalfIntervalNs() > 8'300'000 && pacing.HalfIntervalNs() < 8'500'000);
        // One late frame (a hitch of 100 ms) moves it a little, not to 50 ms.
        pacing.RealFrame(t += 100 * Ms);
        CHECK(pacing.HalfIntervalNs() < 13 * Ms);
        // A pause of seconds is held to the maximum interval.
        CHECK(std::fabs(pacing.RealFrame(t += 5000 * Ms) - 100.0f) < 0.01f);
        // Reset: the next frame is a first frame again.
        pacing.Reset();
        CHECK(pacing.HalfIntervalNs() == 0);
        CHECK(std::fabs(pacing.RealFrame(t += 33 * Ms) - 16.7f) < 0.01f);
    }

    // The shown frames' spacing: even at 60 shown, uneven when the real frame follows the generated one at once.
    {
        framegen::Pacing even, uneven;
        uint64_t t = 1000 * Ms;
        for (int i = 0; i < 120; i++)
        {
            even.Shown(t + uint64_t(i) * 16'666'667);
            uneven.Shown(t + uint64_t(i / 2) * 33'333'333 + uint64_t(i % 2) * 2 * Ms);
        }
        const framegen::Pacing::Summary a = even.TakeSummary(), b = uneven.TakeSummary();
        CHECK(a.count == 119 && std::fabs(a.meanMs - 16.667) < 0.01 && a.deviationMs < 0.01);
        CHECK(b.count == 119 && b.deviationMs > 14.0);
        CHECK(even.TakeSummary().count == 0); // taken
        // A gap of a loading screen is not counted.
        even.Shown(t + 10'000 * Ms);
        CHECK(even.TakeSummary().count == 0);
    }

    // Held back for players: off without the preview whatever the game; with it, only a game that keeps it off.
    {
        using framegen::HeldBack;
        using framegen::Hold;
        CHECK(HeldBack(false, true, false) == Hold::Later && HeldBack(false, false, true) == Hold::Later);
        CHECK(HeldBack(true, true, false) == Hold::None);
        CHECK(HeldBack(true, false, false) == Hold::Game && HeldBack(true, false, true) == Hold::None);
    }

    printf("ArkeyanFrameGenTest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
