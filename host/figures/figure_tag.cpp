#include "figure_tag.h"
#include "figure_crypto.h"
#include <algorithm>
#include <cstring>

namespace figures
{
    namespace
    {
        constexpr char KeyConstant[] = " Copyright (C) 2010 Activision. All Rights Reserved. ";
        constexpr size_t AreaBlocks[2] = { 0x08, 0x24 };
        constexpr size_t AreaSpan = 9 * FigureBlockSize; // 7 data blocks and the 2 trailers between them
        // Giants' second pair: 4 data blocks and the trailer between them (blocks 17, 18, 20, 21 and 45, 46, 48, 49),
        // kept as 64 bytes. Byte 0 is the checksum (LE16), byte 2 the sequence number.
        constexpr size_t ExtraBlocks[2] = { 0x11, 0x2D };
        constexpr size_t ExtraSpan = 5 * FigureBlockSize;
        constexpr size_t ExtraSize = 4 * FigureBlockSize;
        constexpr uint8_t ExtraMarker[2] = { 0x06,
            0x01 }; // stands for the checksum while it is computed (05 00 in the first areas)
        constexpr uint8_t AccessReadOnly[4] = { 0x0F, 0x0F, 0x0F, 0x69 };
        constexpr uint8_t AccessReadWrite[4] = { 0x7F, 0x0F, 0x08, 0x69 };

        bool IsTrailer(size_t block) { return block % 4 == 3; }
        bool IsZero(const uint8_t* data, size_t size)
        {
            for (size_t i = 0; i < size; i++)
            {
                if (data[i] != 0)
                    return false;
            }
            return true;
        }

        Aes128 BlockCipher(const FigureData& tag, size_t block)
        {
            uint8_t input[0x20 + 1 + sizeof(KeyConstant) - 1];
            memcpy(input, tag.data(), 0x20);
            input[0x20] = uint8_t(block);
            memcpy(input + 0x21, KeyConstant, sizeof(KeyConstant) - 1);
            Md5Digest key = Md5(input, sizeof(input));
            return Aes128(key.data());
        }

        uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
        uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
        void Put16(uint8_t* p, uint32_t v)
        {
            p[0] = uint8_t(v);
            p[1] = uint8_t(v >> 8);
        }
        void Put32(uint8_t* p, uint32_t v)
        {
            Put16(p, v);
            Put16(p + 2, v >> 16);
        }

        Date ReadDate(const uint8_t* p) { return { p[0], p[1], p[2], p[3], Le16(p + 4) }; }
        void WriteDate(uint8_t* p, const Date& date)
        {
            p[0] = date.minute;
            p[1] = date.hour;
            p[2] = date.day;
            p[3] = date.month;
            Put16(p + 4, date.year);
        }

        // The three checksums of an area (`a` = its first byte), CRC-16/CCITT-FALSE, little-endian:
        // 0x0E: the first block with 05 00 in place of the checksum itself;
        // 0x0C: the next two data blocks and the one after the trailer (0x10-0x2F, 0x40-0x4F);
        // 0x0A: the last three data blocks (0x50-0x6F, 0x80-0x8F) followed by 0xE0 zero bytes.
        uint16_t HeaderChecksum(const uint8_t* a)
        {
            uint8_t header[16];
            memcpy(header, a, 16);
            header[0x0E] = 0x05;
            header[0x0F] = 0x00;
            return FigureChecksum(header, sizeof(header));
        }

        uint16_t FirstChecksum(const uint8_t* a)
        {
            uint8_t data[0x30];
            memcpy(data, a + 0x10, 0x20);
            memcpy(data + 0x20, a + 0x40, 0x10);
            return FigureChecksum(data, sizeof(data));
        }

        uint16_t SecondChecksum(const uint8_t* a)
        {
            uint8_t data[0x110] = {};
            memcpy(data, a + 0x50, 0x20);
            memcpy(data + 0x20, a + 0x80, 0x10);
            return FigureChecksum(data, sizeof(data));
        }

        bool AreaValid(const uint8_t* a)
        {
            return Le16(a + 0x0E) == HeaderChecksum(a) && Le16(a + 0x0C) == FirstChecksum(a) &&
                   Le16(a + 0x0A) == SecondChecksum(a);
        }

        void SealArea(uint8_t* a)
        {
            Put16(a + 0x0A, SecondChecksum(a));
            Put16(a + 0x0C, FirstChecksum(a));
            Put16(a + 0x0E, HeaderChecksum(a));
        }

