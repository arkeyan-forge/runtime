// See stfs.h. The layout, as read from the dashboard art packages of the discs:
// - the header's size is the big-endian word at 0x340; the data starts at the next 4 KiB boundary, with the first hash
//   table (one block);
// - the volume descriptor at 0x379: +2 the block separation (bit 0 set: one table per level, else two), +3 the file table's
//   block count (little-endian 16 bits), +5 its first block (little-endian 24 bits);
// - data block b is stored after the hash tables that precede it: one table of each level every 170 blocks (level 0) and
//   every 28900 (level 1);
// - a file table entry is 0x40 bytes: the name (0x28 bytes), its length in the low 6 bits of 0x28 (bit 6: the blocks are
//   consecutive; bit 7: a folder), the block count at 0x29 and the first block at 0x2F (little-endian 24 bits), the size at 0x34
//   (big-endian); a level-0 hash entry is 0x18 bytes, the next block of the file in its last 3 (big-endian, 0xFFFFFF: the
//   end).
#include "stfs.h"
#include <algorithm>
#include <cctype>

namespace art
{
    namespace
    {
        constexpr size_t Block = 0x1000, PerTable = 170, PerTable1 = 28900, EntrySize = 0x40, HashSize = 0x18;
        constexpr uint32_t End = 0xFFFFFF;

        uint32_t Big32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        uint32_t Little24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
        uint32_t Big24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }

        bool SameName(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        }
    }

    std::vector<uint8_t> StfsFile(const std::vector<uint8_t>& package, const std::string& name)
    {
        const size_t size = package.size();
        if (size < 0x3A0 || !(std::equal(package.begin(), package.begin() + 4, "CON ") ||
                                std::equal(package.begin(), package.begin() + 4, "LIVE") ||
                                std::equal(package.begin(), package.begin() + 4, "PIRS")))
            return {};
        const uint8_t* p = package.data();
        const size_t first = (size_t(Big32(p + 0x340)) + Block - 1) & ~(Block - 1);
        const uint8_t* volume = p + 0x379;
        const uint32_t shift = (volume[2] & 1) != 0 ? 0 : 1; // the tables per level: 1, or 2 (a shift of one block)
        const uint32_t tableBlocks = uint32_t(volume[3]) | uint32_t(volume[4]) << 8, tableFirst = Little24(volume + 5);
        auto stored = [&](uint32_t b) -> size_t { // the stored position of data block b, in blocks after `first`
            size_t n = b + ((size_t(b / PerTable) + 1) << shift);
            if (b >= PerTable)
                n += (size_t(b / PerTable1) + 1) << shift;
            return first + n * Block;
        };
        auto blockAt = [&](uint32_t b) -> const uint8_t* { return stored(b) + Block <= size ? p + stored(b) : nullptr; };
        // One table per level and no more than 170 data blocks: the one level-0 table at `first` holds every chain.
        const bool oneTable = shift == 0 && size <= first + (PerTable + 1) * Block;

        for (uint32_t t = 0; t < tableBlocks; t++)
        {
            const uint8_t* table = blockAt(tableFirst + t);
            if (table == nullptr)
                return {};
            for (size_t at = 0; at + EntrySize <= Block; at += EntrySize)
            {
                const uint8_t* entry = table + at;
                const size_t length = entry[0x28] & 0x3F;
                if (length == 0 || (entry[0x28] & 0x80) != 0 ||
                    !SameName(std::string_view(reinterpret_cast<const char*>(entry), length), name))
                    continue;
                const bool consecutive = (entry[0x28] & 0x40) != 0;
                const uint32_t blocks = Little24(entry + 0x29), start = Little24(entry + 0x2F), bytes = Big32(entry + 0x34);
                if (!consecutive && !oneTable)
                    return {};
                std::vector<uint8_t> out;
                out.reserve(bytes);
                uint32_t b = start;
                for (uint32_t i = 0; i < blocks && out.size() < bytes; i++)
                {
                    const uint8_t* data = b < End ? blockAt(b) : nullptr;
                    if (data == nullptr)
                        return {};
                    out.insert(out.end(), data, data + std::min<size_t>(Block, bytes - out.size()));
                    // The next block: the chain of the level-0 table (one table: at `first`), or the following one.
                    b = consecutive ? b + 1 : Big24(p + first + size_t(b) * HashSize + 0x15);
                }
                return out.size() == bytes ? out : std::vector<uint8_t>();
            }
        }
        return {};
    }

    std::vector<uint8_t> StfsFile(const Disc& disc, const std::string& path, const std::string& name)
    {
        const uint64_t size = disc.size(path);
        if (size == 0 || size > 64 * 1024 * 1024) // a dashboard art package is well under 2 MiB
            return {};
        std::vector<uint8_t> package(size);
        if (!disc.read(path, 0, package.size(), package.data()))
            return {};
        return StfsFile(package, name);
    }
}
