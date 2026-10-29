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
