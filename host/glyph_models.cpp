// The models that show a button over a glyph texture (host/glyphs.h, Model): which draw to skip, which texture matrix to
// set to the identity. No SDL, no GPU, so that SSAGlyphModelsTest links it alone.
#include "host/glyphs.h"
#include <algorithm>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace glyphs
{
    bool HidesDraw(std::span<const Texture> textures, Style style, uint32_t count, const void* indices)
    {
        if (style == Style::Xbox360 || count == 0 || indices == nullptr)
            return false;
        uint64_t hash = 0;
        for (const Texture& known : textures)
        {
            if (known.model == nullptr || known.model->lettersIndexCount != count ||
                (known.glyph >= Glyph::Controller && style != Style::Keyboard))
                continue;
            if (hash == 0)
                hash = XXH3_64bits(indices, size_t(count) * 2);
            if (hash == known.model->lettersIndexHash)
                return true;
        }
        return false;
    }

    int UvMatrix(std::span<const Texture> textures, uint64_t vertexShader, std::span<const uint64_t> glyphHashes)
    {
        for (const Texture& known : textures)
            if (known.model != nullptr &&
                std::ranges::find(known.model->vertexShaders, vertexShader) != known.model->vertexShaders.end())
                for (uint64_t hash : glyphHashes)
                    if (hash == known.hash)
                        return int(known.model->uvMatrix);
        return -1;
    }
}
