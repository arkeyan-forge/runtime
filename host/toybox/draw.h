#pragma once

// The Toy Box's look: everything drawn, nothing decided (the logic is model.h). The design's tokens and pure logic are
// look.h (docs/runtime/host/ingame_ui.md). Drawn with Dear ImGui into a 1280x720 RGBA image on the CPU
// (host/imgui_software.h), letterboxed over the game like its frames.
#include "host/toybox/model.h"

namespace toybox
{
    constexpr int CanvasWidth = 1280, CanvasHeight = 720;

    // Fonts, the backdrop. False if the fonts did not load.
    bool DrawInit();

    // The current screen into `pixels` (CanvasWidth x CanvasHeight, RGBA). Over the game (the wheel, its list, a toast
    // with the Toy Box closed) the image is transparent around what is drawn.
    void Draw(Model& model, std::vector<uint32_t>& pixels);

    // Mouse, in canvas pixels, on what the last Draw showed: button 1 left, 3 right; `twice` for a double click.
    void Click(Model& model, float x, float y, int button, bool twice);
    // The pointer in canvas pixels (`inside` false: off the canvas): what is under it gets a hover outline. True when that
    // changed since the last Draw (draw again).
    bool Pointer(float x, float y, bool inside);
    // The hoverable hit under the pointer, not the keyboard's selected row/card; "none" for a miss or backdrop.
    std::string HoverTarget();
    // The wheel's place under a canvas point (-1 the centre, -2 outside it).
    int WheelPlaceAt(float x, float y);
}
