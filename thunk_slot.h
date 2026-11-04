#pragma once

#include <cstdint>

// Where CreateHostThunk (loader.cpp) puts its next thunk: the top word of the first run of `wanted` padding words found
// going down from below `next` to `base`, or 0 when there is none. A thunk is only an entry of the function table: a
// padding word (zero, and no function starts there) is never a call target.
template <typename IsPadding>
uint32_t FindThunkSlot(uint32_t next, uint32_t base, uint32_t wanted, IsPadding isPadding)
{
    uint32_t run = 0;
    for (uint32_t address = next - 4; address >= base && address < next; address -= 4)
    {
        run = isPadding(address) ? run + 1 : 0;
        if (run == wanted)
            return address + (wanted - 1) * 4;
    }
    return 0;
}
