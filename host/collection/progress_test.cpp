// Unit test of the achievements and play time (collection/achievements.h, collection/progress.h): the XDBF reader on
// synthetic data and on the discs given on the command line, the unlocks' round trip, the play time's accumulation. Every
// path is in a scratch folder of its own; the player's folders are never touched.
//   SSAProgressTest [<disc image .iso>...]       Exit code 0 = pass. A disc is only read; the counts are printed.
#include "vfs.h"
#include "host/collection/achievements.h"
#include "host/collection/progress.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace
{
    int g_checks, g_failures;

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

    using Bytes = std::vector<uint8_t>;
    void Put16(Bytes& b, uint32_t v)
    {
        b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v));
    }
    void Put32(Bytes& b, uint32_t v)
    {
        Put16(b, v >> 16);
        Put16(b, v & 0xFFFF);
    }
    void Put64(Bytes& b, uint64_t v)
    {
        Put32(b, uint32_t(v >> 32));
        Put32(b, uint32_t(v));
    }

    struct Item
    {
        uint16_t space;
        uint64_t id;
        Bytes data;
    };

    // An XDBF block with the given entries (no free space).
    Bytes MakeXdbf(const std::vector<Item>& items)
    {
        Bytes out;
        Put32(out, 0x58444246);
        Put32(out, 0x10000);
        Put32(out, uint32_t(items.size()));
        Put32(out, uint32_t(items.size()));
        Put32(out, 0);
        Put32(out, 0);
        uint32_t offset = 0;
        for (const Item& item : items)
        {
            Put16(out, item.space);
            Put64(out, item.id);
            Put32(out, offset);
            Put32(out, uint32_t(item.data.size()));
            offset += uint32_t(item.data.size());
        }
        for (const Item& item : items)
            out.insert(out.end(), item.data.begin(), item.data.end());
        return out;
    }

    Bytes MakeStrings(const std::vector<std::pair<uint16_t, std::string>>& strings)
    {
        Bytes out;
        Put32(out, 0x58535452);
        Put32(out, 1);
        Put32(out, 2);
        Put16(out, uint32_t(strings.size()));
        for (const auto& [id, text] : strings)
        {
            Put16(out, id);
            Put16(out, uint32_t(text.size()));
            out.insert(out.end(), text.begin(), text.end());
        }
        return out;
    }

    // One achievement: id, name / unlocked / locked string ids, image id, gamerscore, flags.
    void PutEntry(Bytes& b, uint16_t id, uint16_t name, uint16_t unlocked, uint16_t locked, uint32_t image,
        uint16_t score, uint32_t flags)
    {
        Put16(b, id);
        Put16(b, name);
        Put16(b, unlocked);
        Put16(b, locked);
        Put32(b, image);
        Put16(b, score);
        Put16(b, 0);
        Put32(b, flags);
        for (int i = 0; i < 4; i++)
            Put32(b, 0);
    }

    Bytes SyntheticXdbf()
    {
        Bytes table;
        Put32(table, 0x58414348);
        Put32(table, 1);
        Put32(table, 2);
        Put16(table, 2);
        PutEntry(table, 1, 1, 2, 3, 0x10, 25, 0x8 | 3);
        PutEntry(table, 2, 4, 5, 6, 0x11, 50, 0x3); // not shown while locked: hidden
        return MakeXdbf({
            { 1, 0x58414348, table },
            { 2, 0x10, { 'P', 'N', 'G', '1' } },
            { 2, 0x11, { 'P', 'N', 'G', '2' } },
            { 3, 1,
                MakeStrings({ { 1, "First" }, { 2, "Did the first thing" }, { 3, "Do the first thing" },
                    { 4, "Secret" }, { 5, "It was secret" }, { 6, "???" } }) },
            { 3, 4, MakeStrings({ { 1, "Premier" }, { 2, "A fait la premiere chose" } }) },
        });
    }

    void XdbfChecks()
    {
        Bytes xdbf = SyntheticXdbf();
        achievements::List english = achievements::Parse(xdbf.data(), xdbf.size(), 1, true);
        CHECK(english.items.size() == 2);
        CHECK(english.TotalGamerscore() == 75);
        if (english.items.size() == 2)
        {
            CHECK(english.items[0].id == 1 && english.items[0].name == "First");
            CHECK(english.items[0].unlockedDescription == "Did the first thing" &&
                  english.items[0].lockedDescription == "Do the first thing");
            CHECK(!english.items[0].hidden && english.items[1].hidden);
            CHECK(english.items[0].imagePng == Bytes({ 'P', 'N', 'G', '1' }) &&
                  english.items[1].imagePng == Bytes({ 'P', 'N', 'G', '2' }));
            CHECK(english.items[1].gamerscore == 50 && english.items[1].name == "Secret");
        }
        achievements::List french = achievements::Parse(xdbf.data(), xdbf.size(), 4, false);
        CHECK(french.items.size() == 2);
        if (french.items.size() == 2)
        {
            CHECK(
                french.items[0].name == "Premier" && french.items[0].unlockedDescription == "A fait la premiere chose");
            CHECK(french.items[0].lockedDescription == "Do the first thing"); // missing in French: English
            CHECK(french.items[0].imagePng.empty());
        }
        CHECK(achievements::XdbfLanguage("fr") == 4 && achievements::XdbfLanguage("es-419") == 5 &&
              achievements::XdbfLanguage("sv") == 1);

        // Damaged data: every cut of the block, and noise, must give a list without reading outside the buffer.
        for (size_t cut = 0; cut < xdbf.size(); cut++)
            achievements::Parse(xdbf.data(), cut, 1, true);
        Bytes noisy = xdbf;
        for (size_t i = 0; i < noisy.size(); i++)
        {
            Bytes copy = noisy;
            copy[i] ^= 0xFF;
            achievements::Parse(copy.data(), copy.size(), 4, true);
        }
        CHECK(achievements::Parse(nullptr, 0, 1, true).items.empty());
        CHECK(achievements::Read({ 1, 2, 3 }, 1, true).items.empty());
    }

    // The launcher's cache, from a default.xex of a disc (or nothing: only the empty cases).
    void CacheChecks(const fs::path& scratch, const Bytes& xex)
    {
        fs::path folder = scratch / "cache";
        CHECK(!achievements::CacheIsCurrent(folder, "k"));
        CHECK(achievements::ReadCache(folder, "en").items.empty());
        if (xex.empty())
            return;
        achievements::List direct = achievements::Read(xex, 4, true);
        CHECK(achievements::WriteCache(folder, "k", xex));
        CHECK(achievements::CacheIsCurrent(folder, "k") && !achievements::CacheIsCurrent(folder, "other"));
        achievements::List cached = achievements::ReadCache(folder, "fr");
        CHECK(cached.items.size() == direct.items.size());
        bool same = cached.items.size() == direct.items.size();
        for (size_t i = 0; same && i < cached.items.size(); i++)
        {
            const auto& a = cached.items[i];
            const auto& b = direct.items[i];
            same = a.id == b.id && a.gamerscore == b.gamerscore && a.imageId == b.imageId && a.hidden == b.hidden &&
                   a.name == b.name && a.unlockedDescription == b.unlockedDescription &&
                   a.lockedDescription == b.lockedDescription &&
                   fs::file_size(achievements::CachedImage(folder, a.imageId)) == b.imagePng.size();
        }
        CHECK(same);
        CHECK(achievements::ReadCache(folder, "nl").items.size() == direct.items.size()); // no Dutch: English
    }

    void UnlockChecks(const fs::path& scratch)
    {
        fs::path folder = scratch / "unlocks";
        CHECK(progress::ReadUnlocks(folder).empty());
        bool added = false;
        CHECK(progress::WriteUnlock(folder, 7, 1000, &added) && added);
        CHECK(progress::WriteUnlock(folder, 3, 2000, &added) && added);
        CHECK(progress::WriteUnlock(folder, 7, 3000, &added) && !added); // the first time stays
        // "After a restart": a new read of the file only.
        std::vector<progress::Unlock> unlocks = progress::ReadUnlocks(folder);
        CHECK(unlocks.size() == 2);
        if (unlocks.size() == 2)
            CHECK(unlocks[0].id == 7 && unlocks[0].time == 1000 && unlocks[1].id == 3 && unlocks[1].time == 2000);
        CHECK(!fs::exists(folder / "achievements.txt.savetmp") &&
              fs::directory_iterator(folder) != fs::directory_iterator());
        // A damaged line is skipped, the rest is kept.
        FILE* file = fopen((folder / "achievements.txt").string().c_str(), "wb");
        fputs("# comment\nnot a number\n5 10\n\n99999999999 4\n6\n", file);
        fclose(file);
        unlocks = progress::ReadUnlocks(folder);
        CHECK(unlocks.size() == 2 && unlocks[0].id == 5 && unlocks[0].time == 10 && unlocks[1].id == 6);
    }

    void PlayTimeChecks(const fs::path& scratch)
    {
        fs::path folder = scratch / "playtime";
        CHECK(progress::ReadPlayTime(folder).totalSeconds == 0 && progress::ReadPlayTime(folder).lastEnd == 0);

        progress::PlaySession first;
        first.Begin(folder, 1000, 50.0);
        CHECK(first.Running());
        CHECK(progress::ReadPlayTime(folder).sessions == 1 && progress::ReadPlayTime(folder).lastEnd == 1000);
        CHECK(!first.Tick(1030, 80.0)); // under a minute: nothing written
        CHECK(progress::ReadPlayTime(folder).totalSeconds == 0);
        CHECK(first.Tick(1070, 120.0)); // 70 s in
        CHECK(progress::ReadPlayTime(folder).totalSeconds == 70 && progress::ReadPlayTime(folder).lastEnd == 1070);
        first.End(1100, 150.0);
        CHECK(!first.Running());
        CHECK(progress::ReadPlayTime(folder).totalSeconds == 100 && progress::ReadPlayTime(folder).lastEnd == 1100);

        // A second run adds up; a killed one keeps what the last tick wrote.
        progress::PlaySession second;
        second.Begin(folder, 5000, 10.0);
        second.Tick(5090, 100.0);
        progress::PlayTime time = progress::ReadPlayTime(folder);
        CHECK(time.totalSeconds == 190 && time.sessions == 2 && time.lastEnd == 5090);
        progress::PlaySession none;
        none.Begin({}, 1, 1.0); // no data folder: nothing happens
        CHECK(!none.Running() && !none.Tick(100, 100.0));
    }

    // A real disc: the achievements are read, counted, and their texts and pictures checked.
    void DiscChecks(const char* path)
    {
        printf("Disc %s\n", fs::path(path).filename().string().c_str());
        DiscFileSystem disc;
        CHECK(disc.Open(path));
        Bytes xex = disc.ReadAll("default.xex");
        CHECK(!xex.empty());
        achievements::List english = achievements::Read(xex, 1, true);
        achievements::List french = achievements::Read(xex, 4, false);
        size_t hidden =
            std::count_if(english.items.begin(), english.items.end(), [](const auto& item) { return item.hidden; });
        size_t pictures = std::count_if(
            english.items.begin(), english.items.end(), [](const auto& item) { return item.imagePng.size() > 8; });
        size_t named = std::count_if(
            french.items.begin(), french.items.end(), [](const auto& item) { return !item.name.empty(); });
        printf("  title %08X: %zu achievements, gamerscore %u, %zu hidden, %zu pictures, %zu French names\n",
            english.titleId, english.items.size(), english.TotalGamerscore(), hidden, pictures, named);
        std::map<uint32_t, int> flags;
        for (const auto& item : english.items)
            flags[item.flags]++;
        for (const auto& [value, count] : flags)
            printf("  flags 0x%08X: %d\n", value, count);
        CacheChecks(fs::temp_directory_path() / ("SSAProgressTest-cache-" + std::to_string(getpid())), xex);
        fs::remove_all(fs::temp_directory_path() / ("SSAProgressTest-cache-" + std::to_string(getpid())));
        CHECK(!english.items.empty());
        CHECK(english.TotalGamerscore() > 0);
        CHECK(pictures == english.items.size());
        CHECK(named == english.items.size());
        CHECK(french.items.size() == english.items.size());
        for (const auto& item : english.items)
            CHECK(!item.name.empty() && !item.unlockedDescription.empty());
    }
}

int main(int argc, char** argv)
{
    fs::path scratch = fs::temp_directory_path() / ("SSAProgressTest-" + std::to_string(getpid()));
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    XdbfChecks();
    CacheChecks(scratch, {});
    UnlockChecks(scratch);
    PlayTimeChecks(scratch);
    for (int i = 1; i < argc; i++)
        DiscChecks(argv[i]);

    fs::remove_all(scratch);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
