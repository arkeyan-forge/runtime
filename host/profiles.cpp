#include "host/profiles.h"
#include "host/env.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stb_image.h>

namespace profiles
{
    namespace
    {
        // The next code point of UTF-8 text from `at` (moved past it); 0xFFFD for a broken sequence.
        uint32_t NextCodePoint(std::string_view text, size_t& at)
        {
            auto byte = [&](size_t i) { return uint8_t(text[i]); };
            uint8_t lead = byte(at++);
            if (lead < 0x80)
                return lead;
            size_t extra = lead >= 0xF0 && lead < 0xF8 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC2 && lead < 0xE0 ? 1 : 0;
            if (extra == 0 || lead >= 0xF8)
                return 0xFFFD;
            uint32_t point = lead & (0x3F >> extra);
            for (size_t i = 0; i < extra; i++, at++)
            {
                if (at >= text.size() || (byte(at) & 0xC0) != 0x80)
                    return 0xFFFD;
                point = (point << 6) | (byte(at) & 0x3F);
            }
            return point;
        }

        // The French accented letters (Latin-1's code points, the same numbers as Unicode's) and the plain letters they
        // become when the game's font lacks them or the name is too long to keep them.
        constexpr std::u32string_view FrenchLetters =
            U"\xC0\xC2\xC4\xC6\xC7\xC8\xC9\xCA\xCB\xCE\xCF\xD4\xD6\xD9\xDB\xDC"
            U"\xE0\xE2\xE4\xE6\xE7\xE8\xE9\xEA\xEB\xEE\xEF\xF4\xF6\xF9\xFB\xFC\xFF";
        constexpr std::u32string_view PlainLetters = U"AAAACEEEEIIOOUUUaaaaceeeeiioouuuy";

        // The plain form of a French accented letter ("AE" for "Æ"); empty for any other letter.
        std::u32string_view Plain(char32_t letter)
        {
            if (letter == 0xC6)
                return U"AE";
            if (letter == 0xE6)
                return U"ae";
            size_t at = FrenchLetters.find(letter);
            return at == std::u32string_view::npos ? std::u32string_view() : PlainLetters.substr(at, 1);
        }

        bool IsAsciiAlphanumeric(uint32_t point)
        {
            return (point >= '0' && point <= '9') || (point >= 'A' && point <= 'Z') || (point >= 'a' && point <= 'z');
        }

        bool Contains(std::string_view utf8, uint32_t point)
        {
            for (size_t at = 0; at < utf8.size();)
                if (NextCodePoint(utf8, at) == point)
                    return true;
            return false;
        }

        void AppendUtf8(std::string& text, uint32_t point)
        {
            if (point < 0x80)
                text += char(point);
            else
            {
                text += char(0xC0 | (point >> 6));
                text += char(0x80 | (point & 0x3F));
            }
        }
    }

    std::string GameName(std::string_view utf8, std::string_view fallback, std::string_view drawnAccents)
    {
        // The letters kept, as code points below U+0100: spaces never doubled nor leading, at most MaxNameLength.
        std::u32string letters;
        auto add = [&](std::u32string_view more) {
            for (char32_t letter : more)
                if (!(letter == U' ' && (letters.empty() || letters.back() == U' ')) && letters.size() < MaxNameLength)
                    letters += letter;
        };
        for (size_t at = 0; at < utf8.size();)
        {
            uint32_t point = NextCodePoint(utf8, at);
            std::u32string_view plain = Plain(char32_t(point));
            if (IsAsciiAlphanumeric(point))
                add(std::u32string(1, char32_t(point)));
            else if (!plain.empty())
                add(drawnAccents.empty() || Contains(drawnAccents, point) ? std::u32string(1, char32_t(point))
                                                                          : std::u32string(plain));
            else if (point == 0x152)
                add(U"OE");
            else if (point == 0x153)
                add(U"oe");
            else if (point == 0x178)
                add(U"Y");
            else
                add(U" ");
        }
        while (!letters.empty() && letters.back() == U' ')
            letters.pop_back();
        if (letters.empty())
            return std::string(fallback);
        // UTF-8, the games' encoding; when the accents make it longer than the profile's 15 bytes, the plain letters.
        std::string name;
        for (char32_t letter : letters)
            AppendUtf8(name, letter);
        if (name.size() <= MaxNameLength)
            return name;
        name.clear();
        for (char32_t letter : letters)
        {
            std::u32string_view plain = Plain(letter);
            if (plain.empty())
                name += char(letter);
            for (char32_t part : plain)
                name += char(part);
        }
        return name.substr(0, MaxNameLength);
    }

