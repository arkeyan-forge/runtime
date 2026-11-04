#pragma once

// xenos::Tiled2D for 32-bit texels a tile at a time (d3d/render.cpp, WriteBackDepth): the offset (in texels) of each
// texel of a 32x32 tile, and whether, for this pitch, Tiled2D(x, y) is the tile's 4 KB (tiles in row order) plus the
// offset of (x % 32, y % 32). The offsets come from Tiled2D and the split is checked against it over the whole image;
// without it the write-back calls Tiled2D per texel, which was a third of SSA's GPU thread
// (docs/runtime/gpu/renderer_threads.md).
#include "d3d/xenos.h"
#include <array>
#include <cstdint>

namespace d3d
{
    struct TiledDepthLayout
    {
        uint32_t pitch = 0, rows = 0;
        bool split = false;
        std::array<uint16_t, 32 * 32> inTile{};

        uint32_t Offset(uint32_t x, uint32_t y) const
        {
            return (((y >> 5) * (pitch >> 5) + (x >> 5)) << 10) + inTile[(y & 31) * 32 + (x & 31)];
        }
    };

    inline TiledDepthLayout MakeTiledDepthLayout(uint32_t pitch, uint32_t rows)
    {
        TiledDepthLayout layout;
        layout.pitch = pitch;
        layout.rows = rows;
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 32; x++)
                layout.inTile[y * 32 + x] = uint16_t(xenos::Tiled2D(x, y, pitch, 2) / 4);
        layout.split = true;
        for (uint32_t y = 0; y < rows && layout.split; y++)
            for (uint32_t x = 0; x < pitch && layout.split; x++)
                layout.split = xenos::Tiled2D(x, y, pitch, 2) / 4 == layout.Offset(x, y);
        return layout;
    }
}