        bool AreaBlank(const uint8_t* a)
        {
            for (size_t offset = 0; offset < AreaSpan; offset += FigureBlockSize)
            {
                if (!IsTrailer(offset / FigureBlockSize) && !IsZero(a + offset, FigureBlockSize))
                    return false;
            }
            return true;
        }

        // The extra area starting at block `start`, packed without its trailer.
        void PackExtra(const FigureData& plain, size_t start, uint8_t* out)
        {
            for (size_t i = 0, packed = 0; i < ExtraSpan / FigureBlockSize; i++)
            {
                if (!IsTrailer(start + i))
                    memcpy(out + packed++ * FigureBlockSize, plain.data() + (start + i) * FigureBlockSize,
                        FigureBlockSize);
            }
        }

        void UnpackExtra(FigureData& plain, size_t start, const uint8_t* in)
        {
            for (size_t i = 0, packed = 0; i < ExtraSpan / FigureBlockSize; i++)
            {
                if (!IsTrailer(start + i))
                    memcpy(
                        plain.data() + (start + i) * FigureBlockSize, in + packed++ * FigureBlockSize, FigureBlockSize);
            }
        }

        // CRC-16/CCITT-FALSE of the 64 bytes with the checksum itself replaced by the marker.
        uint16_t ExtraChecksum(const uint8_t* e)
        {
            uint8_t data[ExtraSize];
            memcpy(data, e, sizeof(data));
            memcpy(data, ExtraMarker, sizeof(ExtraMarker));
            return FigureChecksum(data, sizeof(data));
        }

        bool ExtraValid(const uint8_t* e) { return Le16(e) == ExtraChecksum(e); }
        bool ExtraBlank(const uint8_t* e) { return IsZero(e, ExtraSize); }
        void SealExtra(uint8_t* e) { Put16(e, ExtraChecksum(e)); }

        // Index of the extra area to read, or -1 if neither is valid.
        int NewestExtra(const FigureData& plain, uint8_t out[ExtraSize])
        {
            uint8_t a[ExtraSize], b[ExtraSize];
            PackExtra(plain, ExtraBlocks[0], a);
            PackExtra(plain, ExtraBlocks[1], b);
            bool validA = ExtraValid(a), validB = ExtraValid(b);
            int which = validA && validB ? (uint8_t(a[2] + 1) == b[2] ? 1 : 0) : validA ? 0 : validB ? 1 : -1;
            if (which >= 0)
                memcpy(out, which == 0 ? a : b, ExtraSize);
            return which;
        }

        // Index of the area to read, or -1 if neither is valid.
        int NewestArea(const FigureData& plain)
        {
            const uint8_t* a = plain.data() + AreaBlocks[0] * FigureBlockSize;
            const uint8_t* b = plain.data() + AreaBlocks[1] * FigureBlockSize;
            bool validA = AreaValid(a), validB = AreaValid(b);
            if (validA && validB)
                return uint8_t(a[0x09] + 1) == b[0x09] ? 1 : 0;
            return validA ? 0 : validB ? 1 : -1;
        }
    }

    // The game's own table (every hero's character data on Swap Force's disc carries these 20 entries): levels 11-15 are
    // Giants' (96500 = 33000 + 63500), levels 16-20 Swap Force's (197500 = 96500 + 101000).
    const uint32_t LevelExperience[MaxLevelSwapForce] = { 0, 1000, 2200, 3800, 6000, 9000, 13000, 18200, 24800, 33000,
        42700, 53900, 66600, 80800, 96500, 113700, 132400, 152600, 174300, 197500 };

    int LevelForExperience(uint32_t experience, int maxLevel)
    {
        maxLevel = std::clamp(maxLevel, 1, MaxLevelSwapForce);
        int level = 1;
        while (level < maxLevel && experience >= LevelExperience[level])
            level++;
        return level;
    }

    int LevelCap(const std::string& game)
    {
        return game == "ssa" ? MaxLevel : game == "giants" ? MaxLevelGiants : MaxLevelSwapForce;
    }

    uint32_t TotalExperience(const FigureStats& stats)
    {
        return stats.experience + stats.giantsExperience + stats.swapForceExperience;
    }

    uint32_t ExperienceFor(const FigureStats& stats, int cap)
    {
        return cap <= MaxLevel         ? stats.experience
               : cap <= MaxLevelGiants ? stats.experience + stats.giantsExperience
                                       : TotalExperience(stats);
    }

