#pragma once

// A portable game (docs/getting-started/portable.md): one file that holds the built game and everything it reads, so it plays
// with no disc image and no launcher. Its data is a pack: the files back to back, as they are (no padding, no compression),
// then an index, then a fixed trailer at the very end of the file:
//
//   [anything] [file data ...] [index] ["skypack1" | index offset | index size | "SKYPACK!"]    (u64, little-endian)
//   index: "skypack 1\n", then one "<offset>\t<size>\t<name>\n" per file, offsets from the start of the whole file
//
// so a pack may follow the game's executable (Windows, Linux: the loader maps the image and never reads the bytes after it,
// and the 4 GB limit of a Windows image does not apply to them) or stand alone (macOS: <app>/Contents/Resources/game.skypack,
// the bundle keeps its signed executable as it is). Names use '/' and stay relative.
//
// What a portable holds, by name: game/<disc path, lowercase> (the disc's files), portable.txt (Info), keys.ini (the game's
// [<game>.input] section of launcher.ini), collection/<file> (the player's figures and collection.json), icon.png.
//
// Plain C++ (no SDL), shared with Skylauncher (tools/sync_skylauncher.sh): the launcher writes a pack, the game reads it.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace portable
{
    inline constexpr char PackName[] = "game.skypack"; // macOS: in the bundle's Contents/Resources
    inline constexpr char GamePrefix[] = "game/";
    inline constexpr char CollectionPrefix[] = "collection/";
    inline constexpr char InfoName[] = "portable.txt";
    inline constexpr char KeysName[] = "keys.ini";
    inline constexpr char IconName[] = "icon.png";
    inline constexpr size_t TrailerSize = 32;

    struct Entry
    {
        std::string name;
        uint64_t offset = 0; // from the start of the file
        uint64_t size = 0;
    };

    // The pack at the end of `file`. False (with `error`) when the file has no pack or a damaged one: a trailer that points
    // outside the file, an index that does not parse, or a file that overlaps the index or another one.
    bool ReadIndex(const std::filesystem::path& file, std::vector<Entry>& entries, std::string* error = nullptr);
    // The same on an open file of `fileSize` bytes; `read` reads `size` bytes at `offset` and returns how many it read.
    bool ReadIndex(const std::function<size_t(uint64_t offset, void* buffer, size_t size)>& read, uint64_t fileSize,
        std::vector<Entry>& entries, std::string* error = nullptr);
    const Entry* Find(const std::vector<Entry>& entries, std::string_view name);
    // A small entry's bytes (the info, the keys, a figure); empty when it cannot be read.
    std::vector<uint8_t> ReadEntry(const std::filesystem::path& file, const Entry& entry);

    // What the launcher records at export, read back by the game.
    struct Info
    {
        std::string game;     // catalogue id ("ssa")
        std::string version;  // the game's version when it was exported ("0.9.5"): frozen, a portable never updates
        std::string kit;      // the kit it was built from
        std::string xex;      // SHA-256 of the disc's default.xex
        std::string exported; // when, UTC, "20261101-203000": also names the first-launch import (FirstLaunchMarker)
    };
    std::string FormatInfo(const Info& info);
    Info ParseInfo(std::string_view text);

    // Writes a pack: Open, Add each file, Finish. Opened with `append` it follows what the file already holds (the copied
    // executable). Not thread-safe; one writer per file.
    class Writer
    {
    public:
        Writer() = default;
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        ~Writer();

        bool Open(const std::filesystem::path& file, bool append, std::string& error);
        // `read` fills `buffer` with up to `size` bytes at `position` of the file and returns how many it read; `size` is the
        // file's whole size. `cancel` is looked at between two chunks, `progress` gets the bytes written so far.
        using Reader = std::function<size_t(uint64_t position, void* buffer, size_t size)>;
        bool Add(std::string_view name, uint64_t size, const Reader& read, std::string& error);
        bool AddBytes(std::string_view name, std::span<const uint8_t> data, std::string& error);
        bool AddText(std::string_view name, std::string_view text, std::string& error);
        bool AddFile(std::string_view name, const std::filesystem::path& source, std::string& error);
        bool Finish(std::string& error);
        // Closes the file without an index (a failed or cancelled write), so that it can be deleted (Windows).
        void Close();

        const std::atomic<bool>* cancel = nullptr;
        std::function<void(uint64_t written)> progress;
        uint64_t Written() const { return m_written; }

    private:
        std::FILE* m_file = nullptr;
        uint64_t m_position = 0, m_written = 0;
        std::vector<Entry> m_entries;
    };

    // --- First launch of a portable (the game's side) --------------------------------------------------------------------

    // The marker of a portable whose figures and keys were added: <game folder>/portable-<exported>.done. The import runs once
    // per exported copy.
    std::filesystem::path FirstLaunchMarker(const std::filesystem::path& gameFolder, const Info& info);
    // Adds each collection/<file> of the pack to `collectionFolder` when no file of that name is there (an existing figure is
    // never overwritten). Returns how many were added; `skipped` gets those already there, `failed` those that could not be
    // written. The added figures' entries of the pack's collection.json join the folder's own collection.json.
    int ImportCollection(const std::filesystem::path& pack, const std::vector<Entry>& entries,
        const std::filesystem::path& collectionFolder, int* skipped = nullptr, int* failed = nullptr);
}
