#pragma once

#include "platform/platform.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Read-only view of the game's files: the disc's XDVDFS file system read straight from the ISO, a folder holding a copy of
// them ("Copy the game data", host/collection/game_data.h), or the pack of a portable game (host/collection/portable_pack.h:
// its game/ files, at the end of the game's own executable or in game.skypack): Open takes any of them.
class DiscFileSystem
{
public:
    struct Entry
    {
        uint64_t offset; // byte offset in the ISO (or the pack)
        uint32_t size;
        bool directory;
        std::string name; // as on the disc (paths are looked up in lowercase)
        std::string host; // folder mode: the file's path on the host (opened for each read: no handle is kept)
    };

    DiscFileSystem() = default;
    DiscFileSystem(const DiscFileSystem&) = delete;
    DiscFileSystem& operator=(const DiscFileSystem&) = delete;
    ~DiscFileSystem();

    // `path`: a disc image, a folder with the disc's files, or a file ending with a pack (each needs a default.xex).
    bool Open(const char* path);
    bool FromFolder() const { return m_folder; }
    bool FromPack() const { return m_pack; }

    // `path` is relative to the disc root, any case, '\' or '/' separators. Empty means the root.
    const Entry* Find(const std::string& path) const;
    uint32_t Read(const Entry& entry, uint64_t position, void* buffer, uint32_t size) const;
    std::vector<uint8_t> ReadAll(const std::string& path) const;

    // The entries of a directory (`path` as for Find), sorted by name without case.
    std::vector<const Entry*> List(const std::string& path) const;

    // Every file (no directory) with its path as Find takes it (lowercase), sorted by path.
    std::vector<std::pair<std::string, const Entry*>> Files() const;

    static std::string Normalize(std::string path);

private:
    PlatformFile m_file = InvalidPlatformFile;
    uint64_t m_partition = 0;
    std::unordered_map<std::string, Entry> m_entries;
    uint64_t m_imageSize = 0; // the image file's length (0: unknown)
    std::unordered_set<uint32_t> m_directories; // the extents indexed so far, while Open walks
    bool m_folder = false;
    bool m_pack = false;

    bool OpenFolder(const char* path);
    bool OpenPack(const char* path);
    // A real disc's directories are a few sectors and a few levels deep.
    static constexpr uint32_t MaxDirectorySize = 16u << 20;
    static constexpr int MaxDirectoryDepth = 64;
    static constexpr size_t MaxEntries = 1u << 18;
    // False: the tree loops, is too deep or too big. Each directory extent is walked once (m_directories).
    bool IndexDirectory(uint32_t sector, uint32_t size, const std::string& prefix, int depth = 0);
};

extern DiscFileSystem g_disc;
