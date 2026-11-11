#pragma once

// VSync on a display whose refresh is not a multiple of the frame rate (docs/runtime/gpu/plume_backends.md, "VSync and
// the refresh rate"). With FIFO, a 60 FPS game on a 144 Hz screen queues behind the display: each frame waits for a
// refresh, the present blocks when the queue is full, and that wait feeds back into the game's pace. On such a display
// the frames keep their own 60 Hz pace (the emulated blanks) and each is shown at the next refresh without queueing:
// Vulkan MAILBOX, D3D12 flip model at sync interval 0 without tearing (plume_present_latest.patch). No fixed refresh
// shows 60 on 144 evenly (2.4 refreshes a frame: 2, 2, 3...); that needs 120 Hz or a variable refresh display. A
// whole multiple (60, 120, 240; 30 on 60 or 90) keeps strict FIFO: each frame takes the same number of refreshes.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace gpu::presentmode
{
    enum class Mode
    {
        Fifo,      // each frame at a refresh, in order, the present waits when the queue is full
        Latest,    // vsync without queueing: the newest frame at the next refresh
        Immediate, // vsync off
    };

    // `refreshHz` 0: unknown; `fps` 0: no limit (vsync is the limit, so FIFO).
    inline Mode Choose(bool vsync, double refreshHz, uint32_t fps)
    {
        if (!vsync)
            return Mode::Immediate;
        if (refreshHz <= 0 || fps == 0 || double(fps) > refreshHz * 1.01)
            return Mode::Fifo;
        const double ratio = refreshHz / double(fps); // 59.94 / 60 and 119.88 / 60 are multiples
        return std::abs(ratio - std::round(ratio)) <= 0.01 ? Mode::Fifo : Mode::Latest;
    }

    inline const char* Name(Mode mode)
    {
        return mode == Mode::Fifo ? "FIFO" : mode == Mode::Latest ? "the newest frame at each refresh" : "off";
    }

    // How many refreshes each shown frame stayed on the display, from the swap chain's display statistics (plume's
    // RenderDisplayStats): `frames` shown so far, the refresh count or the time (ns) of the last. Read after each
    // present; a step of exactly one frame is counted, by refresh counts when the backend has them (D3D12), else by
    // time over `refreshNs` (Metal).
    struct Cadence
    {
        static constexpr uint32_t MaxHeld = 4; // 4 or more
        std::array<uint32_t, MaxHeld + 1> held = {}; // [n]: frames shown n refreshes ([0] unused)
        uint64_t lastFrames = 0, lastRefreshes = 0, lastTimeNs = 0;
        bool started = false;

        void Add(uint64_t frames, uint64_t refreshes, uint64_t timeNs, uint64_t refreshNs)
        {
            if (started && frames == lastFrames + 1)
            {
                uint64_t n = 0;
                if (refreshes != 0 && lastRefreshes != 0 && refreshes > lastRefreshes)
                    n = refreshes - lastRefreshes;
                else if (timeNs != 0 && lastTimeNs != 0 && timeNs > lastTimeNs && refreshNs != 0)
                    n = (timeNs - lastTimeNs + refreshNs / 2) / refreshNs;
                if (n != 0)
                    held[std::min<uint64_t>(n, MaxHeld)]++;
            }
            if (!started || frames != lastFrames)
            {
                lastFrames = frames;
                lastRefreshes = refreshes;
                lastTimeNs = timeNs;
            }
            started = true;
        }

        uint32_t Counted() const
        {
            uint32_t sum = 0;
            for (uint32_t n = 1; n <= MaxHeld; n++)
                sum += held[n];
            return sum;
        }

        // Starts a new window, keeping the last frame seen.
        void Clear() { held = {}; }
    };
}
