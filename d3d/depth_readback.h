#pragma once

#include <cstdint>

namespace d3d
{
    // The game's CPU reads the scene depth the renderer resolves (the light tiles are classified from it). The game's
    // engine hook (engines/lux/light_tiles.cpp) says which resolve, by its physical address; only that one is written back
    // to guest memory (d3d/render.cpp WriteBackDepth).
    void SetCpuReadDepth(uint32_t physicalAddress);
}
