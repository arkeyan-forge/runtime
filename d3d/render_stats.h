#pragma once
#include <cstdint>

namespace d3d
{
    // Existing [render] counters at the instant of the read, in the current guest frame.
    // The overlay never resets them or adds work to a draw, resolve or swap.
    struct RenderStats
    {
        uint32_t draws = 0, resolves = 0;
    };
    RenderStats ReadRenderStats();
}
