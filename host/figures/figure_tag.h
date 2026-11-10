#pragma once

// Reading and editing what Spyro's Adventure stores on a figure. Layout (docs/runtime/host/figures.md):
// - sector 0 (blocks 0-3) is plain: serial, toy type, variant, header CRC; blocks 4-7 are plain (Swap Force writes block 6,
//   kept as it is here);
// - blocks 8-63 except the sector trailers are AES-128 encrypted, each with its own key:
//   MD5(block 0 | block 1 | block number | " Copyright (C) 2010 Activision. All Rights Reserved. "); zero blocks stay zero;
// - the progress is stored twice, in area A (blocks 8-16) and area B (blocks 36-44), 7 data blocks each; the title
//   writes the area it did not read last, with the sequence number + 1, so a write cut short keeps the other one;
// - Giants and later add a second pair, the extra areas (blocks 17-21 and 45-49, 4 data blocks each, same A/B scheme):
//   Giants' part of the experience (levels 11-15) and its hat. Spyro's Adventure never touches them.
// - traps (toy types 210-220) instead use one pair of 21 data blocks; see TrapStats below.
#include "figure_data.h"
#include <array>
#include <bit>
#include <cstdint>
#include <string>

namespace figures
{
    // Plain (decrypted) image of a tag; sector 0 and trailers are copied as they are.
    FigureData Decrypt(const FigureData& tag);
    FigureData Encrypt(const FigureData& plain);

    uint16_t ToyType(const FigureData& tag);
    uint16_t Variant(const FigureData& tag);

    // Puts the MIFARE key A of every sector into its trailer, as tag dumps store it.
    void WriteSectorKeys(FigureData& tag);

    // A never-played figure of the given toy type, as a complete dump (keys included).
    FigureData CreateFigure(uint16_t toyType, uint16_t variant = 0);

    struct Date
    {
        uint8_t minute = 0, hour = 0, day = 0, month = 0;
        uint16_t year = 0;
    };

    // Progress fields of one area. Offsets are from the area's first block, trailers included.
    struct FigureStats
    {
        uint32_t experience = 0;       // 0x00, 24 bits: Spyro's Adventure's part, up to 33000 (level 10)
        uint32_t giantsExperience =
            0; // extra area +0x03, 16 bits: Giants' part, up to 63500 (levels 11-15); 0 without an extra area
        uint16_t gold = 0;             // 0x03
        uint32_t playSeconds = 0;      // 0x05
        uint8_t sequence = 0;          // 0x09, which area is newer
        uint16_t upgrades = 0;         // 0x10, bit field of bought upgrades and the chosen path
        uint8_t platforms = 0;         // 0x13, consoles the figure was used on (2 = Xbox 360)
        uint16_t ssaHat = 0; // 0x14, the first area's hat: Spyro's Adventure's own (it has hats too); kept as it is
        // unless a caller changes it
        uint16_t hat = 0; // the hat Giants shows: extra area +0x05 if set, else ssaHat. Changing it writes the
        // extra area only, so Spyro's Adventure keeps its own hat
        uint8_t extraSequence = 0; // the extra area's sequence number (0 without one)
        uint32_t swapForceExperience =
            0; // extra area +0x08 (LE32): Swap Force's part (levels 16-20, up to 101000) [V];
        // never counted by Spyro's Adventure or Giants
        uint16_t villain = 0; // read-only compatibility view of TrapStats::selectedVillain; never a character field
        bool villainEvolved = false; // read-only compatibility view of the selected villain's evolved flag (traps only)
        uint8_t wowPow = 0; // Giants' Wow Pow bought (Series 2 figures), 0 or 1: extra area +0x06 bit 0, where the
        // game writes it when Persephone sells it [V] (figures.md, "Upgrades")
        std::array<uint8_t, 8> owner{}; // 0x18, the first owner's id, written by the title
        std::u16string nickname; // 0x20 and 0x40, 16 UTF-16 code units, zero padded
        Date lastPlaced; // 0x50
        uint32_t heroicChallenges = 0; // 0x56, one bit per challenge
        uint16_t heroPoints = 0; // 0x5A
        Date lastReset; // 0x60
    };

    enum class AreaState
    {
        Blank,
        Valid,
        Damaged
    };

    // What the extra areas of the figure are: Blank (none written: every figure Spyro's Adventure made), Valid, Damaged.
    AreaState ReadExtraState(const FigureData& tag);

