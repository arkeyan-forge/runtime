// The models that show a button over a glyph texture (host/glyphs.h, Model): a draw is skipped only when its index count
// and the hash of its indices are a listed model's letters, and only while a style other than the Xbox 360's draws the
// glyph; the texture matrix is set to the identity only for the model's shader with the glyph bound; a keycap's label
// (KeycapLabel). No SDL, no GPU, no game data: ./SSAGlyphModelsTest
#include "host/glyphs.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <cstdio>
#include <cstring>
#include <vector>

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

    // World prompts: a face and letters. A face is told by its indices; two prompts that share a face (A and Y) are told by
    // the letters drawn right after it (PairLetters); nothing pairs in the 360's style.
    const uint16_t face[6] = { 0x0000, 0x0100, 0x0200, 0x0200, 0x0100, 0x0300 };
    const uint16_t lettersB[9] = { 0x0000, 0x0100, 0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800 };
    const uint64_t faceHash = XXH3_64bits(face, sizeof(face));
    const Prompt prompts[] = {
        { 6, faceHash, 9, XXH3_64bits(letters, sizeof(letters)), 0x7001, 256, 128, 0, 1, false, false, 1 },
        { 6, faceHash, 9, XXH3_64bits(lettersB, sizeof(lettersB)), 0x7002, 128, 128, 0, 1, false, false, 1 },
        { 7, 0x1234, 3, 0x5678, 0x7003, 128, 128, 0, 1, false, false, 1 },
    };
    const uint64_t lettersHash = XXH3_64bits(letters, sizeof(letters)),
                   lettersBHash = XXH3_64bits(lettersB, sizeof(lettersB));
    uint64_t hash = 0;
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 6, face, hash) == PromptPart::Face && hash == faceHash);
    EXPECT(PromptPartOf(prompts, Style::Keyboard, 6, face, hash) == PromptPart::Face);
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 9, letters, hash) == PromptPart::Letters && hash == lettersHash);
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 9, lettersB, hash) == PromptPart::Letters && hash == lettersBHash);
    EXPECT(PromptPartOf(prompts, Style::Xbox360, 6, face, hash) == PromptPart::None); // the game's own draws
    EXPECT(PromptPartOf(prompts, Style::Xbox360, 9, letters, hash) == PromptPart::None);
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 6, other, hash) ==
           PromptPart::None); // a face's count, not its indices
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 9, other, hash) == PromptPart::None);
    const uint16_t face7[7] = { 0, 1, 2, 3, 4, 5, 6 };
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 7, face7, hash) == PromptPart::None);
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 4, face, hash) == PromptPart::None); // no mesh of 4 indices
    EXPECT(PromptPartOf({}, Style::PlayStation, 6, face, hash) == PromptPart::None);
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 6, nullptr, hash) == PromptPart::None);
    EXPECT(PromptPartOf(prompts, Style::PlayStation, 0, face, hash) == PromptPart::None);
    // The pair, whichever part came first: the face's hash and the letters' hash name the prompt, so A and Y are told apart.
    EXPECT(PromptOf(prompts, 6, faceHash, 9, lettersHash) == &prompts[0]);
    EXPECT(PromptOf(prompts, 6, faceHash, 9, lettersBHash) == &prompts[1]); // the same face, the other button's letters
    EXPECT(PromptOf(prompts, 6, faceHash, 9, 1) == nullptr); // letters of no prompt
    EXPECT(PromptOf(prompts, 6, faceHash + 1, 9, lettersHash) == nullptr); // another face
    EXPECT(PromptOf(prompts, 7, faceHash, 9, lettersHash) == nullptr); // another count
    EXPECT(PromptOf(prompts, 6, faceHash, 3, lettersHash) == nullptr);
    EXPECT(PromptOf({}, 6, faceHash, 9, lettersHash) == nullptr);

    // The planar coordinates: a 2000 x 1000 face in x and y, four vertices of 16 bytes (position 3 x SNORM16 at 0, FLOAT16_2 at
    // 8, halves swapped: v then u); vertex 4 is not part of the draw. Half 1.0 is 0x3C00, 0.5 is 0x3800.
    auto vertex = [](int16_t x, int16_t y, int16_t z) {
        std::vector<uint8_t> bytes(16, 0xEE);
        memcpy(&bytes[0], &x, 2);
        memcpy(&bytes[2], &y, 2);
        memcpy(&bytes[4], &z, 2);
        return bytes;
    };
    auto build = [&] {
        std::vector<uint8_t> vertices;
        for (auto v : { vertex(-1000, -500, 7), vertex(1000, -500, 7), vertex(-1000, 500, 7), vertex(1000, 500, 7),
                 vertex(0, 0, 0) })
            vertices.insert(vertices.end(), v.begin(), v.end());
        return vertices;
    };
    auto uvOf = [](const std::vector<uint8_t>& vertices, int index) {
        uint16_t halves[2];
        memcpy(halves, &vertices[index * 16 + 8], 4);
        return std::pair<int, int>{ halves[1], halves[0] }; // u, v
    };
    const uint16_t quad[6] = { 0, 1, 2, 2, 1, 3 };
    std::vector<uint8_t> vertices = build();
    Prompt flat = prompts[0];
    EXPECT(PlanarUv(flat, vertices, 16, 0, 8, quad, 0));
    EXPECT(uvOf(vertices, 0) == std::pair(0x0000, 0x0000));
    EXPECT(uvOf(vertices, 1) == std::pair(0x3C00, 0x0000));
    EXPECT(uvOf(vertices, 2) == std::pair(0x0000, 0x3C00));
    EXPECT(uvOf(vertices, 3) == std::pair(0x3C00, 0x3C00));
    EXPECT(uvOf(vertices, 4) == std::pair(0xEEEE, 0xEEEE)); // untouched
    vertices = build();
    flat.flipU = true; // left to right reversed
    EXPECT(PlanarUv(flat, vertices, 16, 0, 8, quad, 0));
    EXPECT(uvOf(vertices, 0) == std::pair(0x3C00, 0x0000));
    EXPECT(uvOf(vertices, 3) == std::pair(0x0000, 0x3C00));
    vertices = build();
    flat.flipU = false;
    flat.uAxis = 1, flat.vAxis = 0; // y runs left to right, x top to bottom
    EXPECT(PlanarUv(flat, vertices, 16, 0, 8, quad, 0));
    EXPECT(uvOf(vertices, 1) == std::pair(0x0000, 0x3C00));
    EXPECT(uvOf(vertices, 2) == std::pair(0x3C00, 0x0000));
    // A base vertex moves the indices; the thin axis has no area; a stream too short for an index is skipped.
    vertices = build();
    const uint16_t shifted[3] = { 0, 1, 2 };
    EXPECT(PlanarUv(prompts[0], vertices, 16, 0, 8, shifted, 1)); // vertices 1, 2, 3
    EXPECT(uvOf(vertices, 1) == std::pair(0x3C00, 0x0000));
    EXPECT(uvOf(vertices, 2) == std::pair(0x0000, 0x3C00));
    EXPECT(uvOf(vertices, 3) == std::pair(0x3C00, 0x3C00));
    EXPECT(uvOf(vertices, 0) == std::pair(0xEEEE, 0xEEEE)); // not named: untouched
    // The game's shader stretches u by 4 about the middle: written divided, the ends at 3/8 and 5/8 (half 0.375 is 0x3600, 0.625 0x3900).
    vertices = build();
    Prompt stretched = prompts[0];
    stretched.uScale = 4;
    EXPECT(PlanarUv(stretched, vertices, 16, 0, 8, quad, 0));
    EXPECT(uvOf(vertices, 0) == std::pair(0x3600, 0x0000));
    EXPECT(uvOf(vertices, 3) == std::pair(0x3900, 0x3C00));
    Prompt thin = prompts[0];
    thin.vAxis = 2;
    vertices = build();
    EXPECT(!PlanarUv(thin, vertices, 16, 0, 8, quad, 0)); // z is 7 everywhere: no area
    const uint16_t beyond[3] = { 0, 3, 200 };
    vertices = build();
    EXPECT(PlanarUv(prompts[0], vertices, 16, 0, 8, beyond, 0)); // vertex 200 does not exist: ignored
    EXPECT(!PlanarUv(prompts[0], vertices, 16, 0, 14, quad, 0)); // coordinates do not fit in the stride

    // Keycaps: a French number row shows its digit, not the symbol it types without Shift (the 3 key: '"').
    EXPECT(KeycapLabel("\"", "3", "3") == "3");
    EXPECT(KeycapLabel("&", "1", "1") == "1");
    EXPECT(KeycapLabel("\xC3\xA9", "2", "2") == "2"); // é
    EXPECT(KeycapLabel("3", "#", "3") == "3"); // US: the digit already
    EXPECT(KeycapLabel("O", "O", "O") == "O");
    EXPECT(KeycapLabel("Backspace", "Backspace", "Backspace") == "Bksp");
    EXPECT(KeycapLabel("Keypad 5", "Keypad 5", "Keypad 5") == "Num5");
    EXPECT(KeycapLabel("", "", "Tab") == "Tab"); // no name on the layout: the setting's
    EXPECT(KeycapLabel(nullptr, nullptr, "Tab") == "Tab");

    printf("glyph models test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