    void SetExperienceFor(FigureStats& stats, int cap, uint32_t experience)
    {
        if (cap <= MaxLevel)
            stats.experience = std::min(experience, MaxBaseExperience);
        else if (cap <= MaxLevelGiants)
        {
            // Giants counts the first two parts only; Swap Force's part stays as it is.
            experience = std::min(experience, MaxBaseExperience + MaxGiantsExperience);
            stats.experience = std::min(experience, MaxBaseExperience);
            stats.giantsExperience = experience - stats.experience;
        }
        else
            SetTotalExperience(stats, experience);
    }

    void SetTotalExperience(FigureStats& stats, uint32_t total)
    {
        total = std::min(total, MaxBaseExperience + MaxGiantsExperience + MaxSwapForceExperience);
        stats.experience = std::min(total, MaxBaseExperience);
        stats.giantsExperience = std::min(total - stats.experience, MaxGiantsExperience);
        stats.swapForceExperience = total - stats.experience - stats.giantsExperience;
    }

    FigureData Decrypt(const FigureData& tag)
    {
        FigureData plain = tag;
        for (size_t block = 8; block < FigureBlockCount; block++)
        {
            uint8_t* data = plain.data() + block * FigureBlockSize;
            if (!IsTrailer(block) && !IsZero(data, FigureBlockSize))
                BlockCipher(tag, block).Decrypt(data, data);
        }
        return plain;
    }

    FigureData Encrypt(const FigureData& plain)
    {
        // Every block of an area in use is encrypted, zero or not (the title does the same); other zero blocks are
        // left zero, as on a new figure.
        bool areaUsed[2], extraUsed[2];
        for (int i = 0; i < 2; i++)
        {
            areaUsed[i] = !AreaBlank(plain.data() + AreaBlocks[i] * FigureBlockSize);
            uint8_t extra[ExtraSize];
            PackExtra(plain, ExtraBlocks[i], extra);
            extraUsed[i] = !ExtraBlank(extra);
        }

        FigureData tag = plain;
        for (size_t block = 8; block < FigureBlockCount; block++)
        {
            if (IsTrailer(block))
                continue;
            uint8_t* data = tag.data() + block * FigureBlockSize;
            bool inUsedArea = false;
            for (int i = 0; i < 2; i++)
            {
                inUsedArea |=
                    areaUsed[i] && block >= AreaBlocks[i] && block < AreaBlocks[i] + AreaSpan / FigureBlockSize;
                inUsedArea |=
                    extraUsed[i] && block >= ExtraBlocks[i] && block < ExtraBlocks[i] + ExtraSpan / FigureBlockSize;
            }
            if (inUsedArea || !IsZero(data, FigureBlockSize))
                BlockCipher(plain, block).Encrypt(data, data);
        }
        return tag;
    }

    uint16_t ToyType(const FigureData& tag) { return Le16(&tag[0x10]); }
    uint16_t Variant(const FigureData& tag) { return Le16(&tag[0x1C]); }

    void WriteSectorKeys(FigureData& tag)
    {
        for (int sector = 0; sector < int(FigureBlockCount / 4); sector++)
        {
            uint8_t* trailer = tag.data() + (sector * 4 + 3) * FigureBlockSize;
            std::array<uint8_t, 6> key = SectorKeyA(tag.data(), sector);
            memcpy(trailer, key.data(), key.size());
            memcpy(trailer + 6, sector == 0 ? AccessReadOnly : AccessReadWrite, 4);
        }
    }

    FigureData CreateFigure(uint16_t toyType, uint16_t variant)
    {
        FigureData tag = CreateBlankFigure(toyType, variant);
        WriteSectorKeys(tag);
        return tag;
    }

    FigureData CreateTrap(uint16_t toyType, uint16_t variant, uint16_t villain)
    {
        FigureData tag = CreateFigure(toyType, variant);
        if (villain != 0)
        {
            FigureStats stats;
            stats.villain = villain;
            WriteStats(tag, stats);
        }
        return tag;
    }

