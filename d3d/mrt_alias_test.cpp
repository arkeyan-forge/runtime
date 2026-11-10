// Checks which render target slots of one draw need a host target of their own. usage: ArkeyanMrtAliasTest
#include "d3d/mrt_alias.h"
#include <cstdio>

namespace
{
    int g_failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)
}

int main()
{
    // Imaginators' G-buffer: albedo at tile 0x400, then two surfaces at tile 0 with the same format and size.
    const uint64_t gbufferKeys[3] = { 0x400, 0, 0 };
    const uint32_t gbufferSurfaces[3] = { 0xA3B8F374, 0xA3B8F3DC, 0xA3B8F444 };
    CHECK(d3d::SlotsNeedingOwnTarget(gbufferKeys, gbufferSurfaces, 3) == 0x4);

    // Distinct keys share nothing; one surface bound twice is the same target on purpose.
    const uint64_t distinctKeys[4] = { 0, 0x2D0, 0x5A0, 0x870 };
    const uint32_t distinctSurfaces[4] = { 1, 2, 3, 4 };
    CHECK(d3d::SlotsNeedingOwnTarget(distinctKeys, distinctSurfaces, 4) == 0);
    const uint64_t sameKeys[2] = { 7, 7 };
    const uint32_t sameSurface[2] = { 5, 5 };
    CHECK(d3d::SlotsNeedingOwnTarget(sameKeys, sameSurface, 2) == 0);

    // Every later repeat of a key needs its own; a single slot never does.
    const uint64_t threeKeys[4] = { 9, 9, 9, 1 };
    const uint32_t threeSurfaces[4] = { 1, 2, 3, 4 };
    CHECK(d3d::SlotsNeedingOwnTarget(threeKeys, threeSurfaces, 4) == 0x6);
    CHECK(d3d::SlotsNeedingOwnTarget(threeKeys, threeSurfaces, 1) == 0);

    if (g_failures == 0)
        printf("ArkeyanMrtAliasTest: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
