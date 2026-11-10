// Checks the wide scene and frame sizes, and the scene's width bound (d3d/ultrawide.h). usage: ArkeyanUltrawideTest
#include "d3d/ultrawide.h"
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
    // A full-resolution scene (1280x720) with no bound: 32:9 gives 2560x720.
    ultrawide::SetScene16x9(1280, 720, false);
    ultrawide::Configure(true, 3840, 1080);
    CHECK(ultrawide::Active());
    CHECK(ultrawide::SceneWidth() == 2560 && ultrawide::SceneHeight() == 720);
    CHECK(ultrawide::FrameWidth() == 2560);

    // Bounded at 2048 (SuperChargers): 32:9 keeps its shape at 2048x576; the frame does not change.
    ultrawide::SetScene16x9(1280, 720, false, 2048);
    ultrawide::Configure(true, 3840, 1080);
    CHECK(ultrawide::SceneWidth() == 2048 && ultrawide::SceneHeight() == 576);
    CHECK(ultrawide::FrameWidth() == 2560);

    // 21:9 is under the bound: 1696x720, as without it.
    ultrawide::SetScene16x9(1280, 720, false, 2048);
    ultrawide::Configure(true, 2560, 1080);
    CHECK(ultrawide::SceneWidth() == 1696 && ultrawide::SceneHeight() == 720);

    // 16:9: nothing is active.
    ultrawide::SetScene16x9(1280, 720, false, 2048);
    ultrawide::Configure(true, 1280, 720);
    CHECK(!ultrawide::Active());

    printf("ultrawide test: %s\n", g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
