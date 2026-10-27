#pragma once

// The primitives a Skylanders tag needs: MD5 (RFC 1321) for the block keys, AES-128 (FIPS-197) for the data blocks,
// and the CRC-48 that derives the MIFARE sector keys. Written from the public specifications.
#include <array>
#include <cstddef>
#include <cstdint>

namespace figures
{
    using Md5Digest = std::array<uint8_t, 16>;
    Md5Digest Md5(const uint8_t* data, size_t size);

    // AES-128 on one 16-byte block (the tag format uses ECB, one block at a time).
    class Aes128
    {
    public:
        explicit Aes128(const uint8_t key[16]);
        void Encrypt(const uint8_t in[16], uint8_t out[16]) const;
        void Decrypt(const uint8_t in[16], uint8_t out[16]) const;

    private:
        uint8_t m_roundKeys[11][16];
    };

    // CRC-48/ECMA-182 polynomial with the tag format's initial value, over the 4-byte serial then the sector number:
    // MIFARE key A of sectors 1-15 (sector 0 has a fixed key). Bytes in the order the sector trailer stores them.
    std::array<uint8_t, 6> SectorKeyA(const uint8_t serial[4], int sector);
}
