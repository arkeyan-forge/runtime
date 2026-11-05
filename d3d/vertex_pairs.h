#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#endif

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

    // The 4-byte words of a vertex (bit i: bytes 4i to 4i+3) that an element of `size` bytes at `offset` covers, for
    // SwapVertexWords; 0 when they do not fit the mask (an unaligned element, or past the 64th word).
    inline uint64_t PairWords(uint32_t offset, uint32_t size)
    {
        if (offset % 4 != 0 || size % 4 != 0 || offset + size > 256)
            return 0;
        uint64_t words = 0;
        for (uint32_t word = offset / 4; word < (offset + size) / 4; word++)
            words |= 1ull << word;
        return words;
    }

    // Guest vertices to host order in one pass, for the GPU's buffer cache: each 32-bit word byte-swapped, and the words
    // of `pairWords` (bit i: word i of every `stride`-byte vertex) 16-bit swapped instead, the result of SwapHalfPairs
    // after the 32-bit swap. `source` starts at a vertex.
    inline void SwapVertexWords(
        const uint8_t* source, uint8_t* destination, size_t bytes, uint32_t stride, uint64_t pairWords)
    {
        size_t i = 0;
        for (; i + 4 <= bytes; i += 4)
        {
            uint32_t value;
            memcpy(&value, source + i, 4);
#if defined(_MSC_VER) && !defined(__clang__)
            value = _byteswap_ulong(value);
#else
            value = __builtin_bswap32(value);
#endif
            memcpy(destination + i, &value, 4);
        }
        memcpy(destination + i, source + i, bytes - i);
        if (pairWords == 0 || stride == 0)
            return;
        for (size_t vertex = 0; vertex < bytes; vertex += stride)
            for (uint64_t words = pairWords; words != 0; words &= words - 1)
            {
                size_t at = vertex + 4 * size_t(std::countr_zero(words));
                if (at + 4 > bytes)
                    break;
                uint32_t value;
                memcpy(&value, destination + at, 4);
                value = value >> 16 | value << 16;
                memcpy(destination + at, &value, 4);
            }
    }
}
