#include "achievements.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <image.h>

namespace achievements
{
    namespace
    {
        constexpr uint32_t XexMagic = 0x58455832;      // "XEX2"
        constexpr uint32_t XdbfMagic = 0x58444246;     // "XDBF"
        constexpr uint32_t XachMagic = 0x58414348;     // "XACH"
        constexpr uint32_t XstrMagic = 0x58535452;     // "XSTR"
        constexpr uint32_t XexHeaderResourceInfo = 0x000002FF;
        constexpr uint32_t XexHeaderExecutionInfo = 0x00040006;

        constexpr uint16_t NamespaceMetadata = 1;
        constexpr uint16_t NamespaceImage = 2;
        constexpr uint16_t NamespaceStrings = 3;

        constexpr size_t XdbfHeaderSize = 0x18;
        constexpr size_t XdbfEntrySize = 18;   // namespace u16, id u64, offset u32, length u32
        constexpr size_t XdbfFreeSize = 8;
        constexpr size_t XachHeaderSize = 14;  // magic, version, size, count u16
        constexpr size_t XachEntrySize = 36;
        constexpr size_t XstrHeaderSize = 14;  // magic, version, size, count u16
        constexpr uint32_t ShownWhileLocked = 0x8;

        uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

        struct Block
        {
            const uint8_t* data = nullptr;
            size_t size = 0;
        };

        // The XDBF block of a namespace and id, inside the bounds of the buffer.
        class Xdbf
        {
        public:
            Xdbf(const uint8_t* data, size_t size) : m_data(data), m_size(size)
            {
                if (size < XdbfHeaderSize || Be32(data) != XdbfMagic)
                    return;
                uint64_t slots = Be32(data + 8), free = Be32(data + 16);
                m_entries = std::min<uint64_t>(Be32(data + 12), slots);
                m_content = XdbfHeaderSize + slots * XdbfEntrySize + free * XdbfFreeSize;
                m_valid = m_content <= size;
            }

            bool Valid() const { return m_valid; }

            Block Find(uint16_t space, uint64_t id) const
            {
                for (uint64_t i = 0; m_valid && i < m_entries; i++)
                {
                    const uint8_t* entry = m_data + XdbfHeaderSize + i * XdbfEntrySize;
                    if (Be16(entry) != space || Be64(entry + 2) != id)
                        continue;
                    uint64_t offset = m_content + Be32(entry + 10), length = Be32(entry + 14);
                    if (offset > m_size || length > m_size - offset)
                        return {};
                    return { m_data + offset, size_t(length) };
                }
                return {};
            }

        private:
            const uint8_t* m_data;
            size_t m_size;
            uint64_t m_entries = 0;
            uint64_t m_content = 0;
            bool m_valid = false;
        };

        // One language's strings: the id, then the UTF-8 text of each.
        bool FindString(const Block& table, uint16_t id, std::string& text)
        {
            if (table.data == nullptr || table.size < XstrHeaderSize || Be32(table.data) != XstrMagic)
                return false;
            uint16_t count = Be16(table.data + 12);
            size_t position = XstrHeaderSize;
            for (uint16_t i = 0; i < count && position + 4 <= table.size; i++)
            {
                uint16_t entryId = Be16(table.data + position), length = Be16(table.data + position + 2);
                position += 4;
                if (length > table.size - position)
                    return false;
                if (entryId == id)
                {
                    text.assign(reinterpret_cast<const char*>(table.data + position), length);
                    return true;
                }
                position += length;
            }
            return false;
        }

        // The text in the language, else in English; "" when neither has it.
        std::string Text(const Xdbf& xdbf, int language, uint16_t id)
        {
            std::string text;
            if (FindString(xdbf.Find(NamespaceStrings, uint64_t(language)), id, text) && !text.empty())
                return text;
            if (language != 1 && FindString(xdbf.Find(NamespaceStrings, 1), id, text))
                return text;
            return {};
        }

        // An XEX2 optional header: its value (inline data, or the file offset of its data).
        bool FindXexHeader(const std::vector<uint8_t>& xex, uint32_t key, uint32_t& value)
        {
            if (xex.size() < 0x18 || Be32(xex.data()) != XexMagic)
                return false;
            uint32_t count = Be32(&xex[0x14]);
            if (count > (xex.size() - 0x18) / 8)
                return false;
            for (uint32_t i = 0; i < count; i++)
            {
                if (Be32(&xex[0x18 + i * 8]) == key)
                {
                    value = Be32(&xex[0x18 + i * 8 + 4]);
                    return true;
                }
            }
            return false;
        }
    }

