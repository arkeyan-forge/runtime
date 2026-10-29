// See lux_images.h. Layout of a UI image stream (found by inspecting uigameimage.str and uiimage.str; docs/runtime/host/game_art.md):
// - "strm" file: 'strm', BE32 file size, BE32 1, BE32 1, BE32 record count, then one zlib stream with the records;
// - unpacked: 8 bytes, then records: BE32 type hash, BE32 name hash, BE32 0, BE32 body size, body;
// - image body: an "APKF" package; its '\0XET' chunk has the image name 0x30 bytes in, the Direct3D texture header
//   (with the GPU fetch constant after BaseFlush/MipFlush = FFFF0000 FFFF0000) follows, and the 'SYHP' chunk gives
//   the pixel data's offset in the body (BE32 at +8) and its size (BE32 at +16).
#include "lux_images.h"
#include <algorithm>
#include <cstring>

namespace art::lux
{
    namespace
    {
        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

        // --- Inflate (RFC 1951), canonical Huffman codes decoded one bit at a time -------------------------------

        struct BitReader
        {
            const uint8_t* data;
            size_t size;
            size_t position = 0; // in bits
            bool overrun = false;

            uint32_t Bits(int count)
            {
                uint32_t value = 0;
                for (int i = 0; i < count; i++)
                {
                    if ((position >> 3) >= size)
                    {
                        overrun = true;
                        return 0;
                    }
                    value |= uint32_t((data[position >> 3] >> (position & 7)) & 1) << i;
                    position++;
                }
                return value;
            }
        };

        struct Huffman
        {
            uint16_t counts[16] = {}; // codes of each length
            uint16_t symbols[320] = {}; // symbols ordered by code

            bool Build(const uint8_t* lengths, int count)
            {
                memset(counts, 0, sizeof(counts));
                for (int i = 0; i < count; i++)
                    counts[lengths[i]]++;
                counts[0] = 0;
                uint16_t offsets[16] = {};
                for (int length = 1; length < 16; length++)
                    offsets[length] = uint16_t(offsets[length - 1] + counts[length - 1]);
                for (int i = 0; i < count; i++)
                {
                    if (lengths[i] != 0)
                        symbols[offsets[lengths[i]]++] = uint16_t(i);
                }
                return true;
            }

            int Decode(BitReader& in) const
            {
                int code = 0, first = 0, index = 0;
                for (int length = 1; length < 16; length++)
                {
                    code |= int(in.Bits(1));
                    int count = counts[length];
                    if (code - first < count)
                        return symbols[index + code - first];
                    index += count;
                    first = (first + count) << 1;
                    code <<= 1;
                    if (in.overrun)
                        return -1;
                }
                return -1;
            }
        };