    // Reads the newest area whose checksums are right. Blank: never written (the title will initialise it).
    // Damaged: data present but no area passes its checksums.
    AreaState ReadStats(const FigureData& tag, FigureStats& stats);

    // Writes `stats` into both areas (sequence numbers stats.sequence + 1 and + 2) with fresh checksums. The extra
    // areas are written only when the figure needs them (Giants' experience or a hat is set) or already has them, so
    // a figure Spyro's Adventure made stays what that game expects. Experience is clamped to 33000 + 63500 + 101000.
    // Trap tags are left unchanged; use WriteTrapStats for their distinct layout.
    void WriteStats(FigureData& tag, const FigureStats& stats);

    // Experience needed for each level (index 0 = level 1). Spyro's Adventure caps characters at level 10, Giants at 15, Swap Force at 20.
    constexpr int MaxLevel = 10; // Spyro's Adventure
    constexpr int MaxLevelGiants = 15; // Giants
    constexpr int MaxLevelSwapForce = 20; // Swap Force
    constexpr uint32_t MaxBaseExperience = 33000; // the first area's part (level 10)
    constexpr uint32_t MaxGiantsExperience = 63500; // the extra area's part (level 15 = 96500 in all)
    constexpr uint32_t MaxSwapForceExperience = 101000; // the extra area's second part (level 20 = 197500 in all)
    extern const uint32_t LevelExperience[MaxLevelSwapForce];
    int LevelForExperience(uint32_t experience, int maxLevel = MaxLevel);
    // The level cap of a game by its catalogue id ("ssa": 10, "giants": 15, "swapforce": 20) or its folder name. Trap Team
    // ("trapteam") and the later games ("superchargers", "imaginators"): 20 [?], the public figure; the
    // 20-level table is Swap Force's own [V]. An unknown id gets 20 too (the top of the table, never Giants' 15, which
    // would hide the levels of a newer game).
    int LevelCap(const std::string& game);

    // Trap Team's packed progress layout [V: local static research; docs/runtime/host/figures.md].
    struct TrapVillain
    {
        uint8_t id = 0; // storage ID, 0 empty; ordinary villains use 1-46 (not resource IDs 1001-1046)
        bool questComplete = false; // evolved, bit 0 of the record's second byte
        uint8_t hat = 0, bauble = 0;
        std::array<char16_t, 16> name{}; // sixteen UTF-16LE units, including any zero padding
    };

    // Packed offset of the first villain record, whose first byte is the selected villain: physical 0x090 in copy A,
    // 0x250 in B [V: a capture in play, 2026-11-02: Sheep Creep (20) written there, count 1, sequence 2].
    constexpr size_t TrapVillainOffset = 0x10;
    constexpr size_t TrapVillainRecordSize = 48;

    struct TrapStats
    {
        uint8_t dataFormat = 0; // candidate initial value [?]; preserve a read value when editing
        uint8_t villainCount = 0;
        uint8_t preloadVillain = 0; // special look, separate from evolution
        uint8_t selectedVillain = 0; // aliases villains[0].id; selecting another villain requires swapping records
        std::array<TrapVillain, 6> villains{}; // packed TrapVillainOffset + 48*i (TrapVillainRecordSize)
        uint8_t sequence = 0;
    };

    // A villain's catalogue (resource) ID, 1001-1046, minus this is the storage ID a trap holds (Gulper: 1009 and 9) [V].
    constexpr uint16_t VillainResourceBase = 1000;

    AreaState ReadTrapStats(const FigureData& tag, TrapStats& stats);
    // Offline edit: preserve identity, trailers and reserved bits/bytes from the newest valid copy. Seal both full
    // copies, starting with the inactive one, with adjacent sequences from the tag (modulo 256).
    // False leaves the tag unchanged: wrong family, damaged pair, count/membership mismatch, or duplicate IDs.
    bool WriteTrapStats(FigureData& tag, const TrapStats& stats);

    // Makes `id` (1-46) the trap's selected villain, as the game does: its record moves to the first slot (an
    // unknown villain gets a free slot, or replaces the selected one when all six are used) and the displaced record
    // moves to the slot it left. Records of other villains are kept. False for id 0 or above 46.
    bool SelectTrapVillain(TrapStats& stats, uint8_t id);

