// The models that show a button over a glyph texture (host/glyphs.h, Model): a draw is skipped only when its index count
// and the hash of its indices are a listed model's letters, and only while a style other than the Xbox 360's draws the
// glyph; the texture matrix is set to the identity only for the model's shader with the glyph bound. No SDL, no GPU, no
// game data: ./SSAGlyphModelsTest
#include "host/glyphs.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <cstdio>

using namespace glyphs;

namespace
{
    int g_checks = 0, g_failures = 0;
#define EXPECT(condition)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)
}

int main()
{
    // Three triangles of a model's letters, and a draw with as many indices but other ones.
    const uint16_t letters[9] = { 0x0000, 0x0100, 0x0200, 0x0200, 0x0100, 0x0300, 0x0400, 0x0500, 0x0600 };
    const uint16_t other[9] = { 0x0000, 0x0200, 0x0100, 0x0200, 0x0100, 0x0300, 0x0400, 0x0500, 0x0600 };
    const uint64_t shaders[] = { 0xABCD, 0xABCF };
    const Model card = { 9, XXH3_64bits(letters, sizeof(letters)), shaders, 5 };
    const Texture textures[] = {
        { 0x1111, "plain glyph", Glyph::A },
        { 0x2222, "card", Glyph::RightTrigger, {}, true, true, &card },
    };

    EXPECT(HidesDraw(textures, Style::Keyboard, 9, letters));
    EXPECT(HidesDraw(textures, Style::PlayStation, 9, letters));
    EXPECT(HidesDraw(textures, Style::XboxOne, 9, letters));
    EXPECT(!HidesDraw(textures, Style::Xbox360, 9, letters)); // the game's own card and letters
    EXPECT(!HidesDraw(textures, Style::Keyboard, 9, other)); // same count, another mesh
    EXPECT(!HidesDraw(textures, Style::Keyboard, 6, letters)); // another count: not hashed
    EXPECT(!HidesDraw(textures, Style::Keyboard, 0, letters));
    EXPECT(!HidesDraw(textures, Style::Keyboard, 9, nullptr));
    EXPECT(!HidesDraw({}, Style::Keyboard, 9, letters));

    // A keyboard-only glyph keeps the game's picture on a pad, so its letters stay too.
    const Texture keyboardOnly[] = { { 0x3333, "diagram", Glyph::Controller, {}, false, false, &card } };
    EXPECT(HidesDraw(keyboardOnly, Style::Keyboard, 9, letters));
    EXPECT(!HidesDraw(keyboardOnly, Style::Nintendo, 9, letters));

    // The texture matrix: the model's shader with the card drawn as a glyph in one of its slots.
    const uint64_t bound[] = { 0, 0x2222, 0 };
    const uint64_t plain[] = { 0x1111, 0, 0 };
    EXPECT(UvMatrix(textures, 0xABCD, bound) == 5);
    EXPECT(UvMatrix(textures, 0xABCF, bound) == 5); // the model's other pass
    EXPECT(UvMatrix(textures, 0xABCE, bound) == -1); // another shader: its constant 5 means something else
    EXPECT(UvMatrix(textures, 0xABCD, plain) == -1); // the card not drawn as a glyph (Xbox 360 style)
    EXPECT(UvMatrix(textures, 0xABCD, {}) == -1);
    EXPECT(UvMatrix({}, 0xABCD, bound) == -1);

    printf("glyph models test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
