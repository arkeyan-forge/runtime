// The plain facts of a figure's tag that every part of the figure code needs (docs/runtime/host/figures.md).
#include "figure_data.h"
#include <cstring>
#include <random>

namespace
{
    // Sector 0 (serial, toy type, header checksum) is read-only on retail figures, every other sector read/write.
    constexpr uint8_t AccessReadOnly[4] = { 0x0F, 0x0F, 0x0F, 0x69 };
    constexpr uint8_t AccessReadWrite[4] = { 0x7F, 0x0F, 0x08, 0x69 };
}

uint16_t FigureChecksum(const uint8_t* data, size_t size)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; i++)
    {
        crc ^= uint16_t(data[i] << 8);
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

// Layout of sector 0: block 0 = 4-byte serial (NUID), its XOR check byte, SAK and ATQA; block 1 = toy type
// (LE16 at 0x10), trading card id (0x14..0x1B, left zero), variant (LE16 at 0x1C), CRC16 of 0x00..0x1D (LE at 0x1E).
// Every other data block stays zero: the title treats such a figure as brand new and initialises it.
FigureData CreateBlankFigure(uint16_t toyType, uint16_t variant)
{
    FigureData data{};
    std::random_device random;
    for (int i = 0; i < 4; i++)
        data[i] = uint8_t(random());
    data[4] = data[0] ^ data[1] ^ data[2] ^ data[3];
    data[5] = 0x81;
    data[6] = 0x01;
    data[7] = 0x0F;

    data[0x10] = uint8_t(toyType);
    data[0x11] = uint8_t(toyType >> 8);
    data[0x1C] = uint8_t(variant);
    data[0x1D] = uint8_t(variant >> 8);
    uint16_t checksum = FigureChecksum(data.data(), 0x1E);
    data[0x1E] = uint8_t(checksum);
    data[0x1F] = uint8_t(checksum >> 8);

    // Sector trailers: key A reads back as zeros through a portal, then the access bits.
    for (size_t sector = 0; sector < FigureBlockCount / 4; sector++)
        memcpy(&data[(sector * 4 + 3) * FigureBlockSize + 6], sector == 0 ? AccessReadOnly : AccessReadWrite, 4);
    return data;
}
