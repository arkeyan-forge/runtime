#pragma once

#include "kernel/objects.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// An XEnumerate handle (XamEnumerate, kernel/xam_achievements.cpp): items handed out in order, at most
// `itemsPerEnumerate` per call. The base class holds fixed-size items, already in guest byte order (the content enumerator,
// kernel/xam_content.cpp, lists the packages on the hard drive); the achievement enumerator overrides Total and Fill, as
// its items carry strings placed after them in the title's buffer.
struct XamEnumerator : KernelObject
{
    uint32_t itemSize = 0;
    uint32_t itemsPerEnumerate = 0;
    std::vector<uint8_t> items; // itemSize bytes each
    uint32_t next = 0; // index of the next item to hand out

    // How many items there are in all.
    virtual uint32_t Total() const { return itemSize != 0 ? uint32_t(items.size() / itemSize) : 0; }
    // Writes up to `maxItems` items from `next` into the guest buffer; returns how many fit in `bufferSize`.
    virtual uint32_t Fill(uint32_t guestBuffer, uint32_t bufferSize, uint32_t maxItems)
    {
        uint32_t count = std::min(maxItems, bufferSize / itemSize);
        if (guestBuffer != 0 && count != 0)
            memcpy(g_memory.Translate(guestBuffer), items.data() + size_t(next) * itemSize, size_t(count) * itemSize);
        return count;
    }
};
