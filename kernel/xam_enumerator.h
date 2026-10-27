#pragma once

#include "kernel/objects.h"
#include <cstdint>
#include <vector>

// An XEnumerate handle (XamEnumerate, kernel/xam_achievements.cpp): fixed-size items, already in guest byte order,
// handed out in order, at most `itemsPerEnumerate` per call. The achievement enumerator has none; the content
// enumerator (kernel/xam_content.cpp) lists the packages on the hard drive.
struct XamEnumerator : KernelObject
{
    uint32_t itemSize = 0;
    uint32_t itemsPerEnumerate = 0;
    std::vector<uint8_t> items; // itemSize bytes each
    uint32_t next = 0; // index of the next item to hand out
};