    // True: a crafted trap (Gulper, normal and evolved, in an offscreen tutorial) was accepted by the game, which played
    // the villain, gave the Skylander back on the second LT, and showed the evolved look [V: docs/runtime/host/figures.md].
    constexpr bool TrapVillainVerified = true;
    // Complete trap dump (0: blank); a populated trap uses the trap codec, including its quest/evolved flag.
    // Invalid toy family or storage ID (>46), or evolved with no villain, throws std::invalid_argument.
    FigureData CreateTrap(uint16_t toyType, uint16_t variant = 0, uint16_t villain = 0, bool evolved = false);

    // SuperChargers' vehicles (toy types 3200-3299) keep the character layout [V: runs of 2026-11-04, docs/runtime/host/
    // figures.md "SuperChargers' vehicles"]. On first use the game writes the first pair (owner, dates, platform, 0x14 = 1,
    // 0x16 bit 0) and the extra pair with +0x03 = 9 and +0x06 = 9 [?], then adds the gearbits the vehicle collects to the
    // extra area's +0x08 (where FigureStats::swapForceExperience sits for a character). 0x4E of the first area is the
    // vehicle's own byte [?]; its mods are not mapped [?]. WriteStats keeps all of them. The upgrade field (0x10) holds the
    // garage's upgrades [V: 0x0023 written by our tools shows shield 2 and weapon 1 in the pause menu, run scL2]: bits 0-4 the
    // shield's five, bits 5-9 the weapon's five.
    inline bool IsVehicleType(uint16_t toyType) { return toyType >= 3200 && toyType <= 3299; }
    constexpr int VehicleUpgradeSteps = 5;
    inline int VehicleShieldLevel(const FigureStats& stats) { return std::popcount(unsigned(stats.upgrades & 0x1F)); }
    inline int VehicleWeaponLevel(const FigureStats& stats)
    {
        return std::popcount(unsigned(stats.upgrades >> 5 & 0x1F));
    }
    constexpr uint32_t MaxGearbits = 65535; // the field's room as the game was seen to use it [?]
    inline uint32_t Gearbits(const FigureStats& stats) { return stats.swapForceExperience; }
    inline void SetGearbits(FigureStats& stats, uint32_t gearbits)
    {
        stats.swapForceExperience = gearbits < MaxGearbits ? gearbits : MaxGearbits;
    }

    // Imaginators' creation crystals (toy types 680-689, one per element; the variant's low byte is the crystal's shape)
    // [V: the owner's dumps, docs/runtime/host/figures.md "Imaginators' crystals"]. The trap's pair of 21 data blocks
    // (A from block 8, B from 36, packed without trailers), the trap's three checksums except that 0x0A covers 16 blocks
    // (0x40-0x13F): the last packed block (physical 34 and 62) is part of the factory signature with blocks 2 and 4,
    // never written. Packed offsets:
    //   0x00 experience (24 bits), 0x03 gold, 0x05 play time (s), 0x09 sequence, 0x20-0x3F name (8-bit, zero ended),
    //   0x40 last placed, 0x50 created (dates as the first area's), 0x60-0x6F and 0x70 = 0x30 [?, kept],
    //   0x78 the recipe format's hash (LE32 0x458D186A), 0x7C the creation: CrystalParams() in order, each the value
    //   minus its minimum on `bits` bits, least significant bit first, 547 bits [V: the disc's GlobalCYOSParams.igz and
    //   every made crystal of the dumps].
    constexpr uint16_t FirstCrystalType = 680, LastCrystalType = 689;
    inline bool IsCrystalType(uint16_t toyType) { return toyType >= FirstCrystalType && toyType <= LastCrystalType; }
    constexpr size_t CrystalNameSize = 32;
    constexpr uint32_t MaxCrystalExperience = 0xFFFFFF;
    constexpr size_t CrystalRecipeFormatOffset = 0x78, CrystalRecipeOffset = 0x7C;
    constexpr uint32_t CrystalRecipeFormat = 0x458D186A; // CCYOSRecipeFormat's hash on the disc

    enum class CrystalParamKind : uint8_t
    {
        Gear, // an attachment (weapons, backpack, hat, armour, ears): a part id
        Shape, // a slider 0-8 (height, muscles, head size...)
        Body, // a body part (head, torso, arms, legs, tail): a part id
        Colour, // a palette index 0-127
        Choice // class, powers, aura, eyes, voice, sounds, music, catch phrases
    };
    struct CrystalParam
    {
        const char* name; // the game's own parameter name
        CrystalParamKind kind;
        int32_t min, max; // the game's ids: stored is id - min; all ones is "none" (a crystal with no tail)
        uint8_t bits;
    };
    // The 74 parameters of a creation in the order the tag stores them (the disc's recipe format) [V].
    constexpr size_t CrystalParamCount = 74;
    const std::array<CrystalParam, CrystalParamCount>& CrystalParams();
    // Index of a parameter by its name ("Class", "head", "Head Skin Primary"), -1 if none.
    int CrystalParamIndex(const char* name);

