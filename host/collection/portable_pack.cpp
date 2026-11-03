#include "portable_pack.h"
#include "datadir.h"
#include "json.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace portable
{
    namespace
    {
        constexpr char HeadMagic[8] = { 's', 'k', 'y', 'p', 'a', 'c', 'k', '1' };
        constexpr char TailMagic[8] = { 'S', 'K', 'Y', 'P', 'A', 'C', 'K', '!' };
        constexpr char IndexHeader[] = "skypack 1\n";
        constexpr size_t ChunkSize = 4u << 20;
        constexpr uint64_t MaxIndex = 64ull << 20;

        uint64_t GetU64(const uint8_t* bytes)
        {
            uint64_t value = 0;
            for (int i = 7; i >= 0; i--)
                value = value << 8 | bytes[i];
            return value;
        }

        void PutU64(uint8_t* bytes, uint64_t value)
        {
            for (int i = 0; i < 8; i++)
                bytes[i] = uint8_t(value >> (8 * i));
        }

        // A name stays inside the pack's tree: relative, '/' separators, no empty, "." or ".." part, no tab or newline.
        bool SafeName(std::string_view name)
        {
            if (name.empty() || name.front() == '/' || name.find_first_of("\\:\t\r\n") != std::string_view::npos)
                return false;
            for (size_t begin = 0; begin <= name.size();)
            {
                size_t end = std::min(name.find('/', begin), name.size());
                std::string_view part = name.substr(begin, end - begin);
                if (part.empty() || part == "." || part == "..")
                    return false;
                begin = end + 1;
            }
            return true;
        }

        bool ParseNumber(std::string_view text, uint64_t& value)
        {
            if (text.empty() || text.size() > 20)
                return false;
            value = 0;
            for (char c : text)
            {
                if (c < '0' || c > '9' || value > (UINT64_MAX - uint64_t(c - '0')) / 10)
                    return false; // not a digit, or past 64 bits: a damaged index
                value = value * 10 + uint64_t(c - '0');
            }
            return true;
        }

        bool Fail(std::string* error, std::string text)
        {
            if (error != nullptr)
                *error = std::move(text);
            return false;
        }

        std::FILE* OpenFile(const fs::path& path, const char* mode)
        {
#if defined(_WIN32)
            std::wstring wide(mode, mode + std::strlen(mode));
            return _wfopen(path.c_str(), wide.c_str());
#else
            return std::fopen(path.c_str(), mode);
#endif
        }

        bool Seek(std::FILE* file, uint64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(file, int64_t(offset), SEEK_SET) == 0;
#else
            return fseeko(file, off_t(offset), SEEK_SET) == 0;
#endif
        }
    }

    bool ReadIndex(const std::function<size_t(uint64_t, void*, size_t)>& read, uint64_t fileSize,
        std::vector<Entry>& entries, std::string* error)
    {
        entries.clear();
        uint8_t trailer[TrailerSize];
        if (fileSize < TrailerSize || read(fileSize - TrailerSize, trailer, TrailerSize) != TrailerSize)
            return Fail(error, "no pack: the file is too short");
        if (std::memcmp(trailer, HeadMagic, 8) != 0 || std::memcmp(trailer + 24, TailMagic, 8) != 0)
            return Fail(error, "no pack: no trailer at the end of the file");
        const uint64_t indexOffset = GetU64(trailer + 8), indexSize = GetU64(trailer + 16);
        if (indexSize > MaxIndex || indexOffset > fileSize - TrailerSize ||
            indexSize > fileSize - TrailerSize - indexOffset)
            return Fail(error, "damaged pack: the index is outside the file");
        std::string index(size_t(indexSize), '\0');
        if (read(indexOffset, index.data(), index.size()) != index.size())
            return Fail(error, "damaged pack: the index cannot be read");
        if (!index.starts_with(IndexHeader))
            return Fail(error, "damaged pack: unknown index version");

        std::map<std::string, size_t, std::less<>> seen;
        std::vector<std::pair<uint64_t, uint64_t>> ranges;
        for (size_t begin = std::strlen(IndexHeader); begin < index.size();)
        {
            size_t end = index.find('\n', begin);
            if (end == std::string::npos)
                return Fail(error, "damaged pack: the index's last line is cut");
            std::string_view line(index.data() + begin, end - begin);
            begin = end + 1;
            size_t tab1 = line.find('\t'), tab2 = tab1 == std::string_view::npos ? tab1 : line.find('\t', tab1 + 1);
            Entry entry;
            if (tab2 == std::string_view::npos || !ParseNumber(line.substr(0, tab1), entry.offset) ||
                !ParseNumber(line.substr(tab1 + 1, tab2 - tab1 - 1), entry.size))
                return Fail(error, "damaged pack: an index line does not parse");
            entry.name = std::string(line.substr(tab2 + 1));
            if (!SafeName(entry.name))
                return Fail(error, "damaged pack: a name leaves the pack (" + entry.name + ")");
            if (entry.offset > indexOffset || entry.size > indexOffset - entry.offset)
                return Fail(error, "damaged pack: " + entry.name + " is outside the data");
            if (!seen.emplace(entry.name, entries.size()).second)
                return Fail(error, "damaged pack: " + entry.name + " is there twice");
            if (entry.size != 0)
                ranges.emplace_back(entry.offset, entry.size);
            entries.push_back(std::move(entry));
        }
        std::sort(ranges.begin(), ranges.end());
        for (size_t i = 1; i < ranges.size(); i++)
            if (ranges[i - 1].first + ranges[i - 1].second > ranges[i].first)
                return Fail(error, "damaged pack: two files overlap");
        return true;
    }

    bool ReadIndex(const fs::path& file, std::vector<Entry>& entries, std::string* error)
    {
        entries.clear();
        std::error_code code;
        const uint64_t size = fs::file_size(file, code);
        if (code)
            return Fail(error, "no pack: " + code.message());
        std::FILE* in = OpenFile(file, "rb");
        if (in == nullptr)
            return Fail(error, "no pack: the file cannot be opened");
        auto read = [&](uint64_t offset, void* buffer, size_t count) -> size_t {
            return Seek(in, offset) ? std::fread(buffer, 1, count, in) : 0;
        };
        bool ok = ReadIndex(read, size, entries, error);
        std::fclose(in);
        return ok;
    }

    const Entry* Find(const std::vector<Entry>& entries, std::string_view name)
    {
        for (const Entry& entry : entries)
            if (entry.name == name)
                return &entry;
        return nullptr;
    }

    std::vector<uint8_t> ReadEntry(const fs::path& file, const Entry& entry)
    {
        std::vector<uint8_t> data;
        if (entry.size > (256ull << 20))
            return data;
        std::FILE* in = OpenFile(file, "rb");
        if (in == nullptr)
            return data;
        data.resize(size_t(entry.size));
        if (!Seek(in, entry.offset) || std::fread(data.data(), 1, data.size(), in) != data.size())
            data.clear();
        std::fclose(in);
        return data;
    }

    std::string FormatInfo(const Info& info)
    {
        return "skyportable 1\ngame=" + info.game + "\nversion=" + info.version + "\nkit=" + info.kit +
               "\nxex=" + info.xex + "\nexported=" + info.exported + "\n";
    }

    Info ParseInfo(std::string_view text)
    {
        Info info;
        for (size_t begin = 0; begin < text.size();)
        {
            size_t end = std::min(text.find('\n', begin), text.size());
            std::string_view line = text.substr(begin, end - begin);
            begin = end + 1;
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            size_t equals = line.find('=');
            if (equals == std::string_view::npos)
                continue;
            std::string_view key = line.substr(0, equals);
            std::string value(line.substr(equals + 1));
            if (key == "game")
                info.game = value;
            else if (key == "version")
                info.version = value;
            else if (key == "kit")
                info.kit = value;
            else if (key == "xex")
                info.xex = value;
            else if (key == "exported")
                info.exported = value;
        }
        return info;
    }

    Writer::~Writer() { Close(); }

    void Writer::Close()
    {
        if (m_file != nullptr)
            std::fclose(m_file);
        m_file = nullptr;
    }

    bool Writer::Open(const fs::path& file, bool append, std::string& error)
    {
        m_file = OpenFile(file, append ? "ab" : "wb");
        if (m_file == nullptr)
        {
            error = "cannot write " + datadir::Text(file);
            return false;
        }
        std::error_code code;
        m_position = append ? fs::file_size(file, code) : 0;
        if (code)
        {
            error = "cannot read the size of " + datadir::Text(file);
            return false;
        }
        m_written = 0;
        m_entries.clear();
        return true;
    }

    bool Writer::Add(std::string_view name, uint64_t size, const Reader& read, std::string& error)
    {
        if (m_file == nullptr)
        {
            error = "the pack is not open";
            return false;
        }
        if (!SafeName(name) || Find(m_entries, name) != nullptr)
        {
            error = "a name the pack cannot hold: " + std::string(name);
            return false;
        }
        std::vector<uint8_t> buffer(size_t(std::min<uint64_t>(size, ChunkSize)));
        for (uint64_t done = 0; done < size;)
        {
            if (cancel != nullptr && cancel->load())
            {
                error = "cancelled";
                return false;
            }
            const size_t want = size_t(std::min<uint64_t>(size - done, buffer.size()));
            const size_t got = read(done, buffer.data(), want);
            if (got != want)
            {
                error = "cannot read " + std::string(name);
                return false;
            }
            if (std::fwrite(buffer.data(), 1, got, m_file) != got)
            {
                error = "cannot write the pack (is the disk full?)";
                return false;
            }
            done += got;
            m_written += got;
            if (progress)
                progress(m_written);
        }
        m_entries.push_back({ std::string(name), m_position, size });
        m_position += size;
        return true;
    }

    bool Writer::AddBytes(std::string_view name, std::span<const uint8_t> data, std::string& error)
    {
        return Add(
            name, data.size(),
            [&](uint64_t position, void* buffer, size_t size) {
                std::memcpy(buffer, data.data() + position, size);
                return size;
            },
            error);
    }

    bool Writer::AddText(std::string_view name, std::string_view text, std::string& error)
    {
        return AddBytes(name, { reinterpret_cast<const uint8_t*>(text.data()), text.size() }, error);
    }

    bool Writer::AddFile(std::string_view name, const fs::path& source, std::string& error)
    {
        std::error_code code;
        const uint64_t size = fs::file_size(source, code);
        std::FILE* in = code ? nullptr : OpenFile(source, "rb");
        if (in == nullptr)
        {
            error = "cannot read " + datadir::Text(source);
            return false;
        }
        bool ok = Add(
            name, size, [&](uint64_t, void* buffer, size_t count) { return std::fread(buffer, 1, count, in); }, error);
        std::fclose(in);
        return ok;
    }

    bool Writer::Finish(std::string& error)
    {
        if (m_file == nullptr)
        {
            error = "the pack is not open";
            return false;
        }
        std::string index = IndexHeader;
        for (const Entry& entry : m_entries)
            index += std::to_string(entry.offset) + '\t' + std::to_string(entry.size) + '\t' + entry.name + '\n';
        uint8_t trailer[TrailerSize];
        std::memcpy(trailer, HeadMagic, 8);
        PutU64(trailer + 8, m_position);
        PutU64(trailer + 16, index.size());
        std::memcpy(trailer + 24, TailMagic, 8);
        bool ok = std::fwrite(index.data(), 1, index.size(), m_file) == index.size() &&
                  std::fwrite(trailer, 1, TrailerSize, m_file) == TrailerSize;
        ok = std::fclose(m_file) == 0 && ok;
        m_file = nullptr;
        if (!ok)
            error = "cannot write the pack's index (is the disk full?)";
        return ok;
    }

    fs::path FirstLaunchMarker(const fs::path& gameFolder, const Info& info)
    {
        std::string stamp;
        for (char c : info.exported)
            if ((c >= '0' && c <= '9') || c == '-')
                stamp += c;
        return gameFolder / ("portable-" + (stamp.empty() ? std::string("0") : stamp) + ".done");
    }

    int ImportCollection(const fs::path& pack, const std::vector<Entry>& entries, const fs::path& collectionFolder,
        int* skipped, int* failed)
    {
        int added = 0, there = 0, lost = 0;
        std::error_code code;
        const std::string indexName = std::string(CollectionPrefix) + "collection.json";
        std::vector<std::string> addedNames;
        for (const Entry& entry : entries)
        {
            if (!entry.name.starts_with(CollectionPrefix) || entry.name == indexName)
                continue;
            const fs::path target =
                collectionFolder / datadir::FromText(entry.name.substr(std::strlen(CollectionPrefix)));
            if (fs::exists(target, code))
            {
                there++;
                continue;
            }
            std::vector<uint8_t> data = ReadEntry(pack, entry);
            if (data.size() != entry.size)
            {
                lost++;
                continue;
            }
            fs::create_directories(target.parent_path(), code);
            fs::path temporary = target;
            temporary += ".part";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
                if (!out)
                {
                    fs::remove(temporary, code);
                    lost++;
                    continue;
                }
            }
            // Not over a file that appeared meanwhile: rename refuses nothing on POSIX, so look again.
            if (fs::exists(target, code))
            {
                fs::remove(temporary, code);
                there++;
                continue;
            }
            fs::rename(temporary, target, code);
            if (code)
            {
                fs::remove(temporary, code);
                lost++;
            }
            else
            {
                added++;
                addedNames.push_back(entry.name.substr(std::strlen(CollectionPrefix)));
            }
        }

        // The added figures' metadata (labels, editions, a swapper's other half) joins this machine's collection.json;
        // its entries for the figures already here stay as they are. A collection.json that doesn't parse is left alone.
        const Entry* packIndex = Find(entries, indexName);
        if (packIndex != nullptr && !addedNames.empty())
        {
            const std::vector<uint8_t> bytes = ReadEntry(pack, *packIndex);
            collection::Json from, local = collection::Json::Object();
            std::string error;
            const fs::path localPath = collectionFolder / "collection.json";
            bool localOk = true;
            if (fs::exists(localPath, code))
            {
                std::ifstream in(localPath, std::ios::binary);
                const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
                localOk = collection::Json::Parse(text, local, error) && local.IsObject();
            }
            if (collection::Json::Parse(std::string(bytes.begin(), bytes.end()), from, error) && localOk)
            {
                collection::Json figures = local["figures"].IsObject() ? local["figures"] : collection::Json::Object();
                for (const std::string& name : addedNames)
                    if (from["figures"][name].IsObject() && !figures.Has(name))
                        figures.Set(name, from["figures"][name]);
                local.Set("figures", std::move(figures));
                if (!local.Has("format"))
                    local.Set("format", 1);
                fs::path temporary = localPath;
                temporary += ".part";
                bool written = false;
                {
                    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                    out << local.Dump();
                    out.flush();
                    written = bool(out);
                }
                // A full disk leaves a short file: the machine's own collection.json stays as it was.
                if (written)
                    fs::rename(temporary, localPath, code);
                if (!written || code)
                    fs::remove(temporary, code);
            }
        }
        if (skipped != nullptr)
            *skipped = there;
        if (failed != nullptr)
            *failed = lost;
        return added;
    }
}
