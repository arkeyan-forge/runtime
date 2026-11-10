// Checks the portable game's pack (portable_pack.h) and the game's reader of it (vfs.h, pack mode) on synthetic files, in a
// scratch folder: a pack after a fake executable and a pack alone, damaged trailers and indexes, a cancelled write, and the
// first-launch import of the figures (never over one that exists). No game data. usage: ArkeyanPortablePackTest <scratch dir>
#include "portable_pack.h"
#include "json.h"
#include "vfs.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using namespace portable;

namespace
{
    int g_failures = 0, g_checks = 0;
#define EXPECT(condition)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    std::vector<uint8_t> Bytes(size_t size, uint32_t seed)
    {
        std::vector<uint8_t> data(size);
        std::mt19937 random(seed);
        for (uint8_t& byte : data)
            byte = uint8_t(random());
        return data;
    }

    void WriteFile(const fs::path& path, const std::vector<uint8_t>& data)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    }

    std::vector<uint8_t> ReadFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    // A disc of a few files: default.xex, a big one (more than one chunk), an empty one, nested folders.
    struct Disc
    {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
            { "default.xex", Bytes(70000, 1) },
            { "media/big.arc", Bytes((9u << 20) + 123, 2) },
            { "media/empty.txt", {} },
            { "media/levels/level_01/a.bin", Bytes(4097, 3) },
        };
    };

    bool WritePack(const fs::path& file, bool append, const Disc& disc, std::string& error,
        const std::atomic<bool>* cancel = nullptr)
    {
        Writer writer;
        writer.cancel = cancel;
        if (!writer.Open(file, append, error))
            return false;
        for (const auto& [path, data] : disc.files)
            if (!writer.AddBytes(std::string(GamePrefix) + path, data, error))
                return false;
        Info info{ "ssa", "0.9.5", "0.8.0", "abc", "20261101-203000" };
        return writer.AddText(InfoName, FormatInfo(info), error) &&
               writer.AddText(KeysName, "[ssa.input]\nkey_a=K\nseeded=1\n", error) &&
               writer.AddBytes("collection/Spyro.sky", Bytes(1024, 7), error) &&
               writer.AddBytes("collection/Eruptor.sky", Bytes(1024, 8), error) &&
               writer.AddText("collection/collection.json",
                   R"({"figures":{"Spyro.sky":{"label":"theirs"},"Eruptor.sky":{"label":"Hot"}}})", error) &&
               writer.Finish(error);
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <scratch dir>\n", argv[0]);
        return 2;
    }
    const fs::path scratch = argv[1];
    fs::remove_all(scratch / "pack");
    fs::create_directories(scratch / "pack");
    const fs::path root = scratch / "pack";
    Disc disc;

    // A pack after a fake executable: the executable's bytes stay as they were, every file reads back.
    const fs::path exe = root / "game.exe";
    const std::vector<uint8_t> image = Bytes(200001, 9);
    WriteFile(exe, image);
    std::string error;
    EXPECT(WritePack(exe, true, disc, error));
    std::vector<uint8_t> whole = ReadFile(exe);
    EXPECT(whole.size() > image.size() && std::equal(image.begin(), image.end(), whole.begin()));
    std::vector<Entry> entries;
    EXPECT(ReadIndex(exe, entries, &error));
    EXPECT(entries.size() == disc.files.size() + 5);
    for (const auto& [path, data] : disc.files)
    {
        const Entry* entry = Find(entries, std::string(GamePrefix) + path);
        EXPECT(entry != nullptr && entry->size == data.size() && entry->offset >= image.size());
        if (entry != nullptr && entry->size < (1u << 20))
            EXPECT(ReadEntry(exe, *entry) == data);
    }
    const Entry* infoEntry = Find(entries, InfoName);
    EXPECT(infoEntry != nullptr);
    if (infoEntry != nullptr)
    {
        std::vector<uint8_t> text = ReadEntry(exe, *infoEntry);
        Info info = ParseInfo(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
        EXPECT(info.game == "ssa" && info.version == "0.9.5" && info.kit == "0.8.0" && info.xex == "abc" &&
               info.exported == "20261101-203000");
        EXPECT(FirstLaunchMarker("/g", info).filename() == "portable-20261101-203000.done");
    }
    // No pack data between the files: they sit back to back, then the index.
    uint64_t data = 0;
    for (const Entry& entry : entries)
        data += entry.size;
    EXPECT(whole.size() > image.size() + data && whole.size() - image.size() - data < 4096);

    // The game's reader: case-insensitive paths, the folders the files imply, a read across the big file's chunks and at its end.
    {
        DiscFileSystem reader;
        EXPECT(reader.Open(exe.string().c_str()));
        EXPECT(reader.FromPack() && !reader.FromFolder());
        EXPECT(reader.ReadAll("DEFAULT.XEX") == disc.files[0].second);
        const DiscFileSystem::Entry* big = reader.Find("Media\\BIG.arc");
        EXPECT(big != nullptr && !big->directory && big->size == disc.files[1].second.size());
        if (big != nullptr)
        {
            std::vector<uint8_t> part(10);
            EXPECT(reader.Read(*big, big->size - 4, part.data(), 10) == 4);
            EXPECT(std::equal(part.begin(), part.begin() + 4, disc.files[1].second.end() - 4));
            EXPECT(reader.Read(*big, (4u << 20) - 3, part.data(), 10) == 10);
            EXPECT(std::equal(part.begin(), part.end(), disc.files[1].second.begin() + (4u << 20) - 3));
        }
        EXPECT(reader.Find("media/levels") != nullptr && reader.Find("media/levels")->directory);
        EXPECT(reader.List("media").size() == 3); // big.arc, empty.txt, levels
        EXPECT(reader.ReadAll("media/empty.txt").empty() && reader.Find("media/empty.txt") != nullptr);
        EXPECT(reader.Find("portable.txt") == nullptr && reader.Find("collection/spyro.sky") == nullptr);
        EXPECT(reader.Files().size() == disc.files.size());
    }

    // A pack alone (macOS: game.skypack in the bundle).
    const fs::path alone = root / PackName;
    EXPECT(WritePack(alone, false, disc, error));
    EXPECT(ReadIndex(alone, entries, &error) && Find(entries, "game/default.xex")->offset == 0);

    // Not a pack: a plain file, a file shorter than a trailer; and the reader falls back (no default.xex anywhere).
    WriteFile(root / "plain.bin", Bytes(5000, 4));
    EXPECT(!ReadIndex(root / "plain.bin", entries, &error) && error.find("no pack") == 0);
    WriteFile(root / "tiny.bin", Bytes(8, 4));
    EXPECT(!ReadIndex(root / "tiny.bin", entries, &error));
    {
        DiscFileSystem reader;
        EXPECT(!reader.Open((root / "plain.bin").string().c_str()) && !reader.FromPack());
    }

    // Damaged: the index offset past the file, the index cut, a name that leaves the tree.
    {
        std::vector<uint8_t> bad = whole;
        bad[bad.size() - TrailerSize + 8 + 7] = 0x7f; // index offset far past the end
        WriteFile(root / "bad1.exe", bad);
        EXPECT(!ReadIndex(root / "bad1.exe", entries, &error) && error.find("damaged") == 0);
        bad = whole;
        bad[bad.size() - TrailerSize + 16] ^= 0x01; // index size one off: the last line is cut or runs into the trailer
        WriteFile(root / "bad2.exe", bad);
        EXPECT(!ReadIndex(root / "bad2.exe", entries, &error));
        Writer writer;
        EXPECT(writer.Open(root / "bad3.pack", false, error));
        EXPECT(!writer.AddText("../outside.txt", "x", error));
        EXPECT(!writer.AddText("game/a\tb", "x", error));
        EXPECT(writer.AddText("game/default.xex", "x", error) && !writer.AddText("game/default.xex", "y", error));
        EXPECT(writer.Finish(error));
    }

    // A cancelled write stops between two chunks.
    {
        std::atomic<bool> cancel{ true };
        EXPECT(!WritePack(root / "cancelled.pack", false, disc, error, &cancel) && error == "cancelled");
        std::error_code removed;
        EXPECT(fs::remove(root / "cancelled.pack", removed) && !removed); // closed: Windows deletes no open file
    }

    // First launch: the figures not in the collection are added, an existing one is never overwritten.
    {
        const fs::path collection = root / "collection";
        const std::vector<uint8_t> mine = Bytes(1024, 99);
        WriteFile(collection / "Spyro.sky", mine);
        const std::string own = R"({"format":1,"figures":{"Spyro.sky":{"label":"mine"}}})";
        WriteFile(collection / "collection.json", std::vector<uint8_t>(own.begin(), own.end()));
        EXPECT(ReadIndex(exe, entries, &error));
        int skipped = 0, failed = -1;
        EXPECT(ImportCollection(exe, entries, collection, &skipped, &failed) == 1 && skipped == 1 && failed == 0);
        EXPECT(ReadFile(collection / "Spyro.sky") == mine);
        EXPECT(ReadFile(collection / "Eruptor.sky") == Bytes(1024, 8));
        // The added figure's metadata joins this machine's index; the figure already here keeps its own.
        const std::vector<uint8_t> merged = ReadFile(collection / "collection.json");
        collection::Json index;
        EXPECT(collection::Json::Parse(std::string(merged.begin(), merged.end()), index, error));
        EXPECT(index["figures"]["Spyro.sky"]["label"].AsString() == "mine");
        EXPECT(index["figures"]["Eruptor.sky"]["label"].AsString() == "Hot");
        EXPECT(ImportCollection(exe, entries, collection, &skipped) == 0 && skipped == 2);
        for (const auto& item : fs::directory_iterator(collection))
            EXPECT(item.path().extension() != ".part");
    }

    fs::remove_all(root);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
