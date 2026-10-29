// Unit test of the save backup library (save_backup.h): atomic writes, the backup rotation, restoring, the running
// game's marker. Every path is in a scratch folder of its own; the player's folders are never touched.
//   SSASavesTest          Exit code 0 = pass.
#include "host/collection/save_backup.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace savebackup;

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

    fs::path g_scratch;

    void Write(const fs::path& path, const std::string& content)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    }

    std::string Read(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    bool HasStamp(const std::vector<Entry>& entries, const std::string& stamp)
    {
        return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.stamp == stamp; });
    }

    void AtomicChecks()
    {
        const fs::path saves = g_scratch / "atomic" / "saves";
        fs::create_directories(saves);
        const fs::path save = saves / "00000002" / "Skylanders - Slot 1" / "SLOT.DAT";
        Write(save, "previous");

        // A write that is interrupted: the temporary holds half of it, the save is still the old one, byte for byte.
        const fs::path temp = TempPath(save);
        Write(temp, "half");
        CHECK(Read(save) == "previous");
        CHECK(Read(temp) == "half");

        // Its commit puts the new bytes in place and keeps the old ones beside it.
        Commit commit = CommitFile(temp, save, saves);
        CHECK(commit.ok);
        CHECK(commit.backedUp && IsStamp(commit.backupStamp));
        CHECK(Read(save) == "half");
        CHECK(!fs::exists(temp));
        CHECK(Read(BackupRoot(saves) / commit.backupStamp / "00000002" / "Skylanders - Slot 1" / "SLOT.DAT") ==
              "previous");

        // Writing the same bytes again is not a new version: no backup, no rename.
        Write(temp, "half");
        commit = CommitFile(temp, save, saves);
        CHECK(commit.ok && !commit.backedUp);
        CHECK(!fs::exists(temp));

        // A whole file written atomically.
        const fs::path other = saves / "00000002" / "Skylanders - Slot 2" / "SLOT.NFO";
        CHECK(WriteFileAtomic(other, "abc", 3));
        CHECK(Read(other) == "abc");
        CHECK(!fs::exists(TempPath(other)));

        // No temporary is left behind by a failed read of the old version.
        CHECK(WriteFileAtomic(saves / "GLOBAL.DAT", "x", 1));
        CHECK(Read(saves / "GLOBAL.DAT") == "x");
        printf("Atomic: an interrupted write leaves the old save intact; a commit replaces it and keeps the old one\n");
    }

    void RotationChecks()
    {
        const fs::path saves = g_scratch / "rotation" / "saves";
        const fs::path save = saves / "00000002" / "Skylanders - Global Data" / "GLOBAL.DAT";
        Write(save, "one version");

        // Three saves a day for ten days.
        for (int day = 1; day <= 10; day++)
            for (int hour = 10; hour <= 12; hour++)
            {
                char stamp[32];
                std::snprintf(stamp, sizeof(stamp), "202610%02d-%02d0000", day, hour);
                Backup backup = BackupFile(saves, save, stamp, false);
                CHECK(backup.ok && backup.copied);
            }
        CHECK(List(saves).size() == 30);

        // The last 10, plus the newest of each of the last 7 days: days 1 to 3 go entirely, and of days 4 to 6 only
        // their newest is kept (days 7 to 10 are inside the last 10).
        CHECK(Rotate(BackupRoot(saves)) == 17);
        std::vector<Entry> entries = List(saves);
        CHECK(entries.size() == 13);
        CHECK(!HasStamp(entries, "20261003-120000"));
        CHECK(!HasStamp(entries, "20261004-100000") && HasStamp(entries, "20261004-120000"));
        CHECK(!HasStamp(entries, "20261006-110000") && HasStamp(entries, "20261006-120000"));
        CHECK(!HasStamp(entries, "20261007-100000") && HasStamp(entries, "20261007-120000"));
        CHECK(HasStamp(entries, "20261008-110000"));
        CHECK(HasStamp(entries, "20261010-120000"));
        // Newest first, with its date, time and size.
        CHECK(entries.front().stamp == "20261010-120000");
        CHECK(entries.front().date == "2026-10-10" && entries.front().time == "12:00:00");
        CHECK(entries.front().bytes == std::string("one version").size() && entries.front().files == 1);

        // A second change in the same second gets a folder of its own: the first version is never overwritten.
        Write(save, "two version");
        Backup first = BackupFile(saves, save, "20261010-130000", false);
        Write(save, "three version");
        Backup second = BackupFile(saves, save, "20261010-130000", false);
        CHECK(first.ok && second.ok && first.stamp == "20261010-130000" && second.stamp == "20261010-130000-2");
        CHECK(Read(first.path) == "two version" && Read(second.path) == "three version");
        printf("Rotation: the last 10 plus the newest of each of the last 7 days, a second change in its own folder\n");
    }

    void RestoreChecks()
    {
        const fs::path saves = g_scratch / "restore" / "saves";
        const fs::path global = saves / "00000002" / "Skylanders - Global Data" / "GLOBAL.DAT";
        const fs::path slot = saves / "00000002" / "Skylanders - Slot 1" / "SLOT.DAT";
        Write(global, std::string("global\0binary", 13));
        Write(slot, "slot one");

        // The version the game keeps, then a newer save on top of it.
        Backup backup = BackupFile(saves, global, "20261025-120000", false);
        CHECK(backup.ok && backup.copied);
        CHECK(BackupFile(saves, slot, "20261025-120000", false).ok);
        Write(global, "global changed");
        Write(slot, "slot changed");
        Write(saves / "00000002" / "Skylanders - Slot 2" / "SLOT.DAT", "a slot the backup does not have");

        Restore restored = RestoreBackup(saves, "20261025-120000");
        CHECK(restored.ok && restored.files == 2);
        CHECK(Read(global) == std::string("global\0binary", 13));
        CHECK(Read(slot) == "slot one");
        // The saves as they were before the restore are kept in a folder of their own.
        CHECK(IsStamp(restored.safetyStamp) && restored.safetyStamp != "20261025-120000");
        CHECK(Read(BackupRoot(saves) / restored.safetyStamp / "00000002" / "Skylanders - Global Data" / "GLOBAL.DAT") ==
              "global changed");
        CHECK(Read(BackupRoot(saves) / restored.safetyStamp / "00000002" / "Skylanders - Slot 1" / "SLOT.DAT") ==
              "slot changed");
        // A restore does not delete a save the backup does not hold.
        CHECK(Read(saves / "00000002" / "Skylanders - Slot 2" / "SLOT.DAT") == "a slot the backup does not have");

        // Nothing to restore, and a name that is not a backup.
        CHECK(!RestoreBackup(saves, "20260101-000000").ok);
        Restore bad = RestoreBackup(saves, "../saves");
        CHECK(!bad.ok && !bad.error.empty());
        printf("Restore: byte for byte, with the current saves kept first\n");
    }

    void FolderChecks()
    {
        const fs::path saves = g_scratch / "folder" / "saves";
        const fs::path package = saves / "00000001" / "Skylanders - Slot 1";
        Write(package / "SLOT.DAT", "slot v1");
        Write(package / "SLOT.NFO", "info");

        // The game replaces a package: the old files are kept first, once.
        Backup first = BackupFolder(saves, package);
        CHECK(first.ok && first.copied && IsStamp(first.stamp));
        CHECK(Read(BackupRoot(saves) / first.stamp / "00000001" / "Skylanders - Slot 1" / "SLOT.DAT") == "slot v1");
        // The same bytes again are not a new version; a changed file is, alone.
        CHECK(BackupFolder(saves, package).ok && List(saves).size() == 1);
        Write(package / "SLOT.DAT", "slot v2");
        Backup second = BackupFolder(saves, package);
        CHECK(second.ok && second.copied && List(saves).size() == 2);
        CHECK(!fs::exists(BackupRoot(saves) / second.stamp / "00000001" / "Skylanders - Slot 1" / "SLOT.NFO"));
        printf("Folder: a replaced package is kept once, unchanged files are not backed up again\n");
    }

    void RunningChecks()
    {
        const fs::path saves = g_scratch / "running" / "saves";
        const fs::path save = saves / "00000002" / "Skylanders - Global Data" / "GLOBAL.DAT";
        Write(save, "kept");
        CHECK(BackupFile(saves, save, "20261025-120000", false).ok);

        CHECK(!GameRunning(saves));
        SetGameRunning(true, saves);
        CHECK(fs::exists(LockFile(saves)));
        CHECK(GameRunning(saves));
        Restore refused = RestoreBackup(saves, "20261025-120000");
        CHECK(!refused.ok && refused.error.find("running") != std::string::npos);
        CHECK(fs::exists(LockFile(saves))); // a refused restore leaves everything alone
        SetGameRunning(false, saves);
        CHECK(!GameRunning(saves) && !fs::exists(LockFile(saves)));
        CHECK(RestoreBackup(saves, "20261025-120000").ok);

        // A marker left by a game that crashed (a process that is gone) is dropped, not obeyed for ever.
        Write(LockFile(saves), "999999999\n");
        CHECK(!GameRunning(saves));
        CHECK(!fs::exists(LockFile(saves)));
        printf("Running: restore refused while the game's marker is alive, dropped once it is gone\n");
    }
}

int main()
{
    g_scratch = fs::temp_directory_path() / ("SSASavesTest-" + std::to_string(getpid()));
    fs::remove_all(g_scratch);
    fs::create_directories(g_scratch);

    AtomicChecks();
    RotationChecks();
    RestoreChecks();
    FolderChecks();
    RunningChecks();

    fs::remove_all(g_scratch);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
