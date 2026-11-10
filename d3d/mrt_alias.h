#pragma once

#include <cstdint>

namespace d3d
{
    // Host colour targets are found by their EDRAM key (base tile, format, size), so that two surface objects over the
    // same EDRAM share one, as on the console. Imaginators' G-buffer binds two different surface objects with the same
    // key (base tile 0, k_8_8_8_8, 1280x720) as render targets 1 and 2 of one draw, and reads them back as two textures:
    // one host target for both keeps only one of them. The slots of one draw's colour set whose key repeats an earlier
    // slot's while their surface is another object (bit i: slot i) need a host target of their own.
    inline uint32_t SlotsNeedingOwnTarget(const uint64_t* keys, const uint32_t* surfaces, uint32_t count)
    {
        uint32_t slots = 0;
        for (uint32_t i = 1; i < count; i++)
            for (uint32_t j = 0; j < i; j++)
                if (keys[i] == keys[j] && surfaces[i] != surfaces[j])
                {
                    slots |= 1u << i;
                    break;
                }
        return slots;
    }
}
