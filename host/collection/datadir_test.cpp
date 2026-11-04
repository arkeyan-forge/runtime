// Unit test of the data folder helper and its migration (datadir.h). Every path is in a scratch folder of its own;
// the migration is only ever called with scratch roots, never with RealOldRoots().
//   SSADatadirTest        Exit code 0 = pass.
#include "host/collection/datadir.h"
#include "host/runtime_names.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace datadir;

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
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    // Every file under `path` with its bytes: equal strings, equal trees.
    std::string Tree(const fs::path& path)
    {
        std::map<std::string, std::string> files;
        std::error_code error;
        for (fs::recursive_directory_iterator it(path, error), end; !error && it != end; it.increment(error))
            if (it->is_regular_file())
                files[fs::relative(it->path(), path).generic_string()] = Read(it->path());
        std::string text;
        for (const auto& [name, bytes] : files)
            text += name + "\n" + bytes + "\n--\n";
        return text;
    }

    bool Has(const fs::path& path) { return fs::exists(fs::symlink_status(path)); }

    bool HasSuffixed(const fs::path& folder, const std::string& prefix)
    {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(folder, error))
            if (entry.path().filename().string().rfind(prefix, 0) == 0)
                return true;
        return false;
    }

    // What the older builds left on a Mac: everything under one "Application Support".
    struct Old
    {
        OldRoots roots;
        fs::path support, root;
    };

    Old MakeMac(const std::string& name)
    {
        Old old;
        old.support = g_scratch / name / "Application Support";
        old.root = old.support / "Skylanders";
        fs::path a = old.support / "SSARecomp", s = old.support / "Skylauncher";
        old.roots = { a, s, {} };
        std::string game = (s / "games" / "abc123").string();
        Write(a / "launcher.ini", "[launcher]\niso=/discs/game.iso\n[install.abc123]\nruntime=" + game +
                                      "/SSARecomp.app\nkit=0.3.0\n[video]\nvsync=0\n");
        Write(a / "SSARecomp.log", "log\n");
        Write(a / "SSARecomp.prev.log", "prev\n");
        Write(a / "skylauncher-game.log", "game\n");
        Write(a / "figures" / "one.sky", "figure one");
        Write(a / "portraits" / "p.png", "png");
        Write(a / "launcher-cache" / "415608AB" / "x.png", "cache");
        Write(a / "xbox-signing" / "key.pfx", "secret");
        Write(a / "SSARecomp" / "pipelines.bin", "pipelines");
        Write(a / "SSARecomp" / "portal.txt", "0 /x\n");
        Write(a / "SSARecomp" / "saves" / "slot1" / "save.bin", std::string("\0\1\2 save", 8));
        Write(s / "games" / "abc123" / "SSARecomp.app" / "Contents" / "MacOS" / "SSARecomp", "binary");
        Write(s / "games" / "abc123" / "install.log", "@done\n");
        Write(s / "games" / "abc123" / "ssabuild-work" / "logs" / "link.log", "link");
        Write(s / "kit" / "v0.3.0" / "SkyBuild", "kit");
        Write(old.root / "Collection" / "fig.sky", "collection bytes"); // already in place
        return old;
    }

    void TestPaths()
    {
        fs::path root = g_scratch / "paths";
        setenv("SKYLANDERS_DATA", root.string().c_str(), 1);
        CHECK(Root() == root);
        CHECK(Game("ssa") == root / "SSA" && Game("giants") == root / "Giants");
        CHECK(LauncherIni() == root / "launcher.ini" && Kits() == root / "kit" && Toolchains() == root / "toolchains" &&
              Logs() == root / "logs");
        CHECK(!Has(root)); // nothing is created
        unsetenv("SKYLANDERS_DATA");
        CHECK(Root().filename() == "Skylanders");
        // Portable mode: portable.txt in the folder makes <folder>/data the root; the program's own folder is found.
        fs::path portable = g_scratch / "portable";
        fs::create_directories(portable);
        CHECK(PortableRoot(portable).empty());
        Write(portable / "portable.txt", "");
        CHECK(PortableRoot(portable) == portable / "data");
        CHECK(PortableRoot({}).empty());
        CHECK(!ProgramFolder().empty() && !fs::exists(ProgramFolder() / "portable.txt"));
        CHECK(!ApplyPortable() && getenv("SKYLANDERS_DATA") == nullptr);
        CHECK(GameFolderName("ssa") == "SSA" && GameFolderName("giants") == "Giants");
        CHECK(GameFolderName("swapforce") == "SwapForce" && Game("swapforce") == Root() / "SwapForce");
        CHECK(GameFolderName("trapteam") == "TrapTeam" && Game("trapteam") == Root() / "TrapTeam");
        CHECK(GameFolderName("superchargers") == "SuperChargers");
    }

    void TestMac()
    {
        Old old = MakeMac("mac");
        std::string collection = Tree(old.root / "Collection"),
                    saves = Tree(old.roots.ssarecomp / "SSARecomp" / "saves");
        fs::path a = old.roots.ssarecomp;
        MigrationReport report = Migrate(old.roots, old.root);
        fs::path game = old.root / "SSA";
        CHECK(report.failed == 0 && report.kept == 0 && report.moved > 10);
        CHECK(Tree(game / "saves") == saves && Tree(old.root / "Collection") == collection);
        CHECK(Read(game / "pipelines.bin") == "pipelines" && Read(game / "portal.txt") == "0 /x\n");
        CHECK(Read(game / "figures" / "one.sky") == "figure one" && Read(game / "portraits" / "p.png") == "png");
        CHECK(Read(game / "launcher-cache" / "415608AB" / "x.png") == "cache");
        CHECK(Read(game / "SSARecomp.log") == "log\n" && Read(game / "SSARecomp.prev.log") == "prev\n");
        CHECK(Read(old.root / "logs" / "skylauncher-game.log") == "game\n");
        CHECK(Read(game / "SSARecomp.app" / "Contents" / "MacOS" / "SSARecomp") == "binary");
        CHECK(Read(game / "install.log") == "@done\n" && Read(game / "ssabuild-work" / "logs" / "link.log") == "link");
        CHECK(Read(old.root / "kit" / "v0.3.0" / "SkyBuild") == "kit");
        // launcher.ini: moved, the install path follows, the ISO's stays
        std::string ini = Read(old.root / "launcher.ini");
        CHECK(ini.find("iso=/discs/game.iso\n") != std::string::npos && ini.find("vsync=0") != std::string::npos);
        CHECK(ini.find("runtime=" + (game / "SSARecomp.app").string() + "\n") != std::string::npos &&
              report.rewritten == 1);
        CHECK(ini.find("Skylauncher") == std::string::npos);
        // old folders: gone once empty, except SSARecomp that still holds xbox-signing
        CHECK(Read(a / "xbox-signing" / "key.pfx") == "secret" && !Has(a / "SSARecomp") && !Has(a / "launcher.ini"));
        CHECK(!Has(old.roots.skylauncher));
        // the log
        std::string log = Read(old.root / "logs" / "migration.log");
        CHECK(log.find("saves") != std::string::npos && log.find("launcher.ini") != std::string::npos);
        // idempotent
        std::string before = Tree(old.root);
        MigrationReport again = Migrate(old.roots, old.root);
        CHECK(!again.Changed() && Tree(old.root) == before);
    }

    void TestWindows()
    {
        fs::path support = g_scratch / "win" / "Roaming", local = g_scratch / "win" / "Local",
                 root = support / "Skylanders";
        OldRoots roots{ support / "SSARecomp", local / "Skylauncher", local / "SSARecomp" };
        Write(roots.ssarecomp / "launcher.ini",
            "[install.ff]\nruntime=" + (local / "Skylauncher" / "games" / "ff" / "SSARecomp.exe").string() + "\n");
        Write(roots.ssarecomp / "SSARecomp" / "saves" / "s.bin", "save");
        Write(roots.skylauncher / "games" / "ff" / "SSARecomp.exe", "exe");
        Write(roots.skylauncher / "games" / "ff" / "ssabuild-work" / "x", "x");
        Write(roots.skylauncher / "kit" / "v0.3.0" / "SkyBuild.exe", "kit");
        Write(roots.localSsarecomp / "toolchains" / "llvm-mingw.zip", "zip");
        Write(roots.localSsarecomp / "toolchains" / "llvm-mingw" / "bin" / "clang.exe", "clang");
        MigrationReport report = Migrate(roots, root);
        CHECK(report.failed == 0 && Read(root / "SSA" / "SSARecomp.exe") == "exe" &&
              Read(root / "SSA" / "saves" / "s.bin") == "save");
        CHECK(Read(root / "toolchains" / "llvm-mingw" / "bin" / "clang.exe") == "clang" &&
              Read(root / "toolchains" / "llvm-mingw.zip") == "zip");
        CHECK(Read(root / "kit" / "v0.3.0" / "SkyBuild.exe") == "kit");
        CHECK(Read(root / "launcher.ini").find("runtime=" + (root / "SSA" / "SSARecomp.exe").string()) !=
              std::string::npos);
        CHECK(!Has(roots.ssarecomp) && !Has(roots.skylauncher) && !Has(roots.localSsarecomp));
        // a second game folder (another disc) stays where it was
        Write(roots.skylauncher / "games" / "aa" / "SSARecomp.exe", "first by hash");
        Write(roots.skylauncher / "games" / "zz" / "SSARecomp.exe", "other");
        Migrate(roots, root);
        CHECK(Has(roots.skylauncher / "games" / "zz" / "SSARecomp.exe"));
    }

    void TestExistingTarget()
    {
        Old old = MakeMac("existing");
        fs::path game = old.root / "SSA";
        Write(game / "saves" / "slot1" / "save.bin", "NEWER save");
        Write(game / "pipelines.bin", "pipelines"); // same bytes as the old one
        std::string oldSaves = Tree(old.roots.ssarecomp / "SSARecomp" / "saves");
        MigrationReport report = Migrate(old.roots, old.root, { false, "T1" });
        CHECK(report.kept == 1 && report.dropped == 1 && report.failed == 0);
        CHECK(Read(game / "saves" / "slot1" / "save.bin") == "NEWER save"); // never overwritten
        CHECK(Tree(game / "saves.old-T1") == oldSaves);                    // the old one is kept beside it
        CHECK(!Has(old.roots.ssarecomp / "SSARecomp"));
        // the same name again: another suffix, nothing lost
        Write(old.roots.ssarecomp / "SSARecomp" / "saves" / "slot9" / "s.bin", "more");
        report = Migrate(old.roots, old.root, { false, "T1" });
        CHECK(report.kept == 1 && Read(game / "saves.old-T1-2" / "slot9" / "s.bin") == "more");
    }

    void TestHalfDone()
    {
        Old old = MakeMac("half");
        fs::path game = old.root / "SSA", a = old.roots.ssarecomp;
        // an earlier run moved the game and stopped before launcher.ini: its install path is still the old one
        fs::create_directories(game);
        fs::rename(old.roots.skylauncher / "games" / "abc123" / "SSARecomp.app", game / "SSARecomp.app");
        // another stopped between the copy and the delete of the figures, and left a partial temporary copy
        fs::create_directories(game / "figures");
        fs::copy(a / "figures" / "one.sky", game / "figures" / "one.sky");
        Write(game / "portraits.migrating" / "junk", "partial");
        MigrationReport report = Migrate(old.roots, old.root);
        CHECK(report.failed == 0 && report.kept == 0 && report.dropped == 1);
        CHECK(!Has(a / "figures") && Read(game / "figures" / "one.sky") == "figure one" &&
              Read(game / "portraits" / "p.png") == "png");
        CHECK(Read(game / "SSARecomp.app" / "Contents" / "MacOS" / "SSARecomp") == "binary");
        CHECK(
            Read(old.root / "launcher.ini").find("runtime=" + (game / "SSARecomp.app").string()) != std::string::npos);
        CHECK(!Migrate(old.roots, old.root).Changed());
    }

    void TestCopyPath()
    {
        Old old = MakeMac("copy");
        fs::path locked = old.roots.ssarecomp / "SSARecomp" / "saves" / "slot1" / "locked.bin";
        Write(locked, "read only");
        fs::permissions(locked, fs::perms::owner_read, fs::perm_options::replace);
        std::string saves = Tree(old.roots.ssarecomp / "SSARecomp" / "saves");
        MigrationReport report = Migrate(old.roots, old.root, { true, {} }); // as if on another volume
        fs::path game = old.root / "SSA";
        CHECK(report.failed == 0 && report.copied > 10 && report.moved == 0);
        CHECK(Tree(game / "saves") == saves && Read(game / "saves" / "slot1" / "locked.bin") == "read only");
        CHECK(Read(game / "saves" / "slot1" / "save.bin") == std::string("\0\1\2 save", 8));
        CHECK(!Has(old.roots.ssarecomp / "SSARecomp") && !HasSuffixed(game, "saves.migrating"));
    }

    void TestFailure()
    {
        if (geteuid() == 0)
            return; // root ignores permissions
        Old old = MakeMac("fail");
        fs::path game = old.root / "SSA";
        fs::create_directories(game);
        fs::permissions(game, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
        std::string saves = Tree(old.roots.ssarecomp / "SSARecomp" / "saves");
        MigrationReport report = Migrate(old.roots, old.root);
        fs::permissions(game, fs::perms::owner_all, fs::perm_options::replace);
        CHECK(report.failed > 0);
        CHECK(Tree(old.roots.ssarecomp / "SSARecomp" / "saves") == saves &&
              Has(old.roots.ssarecomp / "SSARecomp" / "pipelines.bin")); // left in place
        CHECK(Has(old.roots.skylauncher / "games" / "abc123" / "install.log"));
        // once the folder is writable again, the next run finishes the job
        report = Migrate(old.roots, old.root);
        CHECK(report.failed == 0 && Tree(game / "saves") == saves && !Has(old.roots.ssarecomp / "SSARecomp"));
    }

    // A game built before this layout makes its launcher-cache again in the old folder: the next run drops it, no .old-.
    void TestCacheAgain()
    {
        Old old = MakeMac("cache");
        Migrate(old.roots, old.root);
        fs::path game = old.root / "SSA";
        Write(old.roots.ssarecomp / "launcher-cache" / "415608AB" / "y.png", "again");
        MigrationReport report = Migrate(old.roots, old.root);
        CHECK(report.dropped == 1 && report.kept == 0 && !Has(old.roots.ssarecomp / "launcher-cache"));
        CHECK(Read(game / "launcher-cache" / "415608AB" / "x.png") == "cache" &&
              !HasSuffixed(game, "launcher-cache.old-"));
    }

    void TestTrim()
    {
        fs::path folder = g_scratch / "trim";
        for (int i = 0; i < 5; i++)
        {
            fs::path file = folder / "sub" / ("f" + std::to_string(i));
            Write(file, std::string(100, 'x'));
            fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::hours(10 - i));
        }
        uint64_t freed = TrimFolder(folder, 250);
        CHECK(freed == 300 && !Has(folder / "sub" / "f0") && !Has(folder / "sub" / "f2") &&
              Has(folder / "sub" / "f3") && Has(folder / "sub" / "f4"));
        CHECK(TrimFolder(folder, 250) == 0);
    }
}

