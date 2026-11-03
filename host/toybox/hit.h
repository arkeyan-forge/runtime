#pragma once

#include <span>

namespace toybox
{
    // Last drawn is topmost. A backdrop still blocks hits beneath it, even without an action or hover outline.
    template <class Hit>
    int HitAt(std::span<const Hit> hits, float x, float y)
    {
        for (int i = int(hits.size()) - 1; i >= 0; i--)
        {
            const Hit& hit = hits[size_t(i)];
            if (x >= hit.min.x && y >= hit.min.y && x < hit.max.x && y < hit.max.y)
                return i;
        }
        return -1;
    }
}
