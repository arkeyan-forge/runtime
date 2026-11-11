#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

// The automatic 60 -> 30 policy, independent of the clock, renderer and settings file. Each switch is a visible stutter,
// so it switches only when the game really cannot hold 60, and stays (docs/runtime/xbox360/video_vblank.md).
// A sample is `late` when more than one blank passed since the last flip, `fits` when the frame's work ended within
// 3/4 of a blank (only read at 30: the margin 60 needs; for a game that waits for a blank itself, not late). `paused`: a load (a file opened in the last 5 s) or a movie:
// nothing is counted and nothing switches; after it the counts start again, the rate stays.
// At 60, frames are counted in blocks of 60 (a second): the rate halves after `HeavyBlocks` heavy blocks in a row, a
// quarter of their frames late or more (under ~50 FPS for 5 s). A load's hitch, a shader build or two seconds of a busy
// scene never get there; the old rule (20 late in 120 frames) halved on them and came back at once (a Windows run
// switched every few seconds). At 30, the rate stays at least `HoldFrames` (30 s), then 60 comes back once 90 % of the
// last 300 frames (10 s) fit. A drop within 60 s of coming back doubles the hold, up to 2 minutes.
struct FrameRateFallback
{
    static constexpr uint32_t Block = 60, LateInHeavyBlock = 15, HeavyBlocks = 5;
    static constexpr uint32_t HoldFrames = 900, MaxHoldFrames = 3600, ShortSixty = 3600;
    static constexpr uint32_t FitWindow = 300, FitsToRestore = 270;
    enum class Change
    {
        None,
        Halved,
        Restored
    };

    bool active = false;
    bool paused = false; // frames of a load or a movie were skipped since the last counted one
    // At 60: the block being counted, the heavy blocks in a row, the frames since 60 came back (saturated).
    uint32_t inBlock = 0, lateInBlock = 0, heavyBlocks = 0, framesAtSixty = ShortSixty;
    // At 30: the frames held, the hold this time, the last FitWindow frames' `fits` and their sum.
    uint32_t framesAtThirty = 0, holdFrames = HoldFrames;
    std::array<bool, FitWindow> fit = {};
    uint32_t fitNext = 0, fitSamples = 0, fitCount = 0;

    Change Sample(bool enabled, bool late, bool fits, bool pausedNow = false)
    {
        if (!enabled)
        {
            *this = {};
            return Change::None;
        }
        if (pausedNow)
        {
            paused = true;
            return Change::None;
        }
        if (std::exchange(paused, false))
            ClearCounts();
        if (active)
        {
            framesAtThirty++;
            fitCount += uint32_t(fits) - uint32_t(fit[fitNext]);
            fit[fitNext] = fits;
            fitNext = (fitNext + 1) % FitWindow;
            if (fitSamples < FitWindow)
                fitSamples++;
            if (framesAtThirty < holdFrames || fitSamples < FitWindow || fitCount < FitsToRestore)
                return Change::None;
            active = false;
            framesAtSixty = 0;
            ClearCounts();
            return Change::Restored;
        }
        if (framesAtSixty < ShortSixty)
            framesAtSixty++;
        lateInBlock += late;
        if (++inBlock < Block)
            return Change::None;
        heavyBlocks = lateInBlock >= LateInHeavyBlock ? heavyBlocks + 1 : 0;
        inBlock = lateInBlock = 0;
        if (heavyBlocks < HeavyBlocks)
            return Change::None;
        active = true;
        framesAtThirty = 0;
        holdFrames = framesAtSixty < ShortSixty ? std::min(holdFrames * 2, MaxHoldFrames) : HoldFrames;
        ClearCounts();
        return Change::Halved;
    }

private:
    void ClearCounts()
    {
        inBlock = lateInBlock = heavyBlocks = 0;
        fit = {};
        fitNext = fitSamples = fitCount = 0;
    }
};