// A fresh copy of this program in a scratch folder: with portable.txt beside it, ApplyPortable() makes <folder>/data the root, and
// the games it starts inherit that (SKYLANDERS_DATA); without it, nothing changes.
void TestPortableCopy(const char* self)
{
    fs::path folder = g_scratch / "copy";
    fs::create_directories(folder);
    fs::copy_file(self, folder / "probe", fs::copy_options::overwrite_existing);
    auto probe = [&] {
        std::string command = "env -u SKYLANDERS_DATA '" + (folder / "probe").string() + "' --portable-probe";
        FILE* pipe = popen(command.c_str(), "r");
        char line[4096] = {};
        if (pipe == nullptr || fgets(line, sizeof(line), pipe) == nullptr)
            line[0] = 0;
        if (pipe != nullptr)
            pclose(pipe);
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    };
    CHECK(probe().find("portable ") != 0);
    Write(folder / "portable.txt", "");
    std::error_code error;
    CHECK(probe() == "portable " + (fs::weakly_canonical(folder, error) / "data").string());
}

// The 0.8.0 rename (Arkeyan names) moves nothing of the player's: the data folders keep their names, a 0.7.x data
// folder is left as it is, and a game's log is read under either name.
void TestRenameKeepsData()
{
    CHECK(Root().filename() == "Skylanders");
    OldRoots real = RealOldRoots();
    CHECK(real.ssarecomp.filename() == "SSARecomp" && real.skylauncher.filename() == "Skylauncher");
    fs::path base = g_scratch / "rename", root = base / "Skylanders", game = root / "SSA";
    Write(game / "saves" / "slot.sav", "save");
    Write(game / "SSARecomp.log", "old log\n");
    Write(game / "ssabuild-work" / "ppc" / "ppc_config.h", "work");
    Write(root / "launcher.ini",
        "[install.ab]\nruntime=" + (game / "Skylanders Spyro's Adventure").string() + "\nkit=0.7.2\n");
    Write(root / "Collection" / "fig.sky", "figure");
    const std::string before = Tree(root);
    OldRoots none{ base / "SSARecomp", base / "Skylauncher", base / "LocalSSARecomp" };
    MigrationReport report = Migrate(none, root);
    CHECK(!report.Changed() && Tree(root) == before);
    CHECK(runtime_names::ReadLog(game) == game / "SSARecomp.log");
    CHECK(runtime_names::ReadLog(game, true) == game / "Arkeyan.prev.log"); // neither: the current name
    Write(game / "Arkeyan.log", "new log\n");
    CHECK(runtime_names::ReadLog(game) == game / "Arkeyan.log");
    fs::last_write_time(game / "SSARecomp.log", fs::last_write_time(game / "Arkeyan.log") + std::chrono::seconds(5));
    CHECK(runtime_names::ReadLog(game) == game / "SSARecomp.log"); // an old game started after the new one
    CHECK(Read(game / "saves" / "slot.sav") == "save" && Read(game / "SSARecomp.log") == "old log\n");
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--portable-probe")
    {
        printf(ApplyPortable() ? "portable %s\n" : "not portable %s\n", Root().string().c_str());
        return 0;
    }
    g_scratch = fs::temp_directory_path() / ("SSADatadirTest-" + std::to_string(getpid()));
    fs::remove_all(g_scratch);
    fs::create_directories(g_scratch);
    TestPaths();
    TestPortableCopy(argv[0]);
    TestMac();
    TestWindows();
    TestExistingTarget();
    TestHalfDone();
    TestCopyPath();
    TestFailure();
    TestCacheAgain();
    TestTrim();
    TestRenameKeepsData();
    std::error_code error;
    fs::remove_all(g_scratch, error);
    printf("datadir test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
