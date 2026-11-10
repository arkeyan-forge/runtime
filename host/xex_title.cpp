#include "xex_title.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <image.h>
#include <xdbf_wrapper.h>

namespace
{
    constexpr uint32_t XexMagic = 0x58455832; // "XEX2"
    constexpr uint32_t XexHeaderResourceInfo = 0x000002FF;
    constexpr uint32_t XexHeaderExecutionInfo = 0x00040006;
    constexpr uint32_t XexHeaderFileFormatInfo = 0x000003FF;
    constexpr uint32_t XexHeaderImportLibraries = 0x000103FF;

    // SPA (title XDBF) ids of the title's own name string and icon image.
    constexpr uint16_t TitleNameStringId = 0x8000;
    constexpr uint64_t TitleIconImageId = 0x8000;

    uint32_t ReadBE32(const uint8_t* p)
    {
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }

    // Value of an XEX2 optional header: inline data, or the file offset of its data. Bounds-checked, as the
    // file comes from the player.
    std::optional<uint32_t> FindXexHeader(const std::vector<uint8_t>& xex, uint32_t key)
    {
        if (xex.size() < 0x18 || ReadBE32(xex.data()) != XexMagic)
            return {};
        uint32_t count = ReadBE32(&xex[0x14]);
        if (count > (xex.size() - 0x18) / 8)
            return {};
        for (uint32_t i = 0; i < count; i++)
        {
            if (ReadBE32(&xex[0x18 + i * 8]) == key)
                return ReadBE32(&xex[0x18 + i * 8 + 4]);
        }
        return {};
    }
}

std::optional<ExecutionInfo> ReadExecutionInfo(const std::vector<uint8_t>& xex)
{
    auto offset = FindXexHeader(xex, XexHeaderExecutionInfo);
    if (!offset || *offset > xex.size() || xex.size() - *offset < 0x18)
        return {};
    const uint8_t* info = &xex[*offset];
    return ExecutionInfo{ ReadBE32(info), ReadBE32(info + 4), ReadBE32(info + 12) };
}

std::optional<uint32_t> ReadRegion(const std::vector<uint8_t>& xex)
{
    // The security info's offset is at 0x10; its region mask at 0x178 into it.
    if (xex.size() < 0x14 || ReadBE32(xex.data()) != XexMagic)
        return {};
    uint32_t security = ReadBE32(&xex[0x10]);
    if (security == 0 || security > xex.size() || xex.size() - security < 0x17C)
        return {};
    return ReadBE32(&xex[security + 0x178]);
}

bool XexImageReadable(const std::vector<uint8_t>& xex)
{
    if (xex.size() < 0x18 || ReadBE32(xex.data()) != XexMagic)
        return false;
    const uint64_t size = xex.size(), header = ReadBE32(&xex[0x08]), security = ReadBE32(&xex[0x10]);
    const uint64_t count = ReadBE32(&xex[0x14]);
    if (header > size || header < 0x18 + count * 8 || security + 0x180 > header)
        return false;
    const uint64_t imageSize = ReadBE32(&xex[security + 4]);
    if (imageSize < 0x1000 || imageSize > (512u << 20))
        return false;
    const auto format = FindXexHeader(xex, XexHeaderFileFormatInfo);
    if (!format || *format + 8 > header)
        return false;
    const uint64_t infoSize = ReadBE32(&xex[*format]);
    const unsigned encryption = unsigned(xex[*format + 4]) << 8 | xex[*format + 5];
    const unsigned compression = unsigned(xex[*format + 6]) << 8 | xex[*format + 7];
    if (infoSize < 8 || *format + infoSize > header || encryption > 1 || compression > 2)
        return false;
    if (auto imports = FindXexHeader(xex, XexHeaderImportLibraries))
    {
        if (*imports + 12 > header || *imports + ReadBE32(&xex[*imports]) > header ||
            uint64_t(ReadBE32(&xex[*imports + 4])) + 12 > ReadBE32(&xex[*imports]))
            return false;
    }
    const uint64_t data = size - header;
    if (compression == 0) // stored
        return imageSize <= data;
    if (compression == 1) // basic: data and zero runs
    {
        uint64_t stored = 0;
        for (uint64_t at = *format + 8; at + 8 <= *format + infoSize; at += 8)
            stored += ReadBE32(&xex[at]);
        return stored <= data;
    }
    // LZX: the window, then the first block's size and hash (each block holds the next one's).
    if (infoSize < 8 + 4 + 24)
        return false;
    const uint64_t window = ReadBE32(&xex[*format + 8]), firstBlock = ReadBE32(&xex[*format + 12]);
    return window >= 0x8000 && window <= 0x200000 && (window & (window - 1)) == 0 && firstBlock >= 24 &&
           firstBlock <= data;
}

TitleMetadata ReadTitleMetadata(const std::vector<uint8_t>& xex)
{
    TitleMetadata metadata;
    auto execution = ReadExecutionInfo(xex);
    auto table = FindXexHeader(xex, XexHeaderResourceInfo);
    if (!execution || !table || *table > xex.size() - 4)
        return metadata;
    metadata.titleId = execution->titleId;

    // Resource table: its size, then { name[8], address, size } entries. The title's XDBF is named after its title ID.
    uint32_t tableSize = std::min<uint64_t>(ReadBE32(&xex[*table]), xex.size() - *table);
    char name[9];
    snprintf(name, sizeof(name), "%08X", execution->titleId);
    for (uint32_t position = 4; position + 16 <= tableSize; position += 16)
    {
        const uint8_t* resource = &xex[*table + position];
        if (memcmp(resource, name, 8) != 0)
            continue;

        uint32_t address = ReadBE32(resource + 8);
        uint32_t size = ReadBE32(resource + 12);
        Image image = Image::ParseImage(xex.data(), xex.size());
        if (image.data == nullptr || address < image.base || address - image.base > image.size ||
            size > image.size - (address - image.base))
            break;

        XDBFWrapper xdbf(image.data.get() + (address - image.base), size);
        if (xdbf.pBuffer == nullptr)
            break;
        metadata.name = xdbf.GetString(XDBF_LANGUAGE_ENGLISH, TitleNameStringId);
        if (XDBFBlock icon = xdbf.GetResource(XDBF_SPA_NAMESPACE_IMAGE, TitleIconImageId))
            metadata.iconPng.assign(icon.pBuffer, icon.pBuffer + icon.BufferSize);
        break;
    }
    return metadata;
}