    uint32_t List::TotalGamerscore() const
    {
        uint32_t total = 0;
        for (const Info& info : items)
            total += info.gamerscore;
        return total;
    }

    int XdbfLanguage(std::string_view code)
    {
        if (code == "fr") return 4;
        if (code == "de") return 3;
        if (code == "es" || code == "es-419") return 5;
        if (code == "it") return 6;
        if (code == "pt") return 9;
        return 1;
    }

    std::vector<uint8_t> ReadTitleXdbf(const std::vector<uint8_t>& xex)
    {
        uint32_t execution = 0, table = 0;
        if (!FindXexHeader(xex, XexHeaderExecutionInfo, execution) || !FindXexHeader(xex, XexHeaderResourceInfo, table))
            return {};
        if (execution > xex.size() || xex.size() - execution < 0x18 || table > xex.size() - 4)
            return {};

        // Resource table: its size, then { name[8], address, size } entries. The title's XDBF is named after its title ID.
        char name[9];
        snprintf(name, sizeof(name), "%08X", Be32(&xex[execution + 12]));
        uint32_t tableSize = uint32_t(std::min<uint64_t>(Be32(&xex[table]), xex.size() - table));
        for (uint32_t position = 4; position + 16 <= tableSize; position += 16)
        {
            const uint8_t* resource = &xex[table + position];
            if (memcmp(resource, name, 8) != 0)
                continue;
            uint32_t address = Be32(resource + 8), size = Be32(resource + 12);
            Image image = Image::ParseImage(xex.data(), xex.size());
            if (image.data == nullptr || address < image.base || address - image.base > image.size ||
                size > image.size - (address - image.base))
                return {};
            const uint8_t* start = image.data.get() + (address - image.base);
            return std::vector<uint8_t>(start, start + size);
        }
        return {};
    }

