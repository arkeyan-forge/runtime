#include "figure_crypto.h"
#include <cstring>

namespace figures
{
    namespace
    {
        uint32_t RotateLeft(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

        // RFC 1321, section 3.4: per-round shifts and the sine-derived constants.
        constexpr int Shifts[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        };
        constexpr uint32_t Sines[64] = {
            0xD76AA478, 0xE8C7B756, 0x242070DB, 0xC1BDCEEE, 0xF57C0FAF, 0x4787C62A, 0xA8304613, 0xFD469501,
            0x698098D8, 0x8B44F7AF, 0xFFFF5BB1, 0x895CD7BE, 0x6B901122, 0xFD987193, 0xA679438E, 0x49B40821,
            0xF61E2562, 0xC040B340, 0x265E5A51, 0xE9B6C7AA, 0xD62F105D, 0x02441453, 0xD8A1E681, 0xE7D3FBC8,
            0x21E1CDE6, 0xC33707D6, 0xF4D50D87, 0x455A14ED, 0xA9E3E905, 0xFCEFA3F8, 0x676F02D9, 0x8D2A4C8A,
            0xFFFA3942, 0x8771F681, 0x6D9D6122, 0xFDE5380C, 0xA4BEEA44, 0x4BDECFA9, 0xF6BB4B60, 0xBEBFBC70,
            0x289B7EC6, 0xEAA127FA, 0xD4EF3085, 0x04881D05, 0xD9D4D039, 0xE6DB99E5, 0x1FA27CF8, 0xC4AC5665,
            0xF4292244, 0x432AFF97, 0xAB9423A7, 0xFC93A039, 0x655B59C3, 0x8F0CCC92, 0xFFEFF47D, 0x85845DD1,
            0x6FA87E4F, 0xFE2CE6E0, 0xA3014314, 0x4E0811A1, 0xF7537E82, 0xBD3AF235, 0x2AD7D2BB, 0xEB86D391,
        };

        void Md5Block(uint32_t state[4], const uint8_t block[64])
        {
            uint32_t words[16];
            for (int i = 0; i < 16; i++)
                words[i] = block[i * 4] | block[i * 4 + 1] << 8 | block[i * 4 + 2] << 16 | uint32_t(block[i * 4 + 3]) << 24;

            uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
            for (int i = 0; i < 64; i++)
            {
                uint32_t f;
                int g;
                if (i < 16) { f = (b & c) | (~b & d); g = i; }
                else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
                else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
                else { f = c ^ (b | ~d); g = (7 * i) % 16; }
                uint32_t next = d;
                d = c;
                c = b;
                b = b + RotateLeft(a + f + Sines[i] + words[g], Shifts[i]);
                a = next;
            }
            state[0] += a;
            state[1] += b;
            state[2] += c;
            state[3] += d;
        }

        // AES tables, built from their definitions (FIPS-197 section 5.1.1): the S-box is the multiplicative
        // inverse in GF(2^8) followed by an affine transform.
        uint8_t Multiply(uint8_t a, uint8_t b)
        {
            uint8_t product = 0;
            while (b != 0)
            {
                if (b & 1)
                    product ^= a;
                a = uint8_t((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
                b >>= 1;
            }
            return product;
        }

        struct SBoxes
        {
            uint8_t forward[256];
            uint8_t inverse[256];

            SBoxes()
            {
                for (int x = 0; x < 256; x++)
                {
                    uint8_t inv = 0;
                    for (int y = 1; y < 256 && x != 0; y++)
                    {
                        if (Multiply(uint8_t(x), uint8_t(y)) == 1)
                        {
                            inv = uint8_t(y);
                            break;
                        }
                    }
                    uint8_t s = inv;
                    for (int shift = 1; shift < 5; shift++)
                        s ^= uint8_t((inv << shift) | (inv >> (8 - shift)));
                    s ^= 0x63;
                    forward[x] = s;
                    inverse[s] = uint8_t(x);
                }
            }
        };

        const SBoxes& Boxes()
        {
            static const SBoxes boxes;
            return boxes;
        }

        void ShiftRows(uint8_t s[16], bool inverse)
        {
            uint8_t t[16];
            for (int column = 0; column < 4; column++)
            {
                for (int row = 0; row < 4; row++)
                {
                    int from = inverse ? (column - row + 4) % 4 : (column + row) % 4;
                    t[column * 4 + row] = s[from * 4 + row];
                }
            }
            memcpy(s, t, 16);
        }

        void MixColumns(uint8_t s[16], bool inverse)
        {
            static const uint8_t Forward[4] = { 2, 3, 1, 1 };
            static const uint8_t Inverse[4] = { 14, 11, 13, 9 };
            const uint8_t* m = inverse ? Inverse : Forward;
            for (int column = 0; column < 4; column++)
            {
                uint8_t* c = s + column * 4;
                uint8_t t[4];
                for (int row = 0; row < 4; row++)
                {
                    t[row] = uint8_t(Multiply(c[0], m[(4 - row) % 4]) ^ Multiply(c[1], m[(5 - row) % 4]) ^
                        Multiply(c[2], m[(6 - row) % 4]) ^ Multiply(c[3], m[(7 - row) % 4]));
                }
                memcpy(c, t, 4);
            }
        }

        void AddRoundKey(uint8_t s[16], const uint8_t key[16])
        {
            for (int i = 0; i < 16; i++)
                s[i] ^= key[i];
        }
    }

    Md5Digest Md5(const uint8_t* data, size_t size)
    {
        uint32_t state[4] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476 };
        size_t full = size / 64 * 64;
        for (size_t offset = 0; offset < full; offset += 64)
            Md5Block(state, data + offset);

        // Padding: 0x80, zeros, then the message length in bits (little-endian), in one or two blocks.
        uint8_t tail[128] = {};
        size_t rest = size - full;
        memcpy(tail, data + full, rest);
        tail[rest] = 0x80;
        size_t tailSize = rest < 56 ? 64 : 128;
        uint64_t bits = uint64_t(size) * 8;
        for (int i = 0; i < 8; i++)
            tail[tailSize - 8 + i] = uint8_t(bits >> (i * 8));
        for (size_t offset = 0; offset < tailSize; offset += 64)
            Md5Block(state, tail + offset);

        Md5Digest digest;
        for (int i = 0; i < 16; i++)
            digest[i] = uint8_t(state[i / 4] >> ((i % 4) * 8));
        return digest;
    }

    Aes128::Aes128(const uint8_t key[16])
    {
        const SBoxes& boxes = Boxes();
        memcpy(m_roundKeys[0], key, 16);
        uint8_t rcon = 1;
        for (int round = 1; round <= 10; round++)
        {
            const uint8_t* previous = m_roundKeys[round - 1];
            uint8_t* next = m_roundKeys[round];
            uint8_t t[4] = { boxes.forward[previous[13]], boxes.forward[previous[14]], boxes.forward[previous[15]], boxes.forward[previous[12]] };
            t[0] ^= rcon;
            rcon = Multiply(rcon, 2);
            for (int i = 0; i < 16; i++)
            {
                next[i] = previous[i] ^ (i < 4 ? t[i] : next[i - 4]);
            }
        }
    }

    void Aes128::Encrypt(const uint8_t in[16], uint8_t out[16]) const
    {
        const SBoxes& boxes = Boxes();
        uint8_t s[16];
        memcpy(s, in, 16);
        AddRoundKey(s, m_roundKeys[0]);
        for (int round = 1; round <= 10; round++)
        {
            for (uint8_t& byte : s)
                byte = boxes.forward[byte];
            ShiftRows(s, false);
            if (round != 10)
                MixColumns(s, false);
            AddRoundKey(s, m_roundKeys[round]);
        }
        memcpy(out, s, 16);
    }

    void Aes128::Decrypt(const uint8_t in[16], uint8_t out[16]) const
    {
        const SBoxes& boxes = Boxes();
        uint8_t s[16];
        memcpy(s, in, 16);
        AddRoundKey(s, m_roundKeys[10]);
        for (int round = 9; round >= 0; round--)
        {
            ShiftRows(s, true);
            for (uint8_t& byte : s)
                byte = boxes.inverse[byte];
            AddRoundKey(s, m_roundKeys[round]);
            if (round != 0)
                MixColumns(s, true);
        }
        memcpy(out, s, 16);
    }

    std::array<uint8_t, 6> SectorKeyA(const uint8_t serial[4], int sector)
    {
        constexpr uint64_t Mask = (1ull << 48) - 1;
        uint64_t key = 73ull * 2017 * 560381651; // sector 0: 4B 0B 20 10 7C CB on every figure
        if (sector != 0)
        {
            constexpr uint64_t Polynomial = 0x42F0E1EBA9EA3693;
            uint64_t crc = 2ull * 2 * 3 * 1103 * 12868356821;
            const uint8_t input[5] = { serial[0], serial[1], serial[2], serial[3], uint8_t(sector) };
            for (uint8_t byte : input)
            {
                crc ^= uint64_t(byte) << 40;
                for (int bit = 0; bit < 8; bit++)
                    crc = ((crc & (1ull << 47)) ? (crc << 1) ^ Polynomial : crc << 1) & Mask;
            }
            key = crc;
        }

        std::array<uint8_t, 6> bytes;
        for (int i = 0; i < 6; i++)
            bytes[i] = uint8_t(key >> (40 - 8 * i));
        return bytes;
    }
}
