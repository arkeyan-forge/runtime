// The players' profiles (host/profiles.h): the SKY_PLAYER_* and SKY_PLAYER2_* variables and their defaults, the names
// the games' fonts can draw, and the gamer picture tile from a PNG made here. No SDL, no game data: ./ArkeyanProfilesTest
#include "host/profiles.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stb_image_write.h>
#include <string>

using namespace profiles;

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

    std::array<Profile, 2> ParseTable(const std::map<std::string, std::string>& table)
    {
        return Parse([&](const char* name) -> const char* {
            auto found = table.find(name);
            return found == table.end() ? nullptr : found->second.c_str();
        });
    }

    // An RGBA picture as PNG bytes, through stb_image_write.
    std::vector<uint8_t> Png(int width, int height, const std::vector<uint8_t>& rgba)
    {
        std::vector<uint8_t> png;
        stbi_write_png_to_func(
            [](void* context, void* data, int size) {
                auto* bytes = static_cast<std::vector<uint8_t>*>(context);
                bytes->insert(bytes->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
            },
            &png, width, height, 4, rgba.data(), width * 4);
        return png;
    }

    // A tile pixel's A, R, G, B bytes as one number 0xAARRGGBB.
    uint32_t Pixel(const std::vector<uint8_t>& tile, uint32_t pitch, uint32_t x, uint32_t y)
    {
        const uint8_t* p = &tile[size_t(y) * pitch + size_t(x) * 4];
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }
}