    List Parse(const uint8_t* data, size_t size, int language, bool withImages)
    {
        List list;
        Xdbf xdbf(data, size);
        Block table = xdbf.Find(NamespaceMetadata, XachMagic);
        if (!xdbf.Valid() || table.data == nullptr || table.size < XachHeaderSize || Be32(table.data) != XachMagic)
            return list;
        uint32_t count = Be16(table.data + 12);
        count = uint32_t(std::min<size_t>(count, (table.size - XachHeaderSize) / XachEntrySize));
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* entry = table.data + XachHeaderSize + size_t(i) * XachEntrySize;
            Info info;
            info.id = Be16(entry);
            info.name = Text(xdbf, language, Be16(entry + 2));
            info.unlockedDescription = Text(xdbf, language, Be16(entry + 4));
            info.lockedDescription = Text(xdbf, language, Be16(entry + 6));
            info.imageId = Be32(entry + 8);
            info.gamerscore = Be16(entry + 12);
            info.flags = Be32(entry + 16);
            info.hidden = (info.flags & ShownWhileLocked) == 0;
            if (withImages)
            {
                Block image = xdbf.Find(NamespaceImage, info.imageId);
                if (image.data != nullptr)
                    info.imagePng.assign(image.data, image.data + image.size);
            }
            list.items.push_back(std::move(info));
        }
        return list;
    }

    List Read(const std::vector<uint8_t>& xex, int language, bool withImages)
    {
        std::vector<uint8_t> xdbf = ReadTitleXdbf(xex);
        List list = Parse(xdbf.data(), xdbf.size(), language, withImages);
        if (!xex.empty() && xex.size() >= 0x18)
        {
            uint32_t execution = 0;
            if (FindXexHeader(xex, XexHeaderExecutionInfo, execution) && execution <= xex.size() && xex.size() - execution >= 0x18)
                list.titleId = Be32(&xex[execution + 12]);
        }
        return list;
    }

    namespace
    {
        constexpr const char* CacheHeader = "arkeyan-achievements 1";
        constexpr const char* CacheLanguages[] = { "en", "fr", "de", "es", "it", "pt" };

        std::string OneLine(std::string text)
        {
            for (char& c : text)
                if (c == '\t' || c == '\n' || c == '\r')
                    c = ' ';
            return text;
        }

        std::vector<std::string> Split(const std::string& line)
        {
            std::vector<std::string> fields;
            size_t start = 0;
            for (;;)
            {
                size_t tab = line.find('\t', start);
                fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos)
                    return fields;
                start = tab + 1;
            }
        }
    }

    std::filesystem::path CachedImage(const std::filesystem::path& folder, uint32_t imageId)
    {
        return folder / (std::to_string(imageId) + ".png");
    }

    bool CacheIsCurrent(const std::filesystem::path& folder, std::string_view key)
    {
        std::ifstream in(folder / "list.tsv", std::ios::binary);
        std::string header, line;
        return std::getline(in, header) && header == CacheHeader && std::getline(in, line) && line == "key\t" + std::string(key);
    }

    bool WriteCache(const std::filesystem::path& folder, std::string_view key, const std::vector<uint8_t>& xex)
    {
        std::vector<uint8_t> xdbf = ReadTitleXdbf(xex);
        List english = Parse(xdbf.data(), xdbf.size(), 1, true);
        if (english.items.empty())
            return false;
        std::error_code error;
        std::filesystem::create_directories(folder, error);

        std::ostringstream list;
        list << CacheHeader << "\nkey\t" << key << "\n";
        for (const Info& info : english.items)
            list << "A\t" << info.id << "\t" << info.gamerscore << "\t" << info.imageId << "\t" << (info.hidden ? 1 : 0) << "\n";
        for (const char* code : CacheLanguages)
        {
            List texts = Parse(xdbf.data(), xdbf.size(), XdbfLanguage(code), false);
            for (const Info& info : texts.items)
                list << "T\t" << code << "\t" << info.id << "\t" << OneLine(info.name) << "\t" << OneLine(info.unlockedDescription) << "\t"
                     << OneLine(info.lockedDescription) << "\n";
        }
        for (const Info& info : english.items)
        {
            if (info.imagePng.empty())
                continue;
            std::ofstream out(CachedImage(folder, info.imageId), std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(info.imagePng.data()), std::streamsize(info.imagePng.size()));
            if (!out)
                return false;
        }
        // The list goes last, by a rename: a cache with a list is complete.
        std::filesystem::path temp = folder / "list.tsv.tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << list.str();
            if (!out)
                return false;
        }
        std::filesystem::rename(temp, folder / "list.tsv", error);
        return !error;
    }

    List ReadCache(const std::filesystem::path& folder, std::string_view languageCode)
    {
        List list;
        std::ifstream in(folder / "list.tsv", std::ios::binary);
        std::string line;
        if (!std::getline(in, line) || line != CacheHeader)
            return list;
        std::string code(languageCode);
        // Texts by language: the wanted one over English.
        struct Texts { std::string name, unlocked, locked; };
        std::vector<std::pair<uint32_t, Texts>> english, wanted;
        while (std::getline(in, line))
        {
            std::vector<std::string> f = Split(line);
            if (f[0] == "A" && f.size() >= 5)
            {
                Info info;
                info.id = uint32_t(std::strtoul(f[1].c_str(), nullptr, 10));
                info.gamerscore = uint32_t(std::strtoul(f[2].c_str(), nullptr, 10));
                info.imageId = uint32_t(std::strtoul(f[3].c_str(), nullptr, 10));
                info.hidden = f[4] == "1";
                list.items.push_back(std::move(info));
            }
            else if (f[0] == "T" && f.size() >= 6)
            {
                uint32_t id = uint32_t(std::strtoul(f[2].c_str(), nullptr, 10));
                if (f[1] == "en")
                    english.push_back({ id, { f[3], f[4], f[5] } });
                if (f[1] == code)
                    wanted.push_back({ id, { f[3], f[4], f[5] } });
            }
        }
        for (Info& info : list.items)
        {
            for (const auto* source : { &english, &wanted })
                for (const auto& [id, texts] : *source)
                    if (id == info.id)
                    {
                        if (!texts.name.empty()) info.name = texts.name;
                        if (!texts.unlocked.empty()) info.unlockedDescription = texts.unlocked;
                        if (!texts.locked.empty()) info.lockedDescription = texts.locked;
                    }
        }
        return list;
    }
}