    size_t FittingBytes(std::string_view text, size_t capacity)
    {
        if (text.size() <= capacity)
            return text.size();
        size_t length = capacity;
        while (length > 0 && (uint8_t(text[length]) & 0xC0) == 0x80)
            length--;
        return length;
    }

    bool ParseXuid(std::string_view text, uint64_t& xuid)
    {
        if (text.size() != 16 ||
            !std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(uint8_t(c)); }))
            return false;
        uint64_t value = std::strtoull(std::string(text).c_str(), nullptr, 16);
        if (value == 0)
            return false;
        xuid = value;
        return true;
    }

    std::array<Profile, 2> Parse(const std::function<const char*(const char*)>& lookup, std::string_view drawnAccents)
    {
        struct Names
        {
            const char *name, *picture, *xuid, *fallbackName;
            uint64_t fallbackXuid;
        };
        static constexpr Names Players[2] = {
            { "SKY_PLAYER_NAME", "SKY_PLAYER_PICTURE", "SKY_PLAYER_XUID", "Player", DefaultXuid },
            { "SKY_PLAYER2_NAME", "SKY_PLAYER2_PICTURE", "SKY_PLAYER2_XUID", "Player 2", DefaultXuid2 },
        };
        std::array<Profile, 2> result;
        for (size_t i = 0; i < 2; i++)
        {
            const Names& names = Players[i];
            const char* name = skyenv::Get(names.name, lookup);
            Profile& profile = result[i];
            profile.signedIn = i == 0 || (name != nullptr && *name != '\0');
            if (!profile.signedIn)
                continue;
            profile.name = GameName(name ? name : "", names.fallbackName, drawnAccents);
            profile.xuid = names.fallbackXuid;
            if (const char* xuid = skyenv::Get(names.xuid, lookup))
                ParseXuid(xuid, profile.xuid);
            if (const char* picture = skyenv::Get(names.picture, lookup))
                profile.picture = picture;
        }
        return result;
    }

    bool DecodePicture(const uint8_t* data, size_t size, Picture& picture)
    {
        int width = 0, height = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(data, int(size), &width, &height, &channels, 4);
        if (pixels == nullptr)
            return false;
        picture.width = width;
        picture.height = height;
        picture.rgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
        stbi_image_free(pixels);
        return true;
    }

    void WriteTile(const Picture& picture, uint32_t width, uint32_t height, uint32_t pitch, uint8_t* out)
    {
        width = std::min(width, pitch / 4);
        std::fill(out, out + size_t(pitch) * height, uint8_t(0));
        if (picture.width <= 0 || picture.height <= 0 || width == 0 || height == 0)
            return;
        // The centred square, each tile pixel the average of the source pixels it covers (at least one), weighted by
        // their alpha so the colour under transparent pixels does not bleed in.
        const uint32_t side = uint32_t(std::min(picture.width, picture.height));
        const uint32_t left = (uint32_t(picture.width) - side) / 2, top = (uint32_t(picture.height) - side) / 2;
        for (uint32_t y = 0; y < height; y++)
        {
            uint32_t y0 = top + y * side / height, y1 = std::max(top + (y + 1) * side / height, y0 + 1);
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t x0 = left + x * side / width, x1 = std::max(left + (x + 1) * side / width, x0 + 1);
                uint64_t r = 0, g = 0, b = 0, a = 0, count = 0;
                for (uint32_t sy = y0; sy < y1; sy++)
                    for (uint32_t sx = x0; sx < x1; sx++)
                    {
                        const uint8_t* p = &picture.rgba[(size_t(sy) * size_t(picture.width) + sx) * 4];
                        r += uint64_t(p[0]) * p[3];
                        g += uint64_t(p[1]) * p[3];
                        b += uint64_t(p[2]) * p[3];
                        a += p[3];
                        count++;
                    }
                uint8_t* q = out + size_t(y) * pitch + size_t(x) * 4;
                q[0] = uint8_t((a + count / 2) / count);
                q[1] = a ? uint8_t((r + a / 2) / a) : 0;
                q[2] = a ? uint8_t((g + a / 2) / a) : 0;
                q[3] = a ? uint8_t((b + a / 2) / a) : 0;
            }
        }
    }
}