int main()
{
    // Nothing set: today's single profile, Player 2 not signed in.
    std::array<Profile, 2> none = ParseTable({});
    EXPECT(none[0].signedIn && none[0].name == "Player" && none[0].xuid == DefaultXuid && none[0].picture.empty());
    EXPECT(DefaultXuid == 0xE000000000000001ull);
    EXPECT(!none[1].signedIn);

    // Both players.
    std::array<Profile, 2> both = ParseTable({ { "SKY_PLAYER_NAME", "Spyro42" }, { "SKY_PLAYER_PICTURE", "/p/a.png" },
        { "SKY_PLAYER_XUID", "E0000000000000AB" }, { "SKY_PLAYER2_NAME", "Kaos" },
        { "SKY_PLAYER2_XUID", "e0000000000000cd" }, { "SKY_PLAYER2_PICTURE", "/p/b.png" } });
    EXPECT(both[0].name == "Spyro42" && both[0].xuid == 0xE0000000000000ABull && both[0].picture == "/p/a.png");
    EXPECT(both[1].signedIn && both[1].name == "Kaos" && both[1].xuid == 0xE0000000000000CDull &&
           both[1].picture == "/p/b.png");

    // Player 2 needs a name; without an XUID it gets the next offline one; a name of nothing drawable is "Player 2".
    EXPECT(!ParseTable({ { "SKY_PLAYER2_NAME", "" }, { "SKY_PLAYER2_XUID", "E000000000000009" } })[1].signedIn);
    EXPECT(!ParseTable({ { "SKY_PLAYER2_PICTURE", "/p/b.png" } })[1].signedIn);
    std::array<Profile, 2> guest = ParseTable({ { "SKY_PLAYER2_NAME", "\xE2\x98\x85" } });
    EXPECT(guest[1].signedIn && guest[1].name == "Player 2" && guest[1].xuid == DefaultXuid2);

    // A bad XUID keeps the default; an empty name is "Player"; the older SSA_ spelling is read too.
    for (const char* bad :
        { "", "E00000000000001", "E0000000000000011", "E00000000000000G", "0000000000000000", "0xE0000000000001" })
        EXPECT(ParseTable({ { "SKY_PLAYER_XUID", bad } })[0].xuid == DefaultXuid);
    EXPECT(ParseTable({ { "SKY_PLAYER_NAME", "" } })[0].name == "Player");
    EXPECT(ParseTable({ { "SSA_PLAYER_NAME", "Old" } })[0].name == "Old");
    uint64_t xuid = 0;
    EXPECT(ParseXuid("FFFFFFFFFFFFFFFF", xuid) && xuid == ~0ull);

    // Names: Latin letters, digits, spaces and French accents, in UTF-8; anything else a space, never doubled, trimmed;
    // at most 15 characters in at most 15 bytes (the accents dropped when they would not fit).
    EXPECT(GameName("Gill Grunt 2", "Player") == "Gill Grunt 2");
    EXPECT(GameName("\xC3\x89lo\xC3\xAF"
                    "se",
               "Player") == "\xC3\x89lo\xC3\xAF"
                            "se");          // Éloïse
    EXPECT(GameName("\xC3\xA0\xC3\xA2\xC3\xA7\xC3\xA8\xC3\xAA\xC3\xAB\xC3\xAE", "P") ==
           "\xC3\xA0\xC3\xA2\xC3\xA7\xC3\xA8\xC3\xAA\xC3\xAB\xC3\xAE"); // àâçèêëî: 14 bytes
    EXPECT(
        GameName("\xC3\xB4\xC3\xB9\xC3\xBB\xC3\xBC\xC3\xBF\xC3\x86\xC3\xA6\xC3\x87", "P") == "ouuuyAEaeC"); // too long
    EXPECT(GameName("C\xC5\x93ur \xC5\x92uvre", "P") == "Coeur OEuvre"); // Cœur Œuvre
    EXPECT(GameName("Jean-Luc_42!", "P") == "Jean Luc 42");
    EXPECT(GameName("  a   b  ", "P") == "a b");
    EXPECT(GameName("Kaos\xF0\x9F\x94\xA5\xE6\x9D\x8E", "P") == "Kaos"); // an emoji, a CJK letter
    EXPECT(GameName("\xC3\xB1\xC3\x9F\xC3\xB8", "Player") == "Player"); // ñßø: not French
    EXPECT(GameName("ab\xC3", "P") == "ab"); // a cut sequence
    EXPECT(GameName("\xFF\xFE\x80x", "P") == "x"); // not UTF-8
    EXPECT(GameName("ABCDEFGHIJKLMNOPQRST", "P") == "ABCDEFGHIJKLMNO");
    EXPECT(GameName("Am\xC3\xA9lie Poulain", "P") == "Am\xC3\xA9lie Poulain"); // 14 letters, 15 bytes
    EXPECT(GameName("Am\xC3\xA9lie Poulains", "P") == "Amelie Poulains"); // 15 letters, 16 bytes
    EXPECT(GameName("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"
                    "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9",
               "P") == std::string(15, 'e'));
    EXPECT(GameName("", "Player") == "Player");
    for (const char* text : { "\xC3\x89lo\xC3\xAF"
                              "se",
             "\xF0\x9F\x94\xA5 \xC3\xA9t\xC3\xA9 \xC5\x93", "Z\xCC\x81" })
        EXPECT(GameName(text, "Player").size() <= MaxNameLength);

    // A font without some accents: those letters plain, the others kept (SuperChargers' profile font has every capital
    // and only "æéêö" in lower case).
    EXPECT(GameName("\xC3\x89lo\xC3\xAF"
                    "se \xC3\x8F",
               "P", "\xC3\x89\xC3\x8F\xC3\xA9") == "\xC3\x89loise \xC3\x8F");
    EXPECT(GameName("\xC3\xA6\xC3\xA0", "P", "\xC3\xA9") == "aea");
    EXPECT(ParseTable({ { "SKY_PLAYER_NAME", "No\xC3\xABl" } })[0].name == "No\xC3\xABl");
    EXPECT(Parse([](const char* name) { return std::strcmp(name, "SKY_PLAYER_NAME") == 0 ? "No\xC3\xABl" : nullptr; },
               "\xC3\xA9")[0]
               .name == "Noel");

    // A guest buffer shorter than the name: whole characters only.
    EXPECT(
        FittingBytes("\xC3\x89lo", 15) == 4 && FittingBytes("\xC3\x89lo", 1) == 0 && FittingBytes("a\xC3\x89", 2) == 1);
    EXPECT(FittingBytes("a\xC3\x89", 3) == 3 && FittingBytes("abc", 2) == 2 && FittingBytes("", 0) == 0);

    // The picture: four quadrants (red, green, blue, white) of 8 x 8 pixels in a 16 x 16 PNG.
    std::vector<uint8_t> rgba(16 * 16 * 4);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
        {
            uint8_t* p = &rgba[size_t(y * 16 + x) * 4];
            int quadrant = (y >= 8) * 2 + (x >= 8);
            p[0] = quadrant == 0 || quadrant == 3 ? 255 : 0;
            p[1] = quadrant == 1 || quadrant == 3 ? 255 : 0;
            p[2] = quadrant == 2 || quadrant == 3 ? 255 : 0;
            p[3] = 255;
        }
    std::vector<uint8_t> png = Png(16, 16, rgba);
    Picture picture;
    EXPECT(!png.empty() && DecodePicture(png.data(), png.size(), picture) && picture.width == 16 &&
           picture.height == 16 && picture.rgba == rgba);
    Picture broken;
    const uint8_t notPng[] = { 'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g' };
    EXPECT(!DecodePicture(notPng, sizeof(notPng), broken));
    EXPECT(!DecodePicture(png.data(), png.size() / 2, broken));

    // Up to the large gamer tile (64 x 64, pitch 256): bytes A, R, G, B; each quadrant 32 x 32.
    std::vector<uint8_t> tile(256 * 64, 0xCC);
    WriteTile(picture, 64, 64, 256, tile.data());
    EXPECT(tile[0] == 0xFF && tile[1] == 0xFF && tile[2] == 0 && tile[3] == 0);
    EXPECT(Pixel(tile, 256, 0, 0) == 0xFFFF0000u && Pixel(tile, 256, 31, 31) == 0xFFFF0000u);
    EXPECT(Pixel(tile, 256, 32, 0) == 0xFF00FF00u && Pixel(tile, 256, 63, 31) == 0xFF00FF00u);
    EXPECT(Pixel(tile, 256, 0, 32) == 0xFF0000FFu && Pixel(tile, 256, 31, 63) == 0xFF0000FFu);
    EXPECT(Pixel(tile, 256, 32, 32) == 0xFFFFFFFFu && Pixel(tile, 256, 63, 63) == 0xFFFFFFFFu);

    // Down to 4 x 4 in a wider pitch: each pixel the average of a 4 x 4 block; the row's padding cleared.
    std::vector<uint8_t> small(32 * 4, 0xCC);
    WriteTile(picture, 4, 4, 32, small.data());
    EXPECT(Pixel(small, 32, 0, 0) == 0xFFFF0000u && Pixel(small, 32, 3, 3) == 0xFFFFFFFFu);
    EXPECT(small[16] == 0 && small[31] == 0 && small[32 * 3 + 31] == 0);
    // Down to 1 x 1: the average of the four quadrants.
    std::vector<uint8_t> dot(4);
    WriteTile(picture, 1, 1, 4, dot.data());
    EXPECT(Pixel(dot, 4, 0, 0) == 0xFF808080u);

    // A wide picture: the centred square only (the red and blue bars at the sides are cut).
    std::vector<uint8_t> wide(8 * 4 * 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
        {
            uint8_t* p = &wide[size_t(y * 8 + x) * 4];
            p[0] = x < 2 ? 255 : 0;
            p[1] = x >= 2 && x < 6 ? 255 : 0;
            p[2] = x >= 6 ? 255 : 0;
            p[3] = 255;
        }
    Picture widePicture{ 8, 4, wide };
    std::vector<uint8_t> wideTile(8 * 8 * 4);
    WriteTile(widePicture, 8, 8, 32, wideTile.data());
    EXPECT(Pixel(wideTile, 32, 0, 0) == 0xFF00FF00u && Pixel(wideTile, 32, 7, 7) == 0xFF00FF00u);

    // Transparent pixels: their colour does not bleed into the average; a fully transparent block stays clear.
    Picture half{ 2, 2, { 255, 0, 0, 255, 0, 0, 255, 0, 255, 0, 0, 255, 0, 0, 255, 0 } };
    std::vector<uint8_t> one(4), two(8);
    WriteTile(half, 1, 1, 4, one.data());
    EXPECT(Pixel(one, 4, 0, 0) == 0x80FF0000u);
    Picture clear{ 1, 1, { 9, 9, 9, 0 } };
    WriteTile(clear, 2, 1, 8, two.data());
    EXPECT(Pixel(two, 8, 0, 0) == 0 && Pixel(two, 8, 1, 0) == 0);

    // A pitch too narrow for the asked width: the width shrinks to it, nothing is written past the buffer; an empty
    // picture leaves a cleared tile.
    std::vector<uint8_t> narrow(8 * 2 + 4, 0xCC);
    WriteTile(picture, 64, 2, 8, narrow.data());
    EXPECT(Pixel(narrow, 8, 0, 0) == 0xFFFF0000u && narrow[16] == 0xCC && narrow[19] == 0xCC);
    std::vector<uint8_t> empty(16, 0xCC);
    WriteTile(Picture{}, 2, 2, 8, empty.data());
    EXPECT(std::all_of(empty.begin(), empty.end(), [](uint8_t b) { return b == 0; }));

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
