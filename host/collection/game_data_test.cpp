// Checks "Copy the game data" (game_data.h) and the game's reader of the copy (vfs.h, folder mode) on synthetic files, in a
// scratch folder. No game data. usage: SSAGameDataTest <scratch dir>
#include "game_data.h"
#include "vfs.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>

namespace fs = std::filesystem;
using namespace gamedata;

namespace
{
    int g_failures = 0;
#define EXPECT(condition) \
    do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); g_failures++; } } while (0)

    // A fake disc: file contents by path, and how often each was read (the resumed copy must not read what it has).
    struct FakeDisc
    {
        std::map<std::string, std::vector<uint8_t>> files;
        std::map<std::string, int> reads;
        std::string flipOnSecondPass; // a file whose read-back comparison fails (the disc "changes")
        std::map<std::string, int> passes;

        Source MakeSource()
        {
            Source source;
            source.xexSha256 = "abc123";
            for (const auto& [path, data] : files)
                source.files.push_back({ path, data.size() });
            source.read = [this](const SourceFile& file, uint64_t position, void* buffer, size_t size) -> size_t {
                const std::vector<uint8_t>& data = files[file.path];
                if (position == 0)
                    passes[file.path]++;
                reads[file.path]++;
                if (position >= data.size())
                    return 0;
                size = std::min<size_t>(size, data.size() - position);
                memcpy(buffer, &data[position], size);
                if (file.path == flipOnSecondPass && passes[file.path] == 2)
                    static_cast<uint8_t*>(buffer)[0] ^= 0xFF;
                return size;
            };
            return source;
        }
    };

    std::vector<uint8_t> Random(size_t size, uint32_t seed)
    {
        std::mt19937 random(seed);
        std::vector<uint8_t> data(size);
        for (uint8_t& byte : data)
            byte = uint8_t(random());
        return data;
    }

    FakeDisc MakeDisc()
    {
        FakeDisc disc;
        disc.files["default.xex"] = Random(5000, 1);
        disc.files["media/levels/mission01.str"] = Random(9u << 20, 2); // three 4 MiB chunks
        disc.files["media/empty.bin"] = {};
        disc.files["media/sounds/music.xwb"] = Random(300000, 3);
        disc.files["zzz.dat"] = Random(70000, 4);
        return disc;
    }

    bool SameBytes(const fs::path& path, const std::vector<uint8_t>& data)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> read(data.size());
        in.read(reinterpret_cast<char*>(read.data()), std::streamsize(read.size()));
        return in.gcount() == std::streamsize(data.size()) && read == data;
    }

    bool NoPartFiles(const fs::path& folder)
    {
        std::error_code error;
        for (const auto& item : fs::recursive_directory_iterator(folder, error))
            if (item.path().extension() == ".part")
                return false;
        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: SSAGameDataTest <scratch dir>\n");
        return 2;
    }
    fs::path scratch = fs::path(argv[1]) / "game_data_test";
    std::error_code error;
    fs::remove_all(scratch, error);
    fs::create_directories(scratch);

    // Nothing there: no copy.
    EXPECT(Inspect(scratch / "none").state == State::None);

    // The happy path: every file copied, verified, the manifest complete.
    {
        FakeDisc disc = MakeDisc();
        fs::path folder = scratch / "happy" / "gamedata";
        uint64_t lastDone = 0, lastTotal = 0;
        CopyOptions options;
        options.progress = [&](uint64_t done, uint64_t total) { lastDone = done; lastTotal = total; };
        CopyResult result = Copy(disc.MakeSource(), folder, options);
        EXPECT(result.outcome == CopyResult::Outcome::Done);
        EXPECT(lastDone == lastTotal && lastTotal == 5000 + (9u << 20) + 300000 + 70000);
        for (const auto& [path, data] : disc.files)
            EXPECT(SameBytes(folder / path, data));
        EXPECT(NoPartFiles(folder));
        Status status = Inspect(folder);
        EXPECT(status.state == State::Complete && status.files == 5 && status.missing == 0 && status.present == status.total);
        EXPECT(status.xexSha256 == "abc123");

        // The game's reader: a folder is opened like an image; paths are looked up in any case; the bookkeeping is hidden.
        DiscFileSystem reader;
        EXPECT(reader.Open(folder.string().c_str()));
        EXPECT(reader.FromFolder());
        const DiscFileSystem::Entry* entry = reader.Find("Media\\Levels\\Mission01.str");
        EXPECT(entry != nullptr && !entry->directory && entry->size == 9u << 20);
        std::vector<uint8_t> tail(1000);
        EXPECT(entry != nullptr && reader.Read(*entry, (9u << 20) - 1000, tail.data(), 1000) == 1000);
        EXPECT(std::equal(tail.begin(), tail.end(), disc.files["media/levels/mission01.str"].end() - 1000));
        EXPECT(reader.ReadAll("default.xex") == disc.files["default.xex"]);
        EXPECT(reader.Find("gamedata.manifest") == nullptr);
        EXPECT(reader.List("media").size() == 3); // empty.bin, levels, sounds
        EXPECT(reader.Files().size() == 5);
        EXPECT(reader.Find("media/sounds") != nullptr && reader.Find("media/sounds")->directory);

        // Removal: the folder goes; a folder that isn't a copy is refused.
        std::string why;
        EXPECT(!Remove(scratch / "happy", why) && !why.empty());
        EXPECT(fs::exists(folder / "default.xex"));
        EXPECT(Remove(folder, why));
        EXPECT(!fs::exists(folder) && Inspect(folder).state == State::None);
        EXPECT(Remove(folder, why)); // nothing to remove is not an error
    }

    // Not enough space: nothing is copied, and what is needed is said.
    {
        FakeDisc disc = MakeDisc();
        fs::path folder = scratch / "space" / "gamedata";
        CopyOptions options;
        options.freeBytes = 5 << 20;
        CopyResult result = Copy(disc.MakeSource(), folder, options);
        EXPECT(result.outcome == CopyResult::Outcome::NotEnoughSpace);
        EXPECT(result.available == 5u << 20 && result.needed >= 5000 + (9u << 20) + 300000 + 70000);
        EXPECT(Inspect(folder).state == State::None && !fs::exists(folder / "default.xex"));
        EXPECT(disc.reads.empty());
        // Room for everything but the reserve is not enough either.
        options.freeBytes = int64_t(5000 + (9u << 20) + 300000 + 70000);
        EXPECT(Copy(disc.MakeSource(), folder, options).outcome == CopyResult::Outcome::NotEnoughSpace);
        options.freeBytes = options.freeBytes + int64_t(options.reserve);
        EXPECT(Copy(disc.MakeSource(), folder, options).outcome == CopyResult::Outcome::Done);
    }

    // Interrupted, then resumed: the files already there are not read again, the half-written one starts over.
    {
        FakeDisc disc = MakeDisc();
        fs::path folder = scratch / "resume" / "gamedata";
        std::atomic<bool> cancel{ false };
        CopyOptions options;
        options.cancel = &cancel;
        options.progress = [&](uint64_t done, uint64_t) { if (done > 6u << 20) cancel = true; }; // inside the big file
        CopyResult first = Copy(disc.MakeSource(), folder, options);
        EXPECT(first.outcome == CopyResult::Outcome::Cancelled);
        Status partial = Inspect(folder);
        EXPECT(partial.state == State::Partial && partial.missing > 0 && partial.present > 0 && partial.present < partial.total);
        EXPECT(NoPartFiles(folder));
        EXPECT(fs::exists(folder / "default.xex") && !fs::exists(folder / "media/levels/mission01.str"));

        disc.reads.clear();
        cancel = false;
        options.progress = {};
        CopyResult second = Copy(disc.MakeSource(), folder, options);
        EXPECT(second.outcome == CopyResult::Outcome::Done);
        EXPECT(disc.reads.count("default.xex") == 0 && disc.reads.count("media/levels/mission01.str") == 1);
        EXPECT(Inspect(folder).state == State::Complete);
        for (const auto& [path, data] : disc.files)
            EXPECT(SameBytes(folder / path, data));
    }

    // A copy that doesn't match the disc is refused and leaves no file behind.
    {
        FakeDisc disc = MakeDisc();
        disc.flipOnSecondPass = "zzz.dat";
        fs::path folder = scratch / "flip" / "gamedata";
        CopyResult result = Copy(disc.MakeSource(), folder);
        EXPECT(result.outcome == CopyResult::Outcome::Failed && result.error.find("zzz.dat") != std::string::npos);
        EXPECT(!fs::exists(folder / "zzz.dat") && NoPartFiles(folder));
        EXPECT(Inspect(folder).state == State::Partial);
        // Resumed once the disc reads well again: only that file is read, and everything is there.
        disc.flipOnSecondPass.clear();
        disc.passes.clear();
        EXPECT(Copy(disc.MakeSource(), folder).outcome == CopyResult::Outcome::Done);
        EXPECT(Inspect(folder).state == State::Complete);
    }

    // Names that would leave the folder are refused.
    {
        FakeDisc disc;
        disc.files["../escape.bin"] = Random(10, 5);
        CopyResult result = Copy(disc.MakeSource(), scratch / "unsafe" / "gamedata");
        EXPECT(result.outcome == CopyResult::Outcome::Failed);
        EXPECT(!fs::exists(scratch / "unsafe" / "escape.bin"));
    }

    // Fallback: a finished copy that lost a file, got another size, or lost its manifest is Damaged / None, not Complete.
    {
        FakeDisc disc = MakeDisc();
        fs::path folder = scratch / "damaged" / "gamedata";
        EXPECT(Copy(disc.MakeSource(), folder).outcome == CopyResult::Outcome::Done);
        fs::resize_file(folder / "zzz.dat", 100);
        Status shorter = Inspect(folder);
        EXPECT(shorter.state == State::Damaged && shorter.missing == 1 && shorter.problem.find("zzz.dat") != std::string::npos);
        // Copying again repairs it, reading only the damaged file.
        disc.reads.clear();
        EXPECT(Copy(disc.MakeSource(), folder).outcome == CopyResult::Outcome::Done);
        EXPECT(disc.reads.size() == 1 && disc.reads.count("zzz.dat") == 1);
        EXPECT(Inspect(folder).state == State::Complete);
        fs::remove(folder / "media/sounds/music.xwb");
        EXPECT(Inspect(folder).state == State::Damaged);
        { std::ofstream(folder / ManifestName, std::ios::trunc) << "garbage\n"; }
        EXPECT(Inspect(folder).state == State::Damaged);
        fs::remove(folder / ManifestName);
        EXPECT(Inspect(folder).state == State::None);
    }

    // A folder without default.xex is not a game.
    {
        fs::create_directories(scratch / "empty");
        DiscFileSystem reader;
        EXPECT(!reader.Open((scratch / "empty").string().c_str()));
    }

    if (g_failures == 0)
        printf("Game data copy: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