        constexpr uint16_t LengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
            67, 83, 99, 115, 131, 163, 195, 227, 258 };
        constexpr uint8_t LengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
            5, 5, 5, 0 };
        constexpr uint16_t DistanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
            513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        constexpr uint8_t DistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
            10, 11, 11, 12, 12, 13, 13 };

        bool InflateBlock(BitReader& in, const Huffman& literals, const Huffman& distances, std::vector<uint8_t>& out)
        {
            while (true)
            {
                int symbol = literals.Decode(in);
                if (symbol < 0)
                    return false;
                if (symbol < 256)
                {
                    out.push_back(uint8_t(symbol));
                    continue;
                }
                if (symbol == 256)
                    return true;
                symbol -= 257;
                if (symbol >= 29)
                    return false;
                size_t length = LengthBase[symbol] + in.Bits(LengthExtra[symbol]);
                int distanceSymbol = distances.Decode(in);
                if (distanceSymbol < 0 || distanceSymbol >= 30)
                    return false;
                size_t distance = DistanceBase[distanceSymbol] + in.Bits(DistanceExtra[distanceSymbol]);
                if (distance > out.size() || in.overrun)
                    return false;
                size_t from = out.size() - distance;
                for (size_t i = 0; i < length; i++)
                    out.push_back(out[from + i]);
            }
        }

        bool DynamicTables(BitReader& in, Huffman& literals, Huffman& distances)
        {
            static const uint8_t Order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int literalCount = int(in.Bits(5)) + 257;
            int distanceCount = int(in.Bits(5)) + 1;
            int codeCount = int(in.Bits(4)) + 4;
            if (literalCount > 286 || distanceCount > 30)
                return false;

            uint8_t codeLengths[19] = {};
            for (int i = 0; i < codeCount; i++)
                codeLengths[Order[i]] = uint8_t(in.Bits(3));
            Huffman codes;
            codes.Build(codeLengths, 19);

            uint8_t lengths[286 + 30] = {};
            for (int i = 0; i < literalCount + distanceCount;)
            {
                int symbol = codes.Decode(in);
                if (symbol < 0)
                    return false;
                if (symbol < 16)
                {
                    lengths[i++] = uint8_t(symbol);
                    continue;
                }
                uint8_t repeated = 0;
                int count;
                if (symbol == 16)
                {
                    if (i == 0)
                        return false;
                    repeated = lengths[i - 1];
                    count = 3 + int(in.Bits(2));
                }
                else if (symbol == 17)
                    count = 3 + int(in.Bits(3));
                else
                    count = 11 + int(in.Bits(7));
                if (i + count > literalCount + distanceCount)
                    return false;
                while (count-- > 0)
                    lengths[i++] = repeated;
            }
            literals.Build(lengths, literalCount);
            distances.Build(lengths + literalCount, distanceCount);
            return !in.overrun;
        }
        bool ReadText(const uint8_t* p, const uint8_t* end, std::string& text)
        {
            text.clear();
            while (p < end && *p != 0 && text.size() < 64)
                text += char(*p++);
            return p < end && !text.empty();
        }

        // Finds `tag` (4 bytes) in [begin, end).
        const uint8_t* FindTag(const uint8_t* begin, const uint8_t* end, const char* tag)
        {
            auto found = std::search(begin, end, tag, tag + 4);
            return found != end ? found : nullptr;
        }

        // The texture formats of the GPU fetch constant that the UI pictures use.
        bool FormatOf(uint32_t format, TextureFormat& out)
        {
            switch (format)
            {
            case 2: out = TextureFormat::L8; return true; // k_8
            case 6: out = TextureFormat::Argb8; return true; // k_8_8_8_8
            case 18: out = TextureFormat::Dxt1; return true; // k_DXT1
            case 19: out = TextureFormat::Dxt3; return true; // k_DXT2_3
            case 20: out = TextureFormat::Dxt5; return true; // k_DXT4_5
            }
            return false;
        }
    }

    bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
    {
        // zlib header: deflate, window <= 32 KB, no preset dictionary, header checksum.
        if (size < 2 || (data[0] & 0x0F) != 8 || (data[0] >> 4) > 7 || (data[1] & 0x20) != 0 ||
            ((data[0] << 8) | data[1]) % 31 != 0)
            return false;
        BitReader in{ data + 2, size - 2 };
        bool last = false;
        while (!last)
        {
            last = in.Bits(1) != 0;
            uint32_t type = in.Bits(2);
            if (type == 0)
            {
                size_t byte = (in.position + 7) / 8;
                if (byte + 4 > in.size)
                    return false;
                uint16_t length = uint16_t(in.data[byte] | in.data[byte + 1] << 8);
                uint16_t inverse = uint16_t(in.data[byte + 2] | in.data[byte + 3] << 8);
                if (uint16_t(~inverse) != length || byte + 4 + length > in.size)
                    return false;
                out.insert(out.end(), in.data + byte + 4, in.data + byte + 4 + length);
                in.position = (byte + 4 + length) * 8;
            }
            else if (type == 1)
            {
                static const auto Fixed = [] {
                    std::pair<Huffman, Huffman> tables;
                    uint8_t lengths[288];
                    for (int i = 0; i < 288; i++)
                        lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                    tables.first.Build(lengths, 288);
                    uint8_t distances[30];
                    std::fill(std::begin(distances), std::end(distances), uint8_t(5));
                    tables.second.Build(distances, 30);
                    return tables;
                }();
                if (!InflateBlock(in, Fixed.first, Fixed.second, out))
                    return false;
            }
            else if (type == 2)
            {
                Huffman literals, distances;
                if (!DynamicTables(in, literals, distances) || !InflateBlock(in, literals, distances, out))
                    return false;
            }
            else
            {
                return false;
            }
            if (in.overrun)
                return false;
        }
        return true;
    }

    bool UnpackStream(const std::vector<uint8_t>& file, std::vector<uint8_t>& out)
    {
        constexpr size_t HeaderSize = 0x14;
        if (file.size() < HeaderSize || memcmp(file.data(), "strm", 4) != 0)
            return false;
        out.clear();
        return Inflate(file.data() + HeaderSize, file.size() - HeaderSize, out);
    }

    void DecodeImages(const std::vector<uint8_t>& stream, const std::function<bool(const std::string&)>& wanted,
        const std::function<void(const std::string&, const Image&)>& found)
    {
        for (size_t offset = 8; offset + 16 <= stream.size();)
        {
            uint32_t size = Be32(&stream[offset + 12]);
            const uint8_t* body = stream.data() + offset + 16;
            offset += 16 + size_t(size);
            if (offset > stream.size())
                break;
            const uint8_t* end = body + size;
            if (size < 0x100 || memcmp(body, "APKF", 4) != 0)
                continue;

            const uint8_t* header = FindTag(body, std::min(end, body + 0x100), "\0XET");
            std::string name;
            if (header == nullptr || !ReadText(header + 0x30, end, name) || !wanted(name))
                continue;
            static const uint8_t Flushes[8] = { 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };
            const uint8_t* flushes = std::search(header, std::min(end, body + 0x400), Flushes, Flushes + 8);
            const uint8_t* physical = FindTag(header, std::min(end, body + 0x400), "SYHP");
            if (physical == nullptr || flushes + 8 + 24 > end)
                continue;

            // The GPU texture fetch constant (6 words): type, pitch and tiling, format and endianness, size, dimension.
            uint32_t fetch[6];
            for (int i = 0; i < 6; i++)
                fetch[i] = Be32(flushes + 8 + i * 4);
            Texture texture;
            uint32_t dataOffset = Be32(physical + 8), dataSize = Be32(physical + 16);
            bool texture2d = (fetch[0] & 3) == 2 && ((fetch[5] >> 9) & 3) == 1;
            if (!texture2d || !FormatOf(fetch[1] & 0x3F, texture.format) || uint64_t(dataOffset) + dataSize > size)
                continue;
            texture.tiled = (fetch[0] >> 31) != 0;
            texture.pitch = ((fetch[0] >> 22) & 0x1FF) << 5;
            texture.endian = TextureEndian((fetch[1] >> 6) & 3);
            texture.width = (fetch[2] & 0x1FFF) + 1;
            texture.height = ((fetch[2] >> 13) & 0x1FFF) + 1;
            Image image;
            if (DecodeTexture(body + dataOffset, dataSize, texture, image))
                found(name, image);
        }
    }
}
