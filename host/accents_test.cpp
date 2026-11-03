// Exercise the public file APIs on synthetic content, under both Western and non-Western paths.
#include "host/collection/collection.h"
#include "host/collection/datadir.h"
#include "host/collection/game_data.h"
#include "host/collection/progress.h"
#include "host/collection/save_backup.h"
#include "host/launcher.h"
#include "host/log.h"
#include "host/portal_backend.h"
#include "host/texture_pack.h"
#include "vfs.h"
#include "../../tools/arkeyan-builder/process.h"
#include <SDL3/SDL.h>
#include <stb_image_write.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>

namespace
{
    namespace fs = std::filesystem;

    struct Environment
    {
        const char* name;
        std::optional<std::string> previous;

        Environment(const char* variable, const fs::path& value) : name(variable)
        {
            if (const char* old = std::getenv(name))
                previous = old;
            Set(datadir::Text(value));
        }

        void Set(const std::string& value)
        {
#ifdef _WIN32
            _wputenv_s(datadir::FromText(name).c_str(), datadir::FromText(value).c_str());
#else
            setenv(name, value.c_str(), 1);
#endif
        }

        ~Environment()
        {
            if (previous)
                Set(*previous);
            else
            {
#ifdef _WIN32
                _wputenv_s(datadir::FromText(name).c_str(), L"");
#else
                unsetenv(name);
#endif
            }
        }
    };
}

int AccentedRuntimeCase()
{
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok)
        {
            fprintf(stderr, "FAIL accents runtime: %s\n", what);
            failures++;
        }
    };
    {
        const fs::path root = fs::current_path();
        fs::create_directories(root);
        Environment data("SKYLANDERS_DATA", root);
        Environment collectionFolder("SKY_COLLECTION", root / "collection");
        Environment saveFolder("SKY_SAVE_DIR", root / "saves");
        Environment settingsFile("SKY_SETTINGS", root / "settings.ini");
        Environment logFile("SKY_LOG", root / "writer.log");
        check(datadir::Root() == root, "data root from Unicode environment");
#ifdef _WIN32
        {
            Environment roaming("APPDATA", root / "roaming");
            data.Set("");
            check(datadir::Root() == root / "roaming" / "Skylanders", "wide APPDATA default root");
            data.Set(datadir::Text(root));
        }
#endif
        check(datadir::FromText(datadir::Text(root)) == root, "native/UTF-8 round trip");
        check(savebackup::Root() == root / "saves", "save root from Unicode environment");
        check(datadir::FromText(LauncherConfigPath()) == root / "settings.ini", "settings environment");

        LauncherConfig config{ { "ssa", { { "iso", datadir::Text(root / "disc.iso") } } },
            { "ssa.video", { { "fps_limit", "60" } } } };
        check(SaveLauncherConfig(LauncherConfigPath(), config), "write settings");
        check(LoadLauncherConfig(LauncherConfigPath()) == config, "read paths stored in settings");

        // The same CRT boundary used by shader/audio/frame diagnostics and the press-file reader.
        const fs::path diagnostic = root / "diagnostic.txt";
        FILE* file = fopen(datadir::Text(diagnostic).c_str(), "wb");
        check(file != nullptr, "narrow fopen writes UTF-8 path with the production manifest");
        if (file != nullptr)
        {
            check(fwrite("probe", 1, 5, file) == 5, "write diagnostic bytes");
            check(fclose(file) == 0, "close diagnostic writer");
        }
        check(savebackup::ReadFile(diagnostic) == "probe", "read diagnostic at exact native path");
        file = fopen(datadir::Text(diagnostic).c_str(), "rb");
        check(file != nullptr, "narrow fopen reads UTF-8 path");
        if (file != nullptr)
        {
            char bytes[5]{};
            check(fread(bytes, 1, 5, file) == 5 && std::string(bytes, 5) == "probe", "read diagnostic bytes");
            check(fclose(file) == 0, "close diagnostic reader");
        }
        check(fs::remove(diagnostic), "delete diagnostic at exact native path");
        const fs::path narrow = root / "narrow-stream.txt";
        std::ofstream(datadir::Text(narrow), std::ios::binary) << "probe";
        check(savebackup::ReadFile(narrow) == "probe", "narrow stream resolves exact native path");

        const fs::path save = root / "saves" / "slot" / "save.dat";
        check(savebackup::WriteFileAtomic(save, "before", 6), "atomic save write");
        check(savebackup::ReadFile(save) == "before", "save read");
        auto backup = savebackup::BackupFile(root / "saves", save, "20261030-120000");
        check(backup.ok, "backup save");
        check(savebackup::WriteFileAtomic(save, "after", 5), "replace save");
        check(!savebackup::List(root / "saves").empty(), "list save backups");
        check(savebackup::RestoreBackup(root / "saves", "20261030-120000").ok, "restore save");
        check(savebackup::ReadFile(save) == "before", "read restored save");
        check(progress::WriteUnlock(root / "progress", 7, 42), "write achievement progress");
        auto unlocks = progress::ReadUnlocks(root / "progress");
        check(unlocks.size() == 1 && unlocks[0].id == 7 && unlocks[0].time == 42, "read achievement progress");
        check(progress::WritePlayTime(root / "progress", { 12, 42, 1 }), "write play time");
        check(progress::ReadPlayTime(root / "progress").totalSeconds == 12, "read play time");

        std::string figure = collection::Create(9, 0, "synthetic.sky");
        check(!figure.empty(), "create synthetic figure");
        collection::Figure read;
        check(!figure.empty() && collection::Read(figure, read), "read synthetic figure");
        check(collection::List().size() == 1, "list synthetic figure");
        FigureData tag{};
        check(!figure.empty() && LoadFigure(figure, tag), "portal reads figure");
        check(!figure.empty() && SaveFigure(figure, tag), "portal writes figure");
        check(!figure.empty() && collection::SetFavourite(figure, true), "write collection metadata");
        check(!figure.empty() && collection::Delete(figure), "move figure to trash");
        auto trash = collection::Trash();
        check(trash.size() == 1, "list trashed figure");
        if (!trash.empty())
            check(collection::DeleteForGood(trash[0].id), "delete trashed figure");

        gamedata::Source source;
        source.files = { { "default.xex", 9 }, { "folder/probe.txt", 9 } };
        source.read = [](const gamedata::SourceFile&, uint64_t offset, void* to, size_t size) {
            const std::string payload = "synthetic";
            if (offset >= payload.size())
                return size_t(0);
            size = std::min(size, payload.size() - size_t(offset));
            memcpy(to, payload.data() + offset, size);
            return size;
        };
        const fs::path copy = root / "gamedata";
        check(gamedata::Copy(source, copy).outcome == gamedata::CopyResult::Outcome::Done, "write copied data");
        check(gamedata::Inspect(copy).state == gamedata::State::Complete, "read copied-data manifest");
        {
            DiscFileSystem disc;
            check(disc.Open(datadir::Text(copy).c_str()), "VFS opens Unicode folder");
            check(disc.List("folder").size() == 1, "VFS lists copied files");
            const auto bytes = disc.ReadAll("folder/probe.txt");
            check(std::string(bytes.begin(), bytes.end()) == "synthetic", "VFS reads through PlatformOpenFile");
        }
        std::string error;
        check(gamedata::Remove(copy, error), "delete copied data");

        const fs::path png = root / "stb.png";
        const std::vector<uint8_t> pixels(16, 255);
        check(stbi_write_png(datadir::Text(png).c_str(), 2, 2, 4, pixels.data(), 8) != 0, "stb writes PNG path");
        SDL_Surface* image = SDL_LoadPNG(datadir::Text(png).c_str());
        check(image != nullptr, "read PNG from exact Unicode path");
        SDL_DestroySurface(image);

        texpack::Configure(root / "textures", true, {}, false);
        texpack::Dump(123, dds::Format::RGBA8, 2, 2, std::vector<uint8_t>(pixels));
        texpack::Flush();
        texpack::Configure({}, false, root / "textures", true);
        const texpack::Replacement* replacement = texpack::Find(123);
        check(replacement != nullptr, "list texture dump as a replacement");
        std::vector<uint8_t> restored;
        check(replacement && texpack::ReadLevel(*replacement, 0, restored) && restored == pixels, "read texture dump");
        texpack::Configure({}, false, {}, false);

        LogInit();
        fprintf(stderr, "synthetic log writer marker\n");
        fflush(stderr);
        check(!LogPath().empty(), "actual log writer opens Unicode path");
    }
    return failures;
}