    AreaState ReadStats(const FigureData& tag, FigureStats& stats)
    {
        FigureData plain = Decrypt(tag);
        int area = NewestArea(plain);
        stats = {};
        if (area < 0)
        {
            bool blank = AreaBlank(plain.data() + AreaBlocks[0] * FigureBlockSize) &&
                         AreaBlank(plain.data() + AreaBlocks[1] * FigureBlockSize);
            return blank ? AreaState::Blank : AreaState::Damaged;
        }

        const uint8_t* a = plain.data() + AreaBlocks[area] * FigureBlockSize;
        stats.experience = a[0] | a[1] << 8 | a[2] << 16;
        stats.gold = Le16(a + 0x03);
        stats.playSeconds = Le32(a + 0x05);
        stats.sequence = a[0x09];
        stats.upgrades = Le16(a + 0x10);
        stats.platforms = a[0x13];
        stats.ssaHat = Le16(a + 0x14);
        stats.hat = stats.ssaHat;
        memcpy(stats.owner.data(), a + 0x18, stats.owner.size());
        for (size_t i = 0; i < 16; i++)
        {
            char16_t unit = char16_t(Le16(a + (i < 8 ? 0x20 + i * 2 : 0x40 + (i - 8) * 2)));
            if (unit == 0)
                break;
            stats.nickname += unit;
        }
        stats.lastPlaced = ReadDate(a + 0x50);
        stats.heroicChallenges = Le32(a + 0x56);
        stats.heroPoints = Le16(a + 0x5A);
        stats.lastReset = ReadDate(a + 0x60);

        // Giants' part: counted only when the first area says so (bit 0 of 0x16, set by the title on first use).
        uint8_t extra[ExtraSize];
        if (NewestExtra(plain, extra) >= 0)
        {
            stats.extraSequence = extra[2];
            if (a[0x16] & 1)
                stats.giantsExperience = Le16(extra + 3);
            if (extra[5] != 0)
                stats.hat = extra[5];
            stats.swapForceExperience = Le32(extra + 8);
            stats.wowPow = extra[6] & 1;
            stats.villain = Le16(extra + TrapVillainOffset);
        }
        return AreaState::Valid;
    }

    AreaState ReadExtraState(const FigureData& tag)
    {
        FigureData plain = Decrypt(tag);
        uint8_t extra[ExtraSize];
        if (NewestExtra(plain, extra) >= 0)
            return AreaState::Valid;
        uint8_t a[ExtraSize], b[ExtraSize];
        PackExtra(plain, ExtraBlocks[0], a);
        PackExtra(plain, ExtraBlocks[1], b);
        return ExtraBlank(a) && ExtraBlank(b) ? AreaState::Blank : AreaState::Damaged;
    }

    // A date the title could have written: zero (never) or a real calendar field set.
    static Date ClampDate(Date date)
    {
        date.minute = std::min<uint8_t>(date.minute, 59);
        date.hour = std::min<uint8_t>(date.hour, 23);
        date.day = std::min<uint8_t>(date.day, 31);
        date.month = std::min<uint8_t>(date.month, 12);
        return date;
    }

