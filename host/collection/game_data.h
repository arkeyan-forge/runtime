#pragma once

// "Copy the game data": the disc's files copied once into the game's folder (<data>/<Game>/gamedata/), so the disc image is no
// longer needed to play, update or reinstall (docs/runtime/host/game_data.md). Plain C++ (no SDL, no vfs), shared with
// Skylauncher (tools/sync_skylauncher.sh): the launcher makes the copy, the game reads it.
//
// The folder holds the disc's files under their lowercase disc paths, and gamedata.manifest: the disc's default.xex hash, the
// list of files with their sizes, and whether the copy finished. A file is copied to "<name>.part", read back and compared
// with the disc, then renamed: a file that is there with its size is a verified one, so a copy that was interrupted resumes
// with the files it still lacks.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gamedata
{
    inline constexpr char ManifestName[] = "gamedata.manifest";

    enum class State
    {
        None,     // no copy
        Partial,  // started, not finished (interrupted, or cancelled): Copy resumes it
        Complete, // every file is there with its size: the game reads it in preference to the disc
        Damaged,  // it was complete, and a file is missing or has another size now (or the manifest can't be read)
    };

    struct Status
    {
        State state = State::None;
        uint64_t total = 0;   // bytes of all the files, from the manifest
        uint64_t present = 0; // bytes of the files that are there with their size
        size_t files = 0;
        size_t missing = 0;   // files not there, or with another size
        std::string xexSha256; // the default.xex of the disc the copy was made from
        std::string problem;   // Damaged: the first thing wrong
    };

    // <data>/<Game>/gamedata for a game id ("ssa", "giants").
    std::filesystem::path Folder(std::string_view gameId);
    // Reads the manifest and looks at each file's size (no file is read): cheap enough for every start.
    Status Inspect(const std::filesystem::path& folder);

    struct SourceFile
    {
        std::string path; // relative to the disc root, '/' separators, lowercase
        uint64_t size = 0;
    };
    // What to copy from. `read` fills `buffer` with up to `size` bytes at `position` of the file and returns how many it read.
    struct Source
    {
        std::vector<SourceFile> files;
        std::function<size_t(const SourceFile&, uint64_t position, void* buffer, size_t size)> read;
        std::string xexSha256; // recorded in the manifest
    };

    struct CopyOptions
    {
        int64_t freeBytes = -1;                   // the room to assume; < 0: ask the file system (tests)
        uint64_t reserve = 64ull << 20;           // kept free besides the files
        const std::atomic<bool>* cancel = nullptr; // set to stop between two chunks
        std::function<void(uint64_t done, uint64_t total)> progress; // bytes of the whole copy, from any thread
    };
    struct CopyResult
    {
        enum class Outcome
        {
            Done,
            NotEnoughSpace,
            Cancelled,
            Failed
        } outcome = Outcome::Failed;
        std::string error;      // Failed: what went wrong
        uint64_t needed = 0;    // NotEnoughSpace: the bytes still to write (and the reserve)
        uint64_t available = 0; // and the bytes free
    };
    // Copies (or finishes copying) `source` into `folder`; see the header comment.
    CopyResult Copy(const Source& source, const std::filesystem::path& folder, const CopyOptions& options = {});

    // Deletes the copy (the folder, if it holds a manifest). False with `error` when nothing could be removed.
    bool Remove(const std::filesystem::path& folder, std::string& error);
}
