#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The players' profiles a game shows (docs/runtime/host/profiles.md): a name, a picture and an XUID for each of the two
// pads, from SKY_PLAYER_* and SKY_PLAYER2_*. Unset, the game sees the one offline profile it always saw: "Player" with
// XUID E000000000000001 on the first pad, nobody on the second.
namespace profiles
{
    constexpr uint64_t DefaultXuid = 0xE000000000000001ull;
    constexpr uint64_t DefaultXuid2 = 0xE000000000000002ull;
    constexpr size_t MaxNameLength = 15;

    struct Profile
    {
        bool signedIn = false;
        std::string name; // UTF-8, 1 to MaxNameLength bytes (GameName)
        uint64_t xuid = 0;
        std::string picture; // a PNG's path as UTF-8 text, or empty
    };

    // The two profiles through any lookup (getenv, a test's table): the name of a variable to its text or nullptr.
    // Player 2 is signed in when SKY_PLAYER2_NAME is set and not empty. `drawnAccents`: as for GameName.
    std::array<Profile, 2> Parse(
        const std::function<const char*(const char*)>& lookup, std::string_view drawnAccents = {});

    // The profiles of this run, read from the environment on the first call with the accents the game's font draws
    // (game::Adapter::nameAccents). Defined in kernel/xam_user.cpp, which serves them to the game.
    const std::array<Profile, 2>& Current();

    // A name the games' fonts can draw, from UTF-8: Latin letters, digits, spaces and the French accented letters ("œ"
    // becomes "oe"); anything else is a space; spaces are trimmed and never doubled; at most MaxNameLength characters;
    // `fallback` when nothing is left. `drawnAccents` (UTF-8) lists the accented letters the game's font has, the
    // others become plain letters ("ï" "i"); empty: all of them. The result is UTF-8, as the games read it, in at most
    // MaxNameLength bytes (the profile's name field): a name whose accents would not fit loses them all.
    std::string GameName(std::string_view utf8, std::string_view fallback, std::string_view drawnAccents = {});

    // How many of the first bytes of UTF-8 `text` fit in `capacity` bytes without cutting a character.
    size_t FittingBytes(std::string_view text, size_t capacity);

    // 16 hex digits, not all zero; false otherwise.
    bool ParseXuid(std::string_view text, uint64_t& xuid);

    struct Picture
    {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba; // width * height * 4, rows top to bottom
    };

    // A PNG (or any image stb_image reads) as RGBA; false when it is not one.
    bool DecodePicture(const uint8_t* data, size_t size, Picture& picture);

    // The picture as a gamer tile of `width` x `height` pixels, written as rows of `pitch` bytes (the bytes past the
    // tile's width are cleared): the largest centred square of the picture, averaged down (or sampled up), each pixel a
    // big-endian A8R8G8B8 word (the bytes A, R, G, B), the format the games' gamer picture textures use.
    void WriteTile(const Picture& picture, uint32_t width, uint32_t height, uint32_t pitch, uint8_t* out);
}
