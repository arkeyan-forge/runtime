#pragma once

// Saves that cannot be lost: atomic save writes, the previous versions kept as backups, and restoring one of them.
// Shared by the game and Skylauncher (tools/sync_collection.sh, next to datadir); plain C++ and the C library only, so
// no SDL and no game data.
//
// Layout, for a saves folder <saves> (<root>/SSA/saves, or SKY_SAVE_DIR):
//
//   <saves>/<type>/<name>/GLOBAL.DAT   the game's saves, as the title writes them
//   <saves>-backup/<time>/GLOBAL.DAT   the previous version of every file the game overwrote, same relative paths
//   <saves>-backup/game.lock           the running game's process id (restoring is refused while it runs)
//
// <time> is "YYYYMMDD-HHMMSS" in local time, with "-2", "-3"... when the same file changes twice in one second.
//
// Writing: the game writes a save file through TempPath(target), syncs it there and calls CommitFile(), which keeps the
// old file as a backup and renames the temporary over it. A write that is interrupted (crash, power loss, kill) leaves
// the old file exactly as it was; a half-written save can never be seen.
//
// Backups: the last 10, plus the newest one of each of the last 7 days (Rotate), and one log line per backup
// ("[saves] backup <time>") printed by the caller.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace savebackup
{
    // The game's saves folder: SKY_SAVE_DIR when set, else <root>/<Game>/saves. Empty when there is no data folder.
    std::filesystem::path Root(std::string_view game = "ssa");
    // "<saves>-backup" for a saves folder ("<root>/SSA/saves" -> "<root>/SSA/saves-backup"). Root() when `saveDir` is
    // empty.
    std::filesystem::path BackupRoot(const std::filesystem::path& saveDir = {});
    std::filesystem::path LockFile(const std::filesystem::path& saveDir = {});

    // A "<time>" for now, in local time.
    std::string Now();
    // True for the folder names Rotate and Restore accept: "YYYYMMDD-HHMMSS" and an optional "-<n>".
    bool IsStamp(std::string_view stamp);

    // --- Atomic files ------------------------------------------------------------------------------------------------
    //
    // The temporary sibling of `target`, in the same folder (a rename never crosses volumes).
    std::filesystem::path TempPath(const std::filesystem::path& target);
    // Flushes `file` to the disk (fsync; FlushFileBuffers' equivalent).
    bool SyncFile(const std::filesystem::path& file);
    // The parent folder's entry for `file` goes to the disk too (POSIX; a no-op on Windows, where the rename is
    // MOVEFILE_WRITE_THROUGH).
    bool SyncFolder(const std::filesystem::path& folder);
    // Renames `temp` over `target`, replacing it (MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
    // on Windows).
    bool ReplaceFile(const std::filesystem::path& temp, const std::filesystem::path& target);
    // Writes `size` bytes to `target` atomically: a temporary sibling, synced, then renamed over it.
    bool WriteFileAtomic(const std::filesystem::path& target, const void* data, std::size_t size);
    // Copies `from` over `to` atomically (same temporary + rename).
    bool CopyFileAtomic(const std::filesystem::path& from, const std::filesystem::path& to);
    // The bytes of a file; empty when it can't be read (the caller checks with Exists).
    std::string ReadFile(const std::filesystem::path& file);
    bool SameContent(const std::filesystem::path& a, const std::filesystem::path& b);

    // --- Backups -----------------------------------------------------------------------------------------------------

    struct Backup
    {
        bool ok = false;     // false when the copy failed (a full disk): the caller must not overwrite the file
        bool copied = false; // false when the folder already held these bytes: nothing was written
        std::string stamp;   // the "<time>" folder, "" when it failed
        std::filesystem::path path;
        std::string error;
    };
    // Keeps the current `file` in <saves>-backup/<time>/<path relative to saveDir>, then rotates. `stamp` empty means
    // now; `rotate` false leaves rotation to the caller (Restore, which may still be reading an old backup).
    Backup BackupFile(const std::filesystem::path& saveDir, const std::filesystem::path& file, const std::string& stamp = {},
        bool rotate = true);
    // Every file under `saveDir` into one backup folder (Restore's safety copy); returns how many were copied.
    int BackupAll(const std::filesystem::path& saveDir, const std::string& stamp = {}, bool rotate = true);
    // Keeps every file under `folder` (inside `saveDir`) that differs from its newest backup, in one new backup folder:
    // the game replaces a whole package on a save. `ok` false means a copy failed and the folder must not be deleted.
    Backup BackupFolder(const std::filesystem::path& saveDir, const std::filesystem::path& folder);
    // Deletes the backup folders that are neither one of the last 10 nor the newest of each of the last 7 days.
    // Returns how many folders went.
    int Rotate(const std::filesystem::path& backupRoot);

    struct Entry
    {
        std::string stamp; // "YYYYMMDD-HHMMSS"
        std::string date;  // "2026-10-25"
        std::string time;  // "14:03:22"
        std::uint64_t bytes = 0;
        int files = 0;
    };
    // The backups of a saves folder, newest first. Missing folder: none.
    std::vector<Entry> List(const std::filesystem::path& saveDir = {});

    struct Commit
    {
        bool ok = false;
        bool backedUp = false; // the previous version went to <saves>-backup/<backupStamp>
        std::string backupStamp;
        std::string error;
    };
    // Puts `temp` in place of `target`: syncs it, keeps the old target as a backup when its bytes differ, then renames.
    // An identical `temp` is dropped without a rename or a backup.
    Commit CommitFile(const std::filesystem::path& temp, const std::filesystem::path& target, const std::filesystem::path& saveDir);

    struct Restore
    {
        bool ok = false;
        std::string error;      // why it was refused (the game is running, no such backup, a copy failed)
        std::string safetyStamp; // the backup Restore made of the current saves before copying
        int files = 0;
    };
    // Backs up the current saves, then copies <saves>-backup/<stamp> back over them. Refused while the game runs.
    Restore RestoreBackup(const std::filesystem::path& saveDir, const std::string& stamp);

    // --- The running game's marker -----------------------------------------------------------------------------------

    // The marker of a running game: <saves>-backup/game.lock with its process id. GameRunning() also drops a marker
    // whose process is gone (a crash), and a missing or unreadable marker is not running. SetGameRunning(true) writes
    // the current process, false removes it.
    bool GameRunning(const std::filesystem::path& saveDir = {});
    void SetGameRunning(bool running, const std::filesystem::path& saveDir = {});
}
