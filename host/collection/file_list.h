#pragma once

// The list of a folder's files and their SHA-256, and the check that they are all still there, unchanged
// (docs/release/launcher/kit_contract.md, "File list"). The player kit carries one (tools/make_kit.sh); the launcher writes
// one into a game's folder when its install ends. An antivirus that quarantines or "cleans" one of these files is then
// named to the player instead of breaking a build or a game later. Plain C++ (no SDL), shared with Skylauncher
// (tools/sync_skylauncher.sh).
//
// The list, files.sha256, is what `shasum -a 256` prints: one "<64 lowercase hex>  <path>" line per file, paths relative to
// the folder with '/' on every platform, UTF-8; empty lines and lines starting with '#' are skipped. Files that are not in
// the list are ignored (settings, saves, caches, logs).
//
// A full check reads every byte. To keep the launcher's start quick, a check leaves files.sha256.verified next to the list:
// each file's hash, size and modification time once it matched. A file whose size and time are still the ones recorded
// there, for the same listed hash, is taken as unchanged without being read; any other file is hashed again.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace filelist
{
    inline constexpr char ListName[] = "files.sha256";
    inline constexpr char VerifiedName[] = "files.sha256.verified";

    struct Entry
    {
        std::string sha256; // lowercase hex
        std::string path;   // relative, '/' separated, UTF-8
    };

    // SHA-256 of a file as lowercase hex; empty when it can't be read (or `cancel` was set).
    std::string Sha256OfFile(const std::filesystem::path& file, const std::atomic<bool>* cancel = nullptr);

    // False when a line is not "<hash>  <path>" or a path leaves the folder ("..", absolute, '\\', ':').
    bool Parse(std::string_view text, std::vector<Entry>& entries);
    std::string Format(const std::vector<Entry>& entries);

    // Hashes `paths` (relative to `folder`, '/' separated) and writes folder/files.sha256 and its verified record.
    // False, and `error` says why, when a file can't be read or the list can't be written.
    bool Write(const std::filesystem::path& folder, const std::vector<std::string>& paths, std::string& error);

    struct Problem
    {
        enum class Kind
        {
            Missing,
            Changed
        };
        Kind kind;
        std::string path; // as listed; files.sha256 itself when the list can't be read
    };

    struct Result
    {
        bool listed = false; // the folder has a list: false for a kit or a game from before lists (nothing was checked)
        bool cancelled = false;
        std::vector<Problem> problems;
        size_t files = 0; // listed
        size_t hashed = 0; // read in full (the rest matched the verified record)
        uint64_t hashedBytes = 0;
    };

    // Checks folder against its files.sha256, then updates the verified record (best effort: a read-only folder is checked
    // in full every time).
    Result Check(const std::filesystem::path& folder, const std::atomic<bool>* cancel = nullptr);
}
