// Unit test of the figure library: crypto test vectors, tag encryption and progress round trips, the catalog, and the
// cross-game round trips (a figure written by Spyro's Adventure, then Giants, then Spyro's Adventure again). No game data
// is needed; optional arguments add checks against the player's own files (the portraits are SSAArtTest's):
//   SSAFigureTest [<figure dump>...]          each dump must decrypt to a valid or blank tag
//   SSAFigureTest --make <dump> <type>[:<variant hex>] [xp=<total>] [gold=] [hat=] [ssahat=] [sfxp=] [upgrades=<hex>] [wowpow=]
//                                              writes a figure for a test run (docs/runtime/host/figures.md)
//   SSAFigureTest --edit <dump> - [the same fields]
//                                              changes those fields of a dump (a game's write), keeping the rest
//   SSAFigureTest --same-extra <before> <after>
//                                              a game wrote the first pair in between, and the extra areas stayed the
//                                              same (byte for byte, decrypted) and valid, with the same levels and hat
//   SSAFigureTest --roundtrip <before> <after>
//                                              a game ran with the figure: what it wrote and what it must not have touched
//                                              (every block outside the first pair, byte for byte; both pairs' checksums)
// Exit code 0 = pass.
#include "host/collection/catalogue.h"
#include "host/figures/figure_catalog.h"
#include "host/figures/figure_crypto.h"
#include "host/figures/figure_tag.h"
#include "host/portal_backend.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace
{
    int g_checks;
    int g_failures;

    void Check(bool condition, const char* what, int line)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            fprintf(stderr, "FAIL line %d: %s\n", line, what);
        }
    }
