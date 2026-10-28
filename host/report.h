#pragma once

// The "Report a problem" file of the game (docs/runtime/host/gamemenu.md): one text file with the versions, the OS, the CPU,
// the GPU, the settings, the last crash's details, the game's log and the previous run's, and the launcher's game log.
// The format is the launcher's (docs/support.md in Skylauncher): a header, then each file under "===== name =====". The
// home folder and the account and computer names are replaced (Redact), so the file can be sent as it is.
#include <filesystem>
#include <string>

namespace report
{
    // What identifies the player on this machine, to be removed from the text.
    struct Identity
    {
        std::string home;     // the home folder (the user's own, on any system)
        std::string user;     // the account name
        std::string computer; // the machine's name
    };

    // The home folder as "~" (every spelling of its path, and /Users/<name>, /home/<name>, C:\Users\<name>), then the account
    // name as "<user>" and the computer's name as "<computer>", whole words only, case-insensitive. Nothing else changes.
    std::string Redact(const std::string& text, const Identity& who);

    // This machine's identity.
    Identity CurrentIdentity();

    // The fault block of a log: from its last "Fault (" line to the end (the "[crash]" lines and the host stack), empty when
    // the log has none.
    std::string CrashDetails(const std::string& log);

    // The files a report is made of. Empty paths are named as missing.
    struct Sources
    {
        std::string game;             // "Skylanders: Spyro's Adventure"
        std::string gameVersion;
        std::filesystem::path log, previousLog, launcherLog, settingsFile;
    };

    // The whole report, redacted. Each log is cut to its last `maxLogBytes` (the start of a long log is the header, which is also
    // quoted at the top of the report).
    std::string Build(const Sources& sources, const Identity& who, size_t maxLogBytes = 600 * 1024);

    // "<game id>-report-<yyyymmdd-hhmmss>.txt".
    std::string FileName(const std::string& gameId);
}
