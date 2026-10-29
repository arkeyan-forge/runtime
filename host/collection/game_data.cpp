#include "game_data.h"
#include "datadir.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace gamedata
{
    namespace
    {
        constexpr size_t ChunkSize = 4u << 20;

        fs::path Join(const fs::path& folder, const std::string& path) { return folder / datadir::FromText(path); }

        // A disc path must stay inside the folder.
        bool Safe(const std::string& path)
        {
            if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
                path.find(':') != std::string::npos)
                return false;
            std::istringstream parts(path);
            for (std::string part; std::getline(parts, part, '/');)
                if (part.empty() || part == "." || part == "..")
                    return false;
            return true;
        }

        struct Manifest
        {
            bool valid = false;
            bool complete = false;
            std::string xex;
            std::vector<SourceFile> files;
        };

        // "gamedata 1", "xex=<hex>", "complete=0|1", then one "file<TAB>size<TAB>path" per file.
        Manifest ReadManifest(const fs::path& folder)
        {
            Manifest manifest;
            std::ifstream in(folder / ManifestName, std::ios::binary);
            std::string line;
            if (!in || !std::getline(in, line) || line != "gamedata 1")
                return manifest;
            bool sawComplete = false;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.starts_with("xex="))
                    manifest.xex = line.substr(4);
                else if (line.starts_with("complete="))
                {
                    manifest.complete = line == "complete=1";
                    sawComplete = true;
                }
                else if (line.starts_with("file\t"))
                {
                    size_t tab = line.find('\t', 5);
                    if (tab == std::string::npos)
                        return manifest;
                    SourceFile file;
                    file.size = std::strtoull(line.c_str() + 5, nullptr, 10);
                    file.path = line.substr(tab + 1);
                    if (!Safe(file.path))
                        return manifest;
                    manifest.files.push_back(std::move(file));
                }
            }
            manifest.valid = sawComplete && !manifest.files.empty();
            return manifest;
        }

        bool WriteManifest(const fs::path& folder, const Source& source, bool complete)
        {
            fs::path temporary = folder / (std::string(ManifestName) + ".part");
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out << "gamedata 1\nxex=" << source.xexSha256 << "\ncomplete=" << (complete ? 1 : 0) << "\n";
                for (const SourceFile& file : source.files)
                    out << "file\t" << file.size << "\t" << file.path << "\n";
                out.flush();
                if (!out)
                    return false;
            }
            std::error_code error;
            fs::rename(temporary, folder / ManifestName, error);
            return !error;
        }

        bool HasSize(const fs::path& path, uint64_t size)
        {
            std::error_code error;
            return fs::is_regular_file(path, error) && fs::file_size(path, error) == size && !error;
        }

        CopyResult Fail(std::string message)
        {
            CopyResult result;
            result.error = std::move(message);
            return result;
        }
    }

    fs::path Folder(std::string_view gameId) { return datadir::Game(gameId) / "gamedata"; }

    Status Inspect(const fs::path& folder)
    {
        Status status;
        std::error_code error;
        if (!fs::exists(folder / ManifestName, error))
            return status;
        Manifest manifest = ReadManifest(folder);
        if (!manifest.valid)
        {
            status.state = State::Damaged;
            status.problem = "the list of files can't be read";
            return status;
        }
        status.xexSha256 = manifest.xex;
        status.files = manifest.files.size();
        for (const SourceFile& file : manifest.files)
        {
            status.total += file.size;
            if (HasSize(Join(folder, file.path), file.size))
                status.present += file.size;
            else
            {
                if (status.missing++ == 0)
                    status.problem = file.path;
            }
        }
        status.state = !manifest.complete ? State::Partial : status.missing == 0 ? State::Complete : State::Damaged;
        if (status.state != State::Damaged)
            status.problem.clear();
        else
            status.problem = "\"" + status.problem + "\" is missing or has changed";
        return status;
    }

    CopyResult Copy(const Source& source, const fs::path& folder, const CopyOptions& options)
    {
        std::error_code error;
        fs::create_directories(folder, error);
        if (error)
            return Fail("couldn't create " + datadir::Text(folder) + ": " + error.message());
        for (const SourceFile& file : source.files)
            if (!Safe(file.path))
                return Fail("the disc has a file name that can't be copied: " + file.path);

        uint64_t total = 0, remaining = 0;
        for (const SourceFile& file : source.files)
        {
            total += file.size;
            if (!HasSize(Join(folder, file.path), file.size))
                remaining += file.size;
        }
        const uint64_t needed = remaining + options.reserve;
        uint64_t available = 0;
        if (options.freeBytes >= 0)
            available = uint64_t(options.freeBytes);
        else
        {
            fs::space_info space = fs::space(folder, error);
            if (error)
                return Fail("couldn't see how much room is left: " + error.message());
            available = space.available;
        }
        if (available < needed)
        {
            CopyResult result;
            result.outcome = CopyResult::Outcome::NotEnoughSpace;
            result.needed = needed;
            result.available = available;
            return result;
        }

        if (!WriteManifest(folder, source, false))
            return Fail("couldn't write " + datadir::Text(folder / ManifestName));

        uint64_t done = total - remaining;
        auto report = [&] {
            if (options.progress)
                options.progress(done, total);
        };
        auto cancelled = [&] { return options.cancel != nullptr && options.cancel->load(); };
        std::vector<uint8_t> chunk(ChunkSize), check(ChunkSize);
        report();
        for (const SourceFile& file : source.files)
        {
            fs::path target = Join(folder, file.path);
            if (HasSize(target, file.size))
                continue;
            if (cancelled())
            {
                CopyResult result;
                result.outcome = CopyResult::Outcome::Cancelled;
                return result;
            }
            fs::create_directories(target.parent_path(), error);
            fs::path part = target;
            part += ".part";
            const uint64_t before = done;

            // Copy, then read the copy back and compare it with the disc.
            bool stopped = false;
            std::string problem;
            {
                std::ofstream out(part, std::ios::binary | std::ios::trunc);
                if (!out)
                    return Fail("couldn't create " + file.path);
                for (uint64_t position = 0; position < file.size && problem.empty() && !stopped;)
                {
                    size_t want = size_t(std::min<uint64_t>(ChunkSize, file.size - position));
                    if (source.read(file, position, chunk.data(), want) != want)
                        problem = "couldn't read " + file.path + " from the disc";
                    else if (!out.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(want)))
                        problem = "couldn't write " + file.path + " (is the disk full?)";
                    position += want;
                    done += want;
                    report();
                    stopped = cancelled();
                }
                out.flush();
                if (!out && problem.empty())
                    problem = "couldn't write " + file.path + " (is the disk full?)";
            }
            if (problem.empty() && !stopped)
            {
                std::ifstream in(part, std::ios::binary);
                for (uint64_t position = 0; position < file.size && problem.empty();)
                {
                    size_t want = size_t(std::min<uint64_t>(ChunkSize, file.size - position));
                    if (!in.read(reinterpret_cast<char*>(check.data()), std::streamsize(want)) ||
                        source.read(file, position, chunk.data(), want) != want ||
                        std::memcmp(check.data(), chunk.data(), want) != 0)
                        problem = "the copy of " + file.path + " differs from the disc";
                    position += want;
                }
            }
            if (!problem.empty() || stopped)
            {
                fs::remove(part, error);
                done = before;
                if (!problem.empty())
                    return Fail(problem);
                CopyResult result;
                result.outcome = CopyResult::Outcome::Cancelled;
                return result;
            }
            fs::remove(target, error); // a file of another size from an earlier copy
            fs::rename(part, target, error);
            if (error)
                return Fail("couldn't put " + file.path + " in place: " + error.message());
        }

        if (!WriteManifest(folder, source, true))
            return Fail("couldn't write " + datadir::Text(folder / ManifestName));
        CopyResult result;
        result.outcome = CopyResult::Outcome::Done;
        return result;
    }

    bool Remove(const fs::path& folder, std::string& error)
    {
        std::error_code code;
        if (!fs::exists(folder / ManifestName, code))
        {
            error = fs::exists(folder, code) ? "that folder isn't a copy of the game data" : std::string();
            return error.empty();
        }
        fs::remove_all(folder, code);
        if (code)
        {
            error = code.message();
            return false;
        }
        return true;
    }
}
