#pragma once

// A figure's NFC tag: MIFARE Classic 1K, 64 blocks of 16 bytes, as raw tag dumps (.sky/.bin/.dmp) store it.
#include <array>
#include <cstddef>
#include <cstdint>

constexpr size_t FigureBlockSize = 16;
constexpr size_t FigureBlockCount = 64;
using FigureData = std::array<uint8_t, FigureBlockSize * FigureBlockCount>;

// A new, never-played figure of the given character (toy type), with a random serial and valid header checksum.
FigureData CreateBlankFigure(uint16_t toyType, uint16_t variant = 0);
// CRC-16/CCITT-FALSE, the checksum Skylanders tags use.
uint16_t FigureChecksum(const uint8_t* data, size_t size);
