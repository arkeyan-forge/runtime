#include "vfs.h"
#include <algorithm>
#include <cstring>
#include <filesystem>

DiscFileSystem g_disc;

namespace
{
    constexpr uint32_t SectorSize = 2048;
    constexpr char Magic[] = "MICROSOFT*XBOX*MEDIA";
    // Game partition offsets of the known Xbox 360 disc layouts (plain, XGD3, XGD2, XGD1).
    constexpr uint64_t PartitionOffsets[] = { 0x0, 0x2080000, 0xFD90000, 0x18300000 };
    constexpr uint8_t DirectoryAttribute = 0x10;
}

std::string DiscFileSystem::Normalize(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(tolower(c)); });
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}

DiscFileSystem::~DiscFileSystem()
{
    if (m_file != InvalidPlatformFile)
        PlatformCloseFile(m_file);
}

namespace
{
    namespace fs = std::filesystem;

    fs::path FromUtf8(const char* text) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text))); }

    std::string ToUtf8(const fs::path& path)
    {
        std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }
}

bool DiscFileSystem::OpenFolder(const char* path)
{
    fs::path root = FromUtf8(path);
    std::error_code error;
    m_entries[""] = { 0, 0, true, "", "" };
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    {
        std::u8string generic = it->path().lexically_relative(root).generic_u8string();
        std::string relative(generic.begin(), generic.end());
        std::string name = ToUtf8(it->path().filename());
        if (it->is_directory(error))
        {
            m_entries[Normalize(relative)] = { 0, 0, true, name, "" };
            continue;
        }
        // The copy's bookkeeping (game_data.h) is not the game's.
        if (name == "gamedata.manifest" || name.ends_with(".part"))
            continue;
        uint64_t size = it->file_size(error);
        m_entries[Normalize(relative)] = { 0, uint32_t(size), false, name, ToUtf8(it->path()) };
    }
    m_folder = true;
    return !error && Find("default.xex") != nullptr;
}

bool DiscFileSystem::Open(const char* isoPath)
{
    if (m_file != InvalidPlatformFile)
        PlatformCloseFile(m_file);
    m_file = InvalidPlatformFile;
    m_entries.clear();
    m_folder = false;
    std::error_code directoryError;
    if (fs::is_directory(FromUtf8(isoPath), directoryError))
        return OpenFolder(isoPath);
    m_file = PlatformOpenFile(isoPath);
    if (m_file == InvalidPlatformFile)
        return false;

    for (uint64_t offset : PartitionOffsets)
    {
        char magic[20];
        if (PlatformReadFile(m_file, magic, sizeof(magic), offset + 32 * SectorSize) == sizeof(magic) &&
            memcmp(magic, Magic, sizeof(magic)) == 0)
        {
            uint32_t root[2]; // sector, size (little-endian on disc)
            PlatformReadFile(m_file, root, sizeof(root), offset + 32 * SectorSize + 20);
            m_partition = offset;
            m_entries[""] = { offset + uint64_t(root[0]) * SectorSize, root[1], true, "", "" };
            IndexDirectory(root[0], root[1], "");
            return true;
        }
    }
    return false;
}

void DiscFileSystem::IndexDirectory(uint32_t sector, uint32_t size, const std::string& prefix)
{
    std::vector<uint8_t> data(size);
    PlatformReadFile(m_file, data.data(), size, m_partition + uint64_t(sector) * SectorSize);

    // Entries form a binary tree: left/right subtree offsets in dwords, 0xFFFF marks unused space.
    std::vector<uint32_t> pending{ 0 };
    std::vector<bool> visited(size, false);
    while (!pending.empty())
    {
        uint32_t offset = pending.back();
        pending.pop_back();
        if (offset + 14 > size || visited[offset])
            continue;
        visited[offset] = true;

        uint16_t left, right;
        uint32_t entrySector, entrySize;
        memcpy(&left, &data[offset], 2);
        memcpy(&right, &data[offset + 2], 2);
        if (left == 0xFFFF && right == 0xFFFF)
            continue;
        memcpy(&entrySector, &data[offset + 4], 4);
        memcpy(&entrySize, &data[offset + 8], 4);
        uint8_t attributes = data[offset + 12];
        uint8_t nameLength = data[offset + 13];

        std::string name(reinterpret_cast<char*>(&data[offset + 14]), nameLength);
        std::string path = Normalize(prefix + name);
        bool directory = attributes & DirectoryAttribute;
        m_entries[path] = { m_partition + uint64_t(entrySector) * SectorSize, entrySize, directory, name, "" };
        if (directory && entrySize != 0)
            IndexDirectory(entrySector, entrySize, path + "/");

        if (left != 0)
            pending.push_back(left * 4u);
        if (right != 0)
            pending.push_back(right * 4u);
    }
}

const DiscFileSystem::Entry* DiscFileSystem::Find(const std::string& path) const
{
    auto it = m_entries.find(Normalize(path));
    return it != m_entries.end() ? &it->second : nullptr;
}

std::vector<const DiscFileSystem::Entry*> DiscFileSystem::List(const std::string& path) const
{
    std::string prefix = Normalize(path);
    if (!prefix.empty())
        prefix += '/';
    std::vector<std::pair<std::string, const Entry*>> children;
    for (const auto& [key, entry] : m_entries)
    {
        if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0 &&
            key.find('/', prefix.size()) == std::string::npos)
            children.emplace_back(key, &entry);
    }
    std::sort(children.begin(), children.end());
    std::vector<const Entry*> entries;
    for (const auto& child : children)
        entries.push_back(child.second);
    return entries;
}

std::vector<std::pair<std::string, const DiscFileSystem::Entry*>> DiscFileSystem::Files() const
{
    std::vector<std::pair<std::string, const Entry*>> files;
    for (const auto& [path, entry] : m_entries)
        if (!entry.directory)
            files.emplace_back(path, &entry);
    std::sort(files.begin(), files.end());
    return files;
}

uint32_t DiscFileSystem::Read(const Entry& entry, uint64_t position, void* buffer, uint32_t size) const
{
    if (position >= entry.size)
        return 0;

    size = uint32_t(std::min<uint64_t>(size, entry.size - position));
    if (m_folder)
    {
        PlatformFile file = PlatformOpenFile(entry.host.c_str());
        if (file == InvalidPlatformFile)
            return 0;
        int64_t got = PlatformReadFile(file, buffer, size, position);
        PlatformCloseFile(file);
        return got > 0 ? uint32_t(got) : 0;
    }
    int64_t read = PlatformReadFile(m_file, buffer, size, entry.offset + position);
    return read > 0 ? uint32_t(read) : 0;
}

std::vector<uint8_t> DiscFileSystem::ReadAll(const std::string& path) const
{
    const Entry* entry = Find(path);
    if (entry == nullptr || entry->directory)
        return {};

    std::vector<uint8_t> data(entry->size);
    data.resize(Read(*entry, 0, data.data(), entry->size));
    return data;
}