    void WriteStats(FigureData& tag, const FigureStats& unchecked)
    {
        // Whatever the caller passes, the figure never gets more than the games allow (experience of level 10 in the
        // first area, Giants' 63500 and Swap Force's 101000 in the extra one, gold).
        FigureStats stats = unchecked;
        stats.experience = std::min(stats.experience, MaxBaseExperience);
        stats.giantsExperience = std::min(stats.giantsExperience, MaxGiantsExperience);
        stats.swapForceExperience = std::min(stats.swapForceExperience, MaxSwapForceExperience);
        stats.gold = std::min(stats.gold, MaxGold);
        stats.nickname.resize(std::min<size_t>(stats.nickname.size(), 16));
        stats.lastPlaced = ClampDate(stats.lastPlaced);
        stats.lastReset = ClampDate(stats.lastReset);

        // Start from the area the title would read, so that bytes this editor does not know about are kept.
        FigureData plain = Decrypt(tag);
        int newest = NewestArea(plain);
        uint8_t area[AreaSpan] = {};
        if (newest >= 0)
            memcpy(area, plain.data() + AreaBlocks[newest] * FigureBlockSize, AreaSpan);

        // The extra areas (Giants): written when the figure needs them or has them, so that a figure Spyro's Adventure
        // made, and every edit that stays within its limits, keeps the bytes that game expects.
        uint8_t extra[ExtraSize] = {};
        int newestExtra = NewestExtra(plain, extra);
        uint8_t extraA[ExtraSize], extraB[ExtraSize];
        PackExtra(plain, ExtraBlocks[0], extraA);
        PackExtra(plain, ExtraBlocks[1], extraB);
        // A hat only Giants shows (not the first area's own) needs the extra area too.
        bool writeExtra = newestExtra >= 0 || !ExtraBlank(extraA) || !ExtraBlank(extraB) ||
                          stats.giantsExperience != 0 || (stats.hat != 0 && stats.hat != stats.ssaHat) ||
                          stats.swapForceExperience != 0 || stats.wowPow != 0 || stats.villain != 0;

        uint8_t* a = area;
        a[0] = uint8_t(stats.experience);
        a[1] = uint8_t(stats.experience >> 8);
        a[2] = uint8_t(stats.experience >> 16);
        Put16(a + 0x03, stats.gold);
        Put32(a + 0x05, stats.playSeconds);
        Put16(a + 0x10, stats.upgrades);
        a[0x13] = stats.platforms;
        Put16(a + 0x14, stats.ssaHat);
        memcpy(a + 0x18, stats.owner.data(), stats.owner.size());
        memset(a + 0x20, 0, 0x10);
        memset(a + 0x40, 0, 0x10);
        for (size_t i = 0; i < stats.nickname.size() && i < 16; i++)
            Put16(a + (i < 8 ? 0x20 + i * 2 : 0x40 + (i - 8) * 2), stats.nickname[i]);
        WriteDate(a + 0x50, stats.lastPlaced);
        Put32(a + 0x56, stats.heroicChallenges);
        Put16(a + 0x5A, stats.heroPoints);
        WriteDate(a + 0x60, stats.lastReset);
        if (writeExtra)
            a[0x16] |= 1; // the title adds the extra area's experience only then

        // Area A gets sequence + 1 and area B sequence + 2, so B is the newer copy and both hold the same data.
        for (int i = 0; i < 2; i++)
        {
            uint8_t* target = plain.data() + AreaBlocks[i] * FigureBlockSize;
            for (size_t offset = 0; offset < AreaSpan; offset += FigureBlockSize)
            {
                if (!IsTrailer(AreaBlocks[i] + offset / FigureBlockSize))
                    memcpy(target + offset, a + offset, FigureBlockSize);
            }
            target[0x09] = uint8_t(stats.sequence + 1 + i);
            SealArea(target);
        }

        if (writeExtra)
        {
            Put16(extra + 3, stats.giantsExperience);
            // The extra area's hat changes only when Giants would show another one (0 there means the first area's).
            if (stats.hat != (extra[5] != 0 ? extra[5] : stats.ssaHat))
                extra[5] = uint8_t(std::min<uint16_t>(stats.hat, 0xFF));
            Put32(extra + 8, stats.swapForceExperience);
            extra[6] = uint8_t((extra[6] & 0xFE) | (stats.wowPow & 1));
            Put16(extra + TrapVillainOffset, stats.villain);
            uint8_t sequence = newestExtra >= 0 ? extra[2] : stats.extraSequence;
            for (int i = 0; i < 2; i++)
            {
                extra[2] = uint8_t(sequence + 1 + i);
                SealExtra(extra);
                UnpackExtra(plain, ExtraBlocks[i], extra);
            }
        }
        tag = Encrypt(plain);
    }

    std::string NicknameUtf8(const FigureStats& stats)
    {
        std::string text;
        for (char16_t unit : stats.nickname)
        {
            uint32_t c = unit;
            if (c < 0x80)
                text += char(c);
            else if (c < 0x800)
            {
                text += char(0xC0 | (c >> 6));
                text += char(0x80 | (c & 0x3F));
            }
            else
            {
                text += char(0xE0 | (c >> 12));
                text += char(0x80 | ((c >> 6) & 0x3F));
                text += char(0x80 | (c & 0x3F));
            }
        }
        return text;
    }

    void SetNicknameUtf8(FigureStats& stats, const std::string& text)
    {
        // Characters of the Basic Multilingual Plane only (the title's fonts have no others).
        stats.nickname.clear();
        for (size_t i = 0; i < text.size() && stats.nickname.size() < 16;)
        {
            uint8_t lead = uint8_t(text[i]);
            uint32_t c;
            size_t length;
            if (lead < 0x80)
            {
                c = lead;
                length = 1;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                c = lead & 0x1F;
                length = 2;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                c = lead & 0x0F;
                length = 3;
            }
            else
            {
                c = '?';
                length = (lead & 0xF8) == 0xF0 ? 4 : 1;
            }
            if (i + length > text.size())
                break;
            for (size_t k = 1; k < length && length < 4; k++)
                c = (c << 6) | (uint8_t(text[i + k]) & 0x3F);
            stats.nickname += char16_t(c);
            i += length;
        }
    }
}
