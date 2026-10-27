#include "d3d/ultrawide.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace
{
    constexpr float Wide = float(ultrawide::FrameWidth16x9) / ultrawide::FrameHeight;
    constexpr float MaxAspect = 4.0f; // 32:9 is 3.56; anything beyond is clamped (the sides would be mostly sky)

    float g_aspect = Wide;
    uint32_t g_baseWidth = ultrawide::SceneWidth16x9, g_baseHeight = ultrawide::SceneHeight16x9;
    bool g_keepsArea = game::UltrawideSceneKeepsArea;
    uint32_t g_sceneWidth = ultrawide::SceneWidth16x9, g_sceneHeight = ultrawide::SceneHeight16x9;
    uint32_t g_frameWidth = ultrawide::FrameWidth16x9;
    bool g_active;
    std::atomic<int> g_movies;
}

void ultrawide::SetScene16x9(uint32_t width, uint32_t height, bool keepsArea)
{
    g_baseWidth = g_sceneWidth = width;
    g_baseHeight = g_sceneHeight = height;
    g_keepsArea = keepsArea;
}

uint32_t ultrawide::Scene16x9Width() { return g_baseWidth; }
uint32_t ultrawide::Scene16x9Height() { return g_baseHeight; }

void ultrawide::Configure(bool automatic, uint32_t windowWidth, uint32_t windowHeight)
{
    float aspect = windowHeight != 0 ? float(windowWidth) / float(windowHeight) : Wide;
    // A window within 1 % of 16:9 (1366x768, 1920x1080 with a title bar...) behaves exactly as before.
    g_active = automatic && aspect > Wide * 1.01f;
    if (!g_active)
        return;
    g_aspect = std::min(aspect, MaxAspect);
    if (g_keepsArea)
    {
        float area = float(g_baseWidth) * float(g_baseHeight);
        g_sceneWidth = std::min(FrameWidth16x9, uint32_t(std::sqrt(area * g_aspect) / 16.0f) * 16);
        g_sceneHeight = uint32_t(float(g_sceneWidth) / g_aspect / 8.0f) * 8;
    }
    else
        g_sceneWidth = std::max(g_baseWidth, uint32_t(std::lround(g_baseHeight * g_aspect / 32.0f)) * 32);
    g_frameWidth = std::max(FrameWidth16x9, uint32_t(std::lround(FrameHeight * g_aspect)));
    fprintf(stderr, "[ultrawide] window %ux%u: aspect %.3f, scene %ux%u, frame %ux%u\n", windowWidth, windowHeight, g_aspect, g_sceneWidth,
        g_sceneHeight, g_frameWidth, FrameHeight);
}

bool ultrawide::Active() { return g_active; }
float ultrawide::Aspect() { return g_aspect; }
uint32_t ultrawide::SceneWidth() { return g_sceneWidth; }
uint32_t ultrawide::SceneHeight() { return g_sceneHeight; }
uint32_t ultrawide::FrameWidth() { return g_frameWidth; }
void ultrawide::MovieOpened() { g_movies++; }
void ultrawide::MovieClosed() { g_movies = std::max(g_movies - 1, 0); }
bool ultrawide::MoviePlaying() { return g_movies > 0; }
