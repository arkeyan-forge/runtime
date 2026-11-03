#include "portable.h"
#include "collection/datadir.h"
#include "collection/portable_pack.h"
#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

namespace portable
{
    namespace
    {
        struct Own
        {
            std::string path;
            std::vector<Entry> entries;
        };

        const Own& Pack()
        {
            static const Own own = [] {
                Own found;
                const fs::path program = datadir::ProgramPath();
                if (program.empty())
                    return found;
                const fs::path folder = program.parent_path();
                const fs::path pack = folder.filename() == "MacOS" && folder.parent_path().filename() == "Contents"
                                          ? folder.parent_path() / "Resources" / PackName
                                          : program;
                std::vector<Entry> entries;
                if (ReadIndex(pack, entries) && Find(entries, std::string(GamePrefix) + "default.xex") != nullptr)
                    found = { datadir::Text(pack), std::move(entries) };
                return found;
            }();
            return own;
        }

        std::string Text(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }
    }

    const std::string& OwnPack() { return Pack().path; }

    std::vector<uint8_t> ReadOwn(std::string_view name)
    {
        const Own& own = Pack();
        const Entry* entry = own.path.empty() ? nullptr : Find(own.entries, name);
        return entry != nullptr ? ReadEntry(datadir::FromText(own.path), *entry) : std::vector<uint8_t>{};
    }

    std::string FirstLaunch(const GameInfo& game, const std::string& collectionFolder)
    {
        const Own& own = Pack();
        if (own.path.empty())
            return {};
        const Info info = ParseInfo(Text(ReadOwn(InfoName)));
        const fs::path marker = FirstLaunchMarker(datadir::Game(game.id), info);
        std::error_code error;
        if (fs::exists(marker, error))
            return {};

        int skipped = 0, failed = 0;
        const int added = ImportCollection(
            datadir::FromText(own.path), own.entries, datadir::FromText(collectionFolder), &skipped, &failed);

        // The keys: the exported [<game>.input] section, when this machine's launcher.ini has none for the game.
        const std::string section = std::string(game.id) + ".input";
        const std::string configPath = LauncherConfigPath();
        LauncherConfig config = LoadLauncherConfig(configPath);
        const char* keys = "kept (the game already has its own)";
        if (config[section].empty())
        {
            const std::string text = Text(ReadOwn(KeysName));
            std::string current;
            for (size_t begin = 0; begin < text.size();)
            {
                size_t end = std::min(text.find('\n', begin), text.size());
                std::string line = text.substr(begin, end - begin);
                begin = end + 1;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.size() > 2 && line.front() == '[' && line.back() == ']')
                    current = line.substr(1, line.size() - 2);
                else if (size_t equals = line.find('='); current == section && equals != std::string::npos)
                    config[section][line.substr(0, equals)] = line.substr(equals + 1);
            }
            fs::create_directories(datadir::FromText(configPath).parent_path(), error);
            keys = config[section].empty()                  ? "none exported"
                   : SaveLauncherConfig(configPath, config) ? "added"
                                                            : "not saved (launcher.ini cannot be written)";
        }
        // Done once every figure is here; a figure that could not be written (a full disk, a read-only folder) is tried
        // again at the next start.
        if (failed == 0)
        {
            fs::create_directories(marker.parent_path(), error);
            std::ofstream(marker) << "imported\n";
        }
        return "portable: first start of the copy exported " + info.exported + " (v" + info.version +
               "): " + std::to_string(added) + " figures added, " + std::to_string(skipped) + " already there, " +
               std::to_string(failed) + " not written; keys " + keys;
    }
}
