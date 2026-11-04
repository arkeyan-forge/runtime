#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

// The automatic 60 -> 30 policy, independent of the clock, renderer and settings file.
// A sample is late when more than one blank has passed since the last flip. Halving takes a sustained shortfall, a
// frame in six late over 2 s (under ~50 FPS): 3 in 30 frames also caught a single burst of slow frames (a load, a
// camera cut). At 30, 60 is tried again after `fitsToRestore` frames unless 30 itself was not held (more than 1 in 20
// late): one late frame used to restart the count, and a level's streaming hitches (1-3 a second in SSA) kept the game at
// 30 for good (docs/runtime/xbox360/video_vblank.md). A try that fails within 10 s doubles the wait, up to 60 s.
// Frames while the game loads (`loading`: a file opened in the last 5 s) are not counted: a load's slow frames halved the
// rate (Swap Force loading its figure); after the load the window starts empty, and a game at 30 is back at 60 at once.
struct FrameRateFallback
{
    static constexpr uint32_t Window = 120, MissesToHalve = 20;
    static constexpr uint32_t FitsToRestore = 300, MaxFitsToRestore = 1800, ShortSixty = 600, LateOneIn = 20;
    enum class Change
    {
        None,
        Halved,
        Restored
    };

    bool active = false;
    std::array<bool, Window> missed = {};
    uint32_t next = 0, fitsToRestore = FitsToRestore, framesAtSixty = 0;
    uint32_t framesAtThirty = 0, lateAtThirty = 0; // since the halving, or since the last count that did not hold 30
    bool loaded = false;                           // frames of a load were skipped since the last counted one

    Change Sample(bool enabled, bool late, bool loading = false)
    {
        if (!enabled)
        {
            *this = {};
            return Change::None;
        }
        if (loading)
        {
            loaded = true;
            return Change::None;
        }
        if (std::exchange(loaded, false))
        {
            missed = {};
            next = 0;
            if (active)
            {
                active = false;
                framesAtSixty = 0;
                fitsToRestore = FitsToRestore;
                return Change::Restored;
            }
        }
        if (active)
        {
            framesAtThirty++;
            lateAtThirty += late;
            if (framesAtThirty < fitsToRestore)
                return Change::None;
            const bool held = lateAtThirty * LateOneIn <= framesAtThirty;
            framesAtThirty = lateAtThirty = 0;
            if (!held)
                return Change::None;
            active = false;
            missed = {};
            next = framesAtSixty = 0;
            return Change::Restored;
        }
        missed[next] = late;
        next = (next + 1) % Window;
        if (framesAtSixty < ShortSixty)
            framesAtSixty++;
        if (std::count(missed.begin(), missed.end(), true) < MissesToHalve)
            return Change::None;
        active = true;
        framesAtThirty = lateAtThirty = 0;
        fitsToRestore = framesAtSixty < ShortSixty ? std::min(fitsToRestore * 2, MaxFitsToRestore) : FitsToRestore;
        return Change::Halved;
    }
};
