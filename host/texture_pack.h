#pragma once

// HD texture packs (docs/runtime/gpu/texture_packs.md): dumps the game's textures as DDS files named by a hash of their content, and
// finds the replacement a modder put under the same name. Plain C++ (no game or GPU code), so a test can use it.
//
//   <game folder>/textures-dump/<hash>.dds   written when the dump is on, one file per distinct texture
//   <game folder>/textures/<hash>.dds        read at start-up (subfolders allowed): any size, mip count, BC1-BC5 or RGBA8
//   <game folder>/textures-hd/<hash>.dds     the same, made by Arkeyan-Builder's HD texture step; textures/ wins over it
//
// <hash> is 16 lowercase hex digits: ContentHash() of the texture as the renderer uploads it. The renderer looks files up
// by hash in an index made at start-up (only the headers are read, so nothing blocks the render thread); the texture
// threads read the pixels.
#include "host/dds.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace texpack
{
    // XXH3-64 of the texture's top level as uploaded (untiled, byte-swapped, 16-bit formats expanded to RGBA8) with the
    // seed `format | width << 8 | height << 24` (`format` is the Xenos texture format number).
    uint64_t ContentHash(uint32_t format, uint32_t width, uint32_t height, const void* data, size_t size);

    // "00ab...": 16 lowercase hex digits.
    std::string HashName(uint64_t hash);

    struct Replacement
    {
        dds::Info info;
        std::filesystem::path path;
    };

    // Turns the dump on and/or indexes `generated` (when not empty), then `packs` (when `replacing`): a missing folder is
    // no pack; a hash in both is the file in `packs`. `dump` is created when the first texture is written. Logs one line
    // per folder with the counts, and one line per file it can't use.
    void Configure(const std::filesystem::path& dump, bool dumping, const std::filesystem::path& packs, bool replacing,
        const std::filesystem::path& generated = {});
    // The same for a game's folders: datadir::Game(gameId) / "textures-dump", / "textures" (`replacing`: texture_packs)
    // and / "textures-hd" (`hd`: hd_textures, on its own).
    void ConfigureForGame(std::string_view gameId, bool dumping, bool replacing, bool hd);

    bool Dumping();
    // Anything to do per texture: the dump is on or the index holds a file.
    bool Active();

    // The files the index holds (the debug screen).
    size_t Files();

    // The replacement for a content hash, or null. Valid for the whole run.
    const Replacement* Find(uint64_t hash);

    // True the first time `hash` is seen by Dump(): the renderer builds the file's data only then.
    bool ShouldDump(uint64_t hash);
    // Queues a file for the writer thread: `levels[0]` is the top level's rows, packed.
    void Dump(uint64_t hash, dds::Format format, uint32_t width, uint32_t height, std::vector<uint8_t>&& data);

    // Reads level `level` of a replacement (converted to RGBA8 order and opaque as needed). On a read error, logs once
    // for the file and returns false.
    bool ReadLevel(const Replacement& replacement, uint32_t level, std::vector<uint8_t>& out);

    // Waits for the writer to finish what is queued. Also called at exit.
    void Flush();
}
