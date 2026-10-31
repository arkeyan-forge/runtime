#pragma once

// [Alchemy] IGA archives (the .arc and .bld files) and the IGZ files inside them, as far as reading pictures and shaders
// needs. One reader for every version, each a row of a table in the .cpp: IGA v8 and IGZ v6 (Giants) are verified on the
// disc; IGA v10 and IGZ v7 (Swap Force, big-endian) are verified on its disc; IGA v11 is verified on Trap Team's; IGZ
// v8 (Trap Team) is verified on its disc; v9 (the later games) follows public format descriptions [?] (games/swapforce/docs/art.md).
// Formats: games/giants/docs/shaders.md (IGA, IGZ sections), games/giants/docs/art.md (IGZ objects and images),
// docs/runtime/host/game_art.md (the versions). Shared by the runtime, SkyBuild and Skylauncher.
#include "xenos_texture.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace art::alchemy
{
    // Copies [offset, offset + size) of a file to `out`; false if it can't (bytes past the end may read as zero).
    using Reader = std::function<bool(uint64_t offset, size_t size, uint8_t* out)>;

    // Decodes the raw LZMA stream of one chunk (lc=3 lp=0 pb=2) into `out`, which is `size` bytes long. The input may end
    // early (missing bytes read as zero). Returns the bytes written; fewer than `size` means a corrupt stream.
    size_t LzmaDecodeRaw(const uint8_t* input, size_t inputSize, uint8_t* out, size_t size);

    // A format version this reader knows; `verified`: read on a real disc, else from public descriptions only [?].
    struct FormatVersion
    {
        uint32_t version;
        bool verified;
    };
    std::vector<FormatVersion> IgaVersions();
    std::vector<FormatVersion> IgzVersions();

    class IgaArchive
    {
    public:
        // Reads the header and tables of an IGA archive `length` bytes long, of a version IgaVersions() lists. nullopt if
        // it is not one.
        static std::optional<IgaArchive> Open(Reader reader, uint64_t length);
        uint32_t Version() const { return m_version; }
        bool Verified() const { return m_verified; }

        size_t Count() const { return m_entries.size(); }
        // The entry's source path as the name table stores it ("C:/tfb/build/360/levels/.../mainmenu.png/0x....igz").
        const std::string& Name(size_t index) const { return m_entries[index].name; }
        uint32_t Size(size_t index) const { return m_entries[index].size; }
        // The entry's bytes: stored, or chunked LZMA (0x8000 bytes of output per chunk). `size` limits how many are
        // decoded (0: all). Empty if a chunk is corrupt.
        std::vector<uint8_t> Read(size_t index, size_t size = 0) const;
        // The first entry whose name contains `part` (any case), if any.
        std::optional<size_t> Find(const std::string& part) const;

    private:
        struct Entry
        {
            uint32_t offset, size, flags;
            std::string name;
        };
        Reader m_reader;
        uint64_t m_length = 0;
        uint32_t m_version = 0;
        bool m_verified = false;
        uint32_t m_chunkAlign = 0x100;
        std::vector<Entry> m_entries;
    };

    // An IGZ file (big-endian, a version IgzVersions() lists), parsed far enough to list and decode its pictures
    // (igImage2 objects).
    class IgzFile
    {
    public:
        static std::optional<IgzFile> Parse(std::vector<uint8_t> bytes);
        uint32_t Version() const;
        bool Verified() const;

        struct Picture
        {
            std::string
                name; // the source path ("levels/includes/.../portraits/cynder.png"; v7: the .tga it was made from); empty in streamed .arc files
            uint32_t width = 0, height = 0;
            uint32_t format = 0; // the format's hash (an external id); see TextureFormatOf
            uint64_t object = 0; // file offset of the igImage2
        };
        std::vector<Picture> Pictures() const;
        // The picture's pixel bytes as stored (tiled, big-endian words); empty if they can't be found.
        std::vector<uint8_t> Pixels(const Picture& picture) const;

        const std::vector<uint8_t>& Bytes() const { return m_bytes; }

        // v7: the files this one depends on (its TDEP record), as paths: a material names its texture this way
        // ("textures:/JuiceStandard_Collections_Zook_10.igz"). Empty for v6.
        std::vector<std::string> Dependencies() const;

        // The objects themselves, for readers of other data than pictures (the scripts' toy tables: game_text.cpp).
        // File offsets of every object, in file order.
        std::vector<uint64_t> ObjectOffsets() const;
        // The class name of the object at a file offset ("ScriptSet", "StringInfo"...); empty if unknown.
        std::string TypeName(uint64_t object) const;
        // Word `index` (big-endian u32) of the object at a file offset.
        uint32_t Word(uint64_t object, int index) const { return Be32(object + 4 * uint64_t(index)); }
        uint32_t Be32(uint64_t at) const;
        // A serialized pointer to a file offset; nullopt when it points nowhere.
        std::optional<uint64_t> Pointer(uint32_t value) const;
        // The NUL-terminated text at a file offset (at most 512 bytes).
        std::string String(uint64_t at) const;

    private:
        struct Fixup
        {
            uint32_t count = 0;
            size_t start = 0, end = 0; // file offsets of the record's data
        };
        const Fixup* FindFixup(uint32_t type) const;

        size_t m_layout = 0; // the version's row in the .cpp's table
        std::vector<uint8_t> m_bytes;
        std::vector<uint64_t> m_pools; // file offsets of sections 1.. (a pointer's pool n is section n + 1)
        std::vector<std::pair<uint32_t, Fixup>> m_fixups;
        std::vector<std::string> m_types;
        std::vector<std::string> m_strings; // v7: TSTR
    };

    // The texture layout of a picture format hash; false for a format not known. Known: the DXT1, DXT3 and DXT5 hashes
    // (tiled and linear) and Giants' 8-bit one.
    bool TextureFormatOf(uint32_t hash, Texture& texture);

    // Decodes a picture to RGBA, the right way up (Alchemy stores pictures upside down). False if its format is unknown
    // or its pixels are missing.
    bool DecodePicture(const IgzFile& file, const IgzFile::Picture& picture, Image& out);
}