    struct CrystalStats
    {
        uint32_t experience = 0;
        uint16_t gold = 0;
        uint32_t playSeconds = 0;
        uint8_t sequence = 0;
        std::string name; // as stored: 8-bit characters, at most 31
        Date lastPlaced, created;
        uint8_t battleClass = 0; // 1-10 (BattleClassName), the creation's "Class"; 0: not made yet
        bool hasRecipe = false; // the recipe format's hash is there: `creation` holds the stored values
        std::array<uint16_t, CrystalParamCount> creation{}; // stored values (id - min), CrystalParams() order
    };

    // "Knight", "Bowslinger", "Quickshot", "Ninja", "Brawler", "Smasher", "Sorcerer", "Swashbuckler", "Sentinel",
    // "Bazooker" for 1-10 [V: 55 made crystals of the dumps, named by class]; empty otherwise.
    const char* BattleClassName(int battleClass);

    // Imaginators' toys (types 600-699: Senseis, crystals) carry a factory signature in blocks 2, 4, 34 and 62 that the game
    // checks: a crystal without one is "maybe corrupted, repair it" in the game [V: run of 2026-11-04]. True when the tag
    // is such a toy and blocks 2 and 4 are not zero; such a toy made by our tools (CreateFigure) has none.
    bool NeedsFactorySignature(uint16_t toyType);
    bool HasFactorySignature(const FigureData& tag);
    // Such a toy made here (no factory signature) with the trailers of an older build (before 2026-11-05, or a crystal
    // made with variant 0) gets this build's sector keys: without the 2016 sector-0 trailer the game refuses it. False
    // when nothing changed (a signed toy, an older game's toy, a tag already right).
    bool MendVirtualToy(FigureData& tag);

    // A creation's level: the game's own table (every class's character data on the disc, an igIntIntHashTable):
    // levels 1-20 are the older games' 20, then 20000 more each, up to 80 at 1400000 [V]. Without the Senseis' training
    // a creation stops at 20 [?].
    constexpr int MaxCrystalLevel = 80;
    uint32_t CrystalLevelExperience(int level); // experience to reach `level` (1: 0)
    int CrystalLevel(uint32_t experience);

    // Blank: a crystal nobody made a character on yet (the game's creator fills it).
    AreaState ReadCrystalStats(const FigureData& tag, CrystalStats& stats);
    // Writes the known fields into both copies (inactive first, adjacent sequences), keeping every other byte of the
    // newest valid copy and the signature blocks; with `hasRecipe` the creation too. False leaves the tag unchanged:
    // not a crystal, damaged pair, or a blank crystal (CreateCrystal makes the first creation). The name keeps printable
    // ASCII only.
    bool WriteCrystalStats(FigureData& tag, const CrystalStats& stats);
    // A crystal with a creation, as the game's creator leaves one (both copies, the recipe format, the creation). The tag
    // has no factory signature: the real game refuses it unless the port skips that check [?].
    FigureData CreateCrystal(uint16_t toyType, uint16_t variant, const CrystalStats& stats);

    // All the experience a figure has (the three parts), and setting it (the first 33000 go to the first area, the next
    // 63500 to Giants' part, the rest to Swap Force's).
    uint32_t TotalExperience(const FigureStats& stats);
    void SetTotalExperience(FigureStats& stats, uint32_t total);
    // The experience a game with level cap `cap` counts (10: Spyro's Adventure reads the first area only), and setting
    // it as that game would: with cap 10 only the first area's part changes, with cap 15 the first two (Swap Force's stays).
    uint32_t ExperienceFor(const FigureStats& stats, int cap);
    void SetExperienceFor(FigureStats& stats, int cap, uint32_t experience);

    constexpr uint16_t MaxGold = 65000;

    std::string NicknameUtf8(const FigureStats& stats);
    void SetNicknameUtf8(
        FigureStats& stats, const std::string& text); // 16 UTF-16 units at most; malformed bytes and controls dropped
}