int TestAccentedRuntimePaths(const std::filesystem::path& scratch)
{
    int failures = 0;
    for (const auto* name : { u8"données-é", u8"データ" })
    {
        const fs::path root = fs::absolute(scratch / "accents" / fs::path(name));
        fs::remove_all(root);
        fs::create_directories(root);
#ifdef _WIN32
        fs::path self = ExecutableDirectory() / "SSAArtTest.exe";
#else
        fs::path self = ExecutableDirectory() / "SSAArtTest";
#endif
        RunOptions run;
        run.cwd = root;
        run.log = root / "child.log";
        run.timeoutSeconds = 30;
        int result = Run({ Slashes(self), "--accents-case" }, run);
        if (result != 0)
        {
            fprintf(stderr, "FAIL accents runtime case (exit %d):\n%s\n%s\n", result,
                savebackup::ReadFile(root / "child.log").c_str(), savebackup::ReadFile(root / "writer.log").c_str());
            failures++;
        }
        if (savebackup::ReadFile(root / "writer.log").find("synthetic log writer marker") == std::string::npos)
        {
            fprintf(stderr, "FAIL accents runtime: actual log writer did not write the exact Unicode path\n");
            failures++;
        }
        std::error_code error;
        fs::remove_all(root, error);
        if (error || fs::exists(root))
        {
            fprintf(stderr, "FAIL accents runtime: delete Unicode output folder\n");
            failures++;
        }
    }
    return failures;
}
