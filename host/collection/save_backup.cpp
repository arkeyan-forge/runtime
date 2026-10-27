#include "save_backup.h"
#include "datadir.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace savebackup
{
    namespace
    {
        constexpr std::size_t StampLength = 15; // YYYYMMDD-HHMMSS
        constexpr std::size_t KeepCount = 10;   // the last 10 backups...
        constexpr int KeepDays = 7;             // ...plus the newest one of each of the last 7 days

        fs::path Resolve(const fs::path& saveDir)
        {
            return saveDir.empty() ? Root() : saveDir;
        }

        std::string LocalStamp(std::time_t now)
        {
            std::tm parts{};
#if defined(_WIN32)
            localtime_s(&parts, &now);
#else
            localtime_r(&now, &parts);
#endif
            char text[32] = {};
            std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &parts);
            return text;
        }

        // "20261025-140322" -> "2026-10-25" and "14:03:22".
        void SplitStamp(const std::string& stamp, std::string& date, std::string& time)
        {
            if (stamp.size() >= StampLength)
            {
                date = stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2);
                time = stamp.substr(9, 2) + ":" + stamp.substr(11, 2) + ":" + stamp.substr(13, 2);
            }
            else
            {
                date = stamp;
                time.clear();
            }
        }

        // A file name under the saves folder, or nothing when it is outside it (".", "..").
        bool RelativeUnder(const fs::path& file, const fs::path& folder, fs::path& relative)
        {
            std::error_code error;
            relative = fs::relative(file, folder, error);
            if (error || relative.empty() || relative.is_absolute())
                return false;
            for (const fs::path& part : relative)
                if (part == "..")
                    return false;
            return true;
        }

        bool WriteStream(const fs::path& target, const fs::path& temp)
        {
            std::error_code error;
            fs::create_directories(target.parent_path(), error);
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            std::ifstream in(target, std::ios::binary);
            if (!in)
                return false;
            out << in.rdbuf();
            out.close();
            return !out.fail();
        }
    }

    fs::path Root(std::string_view game)
    {
        if (const char* directory = std::getenv("SSA_SAVE_DIR"); directory != nullptr && *directory != '\0')
            return datadir::FromText(directory);

        fs::path path = datadir::Game(game);
        return path.empty() ? path : path / "saves";
    }

    fs::path BackupRoot(const fs::path& saveDir)
    {
        fs::path saves = Resolve(saveDir);
        if (saves.empty())
            return {};
        fs::path root = saves;
        root += "-backup";
        return root;
    }

    fs::path LockFile(const fs::path& saveDir)
    {
        fs::path root = BackupRoot(saveDir);
        return root.empty() ? root : root / "game.lock";
    }

    std::string Now()
    {
        return LocalStamp(std::time(nullptr));
    }

    bool IsStamp(std::string_view stamp)
    {
        if (stamp.size() < StampLength)
            return false;
        for (std::size_t i = 0; i < StampLength; i++)
            if (i == 8 ? stamp[i] != '-' : stamp[i] < '0' || stamp[i] > '9')
                return false;
        if (stamp.size() == StampLength)
            return true;
        // "-<n>": the same second, again.
        if (stamp[StampLength] != '-')
            return false;
        if (stamp.size() == StampLength + 1)
            return false;
        return std::all_of(stamp.begin() + std::ptrdiff_t(StampLength + 1), stamp.end(),
            [](char c) { return c >= '1' && c <= '9'; });
    }

    // --- Atomic files ------------------------------------------------------------------------------------------------

    fs::path TempPath(const fs::path& target)
    {
        fs::path name = target.filename();
        name += ".savetmp";
        return target.parent_path() / name;
    }

    bool SyncFile(const fs::path& file)
    {
#if defined(_WIN32)
        int fd = _wopen(file.c_str(), _O_RDWR | _O_BINARY);
        if (fd < 0)
            fd = _wopen(file.c_str(), _O_RDONLY | _O_BINARY);
        if (fd < 0)
            return false;
        const bool ok = _commit(fd) == 0;
        _close(fd);
        return ok;
#else
        int fd = ::open(file.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        const bool ok = ::fsync(fd) == 0;
        ::close(fd);
        return ok;
#endif
    }

    bool SyncFolder(const fs::path& folder)
    {
#if defined(_WIN32)
        (void)folder; // MOVEFILE_WRITE_THROUGH covers it
        return true;
#else
        int fd = ::open(folder.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        const bool ok = ::fsync(fd) == 0;
        ::close(fd);
        return ok;
#endif
    }

    bool ReplaceFile(const fs::path& temp, const fs::path& target)
    {
#if defined(_WIN32)
        return MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        if (::rename(temp.c_str(), target.c_str()) != 0)
            return false;
        return SyncFolder(target.parent_path());
#endif
    }

    bool WriteFileAtomic(const fs::path& target, const void* data, std::size_t size)
    {
        std::error_code error;
        fs::create_directories(target.parent_path(), error);
        fs::path temp = TempPath(target);
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(static_cast<const char*>(data), std::streamsize(size));
            out.close();
            if (out.fail())
            {
                fs::remove(temp, error);
                return false;
            }
        }
        if (!SyncFile(temp) || !ReplaceFile(temp, target))
        {
            fs::remove(temp, error);
            return false;
        }
        return true;
    }

    std::string ReadFile(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return {};
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool SameContent(const fs::path& a, const fs::path& b)
    {
        std::error_code error;
        if (!fs::exists(a, error) || !fs::exists(b, error) || fs::file_size(a, error) != fs::file_size(b, error) || error)
            return false;
        return ReadFile(a) == ReadFile(b);
    }

    bool CopyFileAtomic(const fs::path& from, const fs::path& to)
    {
        std::error_code error;
        fs::path temp = TempPath(to);
        fs::create_directories(to.parent_path(), error);
        if (!WriteStream(from, temp))
        {
            fs::remove(temp, error);
            return false;
        }
        if (!SyncFile(temp) || !ReplaceFile(temp, to))
        {
            fs::remove(temp, error);
            return false;
        }
        return true;
    }

    // --- Backups -----------------------------------------------------------------------------------------------------

    int Rotate(const fs::path& backupRoot)
    {
        std::vector<std::pair<std::string, fs::path>> folders;
        std::error_code error;
        if (!fs::is_directory(backupRoot, error))
            return 0;
        for (fs::directory_iterator it(backupRoot, error), end; !error && it != end; it.increment(error))
        {
            std::error_code statusError;
            const std::string name = it->path().filename().string();
            if (it->is_directory(statusError) && IsStamp(name))
                folders.emplace_back(name, it->path());
        }
        if (folders.empty())
            return 0;

        // Newest first; the fixed-width stamp makes the names sort by time, the "-<n>" suffix last.
        std::sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

        std::set<std::string> keep;
        for (std::size_t i = 0; i < folders.size() && i < KeepCount; i++)
            keep.insert(folders[i].first);
        std::set<std::string> days;
        for (const auto& [stamp, path] : folders)
        {
            const std::string day = stamp.substr(0, 8);
            if (days.size() < std::size_t(KeepDays) && !days.contains(day))
            {
                days.insert(day);
                keep.insert(stamp);
            }
        }

        int removed = 0;
        for (const auto& [stamp, path] : folders)
        {
            if (keep.contains(stamp))
                continue;
            fs::remove_all(path, error);
            if (!error)
                removed++;
        }
        return removed;
    }

    Backup BackupFile(const fs::path& saveDir, const fs::path& file, const std::string& stamp, bool rotate)
    {
        Backup backup;
        const fs::path saves = Resolve(saveDir);
        if (saves.empty())
        {
            backup.error = "no saves folder";
            return backup;
        }

        fs::path relative;
        if (!RelativeUnder(file, saves, relative))
        {
            backup.error = "the file is outside the saves folder";
            return backup;
        }

        const std::string time = stamp.empty() ? Now() : stamp;
        const fs::path root = BackupRoot(saves);

        // The same file twice in one second goes to "-2", "-3"... so no earlier version is overwritten.
        fs::path destination;
        for (int attempt = 0; attempt < 99; attempt++)
        {
            const std::string name = attempt == 0 ? time : time + "-" + std::to_string(attempt + 1);
            destination = root / name / relative;
            if (!fs::exists(destination))
            {
                backup.stamp = name;
                break;
            }
            if (SameContent(file, destination))
            {
                backup.ok = true;
                backup.copied = false;
                backup.stamp = name;
                backup.path = destination;
                if (rotate)
                    Rotate(root);
                return backup;
            }
        }
        if (backup.stamp.empty())
        {
            backup.error = "no free backup name";
            return backup;
        }

        if (!CopyFileAtomic(file, destination))
        {
            backup.stamp.clear();
            backup.error = "could not write " + destination.string();
            return backup;
        }
        backup.ok = true;
        backup.copied = true;
        backup.path = destination;
        if (rotate)
            Rotate(root);
        return backup;
    }

    int BackupAll(const fs::path& saveDir, const std::string& stamp, bool rotate)
    {
        const fs::path saves = Resolve(saveDir);
        const std::string time = stamp.empty() ? Now() : stamp;
        std::error_code error;
        int copied = 0;
        if (!fs::is_directory(saves, error))
            return 0;
        for (fs::recursive_directory_iterator it(saves, error), end; !error && it != end; it.increment(error))
        {
            std::error_code statusError;
            if (!it->is_regular_file(statusError))
                continue;
            if (it->path().extension() == ".savetmp") // a write that was in flight
                continue;
            Backup backup = BackupFile(saves, it->path(), time, false);
            if (backup.copied)
                copied++;
        }
        if (rotate)
            Rotate(BackupRoot(saves));
        return copied;
    }

    Backup BackupFolder(const fs::path& saveDir, const fs::path& folder)
    {
        Backup result;
        result.ok = true;
        const fs::path saves = Resolve(saveDir);
        std::error_code error;
        if (saves.empty() || !fs::is_directory(folder, error))
            return result;

        const std::string time = Now();
        const std::vector<Entry> entries = List(saves);
        for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        {
            std::error_code statusError;
            if (!it->is_regular_file(statusError) || it->path().extension() == ".savetmp")
                continue;
            fs::path relative;
            if (!RelativeUnder(it->path(), saves, relative))
                continue;

            // The game rewrites a whole package on every save: a file that is the newest backup's bytes again is not a
            // new version.
            bool same = false;
            for (const Entry& entry : entries)
            {
                const fs::path previous = BackupRoot(saves) / entry.stamp / relative;
                if (fs::exists(previous, statusError))
                {
                    same = SameContent(previous, it->path());
                    break;
                }
            }
            if (same)
                continue;

            Backup backup = BackupFile(saves, it->path(), time, false);
            if (!backup.ok)
            {
                result.ok = false;
                result.error = backup.error;
                return result;
            }
            if (backup.copied && result.stamp.empty())
                result.stamp = backup.stamp;
            result.copied = result.copied || backup.copied;
        }
        if (result.copied)
            Rotate(BackupRoot(saves));
        return result;
    }

    std::vector<Entry> List(const fs::path& saveDir)
    {
        std::vector<Entry> entries;
        const fs::path root = BackupRoot(saveDir);
        std::error_code error;
        if (root.empty() || !fs::is_directory(root, error))
            return entries;
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
        {
            std::error_code statusError;
            const std::string name = it->path().filename().string();
            if (!it->is_directory(statusError) || !IsStamp(name))
                continue;
            Entry entry;
            entry.stamp = name;
            SplitStamp(name, entry.date, entry.time);
            std::error_code walkError;
            for (fs::recursive_directory_iterator file(it->path(), walkError), fileEnd; !walkError && file != fileEnd; file.increment(walkError))
            {
                std::error_code sizeError;
                if (!file->is_regular_file(sizeError))
                    continue;
                entry.bytes += fs::file_size(file->path(), sizeError);
                entry.files++;
            }
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.stamp > b.stamp; });
        return entries;
    }

    Commit CommitFile(const fs::path& temp, const fs::path& target, const fs::path& saveDir)
    {
        Commit commit;
        std::error_code error;
        if (!fs::is_regular_file(temp, error))
        {
            commit.error = "the temporary file is missing";
            return commit;
        }
        if (!SyncFile(temp))
        {
            commit.error = "could not sync the temporary file";
            return commit;
        }

        const fs::path saves = Resolve(saveDir);
        if (fs::is_regular_file(target, error))
        {
            if (SameContent(temp, target))
            {
                fs::remove(temp, error); // nothing changed: no rename, no backup
                commit.ok = true;
                return commit;
            }
            // The old version is kept before it is replaced; without a backup the save is not overwritten.
            if (saves.empty())
            {
                commit.error = "no saves folder to back up to";
                return commit;
            }
            Backup backup = BackupFile(saves, target);
            if (!backup.ok)
            {
                commit.error = backup.error;
                return commit;
            }
            if (backup.copied)
            {
                commit.backedUp = true;
                commit.backupStamp = backup.stamp;
            }
        }

        if (!ReplaceFile(temp, target))
        {
            commit.error = "could not replace " + target.string();
            return commit;
        }
        commit.ok = true;
        return commit;
    }

    // --- The running game's marker -----------------------------------------------------------------------------------

    bool GameRunning(const fs::path& saveDir)
    {
        const fs::path lock = LockFile(saveDir);
        if (lock.empty())
            return false;
        std::error_code error;
        if (!fs::exists(lock, error))
            return false;

        const std::string text = ReadFile(lock);
        const unsigned long pid = std::strtoul(text.c_str(), nullptr, 10);
        if (pid == 0)
            return true; // just created by a game that has not written its id yet

        bool alive = false;
#if defined(_WIN32)
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
        if (process != nullptr)
        {
            DWORD code = 0;
            alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
            CloseHandle(process);
        }
        else if (GetLastError() == ERROR_ACCESS_DENIED)
        {
            alive = true; // another user's process
        }
#else
        alive = ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
        if (!alive)
        {
            fs::remove(lock, error); // a game that crashed
            return false;
        }
        return true;
    }

    void SetGameRunning(bool running, const fs::path& saveDir)
    {
        const fs::path lock = LockFile(saveDir);
        if (lock.empty())
            return;
        std::error_code error;
        if (!running)
        {
            fs::remove(lock, error);
            return;
        }
        fs::create_directories(lock.parent_path(), error);
        char text[32];
        std::snprintf(text, sizeof(text), "%ld\n", long(
#if defined(_WIN32)
            GetCurrentProcessId()
#else
            getpid()
#endif
            ));
        WriteFileAtomic(lock, text, std::strlen(text));
    }

    // --- Restore -----------------------------------------------------------------------------------------------------

    Restore RestoreBackup(const fs::path& saveDir, const std::string& stamp)
    {
        Restore restore;
        const fs::path saves = Resolve(saveDir);
        if (saves.empty())
        {
            restore.error = "no saves folder";
            return restore;
        }
        if (!IsStamp(stamp))
        {
            restore.error = "there is no backup " + stamp;
            return restore;
        }
        if (GameRunning(saves))
        {
            restore.error = "the game is running";
            return restore;
        }

        const fs::path root = BackupRoot(saves);
        const fs::path source = root / stamp;
        std::error_code error;
        if (!fs::is_directory(source, error))
        {
            restore.error = "there is no backup " + stamp;
            return restore;
        }

        // The current saves are kept first, in a folder of their own (never the one being restored).
        std::string safety = Now();
        for (int attempt = 0; attempt < 99 && (safety == stamp || fs::exists(root / safety)); attempt++)
            safety = Now() + "-" + std::to_string(attempt + 2);
        BackupAll(saves, safety, false);
        restore.safetyStamp = safety;

        for (fs::recursive_directory_iterator it(source, error), end; !error && it != end; it.increment(error))
        {
            std::error_code statusError;
            if (!it->is_regular_file(statusError))
                continue;
            fs::path relative;
            if (!RelativeUnder(it->path(), source, relative))
                continue;
            if (!CopyFileAtomic(it->path(), saves / relative))
            {
                restore.error = "could not copy " + relative.string();
                Rotate(root);
                return restore;
            }
            restore.files++;
        }
        if (error)
        {
            restore.error = "could not read the backup";
            Rotate(root);
            return restore;
        }

        // Rotation only now: it may drop the folder that was just restored from.
        Rotate(root);
        restore.ok = true;
        return restore;
    }
}
