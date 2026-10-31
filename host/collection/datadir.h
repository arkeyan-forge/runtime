#pragma once

// Where everything of the player's lives (docs/getting-started/settings.md): one folder per OS, "Skylanders", shared by the game, the
// launcher and SkyBuild. Plain C++ and the C library only (no SDL), so SkyBuild can use it too.
//
//   <root>/Collection/      the figures, shared by every game          (collection.h)
//   <root>/launcher.ini     the settings shared by the games and the launcher
//   <root>/kit/             the kits the launcher downloaded, <release tag>/ each
//   <root>/toolchains/      the compilers SkyBuild downloads (Windows)
//   <root>/logs/            the launcher's logs, migration.log
//   <root>/<Game>/          one per game ("SSA"): the built game, its settings, saves, pipelines.bin, portal.txt,
//                           figures, portraits, launcher-cache and SSARecomp.log
//
// <root> is SKYLANDERS_DATA (tests), <program folder>/data when portable.txt is beside the program, else %APPDATA%\Skylanders on Windows, ~/Library/Application
// Support/Skylanders on macOS and $XDG_DATA_HOME/Skylanders (~/.local/share/Skylanders) elsewhere. Nothing here
// creates a folder.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace datadir
{
    std::filesystem::path Root();
    // A game's folder from its catalogue id: "ssa" is "SSA", "giants" is "Giants", "swapforce" is "SwapForce", "trapteam" is "TrapTeam".
    std::string GameFolderName(std::string_view id);
    std::filesystem::path Game(std::string_view id = "ssa");
    std::filesystem::path LauncherIni();
    std::filesystem::path Kits();
    std::filesystem::path Toolchains();
    std::filesystem::path Logs();

    // Portable mode: a file portable.txt in `folder` makes <folder>/data the root (settings, saves, figures, logs, built games).
    // Empty when the file is not there.
    std::filesystem::path PortableRoot(const std::filesystem::path& folder);
    // The folder that holds the running program (on macOS, the one that holds its .app). Empty when it cannot be found.
    std::filesystem::path ProgramFolder();
    // When portable.txt is beside the program and SKYLANDERS_DATA is not set, sets SKYLANDERS_DATA to the portable root, so
    // Root() and the games the launcher starts follow it (and the older folders are not migrated). True when it did.
    // Call first thing in main, before MigrateOnce.
    bool ApplyPortable();

    // The path as UTF-8 text.
    std::string Text(const std::filesystem::path& path);
    std::filesystem::path FromText(std::string_view utf8);

    // Deletes the oldest files under `folder` until what is left is at most `maxBytes`. Returns the bytes freed.
    uint64_t TrimFolder(const std::filesystem::path& folder, uint64_t maxBytes);

    // --- Migration from the older layouts -----------------------------------------------------------------------------
    //
    // Before, the game kept its data in <roaming>/SSARecomp (and, nested, <roaming>/SSARecomp/SSARecomp), Skylauncher
    // its games and kits in its own folder (%LOCALAPPDATA%\Skylauncher on Windows), SkyBuild its compilers in
    // %LOCALAPPDATA%\SSARecomp\toolchains. Migrate() moves them into the layout above:
    //  - entries are RENAMED, one by one; a source that is gone is skipped, so a half-done run is finished by the next;
    //  - nothing is overwritten or deleted: when the target exists the moved entry is kept beside it as
    //    "<name>.old-<time>", unless the target already holds all of its bytes (an interrupted copy), then the source
    //    is the leftover and goes;
    //  - when a rename fails the entry is copied to a temporary name, checked byte for byte, renamed into place, and
    //    only then deleted; if even that fails it stays where it was (reported as failed, retried next time);
    //  - an old folder is removed only once it is empty;
    //  - every action is logged in <root>/logs/migration.log;
    //  - the absolute paths in <root>/launcher.ini that point into a moved folder are rewritten.
    // xbox-signing (Xbox development only) stays in the old SSARecomp folder.
    // Pure: it touches only the folders it is given.
    struct OldRoots
    {
        std::filesystem::path ssarecomp;      // <roaming>/SSARecomp
        std::filesystem::path skylauncher;    // Skylauncher's data folder
        std::filesystem::path localSsarecomp; // %LOCALAPPDATA%\SSARecomp (Windows only)
    };
    struct MigrationOptions
    {
        bool forceCopy = false; // skip the rename, as if the folders were on two volumes (tests)
        std::string stamp;      // the "<time>" of ".old-<time>"; empty: now
    };
    struct MigrationReport
    {
        int moved = 0;     // renamed
        int copied = 0;    // copied, checked, then the original deleted
        int kept = 0;      // a target existed: the moved entry got an .old- suffix
        int dropped = 0;   // the target already held the source's bytes: the source was removed
        int failed = 0;    // left where it was
        int rewritten = 0; // launcher.ini values updated
        bool Changed() const { return moved + copied + kept + dropped + failed + rewritten > 0; }
    };
    MigrationReport Migrate(
        const OldRoots& old, const std::filesystem::path& root, const MigrationOptions& options = {});

    // The old folders of this machine.
    OldRoots RealOldRoots();
    // Migrate(RealOldRoots(), Root()), at most once per process, and never when the run is a test run or portable
    // (SKYLANDERS_DATA, SKY_LAUNCHER_DATA, SKY_SAVE_DIR, SKY_SETTINGS, SKY_LAUNCHER_INI or SKY_COLLECTION is set: the
    // old folders would be the player's own). Call first thing in main, before any path is used.
    MigrationReport MigrateOnce();
}