#define CHECK(condition) Check((condition), #condition, __LINE__)

    std::string Hex(const uint8_t* data, size_t size)
    {
        std::string text;
        char byte[3];
        for (size_t i = 0; i < size; i++)
        {
            snprintf(byte, sizeof(byte), "%02x", data[i]);
            text += byte;
        }
        return text;
    }

    std::string Md5Hex(const char* text)
    {
        figures::Md5Digest digest = figures::Md5(reinterpret_cast<const uint8_t*>(text), strlen(text));
        return Hex(digest.data(), digest.size());
    }

    void TestCrypto()
    {
        // RFC 1321, appendix A.5.
        CHECK(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
        CHECK(Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
        CHECK(Md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
        CHECK(Md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a");

        // FIPS-197, appendix C.1.
        uint8_t key[16], plain[16], cipher[16], back[16];
        for (int i = 0; i < 16; i++)
        {
            key[i] = uint8_t(i);
            plain[i] = uint8_t(i * 0x11);
        }
        figures::Aes128 aes(key);
        aes.Encrypt(plain, cipher);
        CHECK(Hex(cipher, 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");
        aes.Decrypt(cipher, back);
        CHECK(memcmp(back, plain, 16) == 0);

        // Sector 0's key A is the same on every figure.
        uint8_t serial[4] = { 0x12, 0x34, 0x56, 0x78 };
        auto key0 = figures::SectorKeyA(serial, 0);
        CHECK(Hex(key0.data(), 6) == "4b0b20107ccb");
        CHECK(figures::SectorKeyA(serial, 1) != figures::SectorKeyA(serial, 2));
    }

    void TestTag()
    {
        FigureData tag = figures::CreateFigure(14);
        CHECK(figures::ToyType(tag) == 14 && figures::Variant(tag) == 0);
        CHECK((tag[0x1E] | tag[0x1F] << 8) == FigureChecksum(tag.data(), 0x1E));
        CHECK(Hex(&tag[3 * 16], 6) == "4b0b20107ccb" && tag[3 * 16 + 9] == 0x69);
        auto key5 = figures::SectorKeyA(tag.data(), 5);
        CHECK(memcmp(&tag[23 * 16], key5.data(), 6) == 0 && tag[23 * 16 + 6] == 0x7F);

        // A new figure is blank and stays all zero past sector 0 when encrypted.
        figures::FigureStats stats;
        CHECK(figures::ReadStats(tag, stats) == figures::AreaState::Blank);
        CHECK(figures::Encrypt(figures::Decrypt(tag)) == tag);

        stats.experience = 33000;
        stats.gold = 12345;
        stats.playSeconds = 3661;
        stats.upgrades = 0x0105;
        stats.platforms = 2;
        stats.hat = 7;
        stats.heroicChallenges = 0x80000001;
        stats.heroPoints = 42;
        stats.lastPlaced = { 30, 12, 23, 10, 2026 };
        figures::SetNicknameUtf8(stats, "Gillou é ∑ très long nom");
        CHECK(stats.nickname.size() == 16);
        figures::WriteStats(tag, stats);

        // Encrypted on the tag: the gold value does not appear in clear, sector 0 is untouched.
        CHECK(tag[0x83] != (12345 & 0xFF) || tag[0x84] != (12345 >> 8));
        CHECK(figures::ToyType(tag) == 14);

        figures::FigureStats read;
        CHECK(figures::ReadStats(tag, read) == figures::AreaState::Valid);
        CHECK(read.experience == 33000 && read.gold == 12345 && read.playSeconds == 3661 && read.upgrades == 0x0105);
        CHECK(read.platforms == 2 && read.hat == 7 && read.heroicChallenges == 0x80000001 && read.heroPoints == 42);
        CHECK(read.lastPlaced.year == 2026 && read.lastPlaced.month == 10 && read.lastPlaced.minute == 30);
        CHECK(figures::NicknameUtf8(read) == "Gillou é ∑ très ");
        CHECK(read.sequence == 2); // area B, the newer copy

        // Both areas hold the same data with consecutive sequence numbers, so either reading rule finds it.
        FigureData plain = figures::Decrypt(tag);
        CHECK(memcmp(&plain[0x80], &plain[0x240], 9) == 0 && plain[0x89] == 1 && plain[0x249] == 2);
        CHECK(memcmp(&plain[0x90], &plain[0x250], 0x20) == 0 && memcmp(&plain[0xC0], &plain[0x280], 0x30) == 0);
        // Encryption is a bijection of the data blocks.
        CHECK(figures::Encrypt(plain) == tag);

        // A byte changed in area A breaks its second checksum: area B is read. Changed in both: damaged.
        plain[0x100] = 0x5A;
        tag = figures::Encrypt(plain);
        CHECK(figures::ReadStats(tag, read) == figures::AreaState::Valid && read.sequence == 2);
        plain[0x2C0] = 0x5A;
        CHECK(figures::ReadStats(figures::Encrypt(plain), read) == figures::AreaState::Damaged);

        // Rewriting keeps bytes this editor does not know (block 44 of area B, fixed up by hand) and moves on to the
        // next sequence numbers.
        uint8_t check[0x110] = {};
        memcpy(check, &plain[0x290], 0x20);
        memcpy(check + 0x20, &plain[0x2C0], 0x10);
        uint16_t crc = FigureChecksum(check, sizeof(check));
        plain[0x24A] = uint8_t(crc);
        plain[0x24B] = uint8_t(crc >> 8);
        uint8_t header[16]; // the header checksum covers 0x24A..0x24B too
        memcpy(header, &plain[0x240], 16);
        header[14] = 5;
        header[15] = 0;
        crc = FigureChecksum(header, 16);
        plain[0x24E] = uint8_t(crc);
        plain[0x24F] = uint8_t(crc >> 8);
        tag = figures::Encrypt(plain);
        CHECK(figures::ReadStats(tag, read) == figures::AreaState::Valid && read.sequence == 2 && read.gold == 12345);
        read.gold = 500;
        figures::WriteStats(tag, read);
        plain = figures::Decrypt(tag);
        CHECK(plain[0x100] == 0x5A && plain[0x2C0] == 0x5A && plain[0x89] == 3 && plain[0x249] == 4);
        CHECK(figures::ReadStats(tag, read) == figures::AreaState::Valid && read.gold == 500 && read.experience == 33000);

        // The game's limits hold whatever is passed: experience at level 10's, gold at 65000, real date fields.
        read.experience = 0xFFFFFF;
        read.gold = 65535;
        read.lastPlaced = { 99, 99, 99, 99, 2026 };
        figures::WriteStats(tag, read);
        CHECK(figures::ReadStats(tag, read) == figures::AreaState::Valid);
        CHECK(read.experience == 33000 && figures::LevelForExperience(read.experience) == 10 && read.gold == 65000);
        CHECK(read.lastPlaced.minute == 59 && read.lastPlaced.hour == 23 && read.lastPlaced.day == 31 && read.lastPlaced.month == 12);
        read.gold = 500;
        read.experience = 33000;

        // Sequence numbers wrap around: 0xFF then 0x00 is "B newer".
        read.sequence = 0xFE;
        figures::WriteStats(tag, read);
        plain = figures::Decrypt(tag);
        CHECK(plain[0x89] == 0xFF && plain[0x249] == 0x00);
        CHECK(figures::ReadStats(tag, read) == figures::AreaState::Valid && read.sequence == 0x00);

        // Levels.
        CHECK(figures::LevelForExperience(0) == 1 && figures::LevelForExperience(999) == 1 && figures::LevelForExperience(1000) == 2);
        CHECK(figures::LevelForExperience(32999) == 9 && figures::LevelForExperience(33000) == 10 && figures::LevelForExperience(99999) == 10);
    }


    // Raw blocks of the extra areas (17-21 and 45-49), as stored.
    bool ExtraBlocksZero(const FigureData& tag)
    {
        for (size_t block : { 17, 18, 20, 21, 45, 46, 48, 49 })
        {
            for (size_t i = 0; i < FigureBlockSize; i++)
            {
                if (tag[block * FigureBlockSize + i] != 0)
                    return false;
            }
        }
        return true;
    }

    // Giants: the second pair of areas (experience of levels 11-15, the hat), the level cap and the figure kinds.
    void TestGiants()
    {
        // Spyro's Adventure's own figures never get an extra area, however they are edited within its limits.
        FigureData ssa = figures::CreateFigure(14);
        figures::FigureStats stats;
        stats.experience = 33000;
        stats.gold = 65000;
        figures::SetNicknameUtf8(stats, "Gill");
        figures::WriteStats(ssa, stats);
        CHECK(ExtraBlocksZero(ssa) && figures::ReadExtraState(ssa) == figures::AreaState::Blank);
        figures::FigureStats read;
        CHECK(figures::ReadStats(ssa, read) == figures::AreaState::Valid && read.giantsExperience == 0 && read.hat == 0);
        CHECK(figures::LevelForExperience(figures::TotalExperience(read)) == 10);
        FigureData plain = figures::Decrypt(ssa);
        CHECK((plain[0x8 * 16 + 0x16] & 1) == 0 && (plain[0x24 * 16 + 0x16] & 1) == 0);

        // Spyro's Adventure stays byte for byte what it was: 1200 writes within its limits (levels up to 10, no hat) of
        // figures with fixed serials hash to the value the library gave before Giants was added [V: computed with it].
        {
            uint64_t hash = 1469598103934665603ull;
            uint32_t seed = 12345;
            auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
            for (int i = 0; i < 400; i++)
            {
                FigureData tag = CreateBlankFigure(uint16_t(next() % 33), 0);
                tag[0] = uint8_t(i); tag[1] = uint8_t(i >> 8); tag[2] = 0x5A; tag[3] = 0xC3; tag[4] = tag[0] ^ tag[1] ^ tag[2] ^ tag[3];
                uint16_t crc = FigureChecksum(tag.data(), 0x1E);
                tag[0x1E] = uint8_t(crc);
                tag[0x1F] = uint8_t(crc >> 8);
                figures::WriteSectorKeys(tag);
                for (int pass = 0; pass < 3; pass++)
                {
                    figures::FigureStats s;
                    figures::ReadStats(tag, s);
                    s.experience = next() % 33001;
                    s.gold = uint16_t(next() % 65001);
                    s.playSeconds = next();
                    s.upgrades = uint16_t(next());
                    s.platforms = 2;
                    s.heroPoints = uint16_t(next() % 100);
                    s.heroicChallenges = next();
                    s.lastPlaced = { 1, 2, 3, 4, 2026 };
                    figures::SetNicknameUtf8(s, "Gi" + std::to_string(i));
                    figures::WriteStats(tag, s);
                    for (uint8_t byte : tag)
                    {
                        hash ^= byte;
                        hash *= 1099511628211ull;
                    }
                }
            }
            CHECK(hash == 0x9d07d07f4ee1bd11ull);
        }

        // Level 15 and a hat: the first 33000 stay in the first area, the other 63500 go to the extra one.
        FigureData giant = figures::CreateFigure(100);
        figures::FigureStats high;
        figures::SetTotalExperience(high, 96500);
        CHECK(high.experience == 33000 && high.giantsExperience == 63500);
        figures::SetTotalExperience(high, 40000);
        CHECK(high.experience == 33000 && high.giantsExperience == 7000);
        figures::SetTotalExperience(high, 999999);
        CHECK(figures::TotalExperience(high) == 197500 && high.swapForceExperience == 101000); // Swap Force's level 20
        figures::SetTotalExperience(high, 96500);
        CHECK(high.experience == 33000 && high.giantsExperience == 63500 && high.swapForceExperience == 0);
        high.hat = 5;
        high.gold = 40000;
        figures::WriteStats(giant, high);
        CHECK(figures::ReadExtraState(giant) == figures::AreaState::Valid);
        figures::FigureStats back;
        CHECK(figures::ReadStats(giant, back) == figures::AreaState::Valid);
        CHECK(back.experience == 33000 && back.giantsExperience == 63500 && back.hat == 5 && back.gold == 40000);
        CHECK(figures::LevelForExperience(figures::TotalExperience(back), figures::MaxLevelGiants) == 15);
        CHECK(figures::LevelForExperience(figures::TotalExperience(back)) == 10); // Spyro's Adventure's cap
        CHECK(back.extraSequence == 2 && back.sequence == 2);
        plain = figures::Decrypt(giant);
        CHECK((plain[0x8 * 16 + 0x16] & 1) == 1 && (plain[0x24 * 16 + 0x16] & 1) == 1);
        // Both extra copies hold the same data with consecutive sequence numbers; the checksum has the title's value
        // for the same payload [V: the title accepted this figure and played it at level 15 with the hat].
        CHECK(plain[17 * 16 + 2] == 1 && plain[45 * 16 + 2] == 2);
        CHECK(memcmp(&plain[17 * 16 + 3], &plain[45 * 16 + 3], 3) == 0 && plain[17 * 16 + 5] == 5);
        CHECK(plain[17 * 16] == 0xD7 && plain[17 * 16 + 1] == 0x6E && plain[45 * 16] == 0xB3 && plain[45 * 16 + 1] == 0xE2);
        CHECK(figures::Encrypt(plain) == giant);

        // Writing again keeps the extra area's other bytes and moves its sequence on.
        back.gold = 100;
        figures::WriteStats(giant, back);
        figures::FigureStats again;
        CHECK(figures::ReadStats(giant, again) == figures::AreaState::Valid && again.giantsExperience == 63500 && again.hat == 5);
        CHECK(again.extraSequence == 4 && again.sequence == 4);

        // The title's own blank extra area (what it writes on first use) is valid, with its checksum.
        FigureData titleBlank = figures::CreateFigure(101);
        FigureData tp = figures::Decrypt(titleBlank);
        tp[17 * 16 + 2] = 2;
        tp[17 * 16] = 0x30;
        tp[17 * 16 + 1] = 0xD7;
        tp[45 * 16 + 2] = 1;
        tp[45 * 16] = 0x54;
        tp[45 * 16 + 1] = 0x5B;
        CHECK(figures::ReadExtraState(figures::Encrypt(tp)) == figures::AreaState::Valid);

        // A damaged extra area counts as none: no Giants experience, the first area still reads, and a write repairs it.
        FigureData bad = giant;
        FigureData bp = figures::Decrypt(bad);
        bp[17 * 16] ^= 0xFF;
        bp[45 * 16] ^= 0xFF;
        bad = figures::Encrypt(bp);
        CHECK(figures::ReadExtraState(bad) == figures::AreaState::Damaged);
        figures::FigureStats damaged;
        CHECK(figures::ReadStats(bad, damaged) == figures::AreaState::Valid && damaged.giantsExperience == 0 && damaged.hat == 0); // Giants' hat was in the extra area only: the first area's is 0
        figures::WriteStats(bad, back);
        CHECK(figures::ReadExtraState(bad) == figures::AreaState::Valid);

        // The cap per game, and the figure kinds: a LightCore is the same toy type with its own variant.
        CHECK(figures::LevelCap("ssa") == 10 && figures::LevelCap("giants") == 15 && figures::LevelCap("swapforce") == 20);
        CHECK(figures::LevelExperience[14] == 96500 && figures::LevelForExperience(96499, 15) == 14);
        // Swap Force's table (every hero's character data on its disc [V]): level 20 = 197500 = 96500 + 101000.
        CHECK(figures::LevelExperience[15] == 113700 && figures::LevelExperience[16] == 132400 && figures::LevelExperience[17] == 152600 &&
              figures::LevelExperience[18] == 174300 && figures::LevelExperience[19] == 197500);
        CHECK(figures::LevelForExperience(197499, 20) == 19 && figures::LevelForExperience(197500, 20) == 20 &&
              figures::LevelForExperience(999999, 20) == 20 && figures::LevelForExperience(999999, 15) == 15 && figures::LevelForExperience(113700, 20) == 16);
        FigureData lightcore = figures::CreateFigure(100, 0x1206);
        CHECK(figures::ToyType(lightcore) == 100 && figures::Variant(lightcore) == 0x1206);
        figures::WriteStats(lightcore, high);
        CHECK(figures::ToyType(lightcore) == 100 && figures::Variant(lightcore) == 0x1206 && figures::ReadStats(lightcore, back) == figures::AreaState::Valid);
        // The header checksum of sector 0 stays valid after any of it.
        CHECK((lightcore[0x1E] | lightcore[0x1F] << 8) == FigureChecksum(lightcore.data(), 0x1E));

        // Giants' catalogue: SSA's toys plus its own; Giants figures, LightCore's base toy, items and sidekicks.
        CHECK(figures::FindToy(100) == nullptr && figures::FindToy(100, "giants") != nullptr);
        CHECK(figures::FindToy(14, "giants") != nullptr && figures::FindToy(404, "giants") != nullptr);
        for (uint16_t type : { 100, 101, 109, 115, 208, 209, 505, 514, 519, 526, 300, 303 })
            CHECK(figures::FindToy(type, "giants") != nullptr);
        CHECK(figures::FindToy(200, "giants") != nullptr && figures::FindToy(300, "giants") != nullptr);
        CHECK(figures::Catalog("giants").size() > figures::Catalog().size());
        CHECK(figures::FindToy(101, "giants")->kind == figures::ToyKind::Character && figures::FindToy(208, "giants")->kind == figures::ToyKind::MagicItem);
        CHECK(figures::FindToy(505, "giants")->kind == figures::ToyKind::Sidekick);
    }

    void TestCatalog()
    {
        std::set<uint16_t> types;
        int characters = 0;
        for (const figures::ToyInfo& toy : figures::Catalog())
        {
            CHECK(types.insert(toy.toyType).second);
            if (toy.kind == figures::ToyKind::Character)
            {
                characters++;
                CHECK(toy.portrait != nullptr && toy.element != figures::Element::None);
            }
        }
        CHECK(characters == 33);
        for (uint16_t type = 0; type <= 32; type++)
            CHECK(figures::FindToy(type) != nullptr);
        CHECK(figures::FindToy(33) == nullptr && figures::FindToy(404) != nullptr);
        CHECK(figures::PortraitImageName(*figures::FindToy(14)) == "toy_gillgrunt_hud");
        CHECK(figures::PortraitImageName(*figures::FindToy(514)) == "toy_gillgrunt_hud");
        CHECK(figures::PortraitImageName(*figures::FindToy(200)).empty());
        CHECK(figures::StarterFileName(*figures::FindToy(14)) == "014 Gill Grunt.sky");
    }

    // --- Cross-game round trips [Lux] [Alchemy] -------------------------------------------------------------------

    constexpr size_t ExtraData[2][4] = { { 17, 18, 20, 21 }, { 45, 46, 48, 49 } };

    // The decrypted extra areas (both copies), for byte-for-byte comparisons.
    std::vector<uint8_t> ExtraBytes(const FigureData& tag)
    {
        FigureData plain = figures::Decrypt(tag);
        std::vector<uint8_t> out;
        for (const auto& copy : ExtraData)
            for (size_t block : copy)
                out.insert(out.end(), plain.begin() + ptrdiff_t(block * FigureBlockSize), plain.begin() + ptrdiff_t((block + 1) * FigureBlockSize));
        return out;
    }

    // What Spyro's Adventure itself does when it saves a figure [V: its own writes, figures.md]: it takes the newest valid
    // area of the first pair, changes its fields, and writes it into the other area with the next sequence number and
    // fresh checksums. Block 1, the trailers and the extra areas are never written. Written here from the documented
    // rules, without the library's area code, so the two check each other. It keeps the bytes it does not know (0x16).
    void SsaTitleWrite(FigureData& tag, uint32_t experience, uint16_t gold)
    {
        FigureData plain = figures::Decrypt(tag);
        auto area = [&](int i) { return plain.data() + (i == 0 ? 0x08 : 0x24) * FigureBlockSize; };
        figures::FigureStats stats;
        CHECK(figures::ReadStats(tag, stats) == figures::AreaState::Valid);
        int newest = area(1)[0x09] == stats.sequence ? 1 : 0, target = 1 - newest;
        for (size_t block = 0; block < 9; block++) // the 7 data blocks; both areas start a sector, trailers are 3 and 7
            if (block % 4 != 3)
                memcpy(area(target) + block * FigureBlockSize, area(newest) + block * FigureBlockSize, FigureBlockSize);
        uint8_t* a = area(target);
        experience = std::min(experience, figures::MaxBaseExperience);
        a[0] = uint8_t(experience), a[1] = uint8_t(experience >> 8), a[2] = uint8_t(experience >> 16);
        a[3] = uint8_t(gold), a[4] = uint8_t(gold >> 8);
        a[0x09] = uint8_t(stats.sequence + 1);
        uint8_t second[0x110] = {};
        memcpy(second, a + 0x50, 0x20);
        memcpy(second + 0x20, a + 0x80, 0x10);
        uint16_t crc = FigureChecksum(second, sizeof(second));
        a[0x0A] = uint8_t(crc), a[0x0B] = uint8_t(crc >> 8);
        uint8_t first[0x30];
        memcpy(first, a + 0x10, 0x20);
        memcpy(first + 0x20, a + 0x40, 0x10);
        crc = FigureChecksum(first, sizeof(first));
        a[0x0C] = uint8_t(crc), a[0x0D] = uint8_t(crc >> 8);
        uint8_t header[16];
        memcpy(header, a, 16);
        header[0x0E] = 0x05, header[0x0F] = 0x00;
        crc = FigureChecksum(header, sizeof(header));
        a[0x0E] = uint8_t(crc), a[0x0F] = uint8_t(crc >> 8);
        tag = figures::Encrypt(plain);
    }

    // Create, Spyro's Adventure, Giants (level 15, a hat), Spyro's Adventure again: SSA's data intact, every checksum
    // valid, the figure accepted by both games and still level 15 for Giants (docs/runtime/host/figures.md, "Data rules").
    void TestRoundTrips()
    {
        const collection::Catalogue& catalogue = collection::Catalogue::Get();
        FigureData tag = figures::CreateFigure(14); // Gill Grunt
        CHECK(catalogue.Accepts("ssa", 14) && catalogue.Accepts("giants", 14));

        // 1. Spyro's Adventure plays it: the library writes as the title does, then the title's own write.
        figures::FigureStats stats;
        stats.experience = 24800; // level 9
        stats.gold = 1000;
        figures::WriteStats(tag, stats);
        SsaTitleWrite(tag, 30000, 1200);
        CHECK(figures::ReadStats(tag, stats) == figures::AreaState::Valid && stats.experience == 30000 && stats.gold == 1200);
        CHECK(figures::ReadExtraState(tag) == figures::AreaState::Blank);
        uint8_t ssaSequence = stats.sequence;

        // 2. Giants plays it: level 15 and a hat, in the extra areas; the first pair keeps everything Giants does not change.
        figures::SetExperienceFor(stats, figures::LevelCap("giants"), figures::LevelExperience[14]);
        stats.hat = 23;
        stats.gold = 2000;
        figures::WriteStats(tag, stats);
        CHECK(figures::ReadExtraState(tag) == figures::AreaState::Valid);
        CHECK(figures::ReadStats(tag, stats) == figures::AreaState::Valid && stats.sequence == uint8_t(ssaSequence + 2));
        CHECK(stats.experience == 33000 && stats.giantsExperience == 63500 && stats.hat == 23 && stats.ssaHat == 0);
        std::vector<uint8_t> extra = ExtraBytes(tag);

        // 3. Spyro's Adventure plays it again (more gold; its experience is already at its cap of 33000).
        SsaTitleWrite(tag, 33000 + 500, 2500);
        CHECK(ExtraBytes(tag) == extra); // the extra areas, byte for byte
        CHECK(figures::ReadExtraState(tag) == figures::AreaState::Valid);
        figures::FigureStats back;
        CHECK(figures::ReadStats(tag, back) == figures::AreaState::Valid && back.gold == 2500 && back.sequence == uint8_t(ssaSequence + 3));
        CHECK(figures::LevelForExperience(figures::ExperienceFor(back, figures::LevelCap("ssa")), figures::LevelCap("ssa")) == 10);
        CHECK(figures::LevelForExperience(figures::ExperienceFor(back, figures::LevelCap("giants")), figures::LevelCap("giants")) == 15);
        CHECK(back.hat == 23 && back.ssaHat == 0);
        CHECK((tag[0x1E] | tag[0x1F] << 8) == FigureChecksum(tag.data(), 0x1E) && figures::ToyType(tag) == 14);

        // 4. Edited in Spyro's Adventure's Toy Box (cap 10): its part only; Giants' levels stay.
        figures::SetExperienceFor(back, figures::LevelCap("ssa"), 13000);
        CHECK(back.experience == 13000 && back.giantsExperience == 63500);
        figures::SetExperienceFor(back, figures::LevelCap("ssa"), 999999);
        CHECK(back.experience == 33000 && back.giantsExperience == 63500);
        figures::WriteStats(tag, back); // the library rewrites the extra areas too (next sequence), with the same values
        CHECK(figures::ReadStats(tag, back) == figures::AreaState::Valid && back.giantsExperience == 63500 && back.hat == 23);

        // 5. Spyro's Adventure's own hat (first area) and Giants' (extra area) are separate: setting one keeps the other.
        back.ssaHat = 4;
        figures::WriteStats(tag, back);
        CHECK(figures::ReadStats(tag, back) == figures::AreaState::Valid && back.ssaHat == 4 && back.hat == 23);
        FigureData ssaOnly = figures::CreateFigure(9);
        figures::FigureStats plain;
        plain.experience = 1000;
        plain.ssaHat = 4;
        plain.hat = 4;
        figures::WriteStats(ssaOnly, plain);
        CHECK(figures::ReadExtraState(ssaOnly) == figures::AreaState::Blank); // a Spyro's Adventure hat needs no extra area
        CHECK(figures::ReadStats(ssaOnly, plain) == figures::AreaState::Valid && plain.hat == 4 && plain.ssaHat == 4);

        // 6. Swap Force's part (extra +0x08, 101000 = level 20 [V]) is kept through both games' writes.
        FigureData twenty = figures::CreateFigure(100); // Jet-Vac
        figures::FigureStats high;
        figures::SetTotalExperience(high, 96500);
        high.hat = 5;
        high.swapForceExperience = 101000;
        CHECK(figures::TotalExperience(high) == 197500 && figures::ExperienceFor(high, 15) == 96500 && figures::ExperienceFor(high, 10) == 33000 &&
              figures::ExperienceFor(high, 20) == 197500);
        figures::WriteStats(twenty, high);
        std::vector<uint8_t> extra20 = ExtraBytes(twenty);
        SsaTitleWrite(twenty, 33000, 3000);
        CHECK(ExtraBytes(twenty) == extra20);
        CHECK(figures::ReadStats(twenty, high) == figures::AreaState::Valid && high.swapForceExperience == 101000 && high.giantsExperience == 63500);
        high.gold = 10;
        figures::WriteStats(twenty, high); // Giants (the library) writes it: Swap Force's part stays
        CHECK(figures::ReadStats(twenty, high) == figures::AreaState::Valid && high.swapForceExperience == 101000 && high.gold == 10);
        CHECK(figures::LevelForExperience(figures::ExperienceFor(high, 20), 20) == 20 && figures::LevelForExperience(figures::ExperienceFor(high, 15), 15) == 15);
        // Edited in Giants' Toy Box (cap 15): the first two parts only; Swap Force's stays. In Swap Force's (cap 20): all three.
        figures::SetExperienceFor(high, figures::LevelCap("giants"), 40000);
        CHECK(high.experience == 33000 && high.giantsExperience == 7000 && high.swapForceExperience == 101000);
        figures::SetExperienceFor(high, figures::LevelCap("swapforce"), figures::LevelExperience[16]);
        CHECK(high.experience == 33000 && high.giantsExperience == 63500 && high.swapForceExperience == 132400 - 96500);
        figures::SetExperienceFor(high, figures::LevelCap("swapforce"), 5000);
        CHECK(high.experience == 5000 && high.giantsExperience == 0 && high.swapForceExperience == 0);
        // Whatever the caller passes, the figure never holds more than the game's last level.
        high.swapForceExperience = 4000000;
        figures::WriteStats(twenty, high);
        CHECK(figures::ReadStats(twenty, high) == figures::AreaState::Valid && high.swapForceExperience == 101000);
        CHECK(catalogue.Accepts("giants", 100) && !catalogue.Accepts("ssa", 100));

        // A LightCore is a figure like any other: its variant, the same data, through the same round trip.
        FigureData lightcore = figures::CreateFigure(9, 0x1206);
        CHECK(catalogue.Describe(catalogue.Find(9), 0x1206).kind == collection::VariantKind::LightCore);
        figures::FigureStats glow;
        glow.experience = 6000;
        figures::WriteStats(lightcore, glow);
        SsaTitleWrite(lightcore, 9000, 100);
        CHECK(figures::ReadStats(lightcore, glow) == figures::AreaState::Valid); // a writer starts from what the figure holds
        figures::SetTotalExperience(glow, 50000);
        figures::WriteStats(lightcore, glow);
        SsaTitleWrite(lightcore, 33000, 200);
        CHECK(figures::Variant(lightcore) == 0x1206 && figures::ReadStats(lightcore, glow) == figures::AreaState::Valid && glow.giantsExperience == 17000);

        // Upgrades (block 8 +0x10, figures.md "Upgrades"): the first area's field as both games read it; Giants' Wow Pow
        // in the extra area (+0x06 bit 0, where the game wrote it when Persephone sold it [V]), kept through SSA's writes,
        // and a plain figure gets no extra area.
        FigureData series2 = figures::CreateFigure(16, 0x1801);
        figures::FigureStats bought;
        bought.upgrades = 0x0E7F; // a path, the second one, upgrades 1-4, the path's first, the Soul Gem; bits 10-11 kept
        figures::WriteStats(series2, bought);
        CHECK(figures::ReadExtraState(series2) == figures::AreaState::Blank);
        bought.wowPow = 1;
        figures::WriteStats(series2, bought);
        std::vector<uint8_t> extraWow = ExtraBytes(series2);
        CHECK(extraWow.size() > 6 && extraWow[6] == 0x01); // the extra area's +0x06 bit 0, nothing else there
        SsaTitleWrite(series2, 9000, 100);
        CHECK(ExtraBytes(series2) == extraWow);
        CHECK(figures::ReadStats(series2, bought) == figures::AreaState::Valid && bought.wowPow == 1 && bought.upgrades == 0x0E7F);
        bought.wowPow = 0;
        figures::WriteStats(series2, bought);
        CHECK(figures::ReadStats(series2, bought) == figures::AreaState::Valid && bought.wowPow == 0);
    }

    void TestDump(const char* path)
    {
        FigureData tag;
        CHECK(LoadFigure(path, tag));
        figures::FigureStats stats;
        figures::AreaState state = figures::ReadStats(tag, stats);
        CHECK(state != figures::AreaState::Damaged);
        const figures::ToyInfo* toy = figures::FindToy(figures::ToyType(tag));
        printf("%s: %s, %s, level %d (%u XP), %u gold, %u s played, upgrades %04X, owner %s, seq %u\n", path,
            toy != nullptr ? toy->name : "unknown toy", state == figures::AreaState::Valid ? "valid" : state == figures::AreaState::Blank ? "blank" : "DAMAGED",
            figures::LevelForExperience(stats.experience), stats.experience, stats.gold, stats.playSeconds, stats.upgrades,
            Hex(stats.owner.data(), stats.owner.size()).c_str(), stats.sequence);
        figures::AreaState extra = figures::ReadExtraState(tag);
        printf("  toy type %u, variant %04X\n", figures::ToyType(tag), figures::Variant(tag));
        printf("  giants: level %d (%u XP in all, %u in the extra area), hat %u (first area %u), swap force XP %u (level %d), extra %s seq %u\n",
            figures::LevelForExperience(figures::ExperienceFor(stats, figures::MaxLevelGiants), figures::MaxLevelGiants), figures::ExperienceFor(stats, figures::MaxLevelGiants),
            stats.giantsExperience, stats.hat, stats.ssaHat, stats.swapForceExperience,
            figures::LevelForExperience(figures::TotalExperience(stats), figures::MaxLevelSwapForce),
            extra == figures::AreaState::Valid ? "valid" : extra == figures::AreaState::Blank ? "blank" : "DAMAGED", stats.extraSequence);

        // Rewriting what was read gives back a valid tag with the same values.
        if (state == figures::AreaState::Valid)
        {
            figures::WriteStats(tag, stats);
            figures::FigureStats again;
            CHECK(figures::ReadStats(tag, again) == figures::AreaState::Valid && again.gold == stats.gold &&
                again.experience == stats.experience && again.owner == stats.owner);
        }
    }
}

namespace
{
    // --make <dump> <type>[:<variant hex>] [xp=<total>] [gold=] [hat=] [ssahat=] [sfxp=] [upgrades=<hex>] [wowpow=]: a figure for a test run.
    int Make(int argc, char** argv)
    {
        if (argc < 4)
            return 2;
        // --edit <dump> -: the dump itself (its stats, unknown bytes kept) instead of a new figure.
        bool edit = strcmp(argv[1], "--edit") == 0;
        char* end = nullptr;
        uint16_t type = uint16_t(strtoul(argv[3], &end, 10)), variant = end != nullptr && *end == ':' ? uint16_t(strtoul(end + 1, nullptr, 16)) : 0;
        FigureData tag;
        figures::FigureStats stats;
        if (edit)
        {
            if (!LoadFigure(argv[2], tag) || figures::ReadStats(tag, stats) != figures::AreaState::Valid)
                return fprintf(stderr, "cannot read %s\n", argv[2]), 1;
        }
        else
            tag = figures::CreateFigure(type, variant);
        for (int i = 4; i < argc; i++)
        {
            std::string arg = argv[i];
            size_t eq = arg.find('=');
            uint32_t value = eq == std::string::npos ? 0 : uint32_t(strtoul(arg.c_str() + eq + 1, nullptr, 10));
            std::string key = arg.substr(0, eq);
            if (key == "xp") figures::SetTotalExperience(stats, value);
            else if (key == "gold") stats.gold = uint16_t(value);
            else if (key == "hat") stats.hat = uint16_t(value);
            else if (key == "ssahat") stats.ssaHat = uint16_t(value);
            else if (key == "sfxp") stats.swapForceExperience = value;
            else if (key == "upgrades") stats.upgrades = uint16_t(strtoul(arg.c_str() + eq + 1, nullptr, 16));
            else if (key == "wowpow") stats.wowPow = uint8_t(value);
            else return fprintf(stderr, "unknown field %s\n", arg.c_str()), 2;
        }
        if (argc > 4)
            figures::WriteStats(tag, stats);
        FILE* out = fopen(argv[2], "wb");
        bool written = out != nullptr && fwrite(tag.data(), 1, tag.size(), out) == tag.size();
        if (out == nullptr || fclose(out) != 0 || !written)
            return fprintf(stderr, "cannot write %s\n", argv[2]), 1;
        TestDump(argv[2]);
        return g_failures == 0 ? 0 : 1;
    }

    // --same-extra <before> <after>: the extra areas are byte-identical (decrypted) and valid in both.
    int SameExtra(const char* before, const char* after)
    {
        FigureData a, b;
        CHECK(LoadFigure(before, a) && LoadFigure(after, b));
        TestDump(before);
        TestDump(after);
        bool same = ExtraBytes(a) == ExtraBytes(b);
        figures::FigureStats first, second;
        bool wrote = figures::ReadStats(a, first) == figures::AreaState::Valid && figures::ReadStats(b, second) == figures::AreaState::Valid &&
            first.sequence != second.sequence;
        printf("extra areas: %s; before %s, after %s; the first pair was %s\n", same ? "byte-identical" : "DIFFERENT",
            figures::ReadExtraState(a) == figures::AreaState::Valid ? "valid" : "not valid", figures::ReadExtraState(b) == figures::AreaState::Valid ? "valid" : "not valid",
            wrote ? "written in between" : "NOT written in between");
        CHECK(same && wrote && figures::ReadExtraState(b) == figures::AreaState::Valid);
        CHECK(figures::TotalExperience(second) == figures::TotalExperience(first) && second.hat == first.hat &&
            second.swapForceExperience == first.swapForceExperience);
        return g_failures == 0 ? 0 : 1;
    }

    // --roundtrip <before> <after>: Spyro's Adventure ran with the figure in between. Everything outside the first pair of
    // areas (blocks 0-7, 17-35, 45-63: the identity, the extra areas, the trailers) must be byte for byte the same on the
    // tag; the first pair must be valid, and the bytes the game changed in it are listed (decrypted, newest area).
    int RoundTrip(const char* before, const char* after)
    {
        FigureData a, b;
        CHECK(LoadFigure(before, a) && LoadFigure(after, b));
        figures::FigureStats first, second;
        figures::AreaState stateA = figures::ReadStats(a, first), stateB = figures::ReadStats(b, second);
        CHECK(stateA != figures::AreaState::Damaged && stateB != figures::AreaState::Damaged);
        auto inFirstPair = [](size_t block) { return (block >= 8 && block <= 16) || (block >= 36 && block <= 44); };
        size_t outside = 0;
        for (size_t block = 0; block < 64; block++)
            if (!inFirstPair(block) && memcmp(a.data() + block * FigureBlockSize, b.data() + block * FigureBlockSize, FigureBlockSize) != 0)
                outside++;
        CHECK(outside == 0);
        CHECK(ExtraBytes(a) == ExtraBytes(b) && figures::ReadExtraState(a) == figures::ReadExtraState(b));
        CHECK(figures::ToyType(a) == figures::ToyType(b) && figures::Variant(a) == figures::Variant(b));
        bool written = stateB == figures::AreaState::Valid && (stateA != figures::AreaState::Valid || first.sequence != second.sequence);
        std::string changed;
        bool unexpected = false;
        if (written && stateA == figures::AreaState::Valid)
        {
            FigureData pa = figures::Decrypt(a), pb = figures::Decrypt(b);
            auto newest = [](const FigureData& plain, uint8_t sequence) {
                bool second = plain[0x24 * FigureBlockSize + 0x09] == sequence;
                return plain.data() + (second ? 0x24 : 0x08) * FigureBlockSize;
            };
            const uint8_t* x = newest(pa, first.sequence);
            const uint8_t* y = newest(pb, second.sequence);
            for (size_t i = 0; i < 9 * FigureBlockSize; i++)
                if (i % 0x40 < 0x30 && x[i] != y[i]) // the trailer (block 3 and 7 of the area) is not data
                {
                    char text[24];
                    snprintf(text, sizeof(text), " %02zX", i);
                    changed += text;
                    // known fields: experience, gold, play time, sequence, checksums, platforms, owner, last placed, last reset; 0x5F (set on a figure's first use)
                    bool known = i <= 0x0F || i == 0x13 || i == 0x16 || i == 0x5F || (i >= 0x18 && i <= 0x1F) || (i >= 0x50 && i <= 0x55) || (i >= 0x60 && i <= 0x65);
                    unexpected = unexpected || !known;
                }
        }
        printf("round trip: toy %u:%04X; first pair %s (seq %u -> %u); XP %u -> %u, gold %u -> %u; changed offsets:%s%s; outside the first pair: %s; extra areas: %s\n",
            figures::ToyType(b), figures::Variant(b), written ? "written" : "not written", first.sequence, second.sequence, first.experience,
            second.experience, first.gold, second.gold, changed.empty() ? " none" : changed.c_str(), unexpected ? " (UNEXPECTED)" : "",
            outside == 0 ? "identical" : "CHANGED", ExtraBytes(a) == ExtraBytes(b) ? "identical" : "CHANGED");
        CHECK(stateB != figures::AreaState::Valid || written || stateA == figures::AreaState::Valid);
        CHECK(!unexpected);
        return g_failures == 0 ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && (strcmp(argv[1], "--make") == 0 || strcmp(argv[1], "--edit") == 0))
        return Make(argc, argv);
    if (argc == 4 && strcmp(argv[1], "--same-extra") == 0)
        return SameExtra(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--roundtrip") == 0)
        return RoundTrip(argv[2], argv[3]);
    TestCrypto();
    TestTag();
    TestCatalog();
    TestGiants();
    TestRoundTrips();
    for (int i = 1; i < argc; i++)
        TestDump(argv[i]);
    printf("figure test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
