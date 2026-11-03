#pragma once

// The "Edge smoothing" setting (host/settings.h edge_smoothing, docs/getting-started/settings.md): what the game's FXAA pass
// (game::HostKernelPixelShader) does. FXAA also softens fine texture detail, which a higher resolution scale shows less need
// for: Game leaves the pass as the game sets it, Light only filters the strong edges, Off draws the plain copy.
#include <cstdint>
#include <string_view>

namespace edgesmoothing
{
    enum Mode : uint32_t
    {
        Auto, // not chosen: Light from resolution scale 2 in a level, Game at scale 1 and in the game's menus
        Game,
        Light,
        Off
    };

    // `menu`: the title and the game's menus (game::InMenuLevel), 2D art whose cut-out edges only the game's pass smooths
    // (Trap Team's title logo stair-stepped under Light at scale 2); fine 3D texture detail is in the levels.
    constexpr Mode Resolve(Mode mode, uint32_t scale, bool menu)
    {
        return mode != Auto ? mode : scale >= 2 && !menu ? Light : Game;
    }

    // The FXAA shader leaves a pixel alone when its local contrast is below max(minimum, threshold x the brightest luma):
    // the game's own are 0.125 and 0.05. Light raises both; Off raises them past any contrast (a plain copy).
    constexpr float LightThreshold = 0.5f, LightMinimum = 0.2f, OffThreshold = 1000.0f, OffMinimum = 1000.0f;

    // The setting's text: "auto", "game", "light", "off".
    constexpr const char* Name(Mode mode)
    {
        return mode == Game ? "game" : mode == Light ? "light" : mode == Off ? "off" : "auto";
    }

    constexpr bool Parse(std::string_view text, Mode& mode)
    {
        for (Mode m : { Auto, Game, Light, Off })
            if (text == Name(m))
            {
                mode = m;
                return true;
            }
        return false;
    }
}
