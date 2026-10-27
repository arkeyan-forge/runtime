#include "host/texture_pack.h"
#include "host/collection/datadir.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace texpack
{
    namespace
    {
        struct Dumped
        {
            uint64_t hash;
            dds::Format format;
            uint32_t width, height;
            std::vector<uint8_t> data;
        };

        struct State
        {
            std::filesystem::path dumpFolder;
            bool dumping = false;
            std::unordered_map<uint64_t, Replacement> index;

            std::mutex mutex; // the dump queue and the sets below
            std::condition_variable wake;
            std::condition_variable idle;
            std::deque<Dumped> queue;
            bool writing = false;
            std::thread writer;
            bool quit = false;
            std::unordered_set<uint64_t> seen;     // hashes ShouldDump() has answered for
            std::set<std::filesystem::path> failed; // replacement files already reported
        } g_state;

        void Writer()
        {
            std::unique_lock lock(g_state.mutex);
            for (;;)
            {
                g_state.wake.wait(lock, [] { return !g_state.queue.empty() || g_state.quit; });
                if (g_state.queue.empty())
                    return;
                Dumped item = std::move(g_state.queue.front());
                g_state.queue.pop_front();
                g_state.writing = true;
                lock.unlock();

                std::error_code error;
                std::filesystem::path path = g_state.dumpFolder / (HashName(item.hash) + ".dds");
                if (!std::filesystem::exists(path, error))
                {
                    std::filesystem::create_directories(g_state.dumpFolder, error);
                    std::span<const uint8_t> level(item.data);
                    std::vector<uint8_t> file = dds::Write(item.format, item.width, item.height, std::span(&level, 1));
                    std::filesystem::path temporary = path;
                    temporary += ".tmp";
                    {
                        std::ofstream out(temporary, std::ios::binary);
                        out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
                        if (!out)
                            fprintf(stderr, "[texpack] cannot write %s\n", datadir::Text(temporary).c_str());
                    }
                    std::filesystem::rename(temporary, path, error);
                }

                lock.lock();
                g_state.writing = false;
                g_state.idle.notify_all();
            }
        }

        bool ParseHashName(const std::string& stem, uint64_t& hash)
        {
            if (stem.size() != 16)
                return false;
            hash = 0;
            for (char c : stem)
            {
                uint32_t digit;
                if (c >= '0' && c <= '9') digit = uint32_t(c - '0');
                else if (c >= 'a' && c <= 'f') digit = uint32_t(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') digit = uint32_t(c - 'A' + 10);
                else return false;
                hash = hash << 4 | digit;
            }
            return true;
        }

        void IndexFolder(const std::filesystem::path& folder)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(folder, error))
                return;
            size_t invalid = 0, ignored = 0;
            for (std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error), end;
                 !error && it != end; it.increment(error))
            {
                if (!it->is_regular_file(error))
                    continue;
                std::filesystem::path path = it->path();
                std::string extension = path.extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(tolower(c)); });
                if (extension != ".dds")
                    continue;
                std::string name = datadir::Text(path.filename());
                uint64_t hash;
                if (!ParseHashName(datadir::Text(path.stem()), hash))
                {
                    fprintf(stderr, "[texpack] ignored %s: the name is not 16 hex digits\n", name.c_str());
                    ignored++;
                    continue;
                }
                std::ifstream in(path, std::ios::binary);
                uint8_t header[dds::MaxHeaderBytes];
                in.read(reinterpret_cast<char*>(header), sizeof(header));
                std::string why;
                Replacement replacement;
                if (!dds::Parse(std::span<const uint8_t>(header, size_t(in.gcount())), std::filesystem::file_size(path, error), replacement.info, why))
                {
                    fprintf(stderr, "[texpack] ignored %s: %s (the game's texture stays)\n", name.c_str(), why.c_str());
                    invalid++;
                    continue;
                }
                replacement.path = path;
                g_state.index[hash] = std::move(replacement);
            }
            fprintf(stderr, "[texpack] %zu replacement textures in %s (%zu invalid, %zu ignored)\n", g_state.index.size(),
                datadir::Text(folder).c_str(), invalid, ignored);
        }
    }

    uint64_t ContentHash(uint32_t format, uint32_t width, uint32_t height, const void* data, size_t size)
    {
        return XXH3_64bits_withSeed(data, size, uint64_t(format) | uint64_t(width) << 8 | uint64_t(height) << 24);
    }

    std::string HashName(uint64_t hash)
    {
        char name[17];
        snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
        return name;
    }

    void Configure(const std::filesystem::path& dump, bool dumping, const std::filesystem::path& packs, bool replacing)
    {
        g_state.dumpFolder = dump;
        g_state.dumping = dumping;
        if (dumping)
        {
            fprintf(stderr, "[texpack] dumping textures to %s\n", datadir::Text(dump).c_str());
            g_state.writer = std::thread(Writer);
            atexit(Flush);
        }
        if (replacing)
            IndexFolder(packs);
    }

    void ConfigureForGame(std::string_view gameId, bool dumping, bool replacing)
    {
        std::filesystem::path game = datadir::Game(gameId);
        if (game.empty())
            return;
        Configure(game / "textures-dump", dumping, game / "textures", replacing);
    }

    bool Dumping()
    {
        return g_state.dumping;
    }

    bool Active()
    {
        return g_state.dumping || !g_state.index.empty();
    }

    const Replacement* Find(uint64_t hash)
    {
        auto it = g_state.index.find(hash);
        return it != g_state.index.end() ? &it->second : nullptr;
    }

    bool ShouldDump(uint64_t hash)
    {
        if (!g_state.dumping)
            return false;
        std::lock_guard lock(g_state.mutex);
        return g_state.seen.insert(hash).second;
    }

    void Dump(uint64_t hash, dds::Format format, uint32_t width, uint32_t height, std::vector<uint8_t>&& data)
    {
        std::lock_guard lock(g_state.mutex);
        g_state.queue.push_back({ hash, format, width, height, std::move(data) });
        g_state.wake.notify_one();
    }

    bool ReadLevel(const Replacement& replacement, uint32_t level, std::vector<uint8_t>& out)
    {
        const dds::Info& info = replacement.info;
        out.resize(dds::SizeOfLevel(info.format, info.width, info.height, level).Bytes());
        std::ifstream in(replacement.path, std::ios::binary);
        in.seekg(std::streamoff(dds::LevelOffset(info, level)));
        in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
        if (!in)
        {
            std::lock_guard lock(g_state.mutex);
            if (g_state.failed.insert(replacement.path).second)
                fprintf(stderr, "[texpack] cannot read %s: the texture is left black\n", datadir::Text(replacement.path).c_str());
            std::fill(out.begin(), out.end(), 0);
            return false;
        }
        dds::ConvertTexels(info, out.data(), out.size());
        return true;
    }

    void Flush()
    {
        if (!g_state.writer.joinable())
            return;
        {
            std::unique_lock lock(g_state.mutex);
            g_state.idle.wait(lock, [] { return g_state.queue.empty() && !g_state.writing; });
            g_state.quit = true;
            g_state.wake.notify_all();
        }
        g_state.writer.join();
    }
}
