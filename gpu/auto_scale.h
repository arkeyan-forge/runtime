#pragma once

// "Resolution scale auto" steps down when the GPU, not the CPU, limits the frame rate (gpu/gpu.cpp): the scale cannot
// change in play (render targets and resolved textures have it), so a run whose GPU held the frame rate under the limit
// writes the next scale down, for its adapter, to auto_scale.txt in the game's data folder, and the next start with
// auto uses it (both logged). A scale the player chose is never changed; another adapter, or the file deleted, starts
// from the device's own choice again (docs/runtime/gpu/renderer_threads.md, "Where the frame goes").
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace gpu::autoscale
{
    // One line: "<scale> <adapter name>".
    inline std::string Format(uint32_t scale, std::string_view adapter)
    {
        return std::to_string(scale) + " " + std::string(adapter) + "\n";
    }

    // The scale remembered for `adapter`, 0 for none (another adapter, an unreadable file).
    inline uint32_t Parse(std::string_view text, std::string_view adapter)
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.remove_suffix(1);
        const size_t space = text.find(' ');
        uint32_t scale = 0;
        if (space == std::string_view::npos || text.substr(space + 1) != adapter ||
            std::from_chars(text.data(), text.data() + space, scale).ptr != text.data() + space)
            return 0;
        return scale >= 1 && scale <= 10 ? scale : 0;
    }
}
