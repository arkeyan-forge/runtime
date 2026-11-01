#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// The automatic 60 -> 30 policy, independent of the clock, renderer and settings file.
// A sample is late when more than one blank has passed since the last flip.
struct FrameRateFallback
{
    static constexpr uint32_t Window = 30, MissesToHalve = 3;
    static constexpr uint32_t FitsToRestore = 300, MaxFitsToRestore = 9000, ShortSixty = 120;
    enum class Change
    {
        None,
        Halved,
        Restored
    };

    bool active = false;
    std::array<bool, Window> missed = {};
    uint32_t next = 0, fits = 0, fitsToRestore = FitsToRestore, framesAtSixty = 0;

    Change Sample(bool enabled, bool late)
    {
        if (!enabled)
        {
            *this = {};
            return Change::None;
        }
        if (active)
        {
            fits = late ? 0 : fits + 1;
            if (fits < fitsToRestore)
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
        fits = 0;
        fitsToRestore = framesAtSixty < ShortSixty ? std::min(fitsToRestore * 2, MaxFitsToRestore) : FitsToRestore;
        return Change::Halved;
    }
};
