// The models that show a button over a glyph texture (host/glyphs.h, Model): which draw to skip, which texture matrix to
// set to the identity; and what a keycap shows. No SDL, no GPU, so that SSAGlyphModelsTest links it alone.
#include "host/glyphs.h"
#include <algorithm>
#include <utility>
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

    std::string KeycapLabel(const char* plain, const char* shifted, const std::string& settingName)
    {
        std::string label = plain != nullptr && *plain != '\0' ? plain : settingName;
        // A number-row key types a symbol unshifted on some layouts (French: the 3 key gives '"', which Bungee draws as
        // two bars); its keycap is printed with the digit, which Shift types.
        if (shifted != nullptr && shifted[0] >= '0' && shifted[0] <= '9' && shifted[1] == '\0' &&
            !(label.size() == 1 && label[0] >= '0' && label[0] <= '9'))
            return shifted;
        static const std::pair<const char*, const char*> Short[] = {
            { "Return", "Enter" },
            { "Backspace", "Bksp" },
            { "Escape", "Esc" },
            { "Left Shift", "Shift" },
            { "Right Shift", "Shift" },
            { "Left Ctrl", "Ctrl" },
            { "Right Ctrl", "Ctrl" },
            { "Left Alt", "Alt" },
            { "Right Alt", "Alt" },
            { "Left GUI", "Cmd" },
            { "Right GUI", "Cmd" },
            { "CapsLock", "Caps" },
        };
        for (const auto& [from, to] : Short)
            if (label == from)
                return to;
        if (label.starts_with("Keypad "))
            return "Num" + label.substr(7);
        return label;
    }
}
