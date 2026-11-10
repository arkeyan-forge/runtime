// The models that show a button over a glyph texture (host/glyphs.h, Model): which draw to skip, which texture matrix to
// set to the identity; and what a keycap shows. No SDL, no GPU, so that ArkeyanGlyphModelsTest links it alone.
#include "host/glyphs.h"
#include <algorithm>
#include <bit>
#include <cstring>
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

    PromptPart PromptPartOf(
        std::span<const Prompt> prompts, Style style, uint32_t count, const void* indices, uint64_t& hash)
    {
        if (style == Style::Xbox360 || count == 0 || indices == nullptr)
            return PromptPart::None;
        bool hashed = false;
        PromptPart found = PromptPart::None;
        for (const Prompt& prompt : prompts)
        {
            const bool face = prompt.faceCount == count, letters = prompt.lettersCount == count;
            if (!face && !letters)
                continue;
            if (!hashed)
            {
                hash = XXH3_64bits(indices, size_t(count) * 2);
                hashed = true;
            }
            if (face && prompt.faceHash == hash)
                return PromptPart::Face;
            if (letters && prompt.lettersHash == hash)
                found = PromptPart::Letters;
        }
        return found;
    }

    const Prompt* PromptOf(std::span<const Prompt> prompts, uint32_t faceCount, uint64_t faceHash,
        uint32_t lettersCount, uint64_t lettersHash)
    {
        for (const Prompt& prompt : prompts)
            if (prompt.faceCount == faceCount && prompt.faceHash == faceHash && prompt.lettersCount == lettersCount &&
                prompt.lettersHash == lettersHash)
                return &prompt;
        return nullptr;
    }

    namespace
    {
        // A float in [0, 1] as an IEEE half (round to nearest, no denormals needed: the coordinates are 0 or at least 1/4096).
        uint16_t HalfOf(float value)
        {
            uint32_t bits = std::bit_cast<uint32_t>(value);
            if ((bits & 0x7FFFFFFF) < 0x38800000) // below 2^-14: denormal range, flushed (a coordinate that small is 0)
                return uint16_t(bits >> 16 & 0x8000);
            uint32_t half = ((bits & 0x7FFFFFFF) - 0x38000000 + 0x1000) >> 13; // rebias 127 -> 15, round
            return uint16_t((bits >> 16 & 0x8000) | std::min(half, 0x7BFFu));
        }
    }

    bool PlanarUv(const Prompt& prompt, std::span<uint8_t> vertices, uint32_t stride, uint32_t positionOffset,
        uint32_t uvOffset, std::span<const uint16_t> indices, int32_t baseVertex)
    {
        if (stride == 0 || positionOffset + 6 > stride || uvOffset + 4 > stride || prompt.uAxis > 2 || prompt.vAxis > 2)
            return false;
        auto vertexAt = [&](uint16_t index) -> uint8_t* {
            int64_t at = (int64_t(index) + baseVertex) * stride;
            return at >= 0 && at + stride <= int64_t(vertices.size()) ? vertices.data() + at : nullptr;
        };
        auto component = [&](const uint8_t* vertex, uint32_t axis) {
            int16_t value;
            memcpy(&value, vertex + positionOffset + 2 * axis, 2);
            return int32_t(value);
        };
        int32_t low[2] = { INT32_MAX, INT32_MAX }, high[2] = { INT32_MIN, INT32_MIN };
        const uint32_t axes[2] = { prompt.uAxis, prompt.vAxis };
        for (uint16_t index : indices)
            if (const uint8_t* vertex = vertexAt(index))
                for (int i = 0; i < 2; i++)
                {
                    low[i] = std::min(low[i], component(vertex, axes[i]));
                    high[i] = std::max(high[i], component(vertex, axes[i]));
                }
        if (low[0] >= high[0] || low[1] >= high[1])
            return false;
        for (uint16_t index : indices)
            if (uint8_t* vertex = vertexAt(index))
            {
                float u = float(component(vertex, prompt.uAxis) - low[0]) / float(high[0] - low[0]);
                if (prompt.uScale > 0)
                    u = 0.5f + (u - 0.5f) / prompt.uScale;
                float v = float(component(vertex, prompt.vAxis) - low[1]) / float(high[1] - low[1]);
                uint16_t halves[2] = { HalfOf(prompt.flipV ? 1 - v : v), HalfOf(prompt.flipU ? 1 - u : u) };
                memcpy(vertex + uvOffset, halves, 4);
            }
        return true;
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
