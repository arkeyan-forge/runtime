#include "figure_tag.h"
#include "figure_crypto.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace figures
{
    namespace
    {
        constexpr char KeyConstant[] = " Copyright (C) 2010 Activision. All Rights Reserved. ";
        constexpr size_t AreaBlocks[2] = { 0x08, 0x24 };
        constexpr size_t AreaSpan = 9 * FigureBlockSize; // 7 data blocks and the 2 trailers between them
        constexpr size_t TrapSpan = 27 * FigureBlockSize; // 21 data blocks, six intervening trailers
        using TrapArea = std::array<uint8_t, 21 * FigureBlockSize>;
        // Giants' second pair: 4 data blocks and the trailer between them (blocks 17, 18, 20, 21 and 45, 46, 48, 49),
        // kept as 64 bytes. Byte 0 is the checksum (LE16), byte 2 the sequence number.
        constexpr size_t ExtraBlocks[2] = { 0x11, 0x2D };
        constexpr size_t ExtraSpan = 5 * FigureBlockSize;
        constexpr size_t ExtraSize = 4 * FigureBlockSize;
        constexpr uint8_t ExtraMarker[2] = { 0x06,
            0x01 }; // stands for the checksum while it is computed (05 00 in the first areas)
        constexpr uint8_t AccessReadOnly[4] = { 0x0F, 0x0F, 0x0F, 0x69 };
        constexpr uint8_t AccessReadWrite[4] = { 0x7F, 0x0F, 0x08, 0x69 };
        constexpr uint8_t Access2016[4] = { 0xFF, 0x07, 0x80, 0x69 };
        constexpr uint8_t SignatureKey2016 = 0x51;

        bool IsTrailer(size_t block) { return block % 4 == 3; }
        bool IsTrap(const FigureData& tag) { return ToyType(tag) >= 210 && ToyType(tag) <= 220; }
        bool IsCrystal(const FigureData& tag) { return IsCrystalType(ToyType(tag)); }
        // Blocks 34 and 62 of an Imaginators toy: the second half of its factory signature, stored as it is (never
        // encrypted): a tag our tools make keeps them zero, a real one keeps its own.
        bool IsSignatureBlock(const FigureData& tag, size_t block)
        {
            return (block == 34 || block == 62) && ToyType(tag) >= 600 && ToyType(tag) <= 699;
        }
        // Traps and crystals: one pair of 21 data blocks instead of the character areas.
        bool IsPairLayout(const FigureData& tag) { return IsTrap(tag) || IsCrystal(tag); }
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

        TrapArea PackTrap(const FigureData& plain, int copy)
        {
            TrapArea packed{};
            for (size_t i = 0, n = 0; i < TrapSpan / FigureBlockSize; i++)
            {
                size_t block = AreaBlocks[copy] + i;
                if (!IsTrailer(block))
                    memcpy(
                        packed.data() + n++ * FigureBlockSize, plain.data() + block * FigureBlockSize, FigureBlockSize);
            }
            return packed;
        }

        void UnpackTrap(FigureData& plain, int copy, const TrapArea& packed)
        {
            for (size_t i = 0, n = 0; i < TrapSpan / FigureBlockSize; i++)
            {
                size_t block = AreaBlocks[copy] + i;
                if (!IsTrailer(block))
                    memcpy(
                        plain.data() + block * FigureBlockSize, packed.data() + n++ * FigureBlockSize, FigureBlockSize);
            }
        }

        bool TrapValid(const TrapArea& a)
        {
            return Le16(a.data() + 0x0E) == HeaderChecksum(a.data()) &&
                   Le16(a.data() + 0x0C) == FigureChecksum(a.data() + 0x10, 3 * FigureBlockSize) &&
                   Le16(a.data() + 0x0A) == FigureChecksum(a.data() + 0x40, 17 * FigureBlockSize);
        }

        void SealTrap(TrapArea& a)
        {
            Put16(a.data() + 0x0A, FigureChecksum(a.data() + 0x40, 17 * FigureBlockSize));
            Put16(a.data() + 0x0C, FigureChecksum(a.data() + 0x10, 3 * FigureBlockSize));
            Put16(a.data() + 0x0E, HeaderChecksum(a.data()));
        }

        // A crystal's copy: the trap's checksums, 0x0A over 16 blocks (the 21st is the factory signature's).
        bool CrystalValid(const TrapArea& a)
        {
            return Le16(a.data() + 0x0E) == HeaderChecksum(a.data()) &&
                   Le16(a.data() + 0x0C) == FigureChecksum(a.data() + 0x10, 3 * FigureBlockSize) &&
                   Le16(a.data() + 0x0A) == FigureChecksum(a.data() + 0x40, 16 * FigureBlockSize);
        }

        void SealCrystal(TrapArea& a)
        {
            Put16(a.data() + 0x0A, FigureChecksum(a.data() + 0x40, 16 * FigureBlockSize));
            Put16(a.data() + 0x0C, FigureChecksum(a.data() + 0x10, 3 * FigureBlockSize));
            Put16(a.data() + 0x0E, HeaderChecksum(a.data()));
        }

        // The newer of two copies (0, 1), -1 if neither is valid: adjacent sequences modulo 256.
        int NewestOf(bool validA, bool validB, const TrapArea& a, const TrapArea& b)
        {
            return validA && validB ? (uint8_t(a[9] + 1) == b[9] ? 1 : 0) : validA ? 0 : validB ? 1 : -1;
        }

        int NewestTrap(const TrapArea& a, const TrapArea& b) { return NewestOf(TrapValid(a), TrapValid(b), a, b); }

        int NewestCrystal(const TrapArea& a, const TrapArea& b)
        {
            return NewestOf(CrystalValid(a), CrystalValid(b), a, b);
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
            if (!IsTrailer(block) && !IsSignatureBlock(tag, block) && !IsZero(data, FigureBlockSize))
                BlockCipher(tag, block).Decrypt(data, data);
        }
        return plain;
    }

    FigureData Encrypt(const FigureData& plain)
    {
        // Every block of an area in use is encrypted, zero or not (the title does the same); other zero blocks are
        // left zero, as on a new figure.
        bool trap = IsPairLayout(plain);
        bool areaUsed[2], extraUsed[2];
        for (int i = 0; i < 2; i++)
        {
            TrapArea packed = trap ? PackTrap(plain, i) : TrapArea{};
            // A crystal's 21st block is its signature, there on a blank one too: the area is in use by the 20 others.
            size_t used = IsCrystal(plain) ? 20 * FigureBlockSize : packed.size();
            areaUsed[i] =
                trap ? !IsZero(packed.data(), used) : !AreaBlank(plain.data() + AreaBlocks[i] * FigureBlockSize);
            uint8_t extra[ExtraSize];
            PackExtra(plain, ExtraBlocks[i], extra);
            extraUsed[i] = !trap && !ExtraBlank(extra);
        }

        FigureData tag = plain;
        for (size_t block = 8; block < FigureBlockCount; block++)
        {
            if (IsTrailer(block) || IsSignatureBlock(plain, block))
                continue;
            uint8_t* data = tag.data() + block * FigureBlockSize;
            bool inUsedArea = false;
            for (int i = 0; i < 2; i++)
            {
                inUsedArea |= areaUsed[i] && block >= AreaBlocks[i] &&
                              block < AreaBlocks[i] + (trap ? TrapSpan : AreaSpan) / FigureBlockSize;
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
        // A toy of 2016 (Imaginators: variant 0x5000 and up, or a toy type of 600-699 whatever its variant) has its own
        // sector-0 trailer, and its last byte names the public key the game checks the factory signature with: without
        // 0x51 the game does not even check it and calls the toy corrupted [V: every retail dump of the owner's; the runs
        // of 2026-11-05: a crystal the launcher made with variant 0 was refused, the same with this trailer opens the
        // creator].
        if (Variant(tag) >= 0x5000 || NeedsFactorySignature(ToyType(tag)))
        {
            uint8_t* trailer = tag.data() + 3 * FigureBlockSize;
            memcpy(trailer + 6, Access2016, 4);
            trailer[15] = SignatureKey2016;
        }
    }

    FigureData CreateFigure(uint16_t toyType, uint16_t variant)
    {
        FigureData tag = CreateBlankFigure(toyType, variant);
        WriteSectorKeys(tag);
        return tag;
    }

    FigureData CreateTrap(uint16_t toyType, uint16_t variant, uint16_t villain, bool evolved)
    {
        if (toyType < 210 || toyType > 220 || villain > 46 || (villain == 0 && evolved))
            throw std::invalid_argument("invalid trap family or villain");
        FigureData tag = CreateFigure(toyType, variant);
        if (villain != 0)
        {
            TrapStats stats;
            stats.villainCount = 1;
            stats.selectedVillain = stats.villains[0].id = uint8_t(villain);
            stats.villains[0].questComplete = evolved;
            WriteTrapStats(tag, stats);
        }
        return tag;
    }

    AreaState ReadTrapStats(const FigureData& tag, TrapStats& stats)
    {
        stats = {};
        if (!IsTrap(tag))
            return AreaState::Damaged;
        FigureData plain = Decrypt(tag);
        TrapArea a = PackTrap(plain, 0), b = PackTrap(plain, 1);
        int newest = NewestTrap(a, b);
        if (newest < 0)
            return IsZero(a.data(), a.size()) && IsZero(b.data(), b.size()) ? AreaState::Blank : AreaState::Damaged;
        const TrapArea& area = newest == 0 ? a : b;
        stats.dataFormat = area[0];
        stats.villainCount = area[1];
        stats.preloadVillain = area[7];
        stats.sequence = area[9];
        stats.selectedVillain = area[TrapVillainOffset];
        for (size_t i = 0; i < stats.villains.size(); i++)
        {
            const uint8_t* record = area.data() + TrapVillainOffset + TrapVillainRecordSize * i;
            TrapVillain& villain = stats.villains[i];
            villain.id = record[0];
            villain.questComplete = (record[1] & 1) != 0;
            villain.hat = record[2];
            villain.bauble = record[3];
            for (size_t unit = 0; unit < villain.name.size(); unit++)
                villain.name[unit] = char16_t(Le16(record + 4 + unit * 2));
        }
        return AreaState::Valid;
    }

    bool SelectTrapVillain(TrapStats& stats, uint8_t id)
    {
        if (id == 0 || id > 46)
            return false;
        size_t at = stats.villains.size();
        size_t empty = at;
        for (size_t i = 0; i < stats.villains.size(); i++)
        {
            if (stats.villains[i].id == id)
                at = i;
            else if (stats.villains[i].id == 0 && empty == stats.villains.size())
                empty = i;
        }
        if (at == stats.villains.size())
        {
            if (empty != stats.villains.size())
            {
                std::swap(stats.villains[0], stats.villains[empty]);
                stats.villainCount++;
            }
            stats.villains[0] = {};
            stats.villains[0].id = id;
        }
        else
            std::swap(stats.villains[0], stats.villains[at]);
        stats.selectedVillain = id;
        return true;
    }

    bool WriteTrapStats(FigureData& tag, const TrapStats& stats)
    {
        if (!IsTrap(tag) || stats.selectedVillain != stats.villains[0].id)
            return false;
        std::array<bool, 256> seen{};
        size_t count = 0;
        for (const TrapVillain& villain : stats.villains)
        {
            if (villain.id == 0)
                continue;
            if (seen[villain.id])
                return false;
            seen[villain.id] = true;
            count++;
        }
        if (count != stats.villainCount || (count != 0 && stats.selectedVillain == 0))
            return false;

        FigureData plain = Decrypt(tag);
        TrapArea a = PackTrap(plain, 0), b = PackTrap(plain, 1);
        int newest = NewestTrap(a, b);
        if (newest < 0 && (!IsZero(a.data(), a.size()) || !IsZero(b.data(), b.size())))
            return false;
        TrapArea area = newest == 0 ? a : newest == 1 ? b : TrapArea{};
        uint8_t sequence = newest < 0 ? 0 : area[9];
        area[0] = stats.dataFormat;
        area[1] = stats.villainCount;
        area[7] = stats.preloadVillain;
        for (size_t i = 0; i < stats.villains.size(); i++)
        {
            uint8_t* record = area.data() + TrapVillainOffset + TrapVillainRecordSize * i;
            const TrapVillain& villain = stats.villains[i];
            record[0] = villain.id;
            record[1] = uint8_t((record[1] & 0xFE) | uint8_t(villain.questComplete));
            record[2] = villain.hat;
            record[3] = villain.bauble;
            for (size_t unit = 0; unit < villain.name.size(); unit++)
                Put16(record + 4 + unit * 2, villain.name[unit]);
        }
        // Prepare the inactive copy first. This produces an offline image, not physical portal write scheduling.
        int first = newest < 0 ? 0 : 1 - newest;
        for (int i = 0; i < 2; i++)
        {
            area[9] = uint8_t(sequence + 1 + i);
            SealTrap(area);
            UnpackTrap(plain, i == 0 ? first : 1 - first, area);
        }
        tag = Encrypt(plain);
        return true;
    }

    AreaState ReadStats(const FigureData& tag, FigureStats& stats)
    {
        if (IsCrystal(tag))
        {
            // The fields a character shares with a crystal, so lists, search and edits treat it as one.
            CrystalStats crystal;
            AreaState state = ReadCrystalStats(tag, crystal);
            stats = {};
            stats.experience = crystal.experience;
            stats.gold = crystal.gold;
            stats.playSeconds = crystal.playSeconds;
            stats.sequence = crystal.sequence;
            for (char c : crystal.name)
                stats.nickname += char16_t(uint8_t(c));
            stats.lastPlaced = crystal.lastPlaced;
            stats.lastReset = crystal.created;
            return state;
        }
        if (IsTrap(tag))
        {
            TrapStats trap;
            AreaState state = ReadTrapStats(tag, trap);
            stats = {};
            stats.villain = trap.selectedVillain;
            stats.villainEvolved = trap.villains[0].questComplete;
            stats.sequence = trap.sequence;
            return state;
        }
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
        }
        return AreaState::Valid;
    }

    AreaState ReadExtraState(const FigureData& tag)
    {
        if (IsPairLayout(tag))
            return AreaState::Blank; // traps have no character extra pair
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
        if (IsCrystal(tag))
        {
            // The shared fields (ReadStats) back into the crystal; its creation and battle class stay.
            CrystalStats crystal;
            if (ReadCrystalStats(tag, crystal) != AreaState::Valid)
                return;
            crystal.experience = std::min(unchecked.experience, MaxCrystalExperience);
            crystal.gold = std::min(unchecked.gold, MaxGold);
            crystal.playSeconds = unchecked.playSeconds;
            crystal.name.clear();
            for (char16_t unit : unchecked.nickname)
                crystal.name += unit < 0x100 ? char(unit) : '?';
            crystal.lastPlaced = ClampDate(unchecked.lastPlaced);
            crystal.created = ClampDate(unchecked.lastReset);
            WriteCrystalStats(tag, crystal);
            return;
        }
        if (IsTrap(tag))
            return; // character fields overlap trap records; use WriteTrapStats
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
                          stats.swapForceExperience != 0 || stats.wowPow != 0;

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
        // The nickname's bytes only when it changes: past its end SuperChargers keeps a vehicle's own data (0x4E) [V].
        std::u16string stored;
        for (size_t i = 0; i < 16; i++)
        {
            char16_t unit = char16_t(Le16(a + (i < 8 ? 0x20 + i * 2 : 0x40 + (i - 8) * 2)));
            if (unit == 0)
                break;
            stored += unit;
        }
        if (stored != stats.nickname)
        {
            memset(a + 0x20, 0, 0x10);
            memset(a + 0x40, 0, 0x10);
            for (size_t i = 0; i < stats.nickname.size() && i < 16; i++)
                Put16(a + (i < 8 ? 0x20 + i * 2 : 0x40 + (i - 8) * 2), stats.nickname[i]);
        }
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

    const char* BattleClassName(int battleClass)
    {
        static const char* const names[] = { "Knight", "Bowslinger", "Quickshot", "Ninja", "Brawler", "Smasher",
            "Sorcerer", "Swashbuckler", "Sentinel", "Bazooker" };
        return battleClass >= 1 && battleClass <= 10 ? names[battleClass - 1] : "";
    }

    const std::array<CrystalParam, CrystalParamCount>& CrystalParams()
    {
        // The disc's LooseData/GlobalCYOSParams.igz (CCYOSRecipeFormat and its settings lists), in its order: each
        // parameter's name, range and width (a slider's 4 bits and a colour's 7 are the settings' own; the others the
        // bits of their count of values).
        static const std::array<CrystalParam, CrystalParamCount> params = { {
            { "weaponR", CrystalParamKind::Gear, 30000, 30999, 10 },
            { "weaponL", CrystalParamKind::Gear, 30000, 30999, 10 },
            { "Back Attachment (Wings / Backpack)", CrystalParamKind::Gear, 13000, 13999, 10 },
            { "Hat", CrystalParamKind::Gear, 14000, 14999, 10 },
            { "armorLeg", CrystalParamKind::Gear, 11000, 11999, 10 },
            { "armorArm", CrystalParamKind::Gear, 10000, 10999, 10 },
            { "armorShoulder", CrystalParamKind::Gear, 12000, 12999, 10 },
            { "Ear", CrystalParamKind::Gear, 16000, 16999, 10 },
            { "parametricLowerBodyMass", CrystalParamKind::Shape, 0, 8, 4 },
            { "parametricUpperBodyMass", CrystalParamKind::Shape, 0, 8, 4 },
            { "parametricHeight", CrystalParamKind::Shape, 0, 8, 4 },
            { "muscleScale", CrystalParamKind::Shape, 0, 8, 4 },
            { "head scale", CrystalParamKind::Shape, 0, 8, 4 },
            { "scaleTailWidth", CrystalParamKind::Shape, 0, 8, 4 },
            { "head", CrystalParamKind::Body, 0, 999, 10 },
            { "torso", CrystalParamKind::Body, 1000, 1999, 10 },
            { "arms", CrystalParamKind::Body, 2000, 2999, 10 },
            { "legs", CrystalParamKind::Body, 3000, 3999, 10 },
            { "tail", CrystalParamKind::Body, 15000, 15099, 7 },
            { "Head Skin Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Head Skin Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Head Aux 1", CrystalParamKind::Colour, 0, 127, 7 },
            { "Head Aux 2", CrystalParamKind::Colour, 0, 127, 7 },
            { "Head Aux 3", CrystalParamKind::Colour, 0, 127, 7 },
            { "Ear Skin", CrystalParamKind::Colour, 0, 127, 7 },
            { "Arms Skin Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Arms Skin Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Arms Aux 1", CrystalParamKind::Colour, 0, 127, 7 },
            { "Arms Aux 2", CrystalParamKind::Colour, 0, 127, 7 },
            { "Arms Aux 3", CrystalParamKind::Colour, 0, 127, 7 },
            { "Torso Skin Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Torso Skin Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Torso Aux 1", CrystalParamKind::Colour, 0, 127, 7 },
            { "Torso Aux 2", CrystalParamKind::Colour, 0, 127, 7 },
            { "Torso Aux 3", CrystalParamKind::Colour, 0, 127, 7 },
            { "Legs Skin Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Legs Skin Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Legs Aux 1", CrystalParamKind::Colour, 0, 127, 7 },
            { "Legs Aux 2", CrystalParamKind::Colour, 0, 127, 7 },
            { "Legs Aux 3", CrystalParamKind::Colour, 0, 127, 7 },
            { "Eye Pupil", CrystalParamKind::Colour, 0, 127, 7 },
            { "Eye Sclera", CrystalParamKind::Colour, 0, 127, 7 },
            { "Tail Skin Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Tail Skin Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Ears Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Ears Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Ears Tertiary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Helmet Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Helmet Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Helmet Tertiary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Arm Guards Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Arm Guards Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Arm Guards Tertiary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Shoulder Guards Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Shoulder Guards Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Shoulder Guards Tertiary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Backpack Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Backpack Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Backpack Tertiary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Leg Guards Primary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Leg Guards Secondary", CrystalParamKind::Colour, 0, 127, 7 },
            { "Leg Guards Tertiary", CrystalParamKind::Colour, 0, 127, 7 },
            { "SecondaryPower", CrystalParamKind::Choice, 20000, 20999, 10 },
            { "TertiaryPower", CrystalParamKind::Choice, 21000, 21999, 10 },
            { "Class", CrystalParamKind::Choice, 0, 63, 7 },
            { "Aura", CrystalParamKind::Choice, 33134, 33166, 6 },
            { "soundTheme", CrystalParamKind::Choice, 32600, 32799, 8 },
            { "eyeMaterial", CrystalParamKind::Choice, 33167, 33199, 6 },
            { "catchphrase1", CrystalParamKind::Choice, 32000, 32199, 8 },
            { "catchphrase2", CrystalParamKind::Choice, 32200, 32399, 8 },
            { "musicTheme", CrystalParamKind::Choice, 32400, 32599, 8 },
            { "voice", CrystalParamKind::Choice, 32800, 32999, 8 },
            { "voiceFx", CrystalParamKind::Choice, 33000, 33099, 7 },
            { "PrimaryPower", CrystalParamKind::Choice, 22000, 22499, 9 },
        } };
        return params;
    }

    int CrystalParamIndex(const char* name)
    {
        const auto& params = CrystalParams();
        for (size_t i = 0; i < params.size(); i++)
            if (strcmp(params[i].name, name) == 0)
                return int(i);
        return -1;
    }

    bool NeedsFactorySignature(uint16_t toyType) { return toyType >= 600 && toyType <= 699; }

    bool HasFactorySignature(const FigureData& tag)
    {
        return NeedsFactorySignature(ToyType(tag)) && !IsZero(tag.data() + 2 * FigureBlockSize, FigureBlockSize) &&
               !IsZero(tag.data() + 4 * FigureBlockSize, FigureBlockSize);
    }

    bool MendVirtualToy(FigureData& tag)
    {
        if (!NeedsFactorySignature(ToyType(tag)) || HasFactorySignature(tag))
            return false;
        FigureData mended = tag;
        WriteSectorKeys(mended);
        if (mended == tag)
            return false;
        tag = mended;
        return true;
    }

    uint32_t CrystalLevelExperience(int level)
    {
        if (level <= 1)
            return 0;
        if (level <= MaxLevelSwapForce)
            return LevelExperience[level - 1];
        return 220000 + 20000 * uint32_t(std::min(level, MaxCrystalLevel) - 21);
    }

    int CrystalLevel(uint32_t experience)
    {
        int level = 1;
        while (level < MaxCrystalLevel && experience >= CrystalLevelExperience(level + 1))
            level++;
        return level;
    }

    namespace
    {
        constexpr size_t CrystalRecipeBytes = 69; // 547 bits

        void ReadRecipe(const TrapArea& area, CrystalStats& stats)
        {
            stats.hasRecipe = Le32(&area[CrystalRecipeFormatOffset]) == CrystalRecipeFormat;
            if (!stats.hasRecipe)
                return;
            size_t bit = 0;
            const auto& params = CrystalParams();
            for (size_t i = 0; i < params.size(); i++)
            {
                uint32_t value = 0;
                for (uint8_t k = 0; k < params[i].bits; k++, bit++)
                    value |= uint32_t((area[CrystalRecipeOffset + bit / 8] >> (bit % 8)) & 1) << k;
                stats.creation[i] = uint16_t(value);
            }
            stats.battleClass = uint8_t(stats.creation[size_t(CrystalParamIndex("Class"))]);
        }

        void WriteRecipe(TrapArea& area, const CrystalStats& stats)
        {
            Put32(&area[CrystalRecipeFormatOffset], CrystalRecipeFormat);
            CrystalStats values = stats;
            if (stats.battleClass >= 1 && stats.battleClass <= 10)
                values.creation[size_t(CrystalParamIndex("Class"))] = stats.battleClass;
            memset(&area[CrystalRecipeOffset], 0, CrystalRecipeBytes);
            size_t bit = 0;
            for (size_t i = 0; i < CrystalParams().size(); i++)
            {
                uint32_t value = std::min<uint32_t>(values.creation[i], (1u << CrystalParams()[i].bits) - 1);
                for (uint8_t k = 0; k < CrystalParams()[i].bits; k++, bit++)
                    area[CrystalRecipeOffset + bit / 8] |= uint8_t(((value >> k) & 1) << (bit % 8));
            }
        }

        void WriteCrystalFields(TrapArea& area, const CrystalStats& stats)
        {
            uint32_t experience = std::min(stats.experience, MaxCrystalExperience);
            area[0] = uint8_t(experience);
            area[1] = uint8_t(experience >> 8);
            area[2] = uint8_t(experience >> 16);
            Put16(&area[0x03], std::min(stats.gold, MaxGold));
            Put32(&area[0x05], stats.playSeconds);
            memset(&area[0x20], 0, CrystalNameSize);
            for (size_t i = 0, n = 0; i < stats.name.size() && n < CrystalNameSize - 1; i++)
            {
                if (stats.name[i] >= 0x20 && stats.name[i] < 0x7F)
                    area[0x20 + n++] = uint8_t(stats.name[i]);
            }
            WriteDate(&area[0x40], ClampDate(stats.lastPlaced));
            WriteDate(&area[0x50], ClampDate(stats.created));
            if (stats.hasRecipe)
                WriteRecipe(area, stats);
        }
    }

    AreaState ReadCrystalStats(const FigureData& tag, CrystalStats& stats)
    {
        stats = {};
        if (!IsCrystal(tag))
            return AreaState::Damaged;
        FigureData plain = Decrypt(tag);
        TrapArea a = PackTrap(plain, 0), b = PackTrap(plain, 1);
        int newest = NewestCrystal(a, b);
        if (newest < 0)
        {
            // The signature block (the 21st) is there on every crystal, made or not.
            bool blank = IsZero(a.data(), 20 * FigureBlockSize) && IsZero(b.data(), 20 * FigureBlockSize);
            return blank ? AreaState::Blank : AreaState::Damaged;
        }
        const TrapArea& area = newest == 0 ? a : b;
        stats.experience = area[0] | area[1] << 8 | area[2] << 16;
        stats.gold = Le16(&area[0x03]);
        stats.playSeconds = Le32(&area[0x05]);
        stats.sequence = area[0x09];
        for (size_t i = 0; i < CrystalNameSize && area[0x20 + i] != 0; i++)
            stats.name += char(area[0x20 + i]);
        stats.lastPlaced = ReadDate(&area[0x40]);
        stats.created = ReadDate(&area[0x50]);
        ReadRecipe(area, stats);
        return AreaState::Valid;
    }

    bool WriteCrystalStats(FigureData& tag, const CrystalStats& stats)
    {
        if (!IsCrystal(tag))
            return false;
        FigureData plain = Decrypt(tag);
        TrapArea a = PackTrap(plain, 0), b = PackTrap(plain, 1);
        int newest = NewestCrystal(a, b);
        if (newest < 0)
            return false;
        TrapArea area = newest == 0 ? a : b;
        uint8_t sequence = area[0x09];
        CrystalStats read;
        ReadRecipe(area, read);
        CrystalStats edited = stats;
        if (!stats.hasRecipe && read.hasRecipe)
        {
            // Only the class changes: the creation stays as stored.
            edited.creation = read.creation;
            edited.hasRecipe = true;
        }
        WriteCrystalFields(area, edited);
        // The inactive copy first, as the trap writer; the signature block is each copy's own and stays.
        int first = 1 - newest;
        for (int i = 0; i < 2; i++)
        {
            int copy = i == 0 ? first : newest;
            TrapArea out = area;
            memcpy(
                out.data() + 20 * FigureBlockSize, (copy == 0 ? a : b).data() + 20 * FigureBlockSize, FigureBlockSize);
            out[0x09] = uint8_t(sequence + 1 + i);
            SealCrystal(out);
            UnpackTrap(plain, copy, out);
        }
        tag = Encrypt(plain);
        return true;
    }

    FigureData CreateCrystal(uint16_t toyType, uint16_t variant, const CrystalStats& stats)
    {
        if (!IsCrystalType(toyType))
            throw std::invalid_argument("not a creation crystal");
        FigureData tag = CreateFigure(toyType, variant);
        FigureData plain = Decrypt(tag);
        TrapArea area{};
        area[0x70] = 0x30; // every made crystal of the dumps [?]
        CrystalStats made = stats;
        made.hasRecipe = true;
        WriteCrystalFields(area, made);
        for (int copy = 0; copy < 2; copy++)
        {
            area[0x09] = uint8_t(stats.sequence + 1 + copy);
            SealCrystal(area);
            UnpackTrap(plain, copy, area);
        }
        return Encrypt(plain);
    }

    std::string NicknameUtf8(const FigureStats& stats)
    {
        std::string text;
        for (char16_t unit : stats.nickname)
        {
            uint32_t c = unit;
            if (c == 0)
                break;
            if (c >= 0xD800 && c <= 0xDFFF) // half of a pair: no valid UTF-8 for it alone
                c = '?';
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
        // Characters of the Basic Multilingual Plane only (the title's fonts have no others): a character past it is
        // kept as '?'. Bytes that are not well-formed UTF-8 (a stray byte, an overlong form, a lone surrogate) and
        // control characters are dropped.
        stats.nickname.clear();
        for (size_t i = 0; i < text.size() && stats.nickname.size() < 16;)
        {
            uint8_t lead = uint8_t(text[i]);
            uint32_t c;
            size_t length;
            uint32_t least;
            if (lead < 0x80)
            {
                c = lead;
                length = 1;
                least = 0;
            }
            else if (lead >= 0xC2 && lead <= 0xDF)
            {
                c = lead & 0x1F;
                length = 2;
                least = 0x80;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                c = lead & 0x0F;
                length = 3;
                least = 0x800;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                c = lead & 0x07;
                length = 4;
                least = 0x10000;
            }
            else
            {
                i++; // a continuation byte alone, or a lead that never starts a character
                continue;
            }
            bool good = i + length <= text.size();
            for (size_t k = 1; good && k < length; k++)
            {
                good = (uint8_t(text[i + k]) & 0xC0) == 0x80;
                c = (c << 6) | (uint8_t(text[i + k]) & 0x3F);
            }
            if (!good || c < least || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            {
                i++;
                continue;
            }
            i += length;
            if (c < 0x20 || (c >= 0x7F && c <= 0x9F))
                continue;
            stats.nickname += c > 0xFFFF ? u'?' : char16_t(c);
        }
    }
}
