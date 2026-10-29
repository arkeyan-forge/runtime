#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// An .ico holding one PNG as it is (Windows Vista and later read PNG entries, at any size): SSABuild embeds the game's
// title icon in the Windows executable with it (docs/runtime/host/game_art.md, "The app icon"). Empty when `png` is not a
// PNG with a square size of 1 to 256 pixels (the IHDR is read, nothing is decoded).
inline std::vector<uint8_t> MakeIco(const std::vector<uint8_t>& png)
{
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    auto be32 = [&](size_t at) {
        return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) | (uint32_t(png[at + 2]) << 8) | png[at + 3];
    };
    if (png.size() < 33 || !std::equal(signature, signature + 8, png.begin()) || png[12] != 'I' || png[13] != 'H' ||
        png[14] != 'D' || png[15] != 'R')
        return {};
    uint32_t width = be32(16), height = be32(20);
    if (width == 0 || width > 256 || width != height)
        return {};
    std::vector<uint8_t> ico = { 0, 0, 1, 0, 1, 0 }; // reserved, type 1 (icon), one image
    auto le32 = [&](uint32_t value) {
        for (int i = 0; i < 4; i++)
            ico.push_back(uint8_t(value >> (8 * i)));
    };
    auto le16 = [&](uint16_t value) {
        ico.push_back(uint8_t(value));
        ico.push_back(uint8_t(value >> 8));
    };
    ico.push_back(uint8_t(width & 255)); // 256 is stored as 0
    ico.push_back(uint8_t(height & 255));
    ico.push_back(0); // no palette
    ico.push_back(0);
    le16(1);  // planes
    le16(32); // bits per pixel
    le32(uint32_t(png.size()));
    le32(22); // offset: 6 + 16
    ico.insert(ico.end(), png.begin(), png.end());
    return ico;
}
