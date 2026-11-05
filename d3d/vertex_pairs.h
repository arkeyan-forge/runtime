#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace d3d
{
    // Vertex data is byte-swapped in 32-bit words (render.cpp), which leaves a 16-bit component pair's halves swapped:
    // (y, x) for (x, y). Swaps them back in the `size` bytes at `offset` of every `stride`-byte vertex of `data`.
    inline void SwapHalfPairs(uint8_t* data, size_t bytes, uint32_t stride, uint32_t offset, uint32_t size)
    {
        if (stride == 0)
            return;
        for (size_t vertex = 0; vertex + offset + size <= bytes; vertex += stride)
            for (uint32_t word = 0; word + 4 <= size; word += 4)
            {
                uint8_t* at = data + vertex + offset + word;
                uint32_t value;
                memcpy(&value, at, 4);
                value = value >> 16 | value << 16;
                memcpy(at, &value, 4);
            }
    }
}
