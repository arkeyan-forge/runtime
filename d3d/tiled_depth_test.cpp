// Checks that the depth write-back's tile-by-tile offsets (d3d/tiled_depth.h) are xenos::Tiled2D's for the pitches a
// 32-bit tiled target has. usage: ArkeyanTiledDepthTest
#include "d3d/tiled_depth.h"
#include <cstdio>

int main()
{
    int failures = 0;
    for (uint32_t pitch : { 32u, 64u, 640u, 1024u, 1280u, 1312u, 2560u })
    {
        const uint32_t rows = 736;
        const d3d::TiledDepthLayout layout = d3d::MakeTiledDepthLayout(pitch, rows);
        if (!layout.split)
        {
            fprintf(stderr, "FAIL pitch %u: not tile by tile\n", pitch);
            failures++;
        }
        for (uint32_t y = 0; y < rows; y += 7)
            for (uint32_t x = 0; x < pitch; x += 3)
                if (layout.Offset(x, y) != d3d::xenos::Tiled2D(x, y, pitch, 2) / 4 && failures++ < 10)
                    fprintf(stderr, "FAIL pitch %u (%u, %u)\n", pitch, x, y);
    }
    if (failures == 0)
        printf("tiled depth test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
