#pragma once
#include <cstddef>

namespace d3d
{
    // The renderer's host textures now (the debug screen); false while the game's render thread holds them, without
    // waiting for it.
    bool TryCountTextures(size_t& count);
}
