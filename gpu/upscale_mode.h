#pragma once

// The upscaling settings (docs/runtime/gpu/upscaling.md): [<game>.video] upscaler = off | fsr3 and fsr_quality =
// native | quality | balanced | performance | ultra_performance (SKY_UPSCALER, SKY_FSR_QUALITY), and how they set the
// scales.
//
// The game's targets render at a whole multiple of the guest's size, so a preset picks the scene's scale nearest to the
// frame's scale over its ratio (FSR's 1.5, 1.7, 2, 3): at a 1440p window (frame scale 2) every preset renders the scene
// at scale 1; at 4K (frame scale 3) quality, balanced and performance at 2, ultra performance at 1. When that is the
// frame's own scale (a 720p window), FSR runs at 1:1, as anti-aliasing. Native (AMD's native AA, ratio 1) always does:
// the scene at the frame's scale, so frame generation runs without a smaller picture.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace upscalemode
{
    enum class Upscaler : uint8_t
    {
        Off,
        Fsr3,
        MetalFx, // macOS, internal comparison only: SKY_UPSCALER=metalfx, never in the settings file or the menus
    };

    enum class Quality : uint8_t
    {
        Quality,
        Balanced,
        Performance,
        UltraPerformance,
        Native, // last: the others keep their values; the menus list it first
    };
    constexpr uint32_t QualityCount = 5;

    // `internal`: the environment variable, which may also name MetalFX.
    inline bool ParseUpscaler(std::string_view text, Upscaler& out, bool internal = false)
    {
        if (text == "off" || text == "0")
            out = Upscaler::Off;
        else if (text == "fsr3")
            out = Upscaler::Fsr3;
        else if (internal && text == "metalfx")
            out = Upscaler::MetalFx;
        else
            return false;
        return true;
    }

    inline const char* Name(Upscaler upscaler)
    {
        switch (upscaler)
        {
        case Upscaler::Fsr3: return "fsr3";
        case Upscaler::MetalFx: return "metalfx";
        default: return "off";
        }
    }

    inline bool ParseQuality(std::string_view text, Quality& out)
    {
        if (text == "quality")
            out = Quality::Quality;
        else if (text == "balanced")
            out = Quality::Balanced;
        else if (text == "performance")
            out = Quality::Performance;
        else if (text == "ultra_performance")
            out = Quality::UltraPerformance;
        else if (text == "native")
            out = Quality::Native;
        else
            return false;
        return true;
    }

    inline const char* Name(Quality quality)
    {
        switch (quality)
        {
        case Quality::Balanced: return "balanced";
        case Quality::Performance: return "performance";
        case Quality::UltraPerformance: return "ultra_performance";
        case Quality::Native: return "native";
        default: return "quality";
        }
    }

    // FSR 3's ratio of the output to the render size for each preset.
    constexpr float Ratio(Quality quality)
    {
        switch (quality)
        {
        case Quality::Balanced: return 1.7f;
        case Quality::Performance: return 2.0f;
        case Quality::UltraPerformance: return 3.0f;
        case Quality::Native: return 1.0f;
        default: return 1.5f;
        }
    }

    // The frame's scale for a window: its height over the guest's 720 lines, rounded, at least 1.
    inline uint32_t FrameScale(uint32_t windowHeight)
    {
        return std::max(uint32_t(std::lround(double(windowHeight) / 720.0)), 1u);
    }

    // The scene's scale for a preset: the frame's scale over the preset's ratio, rounded, from 1 to the frame's scale.
    inline uint32_t SceneScale(uint32_t frameScale, Quality quality)
    {
        const long scale = std::lround(double(frameScale) / double(Ratio(quality)));
        return uint32_t(std::clamp(scale, 1L, long(std::max(frameScale, 1u))));
    }
}
