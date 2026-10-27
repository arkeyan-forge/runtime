// Ultrawide: a window wider than 16:9 shows more of the world instead of bars or a stretched picture.
//
// The game draws the 3D scene into 16:9 targets (SSA 1120x704, Giants 1000x600) and composes it with the HUD into a
// 1280x720 frame. With a wider window the camera's aspect is widened (Hor+: the vertical field of view stays), the
// scene targets become `scene height x aspect` wide and the frame `720 x aspect`; the composite fills the frame and the HUD, drawn for 1280x720,
// stays centred (d3d/render.cpp). The sizes are chosen once, at start, from the window: the targets cannot change while
// the title runs, so a later resize only changes how the presenter scales the frame.
// The game's own addresses are in engines/<engine>/ultrawide.cpp and games/<id>/adapter.cpp.
#pragma once

#include "game_hooks.h"
#include <cstdint>

namespace ultrawide
{
    // The game's 3D scene at 16:9 (SSA 1120x704, Giants 1000x600: games/<id>/addresses.h).
    constexpr uint32_t SceneWidth16x9 = game::UltrawideSceneWidth, SceneHeight16x9 = game::UltrawideSceneHeight;
    constexpr uint32_t FrameWidth16x9 = 1280, FrameHeight = 720;

    // `automatic` is [video] aspect=auto; the window's size in pixels. Called once before the title starts.
    void Configure(bool automatic, uint32_t windowWidth, uint32_t windowHeight);

    // Whether the window is wider than 16:9 and the setting allows it.
    bool Active();

    // The window's aspect when Active(), else 16:9 (the game's own).
    float Aspect();

    // The 3D scene's size: SceneWidth16x9 x SceneHeight16x9 unless Active(). Wide (SSA): the height stays, the width is
    // height x aspect, a multiple of 32 (the sun tiles are 32x32 pixels). With game::UltrawideSceneKeepsArea (Giants)
    // the scene keeps its 16:9 pixel count and is at most 1280 wide, so that it still fits the console's 10 MB of
    // EDRAM (a bigger one makes the game split it into tiles) and the game's viewports (capped at the back buffer's
    // width): 2560x1080 gives 1184x496; width a multiple of 16, height of 8 (the passes halve it three times).
    uint32_t SceneWidth();
    uint32_t SceneHeight();

    // The frame's width (its height stays 720): 1280 unless Active(), else 720 x aspect.
    uint32_t FrameWidth();

    // Movies keep 16:9 with black bars: while one is open (the Bink hooks count them, d3d/bink_display.cpp), the scene
    // the game may still draw behind it stays 16:9 too and the sides are cleared to black.
    void MovieOpened();
    void MovieClosed();
    bool MoviePlaying();
}
