#pragma once

// [Lux] The UI pictures of Spyro's Adventure's disc: "strm" files (one zlib stream) holding APKF texture packages.
// Layout found by inspecting uigameimage.str, uiimage.str and ui/<lang>/startmenu.str (docs/runtime/host/figures.md,
// docs/runtime/host/game_art.md). Shared with Skylauncher: no runtime dependency.
#include "xenos_texture.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace art::lux
{
    // zlib stream (RFC 1950/1951) -> bytes; false if malformed or if it would unpack to more than `limit` bytes (a
    // crafted stream reaches 1000 times its size).
    constexpr size_t InflateLimit = size_t(512) << 20;
    bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit = InflateLimit);
    // The same without the zlib header: raw deflate (RFC 1951), as Alchemy's archives chunk it (alchemy_archive.cpp).
    bool InflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit = InflateLimit);
    // "strm" file: 'strm', BE32 file size, BE32 1, BE32 1, BE32 record count, then one zlib stream -> its contents.
    bool UnpackStream(const std::vector<uint8_t>& file, std::vector<uint8_t>& out);

    // Calls `found(name, image)` for every 2D picture (DXT1, DXT3, DXT5, A8R8G8B8, 8-bit) of an unpacked stream whose
    // name `wanted(name)` accepts. The name is the texture's own ("toy_spyrojr_hud") or, for start-menu packages, the
    // file name of its source path ("skylanderslogo.dds").
    void DecodeImages(const std::vector<uint8_t>& stream, const std::function<bool(const std::string&)>& wanted,
        const std::function<void(const std::string&, const Image&)>& found);

    // A 2D texture package of a stream: the texture's own name, its layout (the GPU fetch constant it carries) and its
    // bytes as on the disc (tiled, in the console's byte order), pointing into the stream.
    struct Package
    {
        std::string name;
        Texture texture;
        const uint8_t* data = nullptr;
        size_t size = 0;
    };
    // Calls `found` for every 2D texture package of an unpacked stream or of a bare APKF file (textures/*.apk). A level's
    // stream (levels/<name>/mission.str) starts with a table the UI streams do not have and nests records in others,
    // so its records are found by their APKF body rather than walked from the start.
    void ForEachPackage(const std::vector<uint8_t>& stream, const std::function<void(const Package&)>& found);
}
