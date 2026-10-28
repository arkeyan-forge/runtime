#include "collection.h"
#include "datadir.h"
#include "json.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include "../env.h"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace collection
{
    namespace
    {
        constexpr const char* IndexName = "collection.json";

        int64_t ProcessId()
        {
#ifdef _WIN32
            return int64_t(GetCurrentProcessId());
#else
            return int64_t(getpid());
#endif
        }

        bool ProcessAlive(int64_t pid)
        {
            if (pid <= 0)
                return false;
#ifdef _WIN32
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
            if (process == nullptr)
                return GetLastError() == ERROR_ACCESS_DENIED; // exists, not ours to open
            DWORD code = 0;
            bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
            CloseHandle(process);
            return alive;
#else
            return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
        }

        std::string Now()
        {
            time_t now = time(nullptr);
            struct tm utc;
#ifdef _WIN32
            gmtime_s(&utc, &now);
#else
            gmtime_r(&now, &utc);
#endif
            char text[32];
            strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
            return text;
        }

        std::string FileName(const std::string& path)
        {
            size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        std::string Directory(const std::string& path)
        {
            size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? std::string() : path.substr(0, slash);
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(tolower(c)); });
            return text;
        }

        bool Exists(const std::string& path)
        {
            return SDL_GetPathInfo(path.c_str(), nullptr);
        }

        bool LoadBytes(const std::string& path, std::string& bytes)
        {
            size_t size = 0;
            char* data = static_cast<char*>(SDL_LoadFile(path.c_str(), &size));
            if (data == nullptr)
                return false;
            bytes.assign(data, size);
            SDL_free(data);
            return true;
        }

        bool LoadDump(const std::string& path, FigureData& tag)
        {
            std::string bytes;
            if (!LoadBytes(path, bytes) || bytes.size() < tag.size())
                return false;
            memcpy(tag.data(), bytes.data(), tag.size());
            return true;
        }

        // In place, the first 1 KB only: some tools keep their own data after the tag image.
        bool SaveDump(const std::string& path, const FigureData& tag)
        {
            SDL_IOStream* file = SDL_IOFromFile(path.c_str(), "r+b");
            if (file == nullptr)
                return false;
            bool written = SDL_WriteIO(file, tag.data(), tag.size()) == tag.size();
            return SDL_CloseIO(file) && written;
        }

        // Write to a temporary file and rename it over, so readers never see half a file.
        bool SaveAtomically(const std::string& path, const std::string& text)
        {
            std::string temporary = path + ".tmp";
            if (!SDL_SaveFile(temporary.c_str(), text.data(), text.size()))
                return false;
            if (SDL_RenamePath(temporary.c_str(), path.c_str()))
                return true;
            SDL_RemovePath(temporary.c_str());
            return false;
        }

        Json LoadJson(const std::string& path)
        {
            std::string text, error;
            Json json;
            if (LoadBytes(path, text) && !Json::Parse(text, json, error))
                fprintf(stderr, "[collection] %s: %s\n", path.c_str(), error.c_str());
            return json.IsObject() ? json : Json::Object();
        }

        // Loads the index for a write. A missing file is a new, empty index; a file that exists but cannot be read or
        // parsed is bad: it is never replaced by a near-empty index. It is kept as collection.json.bad-<time> (once per
        // bad state) and writes are refused, each attempt re-reading the file, until it loads again.
        bool LoadIndexForWrite(const std::string& path, Json& index)
        {
            static std::string keptAs;
            std::string text, error;
            bool bad = false;
            if (!Exists(path))
                index = Json::Object();
            else if (!LoadBytes(path, text))
            {
                error = "cannot read the file";
                bad = true;
            }
            else if (!Json::Parse(text, index, error) || !index.IsObject())
            {
                if (error.empty())
                    error = "not a JSON object";
                bad = true;
            }
            if (!bad)
            {
                keptAs.clear();
                return true;
            }
            if (keptAs.empty() && !text.empty())
            {
                time_t now = time(nullptr);
                struct tm utc;
#ifdef _WIN32
                gmtime_s(&utc, &now);
#else
                gmtime_r(&now, &utc);
#endif
                char stamp[32];
                strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &utc);
                std::string copy = path + ".bad-" + stamp;
                if (SDL_SaveFile(copy.c_str(), text.data(), text.size()))
                    keptAs = copy;
                fprintf(stderr, "[collection] %s: %s; kept as %s\n", path.c_str(), error.c_str(), copy.c_str());
            }
            fprintf(stderr, "[collection] %s: %s; index writes refused until it loads again\n", path.c_str(),
                error.c_str());
            return false;
        }

        // collection.json: read, changed and written back at once, so the launcher and a game can both use it.
        template <typename Change>
        bool UpdateIndex(Change change)
        {
            SDL_CreateDirectory(Folder().c_str());
            std::string path = PathOf(IndexName);
            Json index;
            if (!LoadIndexForWrite(path, index))
                return false;
            if (!index.Has("format"))
                index.Set("format", 1);
            change(index);
            return SaveAtomically(path, index.Dump());
        }

        // The metadata object of a dump, created on demand.
        template <typename Change>
        bool UpdateFigure(const std::string& path, Change change)
        {
            return UpdateIndex([&](Json& index) {
                Json entries = index["figures"].IsObject() ? index["figures"] : Json::Object();
                Json entry = entries[FileName(path)].IsObject() ? entries[FileName(path)] : Json::Object();
                change(entry);
                entries.Set(FileName(path), std::move(entry));
                index.Set("figures", std::move(entries));
            });
        }

        std::vector<std::string> DumpNames(const std::string& folder)
        {
            std::vector<std::string> names;
            int count = 0;
            if (char** found = SDL_GlobDirectory(folder.c_str(), "*", SDL_GLOB_CASEINSENSITIVE, &count))
            {
                for (int i = 0; i < count; i++)
                {
                    if (IsDumpName(found[i]))
                        names.push_back(found[i]);
                }
                SDL_free(found);
            }
            std::sort(names.begin(), names.end());
            return names;
        }

        // "<stem><suffix><ext>" with " (2)", " (3)"... until no such file exists in the collection.
        std::string FreeName(const std::string& wanted)
        {
            size_t dot = wanted.find_last_of('.');
            std::string stem = dot == std::string::npos ? wanted : wanted.substr(0, dot);
            std::string extension = dot == std::string::npos ? ".sky" : wanted.substr(dot);
            std::string name = stem + extension;
            for (int n = 2; Exists(PathOf(name)); n++)
                name = stem + " (" + std::to_string(n) + ")" + extension;
            return name;
        }

        // Characters file systems refuse, and a leading dot, replaced.
        std::string SafeName(std::string name)
        {
            for (char& c : name)
            {
                if (strchr("/\\:*?\"<>|", c) != nullptr || uint8_t(c) < 0x20)
                    c = '_';
            }
            if (!name.empty() && name[0] == '.')
                name[0] = '_';
            return name;
        }

        bool Refuse(std::string* error, const std::string& text)
        {
            if (error != nullptr)
                *error = text;
            return false;
        }

        bool CopyBytes(const std::string& from, const std::string& to)
        {
            std::string bytes;
            return LoadBytes(from, bytes) && SDL_SaveFile(to.c_str(), bytes.data(), bytes.size());
        }

        int Order(const Figure& figure)
        {
            return figure.toy != nullptr ? int(figure.toy->category) * 16 + int(figure.toy->element) : 1 << 20;
        }
    }

    std::string Figure::DisplayName() const
    {
        if (!label.empty())
            return label;
        return !edition.empty() ? edition : variantInfo.name;
    }

    VariantKind KindIn(const Figure& figure, const std::string& game)
    {
        if (figure.toy == nullptr || figure.editionKind != VariantKind::Base || game.empty())
            return figure.Kind();
        Catalogue::Reading reading = Catalogue::Get().Read(game, figure.toy, figure.variant);
        return reading.works ? reading.kind : figure.Kind();
    }

    std::string NameIn(const Figure& figure, const std::string& game)
    {
        if (figure.toy == nullptr || !figure.label.empty() || !figure.edition.empty() || game.empty())
            return figure.DisplayName();
        Catalogue::Reading reading = Catalogue::Get().Read(game, figure.toy, figure.variant);
        if (!reading.works || reading.exact)
            return figure.DisplayName();
        return reading.seen != nullptr ? reading.seen->name : figure.toy->name;
    }

    bool Figure::IsSwapTop() const
    {
        return toy != nullptr && toy->category == Category::SwapTop;
    }

    bool Figure::IsSwapBottom() const
    {
        return toy != nullptr && toy->category == Category::SwapBottom;
    }

    bool HasProgress(const Figure& figure)
    {
        return figure.toy != nullptr && figure.toy->category <= Category::Mini;
    }

    std::string LevelText(const Figure& figure, const std::string& game, bool french)
    {
        if (!HasProgress(figure))
            return {};
        if (figure.state == figures::AreaState::Blank)
            return french ? "Nouveau" : "New";
        if (figure.state != figures::AreaState::Valid)
            return french ? "Illisible" : "Unreadable";
        int cap = figures::LevelCap(game);
        char text[48];
        if (figure.level > cap)
            snprintf(text, sizeof(text), french ? "Niveau %d (%d)" : "Level %d (%d)", cap, figure.level);
        else
            snprintf(text, sizeof(text), french ? "Niveau %d" : "Level %d", figure.level);
        return text;
    }

    std::string DefaultFolder()
    {
        std::filesystem::path folder = datadir::Root();
        return folder.empty() ? std::string("Collection") : datadir::Text(folder / "Collection");
    }

    std::string Folder()
    {
        static const std::string folder = [] {
            std::string chosen;
            if (const char* path = skyenv::Get("SKY_COLLECTION"); path != nullptr && *path != '\0')
                chosen = path;
            else if (const char* old = skyenv::Get("SKY_FIGURES"); old != nullptr && *old != '\0')
                chosen = old;
            else
                chosen = DefaultFolder();
            while (chosen.size() > 1 && (chosen.back() == '/' || chosen.back() == '\\'))
                chosen.pop_back();
            return chosen;
        }();
        return folder;
    }

    std::string PathOf(const std::string& file)
    {
        return Folder() + "/" + file;
    }

    bool IsDumpName(const std::string& name)
    {
        std::string extension = name.size() > 4 ? Lower(name.substr(name.size() - 4)) : "";
        return extension == ".sky" || extension == ".bin" || extension == ".dmp";
    }

    // --- Locks ---------------------------------------------------------------------------------------------------------

    LockInfo GetLock(const std::string& path)
    {
        LockInfo info;
        std::string text, error;
        if (path.empty() || !LoadBytes(path + ".lock", text))
            return info;
        info.locked = true;
        Json json;
        if (!Json::Parse(text, json, error) || !json.IsObject())
        {
            info.stale = true;
            return info;
        }
        info.game = json["game"].AsString();
        info.pid = json["pid"].AsInt();
        info.since = json["since"].AsString();
        info.mine = info.pid == ProcessId();
        info.stale = !info.mine && !ProcessAlive(info.pid);
        return info;
    }

    bool Lock(const std::string& path, const std::string& game)
    {
        LockInfo current = GetLock(path);
        if (current.Busy())
            return false;
        Json lock = Json::Object();
        lock.Set("game", game);
        lock.Set("pid", ProcessId());
        lock.Set("since", Now());
        return SaveAtomically(path + ".lock", lock.Dump());
    }

    void Unlock(const std::string& path)
    {
        if (GetLock(path).mine)
            SDL_RemovePath((path + ".lock").c_str());
    }

    void BreakLock(const std::string& path)
    {
        SDL_RemovePath((path + ".lock").c_str());
    }

    int ClearStaleLocks()
    {
        int cleared = 0;
        for (const std::string& name : DumpNames(Folder()))
        {
            std::string path = PathOf(name);
            if (GetLock(path).stale)
            {
                BreakLock(path);
                cleared++;
            }
        }
        return cleared;
    }

    // --- Figures -------------------------------------------------------------------------------------------------------

    namespace
    {
        // A dump and its metadata from an index already loaded (List reads collection.json once for every dump).
        bool ReadWith(const std::string& path, const Json& index, Figure& figure)
        {
            FigureData tag;
            if (!LoadDump(path, tag))
                return false;
            const Catalogue& catalogue = Catalogue::Get();
            figure = {};
            figure.path = path;
            figure.file = FileName(path);
            figure.type = figures::ToyType(tag);
            figure.variant = figures::Variant(tag);
            figure.toy = catalogue.Find(figure.type);
            figure.variantInfo = catalogue.Describe(figure.toy, figure.variant);
            figure.state = figures::ReadStats(tag, figure.stats);
            figure.level = figures::LevelForExperience(figures::TotalExperience(figure.stats), figures::MaxLevelSwapForce);
            figure.lock = GetLock(path);

            const Json& entry = index["figures"][figure.file];
            figure.label = entry["label"].AsString();
            figure.edition = entry["edition"].AsString();
            VariantKind kind = ParseVariantKind(entry["edition_kind"].AsString());
            figure.editionKind = figure.edition.empty() || kind == VariantKind::Count ? VariantKind::Base : kind;
            figure.favourite = entry["favourite"].AsBool();
            figure.lastGame = entry["last_game"].AsString();
            figure.lastUsed = entry["last_used"].AsString();
            figure.origin = entry["origin"].AsString();
            for (const auto& [game, use] : entry["games"].Members())
                figure.uses.push_back({ game, use["last_used"].AsString(), int(use["uses"].AsInt()) });

            // A swapper's link counts when the other dump is there, links back, and is the other half.
            const std::string& partner = entry["partner"].AsString();
            if (!partner.empty() && (figure.IsSwapTop() || figure.IsSwapBottom()) &&
                index["figures"][partner]["partner"].AsString() == figure.file)
            {
                FigureData other;
                const Toy* otherToy = LoadDump(Directory(path) + "/" + partner, other) ? catalogue.Find(figures::ToyType(other)) : nullptr;
                if (otherToy != nullptr && otherToy->category == (figure.IsSwapTop() ? Category::SwapBottom : Category::SwapTop))
                    figure.partner = partner;
            }
            return true;
        }
    }

    std::string Figure::LastUsedIn(const std::string& game) const
    {
        for (const Use& use : uses)
        {
            if (use.game == game)
                return use.lastUsed;
        }
        return {};
    }

    bool Read(const std::string& path, Figure& figure)
    {
        return ReadWith(path, LoadJson(Directory(path) + "/" + IndexName), figure);
    }

    std::vector<Figure> List(const Filter& filter)
    {
        const Catalogue& catalogue = Catalogue::Get();
        std::string text = Lower(filter.text);
        std::vector<Figure> list;
        Json index = LoadJson(PathOf(IndexName));
        auto passes = [&](const Figure& figure) {
            if (!filter.game.empty() && !catalogue.Accepts(filter.game, figure.type))
                return false;
            if (filter.category >= 0 && (figure.toy == nullptr || int(figure.toy->category) != filter.category))
                return false;
            if (filter.element >= 0 && (figure.toy == nullptr || int(figure.toy->element) != filter.element))
                return false;
            if ((filter.kind >= 0 && int(figure.Kind()) != filter.kind) || (filter.favouritesOnly && !figure.favourite))
                return false;
            if (!text.empty())
            {
                std::string haystack = Lower(figure.variantInfo.name + "\n" + figure.label + "\n" + figure.edition + "\n"
                    + figures::NicknameUtf8(figure.stats) + "\n" + figure.file);
                if (haystack.find(text) == std::string::npos)
                    return false;
            }
            return true;
        };
        // A swapper is one card: it is listed (both halves) when either half passes the filter.
        std::vector<Figure> all;
        std::vector<bool> pass;
        std::map<std::string, size_t> byFile;
        for (const std::string& name : DumpNames(Folder()))
        {
            Figure figure;
            if (!ReadWith(PathOf(name), index, figure))
                continue;
            pass.push_back(passes(figure));
            byFile[figure.file] = all.size();
            all.push_back(std::move(figure));
        }
        for (size_t i = 0; i < all.size(); i++)
        {
            auto other = all[i].partner.empty() ? byFile.end() : byFile.find(all[i].partner);
            if (pass[i] || (other != byFile.end() && pass[other->second]))
                list.push_back(std::move(all[i]));
        }
        std::stable_sort(list.begin(), list.end(), [](const Figure& a, const Figure& b) {
            if (Order(a) != Order(b))
                return Order(a) < Order(b);
            if (a.type != b.type)
                return a.type < b.type;
            return a.variant < b.variant;
        });
        return list;
    }

    namespace
    {
        std::string DefaultName(uint16_t type, uint16_t variant)
        {
            char prefix[8];
            snprintf(prefix, sizeof(prefix), "%03u ", type);
            return prefix + Catalogue::Get().Describe(Catalogue::Get().Find(type), variant).name + ".sky";
        }

        std::string SaveNew(const FigureData& tag, const std::string& wanted)
        {
            std::string name = FreeName(SafeName(wanted));
            SDL_CreateDirectory(Folder().c_str());
            std::string path = PathOf(name);
            if (!SDL_SaveFile(path.c_str(), tag.data(), tag.size()))
            {
                fprintf(stderr, "[collection] cannot create %s\n", path.c_str());
                return {};
            }
            return path;
        }

        std::string CreateUnmarked(uint16_t type, uint16_t variant, const std::string& file)
        {
            return SaveNew(figures::CreateFigure(type, variant), file.empty() ? DefaultName(type, variant) : file);
        }

        void SetOrigin(const std::string& path, const std::string& origin);
    }

    std::string Create(uint16_t type, uint16_t variant, const std::string& file)
    {
        std::string path = CreateUnmarked(type, variant, file);
        if (!path.empty())
            SetOrigin(path, "created");
        return path;
    }

    uint16_t MatchingHalf(uint16_t type)
    {
        if (type >= 2000 && type <= 2015)
            return uint16_t(type - 1000);
        if (type >= 1000 && type <= 1015)
            return uint16_t(type + 1000);
        return 0;
    }

    SwapperPaths CreateSwapper(uint16_t topType, uint16_t bottomType, uint16_t topVariant, uint16_t bottomVariant)
    {
        const Catalogue& catalogue = Catalogue::Get();
        const Toy* top = catalogue.Find(topType);
        const Toy* bottom = catalogue.Find(bottomType);
        if (top == nullptr || bottom == nullptr || top->category != Category::SwapTop || bottom->category != Category::SwapBottom)
            return {};
        SwapperPaths made;
        // No version given: the standard swapper, each half under the id it is sold with.
        if (topVariant == 0)
            topVariant = StandardVariant(*top);
        if (bottomVariant == 0)
            bottomVariant = StandardVariant(*bottom);
        made.top = CreateUnmarked(topType, topVariant, {});
        made.bottom = CreateUnmarked(bottomType, bottomVariant, {});
        if (!made.top.empty() && !made.bottom.empty())
        {
            SetOrigin(made.top, "created");
            SetOrigin(made.bottom, "created");
            if (Pair(made.top, made.bottom))
                return made;
        }
        for (const std::string& path : { made.top, made.bottom })
        {
            if (!path.empty())
                SDL_RemovePath(path.c_str());
        }
        return {};
    }

    int CreateMissing(const std::vector<uint16_t>& types)
    {
        std::vector<uint16_t> present;
        for (const std::string& name : DumpNames(Folder()))
        {
            FigureData tag;
            if (LoadDump(PathOf(name), tag) && figures::Variant(tag) == 0)
                present.push_back(figures::ToyType(tag));
        }
        int created = 0;
        for (uint16_t type : types)
        {
            if (std::find(present.begin(), present.end(), type) == present.end() && !CreateUnmarked(type, 0, {}).empty())
                created++;
        }
        return created;
    }

    bool WriteStats(const std::string& path, const figures::FigureStats& stats, std::string* error)
    {
        if (GetLock(path).Busy())
            return Refuse(error, "in use by " + GetLock(path).game);
        FigureData tag;
        if (!LoadDump(path, tag))
            return Refuse(error, "not a figure dump");
        if (Backup(path).empty())
            return Refuse(error, "cannot back up the dump first");
        PruneBackups(path);
        figures::WriteStats(tag, stats);
        return SaveDump(path, tag) || Refuse(error, "cannot write the file");
    }

    bool SetNickname(const std::string& path, const std::string& utf8, std::string* error)
    {
        Figure figure;
        if (!Read(path, figure))
            return Refuse(error, "not a figure dump");
        figures::SetNicknameUtf8(figure.stats, utf8);
        return WriteStats(path, figure.stats, error);
    }

    bool Rename(const std::string& path, const std::string& file, std::string* error)
    {
        std::string name = SafeName(file);
        if (name.empty())
            return Refuse(error, "empty name");
        if (!IsDumpName(name))
            name += path.substr(path.find_last_of('.'));
        std::string target = Directory(path) + "/" + name;
        if (target == path)
            return true;
        if (GetLock(path).Busy())
            return Refuse(error, "in use by " + GetLock(path).game);
        if (Exists(target))
            return Refuse(error, name + " already exists");
        if (!SDL_RenamePath(path.c_str(), target.c_str()))
            return Refuse(error, "cannot rename the file");
        if (GetLock(path).mine)
            SDL_RenamePath((path + ".lock").c_str(), (target + ".lock").c_str());
        std::string backups = Folder() + "/backups/";
        if (Exists(backups + FileName(path)))
            SDL_RenamePath((backups + FileName(path)).c_str(), (backups + name).c_str());
        UpdateIndex([&](Json& index) {
            Json entries = index["figures"];
            if (entries.Has(FileName(path)))
            {
                Json entry = entries[FileName(path)];
                entries.Erase(FileName(path));
                const std::string partner = entry["partner"].AsString();
                if (!partner.empty() && entries[partner]["partner"].AsString() == FileName(path))
                {
                    Json other = entries[partner];
                    other.Set("partner", name);
                    entries.Set(partner, std::move(other));
                }
                entries.Set(name, std::move(entry));
                index.Set("figures", std::move(entries));
            }
        });
        return true;
    }

    bool SetLabel(const std::string& path, const std::string& label)
    {
        return UpdateFigure(path, [&](Json& entry) {
            if (label.empty())
                entry.Erase("label");
            else
                entry.Set("label", label);
        });
    }

    bool SetFavourite(const std::string& path, bool favourite)
    {
        return UpdateFigure(path, [&](Json& entry) {
            if (favourite)
                entry.Set("favourite", true);
            else
                entry.Erase("favourite");
        });
    }

    bool SetEdition(const std::string& path, const std::string& edition, VariantKind kind)
    {
        return UpdateFigure(path, [&](Json& entry) {
            if (edition.empty())
            {
                entry.Erase("edition");
                entry.Erase("edition_kind");
                return;
            }
            entry.Set("edition", edition);
            entry.Set("edition_kind", VariantKindId(kind));
        });
    }

    void Touch(const std::string& path, const std::string& game)
    {
        UpdateFigure(path, [&](Json& entry) {
            std::string now = Now();
            entry.Set("last_game", game);
            entry.Set("last_used", now);
            Json games = entry["games"].IsObject() ? entry["games"] : Json::Object();
            Json use = Json::Object();
            use.Set("last_used", now);
            use.Set("uses", games[game]["uses"].AsInt() + 1);
            games.Set(game, std::move(use));
            entry.Set("games", std::move(games));
        });
    }

    std::vector<std::string> Recent(const std::string& game, size_t count)
    {
        Json index = LoadJson(PathOf(IndexName));
        std::vector<std::pair<std::string, std::string>> used; // time, file
        for (const auto& [file, entry] : index["figures"].Members())
        {
            const std::string& when = entry["games"][game]["last_used"].AsString();
            if (!when.empty() && Exists(PathOf(file)))
                used.emplace_back(when, file);
        }
        std::stable_sort(used.begin(), used.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::string> paths;
        for (size_t i = 0; i < used.size() && i < count; i++)
            paths.push_back(PathOf(used[i].second));
        return paths;
    }

    std::string Backup(const std::string& path)
    {
        std::string folder = Folder() + "/backups/" + FileName(path);
        SDL_CreateDirectory(folder.c_str());
        std::string stamp = Now();
        stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end()); // 2026-10-24T101500Z
        std::string backup = folder + "/" + stamp + ".sky";
        for (int n = 2; Exists(backup); n++)
            backup = folder + "/" + stamp + "-" + std::to_string(n) + ".sky";
        return CopyBytes(path, backup) ? backup : std::string();
    }

    std::vector<std::string> Backups(const std::string& path)
    {
        std::string folder = Folder() + "/backups/" + FileName(path);
        std::vector<std::string> backups;
        for (const std::string& name : DumpNames(folder))
            backups.push_back(folder + "/" + name);
        std::sort(backups.rbegin(), backups.rend());
        return backups;
    }

    bool Restore(const std::string& path, const std::string& backup, std::string* error)
    {
        if (GetLock(path).Busy())
            return Refuse(error, "in use by " + GetLock(path).game);
        FigureData tag;
        if (!LoadDump(backup, tag))
            return Refuse(error, "the backup is not a figure dump");
        if (Exists(path) && Backup(path).empty())
            return Refuse(error, "cannot back up the current dump first");
        bool copied = CopyBytes(backup, path);
        PruneBackups(path);
        return copied || Refuse(error, "cannot write the file");
    }

    int PruneBackups(const std::string& path, size_t keep)
    {
        std::vector<std::string> backups = Backups(path);
        int removed = 0;
        for (size_t i = keep; i < backups.size(); i++)
            removed += SDL_RemovePath(backups[i].c_str()) ? 1 : 0;
        return removed;
    }

    bool ResetToNew(const std::string& path, std::string* error)
    {
        if (GetLock(path).Busy())
            return Refuse(error, "in use by " + GetLock(path).game);
        FigureData tag;
        if (!LoadDump(path, tag))
            return Refuse(error, "not a figure dump");
        if (Backup(path).empty())
            return Refuse(error, "cannot back up the dump first");
        PruneBackups(path);
        // Sectors 1-15 hold the progress (encrypted; a zero block stays zero): cleared but for the trailers (keys).
        for (size_t block = 4; block < FigureBlockCount; block++)
        {
            if (block % 4 != 3)
                memset(tag.data() + block * FigureBlockSize, 0, FigureBlockSize);
        }
        return SaveDump(path, tag) || Refuse(error, "cannot write the file");
    }

    namespace
    {
        // One dump, copied with its progress and metadata (not its link).
        std::string DuplicateHalf(const std::string& path, std::string* error)
        {
            Figure figure;
            FigureData tag;
            if (!Read(path, figure) || !LoadDump(path, tag))
                return Refuse(error, "not a figure dump"), std::string();
            // Same progress under a new serial: every block's key depends on blocks 0 and 1, so decrypt and encrypt again.
            FigureData plain = figures::Decrypt(tag);
            FigureData copy = figures::CreateFigure(figure.type, figure.variant);
            for (size_t block = 4; block < FigureBlockCount; block++)
            {
                if (block % 4 != 3)
                    memcpy(copy.data() + block * FigureBlockSize, plain.data() + block * FigureBlockSize, FigureBlockSize);
            }
            size_t dot = figure.file.find_last_of('.');
            std::string stem = dot == std::string::npos ? figure.file : figure.file.substr(0, dot);
            std::string target = SaveNew(figures::Encrypt(copy), stem + " (copy).sky");
            if (target.empty())
                return Refuse(error, "cannot write the copy"), std::string();
            UpdateFigure(target, [&](Json& entry) {
                entry.Set("label", figure.DisplayName() + " (copy)");
                entry.Set("origin", "created");
                if (!figure.edition.empty())
                {
                    entry.Set("edition", figure.edition);
                    entry.Set("edition_kind", VariantKindId(figure.editionKind));
                }
            });
            return target;
        }
    }

    std::string Duplicate(const std::string& path, std::string* error)
    {
        Figure figure;
        if (!Read(path, figure))
            return Refuse(error, "not a figure dump"), std::string();
        std::string copy = DuplicateHalf(path, error);
        if (!copy.empty() && !figure.partner.empty())
        {
            std::string other = DuplicateHalf(PathOf(figure.partner), nullptr);
            if (!other.empty())
                Pair(copy, other);
        }
        return copy;
    }

    // --- Swappers --------------------------------------------------------------------------------------------------------

    namespace
    {
        // Whether the half or its partner is in use by another process (the reason in `error`).
        bool InUse(const Figure& figure, std::string* error)
        {
            for (const std::string& path : { figure.path, figure.partner.empty() ? std::string() : PathOf(figure.partner) })
            {
                LockInfo lock = GetLock(path);
                if (!path.empty() && lock.Busy())
                {
                    Refuse(error, FileName(path) + " is in use by " + lock.game);
                    return true;
                }
            }
            return false;
        }

        // Writes (or, with an empty `to`, removes) the "partner" entry of the dump `file`.
        void SetLink(Json& entries, const std::string& file, const std::string& to)
        {
            if (to.empty() && !entries.Has(file))
                return;
            Json entry = entries[file].IsObject() ? entries[file] : Json::Object();
            if (to.empty())
                entry.Erase("partner");
            else
                entry.Set("partner", to);
            entries.Set(file, std::move(entry));
        }
    }

    bool Pair(const std::string& a, const std::string& b, std::string* error)
    {
        Figure fa, fb;
        if (!Read(a, fa) || !Read(b, fb))
            return Refuse(error, "not a figure dump");
        if (!((fa.IsSwapTop() && fb.IsSwapBottom()) || (fa.IsSwapBottom() && fb.IsSwapTop())))
            return Refuse(error, "a swapper is one top and one bottom");
        if (InUse(fa, error) || InUse(fb, error))
            return false;
        bool done = UpdateIndex([&](Json& index) {
            Json entries = index["figures"].IsObject() ? index["figures"] : Json::Object();
            for (const Figure* figure : { &fa, &fb })
            {
                if (!figure->partner.empty())
                    SetLink(entries, figure->partner, {});
            }
            SetLink(entries, fa.file, fb.file);
            SetLink(entries, fb.file, fa.file);
            index.Set("figures", std::move(entries));
        });
        return done || Refuse(error, "collection.json cannot be updated");
    }

    bool Unpair(const std::string& path, std::string* error)
    {
        Figure figure;
        if (!Read(path, figure))
            return Refuse(error, "not a figure dump");
        if (figure.partner.empty())
            return true;
        if (InUse(figure, error))
            return false;
        bool done = UpdateIndex([&](Json& index) {
            Json entries = index["figures"].IsObject() ? index["figures"] : Json::Object();
            SetLink(entries, figure.file, {});
            SetLink(entries, figure.partner, {});
            index.Set("figures", std::move(entries));
        });
        return done || Refuse(error, "collection.json cannot be updated");
    }

    bool ChangeHalf(const std::string& keep, const std::string& replacement, std::string* error)
    {
        Figure kept, other;
        if (!Read(keep, kept) || !Read(replacement, other))
            return Refuse(error, "not a figure dump");
        if (kept.partner == other.file)
            return true;
        std::string oldHalf = kept.partner.empty() ? std::string() : PathOf(kept.partner);
        std::string displaced = other.partner.empty() ? std::string() : PathOf(other.partner);
        if (!Pair(keep, replacement, error))
            return false;
        // The replacement's old swapper takes the half that was left over: it has the category `kept` has.
        if (!oldHalf.empty() && !displaced.empty())
            Pair(displaced, oldHalf);
        return true;
    }

    std::vector<const Figure*> Card::Halves() const
    {
        std::vector<const Figure*> halves = { first };
        if (second != nullptr)
            halves.push_back(second);
        return halves;
    }

    std::vector<Card> Cards(const std::vector<Figure>& list)
    {
        std::map<std::string, size_t> byFile;
        for (size_t i = 0; i < list.size(); i++)
            byFile[list[i].file] = i;
        std::vector<Card> cards;
        for (const Figure& figure : list)
        {
            auto other = figure.partner.empty() ? byFile.end() : byFile.find(figure.partner);
            if (other == byFile.end())
                cards.push_back({ &figure, nullptr });
            else if (figure.IsSwapTop())
                cards.push_back({ &figure, &list[other->second] });
        }
        return cards;
    }

    Card CardOf(const std::vector<Figure>& list, size_t index)
    {
        const Figure& figure = list[index];
        for (const Figure& other : list)
        {
            if (!figure.partner.empty() && other.file == figure.partner)
                return figure.IsSwapTop() ? Card{ &figure, &other } : Card{ &other, &figure };
        }
        return { &figure, nullptr };
    }

    std::string SwapperName(const std::string& top, const std::string& bottom)
    {
        auto trimSuffix = [](std::string name) {
            for (const char* suffix : { " (top)", " (bottom)" })
            {
                size_t length = strlen(suffix);
                if (name.size() > length && name.compare(name.size() - length, length, suffix) == 0)
                    name.erase(name.size() - length);
            }
            return name;
        };
        std::string topPlain = trimSuffix(top);
        std::string bottomPlain = trimSuffix(bottom);
        size_t lastSpace = topPlain.find_last_of(' ');
        std::string topPart = lastSpace == std::string::npos ? topPlain : topPlain.substr(0, lastSpace);
        if (topPart.empty())
            topPart = topPlain;
        size_t bottomLastSpace = bottomPlain.find_last_of(' ');
        std::string bottomLast = bottomLastSpace == std::string::npos ? bottomPlain : bottomPlain.substr(bottomLastSpace + 1);
        return topPart + " " + bottomLast;
    }

    std::string CardName(const Card& card)
    {
        if (card.second == nullptr || !card.first->label.empty())
            return card.first->DisplayName();
        auto plain = [](std::string name) {
            for (const char* suffix : { " (top)", " (bottom)" })
            {
                size_t length = strlen(suffix);
                if (name.size() > length && name.compare(name.size() - length, length, suffix) == 0)
                    name.erase(name.size() - length);
            }
            return name;
        };
        // 2000 + n and 1000 + n are the same swapper's top and bottom.
        if (card.first->type - 2000 == card.second->type - 1000)
            return plain(card.first->DisplayName());
        return SwapperName(card.first->DisplayName(), card.second->DisplayName());
    }

    std::string CardLevelText(const Card& card, const std::string& game, bool french)
    {
        return LevelText(*card.first, game, french);
    }

    // --- Ownership and the tidy ------------------------------------------------------------------------------------------

    namespace
    {
        void SetOrigin(const std::string& path, const std::string& origin)
        {
            UpdateFigure(path, [&](Json& entry) { entry.Set("origin", origin); });
        }

        // ISO 8601 UTC time `seconds` ago (comparable as text with Now()).
        std::string Ago(int64_t seconds)
        {
            time_t then = time(nullptr) - time_t(seconds);
            struct tm utc;
#ifdef _WIN32
            gmtime_s(&utc, &then);
#else
            gmtime_r(&then, &utc);
#endif
            char text[32];
            strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
            return text;
        }
    }

    bool UntouchedStarter(const Figure& figure)
    {
        return figure.state == figures::AreaState::Blank && figure.variant == 0 && figure.toy != nullptr
            && figure.file == DefaultName(figure.type, 0) && figure.origin.empty() && figure.label.empty()
            && !figure.favourite && figure.edition.empty();
    }

    std::vector<std::string> UntouchedStarters()
    {
        std::vector<std::string> paths;
        for (const Figure& figure : List())
        {
            if (UntouchedStarter(figure))
                paths.push_back(figure.path);
        }
        return paths;
    }

    int Tidy()
    {
        int moved = 0;
        for (const std::string& path : UntouchedStarters())
        {
            // One on a portal right now is being played: leave it.
            if (!GetLock(path).locked && Delete(path, "tidy"))
                moved++;
        }
        UpdateIndex([](Json& index) { index.Set("tidy_done", true); });
        if (moved != 0)
            fprintf(stderr, "[collection] tidy: %d never-played figure(s) the game had made moved to the trash\n", moved);
        return moved;
    }

    bool TidyDone()
    {
        return LoadJson(PathOf(IndexName))["tidy_done"].AsBool();
    }

    // --- Trash -----------------------------------------------------------------------------------------------------------

    bool Delete(const std::string& path, const std::string& reason, std::string* error)
    {
        LockInfo lock = GetLock(path);
        if (lock.Busy())
            return Refuse(error, "in use by " + lock.game);
        std::string folder = Folder() + "/trash";
        SDL_CreateDirectory(folder.c_str());
        std::string stamp = Now();
        stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
        std::string id = stamp + " " + FileName(path);
        for (int n = 2; Exists(folder + "/" + id); n++)
            id = stamp + "-" + std::to_string(n) + " " + FileName(path);
        // The index first: when it cannot be written (collection.json unreadable or bad) nothing has moved, so the file and
        // its metadata never disagree; when the move fails the entry is taken back out.
        auto record = [&](bool inTrash) {
            return UpdateIndex([&](Json& index) {
                Json entries = index["figures"].IsObject() ? index["figures"] : Json::Object();
                Json trash = index["trash"].IsObject() ? index["trash"] : Json::Object();
                if (inTrash)
                {
                    Json item = Json::Object();
                    item.Set("file", FileName(path));
                    item.Set("deleted", Now());
                    item.Set("reason", reason);
                    if (entries.Has(FileName(path)))
                    {
                        item.Set("metadata", entries[FileName(path)]);
                        entries.Erase(FileName(path));
                    }
                    trash.Set(id, std::move(item));
                }
                else
                {
                    if (trash[id]["metadata"].IsObject())
                        entries.Set(FileName(path), trash[id]["metadata"]);
                    trash.Erase(id);
                }
                index.Set("figures", std::move(entries));
                index.Set("trash", std::move(trash));
            });
        };
        if (!record(true))
            return Refuse(error, "collection.json cannot be updated (unreadable, or kept as .bad-*)");
        if (!SDL_RenamePath(path.c_str(), (folder + "/" + id).c_str()))
        {
            record(false);
            return Refuse(error, "cannot move the file to the trash");
        }
        if (lock.locked)
            BreakLock(path);
        return true;
    }

    std::vector<TrashEntry> Trash()
    {
        Json index = LoadJson(PathOf(IndexName));
        std::vector<TrashEntry> entries;
        for (const auto& [id, item] : index["trash"].Members())
        {
            std::string path = Folder() + "/trash/" + id;
            if (Exists(path))
                entries.push_back({ id, item["file"].AsString(), item["deleted"].AsString(), item["reason"].AsString(), path });
        }
        std::stable_sort(entries.begin(), entries.end(), [](const TrashEntry& a, const TrashEntry& b) { return a.deleted > b.deleted; });
        return entries;
    }

    std::string Undelete(const std::string& id)
    {
        Json index = LoadJson(PathOf(IndexName));
        const Json& item = index["trash"][id];
        std::string from = Folder() + "/trash/" + id;
        if (!item.IsObject() || !Exists(from))
            return {};
        std::string name = FreeName(item["file"].AsString());
        if (!SDL_RenamePath(from.c_str(), PathOf(name).c_str()))
            return {};
        UpdateIndex([&](Json& index) {
            Json trash = index["trash"];
            if (trash[id]["metadata"].IsObject())
            {
                Json entries = index["figures"].IsObject() ? index["figures"] : Json::Object();
                entries.Set(name, trash[id]["metadata"]);
                index.Set("figures", std::move(entries));
            }
            trash.Erase(id);
            index.Set("trash", std::move(trash));
        });
        return PathOf(name);
    }

    bool DeleteForGood(const std::string& id)
    {
        bool removed = SDL_RemovePath((Folder() + "/trash/" + id).c_str());
        UpdateIndex([&](Json& index) {
            Json trash = index["trash"];
            trash.Erase(id);
            index.Set("trash", std::move(trash));
        });
        return removed;
    }

    int PurgeTrash(int days)
    {
        std::string cutoff = Ago(int64_t(days) * 86400);
        int purged = 0;
        for (const TrashEntry& entry : Trash())
        {
            if (entry.deleted < cutoff && DeleteForGood(entry.id))
                purged++;
        }
        return purged;
    }

    // --- Import and export ---------------------------------------------------------------------------------------------

    namespace
    {
        // A 1 KB tag image: block 1's checksum over blocks 0-1 is right.
        bool IsTag(const std::string& bytes)
        {
            if (bytes.size() < sizeof(FigureData))
                return false;
            auto data = reinterpret_cast<const uint8_t*>(bytes.data());
            return FigureChecksum(data, 0x1E) == uint16_t(data[0x1E] | data[0x1F] << 8);
        }

        // The first 1 KB of every dump of the collection, to skip copies.
        std::vector<std::string> Tags()
        {
            std::vector<std::string> tags;
            for (const std::string& name : DumpNames(Folder()))
            {
                std::string bytes;
                if (LoadBytes(PathOf(name), bytes) && bytes.size() >= sizeof(FigureData))
                    tags.push_back(bytes.substr(0, sizeof(FigureData)));
            }
            return tags;
        }
    }

    ImportReport Import(const std::vector<std::string>& paths)
    {
        ImportReport report;
        std::vector<std::string> files;
        for (const std::string& path : paths)
        {
            SDL_PathInfo info;
            if (SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY)
            {
                for (const std::string& name : DumpNames(path))
                    files.push_back(path + "/" + name);
            }
            else
                files.push_back(path);
        }
        std::vector<std::string> tags = Tags();
        for (const std::string& file : files)
        {
            std::string bytes;
            if (!LoadBytes(file, bytes) || !IsTag(bytes))
            {
                report.invalid++;
                continue;
            }
            std::string tag = bytes.substr(0, sizeof(FigureData));
            if (std::find(tags.begin(), tags.end(), tag) != tags.end())
            {
                report.duplicates++;
                continue;
            }
            std::string name = FreeName(SafeName(IsDumpName(FileName(file)) ? FileName(file) : FileName(file) + ".sky"));
            SDL_CreateDirectory(Folder().c_str());
            if (!SDL_SaveFile(PathOf(name).c_str(), bytes.data(), bytes.size()))
            {
                report.invalid++;
                continue;
            }
            tags.push_back(tag);
            SetOrigin(PathOf(name), "imported");
            report.added++;
            report.paths.push_back(PathOf(name));
            uint16_t type = uint16_t(uint8_t(tag[0x10]) | uint8_t(tag[0x11]) << 8);
            if (Catalogue::Get().Find(type) == nullptr)
                report.unknown.push_back(type);
        }
        fprintf(stderr, "[collection] import: %d added, %d already there, %d not figure dumps\n", report.added,
            report.duplicates, report.invalid);
        return report;
    }

    std::string ImportTag(const FigureData& tag, std::string* error)
    {
        std::string bytes(reinterpret_cast<const char*>(tag.data()), tag.size());
        if (!IsTag(bytes))
            return Refuse(error, "not a complete tag"), std::string();
        for (const std::string& name : DumpNames(Folder()))
        {
            std::string other;
            if (LoadBytes(PathOf(name), other) && other.substr(0, tag.size()) == bytes)
                return PathOf(name);
        }
        uint16_t type = figures::ToyType(tag), variant = figures::Variant(tag);
        std::string name = DefaultName(type, variant);
        std::string path = SaveNew(tag, name.substr(0, name.size() - 4) + " (portal).sky");
        if (path.empty())
            return Refuse(error, "cannot write the file"), std::string();
        SetOrigin(path, "portal");
        return path;
    }

    int Export(const std::vector<std::string>& paths, const std::string& folder)
    {
        SDL_CreateDirectory(folder.c_str());
        int copied = 0;
        for (const std::string& path : paths)
            copied += CopyBytes(path, folder + "/" + FileName(path)) ? 1 : 0;
        return copied;
    }

    // --- Views and the wheel ---------------------------------------------------------------------------------------------

    View GetView(const std::string& game, const std::string& app)
    {
        Json index = LoadJson(PathOf(IndexName));
        const Json& saved = index["views"][game][app];
        View view;
        if (saved.IsObject())
        {
            view.group = saved["group"].IsString() ? saved["group"].AsString() : view.group;
            view.order = saved["order"].IsString() ? saved["order"].AsString() : view.order;
            view.show = saved["show"].IsString() ? saved["show"].AsString() : view.show;
            view.foldForeign = saved["fold_foreign"].AsBool(view.foldForeign);
        }
        return view;
    }

    namespace
    {
        template <typename Change>
        bool UpdateViews(const std::string& game, Change change)
        {
            return UpdateIndex([&](Json& index) {
                Json views = index["views"].IsObject() ? index["views"] : Json::Object();
                Json forGame = views[game].IsObject() ? views[game] : Json::Object();
                change(forGame);
                views.Set(game, std::move(forGame));
                index.Set("views", std::move(views));
            });
        }
    }

    bool SetView(const std::string& game, const std::string& app, const View& view)
    {
        return UpdateViews(game, [&](Json& forGame) {
            Json saved = Json::Object();
            saved.Set("group", view.group);
            saved.Set("order", view.order);
            saved.Set("show", view.show);
            saved.Set("fold_foreign", view.foldForeign);
            forGame.Set(app, std::move(saved));
        });
    }

    std::vector<WheelPlace> GetWheel(const std::string& game, const std::string& seat)
    {
        Json index = LoadJson(PathOf(IndexName));
        const Json& saved = index["views"][game]["wheel"][seat];
        std::vector<WheelPlace> places(8);
        for (size_t i = 0; i < places.size() && i < saved.Items().size(); i++)
            places[i] = { saved.Items()[i]["file"].AsString(), saved.Items()[i]["pinned"].AsBool() };
        return places;
    }

    bool SetWheel(const std::string& game, const std::string& seat, const std::vector<WheelPlace>& places)
    {
        return UpdateViews(game, [&](Json& forGame) {
            Json wheel = forGame["wheel"].IsObject() ? forGame["wheel"] : Json::Object();
            Json list = Json::Array();
            for (const WheelPlace& place : places)
            {
                Json item = Json::Object();
                item.Set("file", place.file);
                if (place.pinned)
                    item.Set("pinned", true);
                list.Push(std::move(item));
            }
            wheel.Set(seat, std::move(list));
            forGame.Set("wheel", std::move(wheel));
        });
    }

    std::vector<std::string> GetWheelSkipped(const std::string& game, const std::string& seat)
    {
        Json index = LoadJson(PathOf(IndexName));
        std::vector<std::string> files;
        for (const Json& item : index["views"][game]["wheel_skipped"][seat].Items())
            files.push_back(item.AsString());
        return files;
    }

    bool SetWheelSkipped(const std::string& game, const std::string& seat, const std::vector<std::string>& files)
    {
        return UpdateViews(game, [&](Json& forGame) {
            Json skipped = forGame["wheel_skipped"].IsObject() ? forGame["wheel_skipped"] : Json::Object();
            Json list = Json::Array();
            for (const std::string& file : files)
                list.Push(Json(file));
            skipped.Set(seat, std::move(list));
            forGame.Set("wheel_skipped", std::move(skipped));
        });
    }

    namespace
    {
        // The player seats of a game that read a figure (their ids: "p1", "p2").
        std::vector<std::string> WheelSeats(const std::string& game, const Figure& figure)
        {
            std::vector<std::string> seats;
            const Game* g = Catalogue::Get().FindGame(game);
            if (g == nullptr || figure.toy == nullptr || !Catalogue::Get().Accepts(game, figure.type))
                return seats;
            for (const Seat& seat : g->seats)
            {
                if (seat.player > 0 && std::find(seat.accepts.begin(), seat.accepts.end(), figure.toy->category) != seat.accepts.end())
                    seats.push_back(seat.id);
            }
            return seats;
        }
    }

    bool OnWheel(const std::string& game, const std::string& file)
    {
        Figure figure;
        if (!Read(PathOf(file), figure))
            return false;
        if (figure.IsSwapBottom() && !figure.partner.empty())
            return OnWheel(game, figure.partner);
        for (const std::string& seat : WheelSeats(game, figure))
        {
            for (const WheelPlace& place : GetWheel(game, seat))
            {
                if (place.pinned && place.file == file)
                    return true;
            }
        }
        return false;
    }

    bool SetOnWheel(const std::string& game, const std::string& file, bool on)
    {
        Figure figure;
        if (!Read(PathOf(file), figure))
            return false;
        // A swapper's card is its top: the wheel keeps the top's file.
        if (figure.IsSwapBottom() && !figure.partner.empty())
            return SetOnWheel(game, figure.partner, on);
        std::vector<std::string> seats = WheelSeats(game, figure);
        bool done = !seats.empty();
        for (const std::string& seat : seats)
        {
            std::vector<WheelPlace> places = GetWheel(game, seat);
            std::vector<std::string> skipped = GetWheelSkipped(game, seat);
            std::erase(skipped, file);
            if (on)
            {
                auto here = std::find_if(places.begin(), places.end(), [&](const WheelPlace& p) { return p.file == file; });
                if (here == places.end())
                    here = std::find_if(places.begin(), places.end(), [](const WheelPlace& p) { return p.file.empty(); });
                if (here == places.end())
                    here = std::find_if(places.begin(), places.end(), [](const WheelPlace& p) { return !p.pinned; });
                if (here == places.end())
                {
                    done = false;
                    continue;
                }
                *here = { file, true };
            }
            else
            {
                for (WheelPlace& place : places)
                {
                    if (place.file == file)
                        place = {};
                }
                skipped.push_back(file);
            }
            done = SetWheel(game, seat, places) && SetWheelSkipped(game, seat, skipped) && done;
        }
        return done;
    }

    // --- Sections --------------------------------------------------------------------------------------------------------

    GroupBy ParseGroupBy(const std::string& id)
    {
        static const char* const Ids[] = { "smart", "element", "game", "category", "series", "level", "name" };
        for (size_t i = 0; i < std::size(Ids); i++)
        {
            if (id == Ids[i])
                return GroupBy(i);
        }
        return GroupBy::Smart;
    }

    OrderBy ParseOrderBy(const std::string& id)
    {
        static const char* const Ids[] = { "recent", "level", "name", "toy" };
        for (size_t i = 0; i < std::size(Ids); i++)
        {
            if (id == Ids[i])
                return OrderBy(i);
        }
        return OrderBy::Recent;
    }

    namespace
    {
        // The elements in the order the games' menus show them.
        constexpr Element ElementOrder[] = { Element::Magic, Element::Earth, Element::Water, Element::Air, Element::Fire,
            Element::Life, Element::Undead, Element::Tech, Element::Light, Element::Dark };

        // Variants and other editions of a character sort together: "Legendary Spyro" next to "Spyro".
        std::string CharacterKey(const Figure& figure)
        {
            std::string name = figure.toy != nullptr ? figure.toy->name : figure.file;
            for (const char* prefix : { "Legendary ", "Dark " })
            {
                if (name.rfind(prefix, 0) == 0 && name.size() > strlen(prefix))
                    name = name.substr(strlen(prefix));
            }
            return Lower(name);
        }
    }

    std::vector<Section> Group(const std::vector<Figure>& list, GroupBy group, OrderBy order, const std::string& game)
    {
        const Catalogue& catalogue = Catalogue::Get();
        auto foreign = [&](const Figure& f) { return f.toy != nullptr && !game.empty() && !catalogue.Accepts(game, f.type); };

        // Order inside a section: characters by the chosen key (a character's best member), the plain toy first.
        std::map<std::string, std::string> recentOf; // character key -> its newest use (text compares as time)
        std::map<std::string, int> levelOf;
        for (const Figure& f : list)
        {
            std::string key = CharacterKey(f), when = f.LastUsedIn(game);
            recentOf[key] = std::max(recentOf[key], when);
            levelOf[key] = std::max(levelOf[key], f.level);
        }
        auto before = [&](size_t ia, size_t ib) {
            const Figure &a = list[ia], &b = list[ib];
            std::string ka = CharacterKey(a), kb = CharacterKey(b);
            if (ka != kb)
            {
                switch (order)
                {
                case OrderBy::Recent:
                    if (recentOf[ka] != recentOf[kb])
                        return recentOf[ka] > recentOf[kb];
                    break;
                case OrderBy::Level:
                    if (levelOf[ka] != levelOf[kb])
                        return levelOf[ka] > levelOf[kb];
                    break;
                case OrderBy::Name: return ka < kb;
                case OrderBy::Toy: break;
                }
                return ia < ib; // List's order: category, element, toy type
            }
            if ((a.toy != nullptr && a.toy->edition != VariantKind::Base) != (b.toy != nullptr && b.toy->edition != VariantKind::Base))
                return b.toy != nullptr && b.toy->edition != VariantKind::Base;
            if (a.type != b.type || a.variant != b.variant)
                return a.type != b.type ? a.type < b.type : a.variant < b.variant;
            if (order == OrderBy::Level && a.level != b.level)
                return a.level > b.level;
            return ia < ib;
        };

        std::vector<Section> sections;
        auto add = [&](Section::Kind kind, int value, const std::string& gameId, std::vector<size_t> items, bool sort = true) {
            if (items.empty())
                return;
            if (sort)
                std::stable_sort(items.begin(), items.end(), before);
            sections.push_back({ kind, value, gameId, std::move(items) });
        };

        // A linked swapper is one card, its top's: its bottom is left out.
        std::map<std::string, size_t> byFile;
        for (size_t i = 0; i < list.size(); i++)
            byFile[list[i].file] = i;
        std::vector<size_t> main, unknown, others;
        for (size_t i = 0; i < list.size(); i++)
        {
            if (list[i].IsSwapBottom() && !list[i].partner.empty() && byFile.count(list[i].partner) != 0)
                continue;
            if (list[i].toy == nullptr)
                unknown.push_back(i);
            else if (foreign(list[i]))
                others.push_back(i);
            else
                main.push_back(i);
        }

        auto byElement = [&]() {
            for (Element element : ElementOrder)
            {
                std::vector<size_t> items;
                for (size_t i : main)
                {
                    if (list[i].toy->element == element)
                        items.push_back(i);
                }
                add(Section::Kind::Element, int(element), {}, items);
            }
            // Toys without an element (items, adventure packs, traps...): by category.
            for (int category = 0; category < int(Category::Count); category++)
            {
                std::vector<size_t> items;
                for (size_t i : main)
                {
                    if (list[i].toy->element >= Element::None && int(list[i].toy->category) == category)
                        items.push_back(i);
                }
                add(Section::Kind::Category, category, {}, items);
            }
        };

        switch (group)
        {
        case GroupBy::Smart:
        {
            // The shelf: every figure once. Favourites first, then the figures this game took (newest first), then the
            // rest by element (the menus' order) and category; a character's variants next to it.
            auto rank = [&](size_t i) {
                const Toy* toy = list[i].toy;
                for (size_t e = 0; e < std::size(ElementOrder); e++)
                {
                    if (toy->element == ElementOrder[e])
                        return int(e);
                }
                return 100 + int(toy->category);
            };
            std::vector<size_t> items = main;
            std::stable_sort(items.begin(), items.end(), [&](size_t a, size_t b) {
                if (list[a].favourite != list[b].favourite)
                    return list[a].favourite;
                std::string ua = list[a].LastUsedIn(game), ub = list[b].LastUsedIn(game);
                if (ua != ub)
                    return ua > ub; // newest first; never taken ("") last
                if (rank(a) != rank(b))
                    return rank(a) < rank(b);
                return before(a, b);
            });
            add(Section::Kind::Shelf, 0, {}, items, false);
            break;
        }
        case GroupBy::Element: byElement(); break;
        case GroupBy::Game:
            for (const Game& g : catalogue.Games())
            {
                std::vector<size_t> items;
                for (size_t i : main)
                {
                    if (list[i].toy->game == g.id)
                        items.push_back(i);
                }
                add(Section::Kind::Game, 0, g.id, items);
            }
            break;
        case GroupBy::Category:
            for (int category = 0; category < int(Category::Count); category++)
            {
                std::vector<size_t> items;
                for (size_t i : main)
                {
                    if (int(list[i].toy->category) == category)
                        items.push_back(i);
                }
                add(Section::Kind::Category, category, {}, items);
            }
            break;
        case GroupBy::Series:
            for (int kind = 0; kind < int(VariantKind::Count); kind++)
            {
                std::vector<size_t> items;
                for (size_t i : main)
                {
                    VariantKind k = list[i].Kind() != VariantKind::Base ? list[i].Kind() : list[i].toy->edition;
                    if (int(k) == kind)
                        items.push_back(i);
                }
                add(Section::Kind::Series, kind, {}, items);
            }
            break;
        case GroupBy::Level:
            for (int level = figures::MaxLevelSwapForce; level >= 1; level--)
            {
                std::vector<size_t> items;
                for (size_t i : main)
                {
                    bool progress = list[i].toy->category <= Category::Mini;
                    if (progress && list[i].level == level)
                        items.push_back(i);
                }
                add(Section::Kind::Level, level, {}, items);
            }
            for (int category = int(Category::Trap); category < int(Category::Count); category++)
            {
                std::vector<size_t> items;
                for (size_t i : main)
                {
                    if (int(list[i].toy->category) == category)
                        items.push_back(i);
                }
                add(Section::Kind::Category, category, {}, items);
            }
            break;
        case GroupBy::Name:
        {
            std::map<int, std::vector<size_t>> letters;
            for (size_t i : main)
            {
                std::string name = list[i].DisplayName();
                int letter = !name.empty() && isalpha(uint8_t(name[0])) ? toupper(uint8_t(name[0])) : '#';
                letters[letter].push_back(i);
            }
            OrderBy saved = order;
            order = OrderBy::Name;
            for (auto& [letter, items] : letters)
                add(Section::Kind::Letter, letter, {}, items);
            order = saved;
            break;
        }
        }
        add(Section::Kind::Unknown, 0, {}, unknown);
        add(Section::Kind::Foreign, 0, {}, others);
        return sections;
    }

    // --- Migration -----------------------------------------------------------------------------------------------------

    std::string LegacyFolder()
    {
        if (const char* from = skyenv::Get("SKY_MIGRATE_FROM"))
            return from;
        if (skyenv::Get("SKY_COLLECTION") != nullptr || skyenv::Get("SKY_FIGURES") != nullptr)
            return {};
        // Where the game's own figures folder is now; the datadir migration moved it there from SSARecomp/figures.
        std::filesystem::path folder = datadir::Game("ssa");
        return folder.empty() ? std::string() : datadir::Text(folder / "figures");
    }

    Migration Migrate(const std::string& from)
    {
        Migration result;
        std::string source = from;
        while (source.size() > 1 && (source.back() == '/' || source.back() == '\\'))
            source.pop_back();
        std::vector<std::string> names = DumpNames(source);
        if (source.empty() || names.empty() || source == Folder())
            return result;

        Json index = LoadJson(PathOf(IndexName));
        const Json& done = index["migrated"][source];
        std::vector<std::pair<std::string, std::string>> moved;
        std::vector<std::string> existing = DumpNames(Folder());
        for (const std::string& name : names)
        {
            if (done.Has(name))
                continue;
            std::string bytes;
            if (!LoadBytes(source + "/" + name, bytes) || bytes.size() < sizeof(FigureData))
            {
                result.failed++;
                continue;
            }
            // The same dump already in the collection (a second run, a copy made by hand): point at it.
            std::string target;
            for (const std::string& other : existing)
            {
                std::string otherBytes;
                if (LoadBytes(PathOf(other), otherBytes) && otherBytes == bytes)
                {
                    target = other;
                    break;
                }
            }
            if (!target.empty())
                result.matched++;
            else
            {
                SDL_CreateDirectory(Folder().c_str());
                target = FreeName(name);
                if (!SDL_SaveFile(PathOf(target).c_str(), bytes.data(), bytes.size()))
                {
                    result.failed++;
                    continue;
                }
                existing.push_back(target);
                result.copied++;
            }
            moved.emplace_back(name, target);
        }
        if (!moved.empty())
        {
            UpdateIndex([&](Json& index) {
                Json migrated = index["migrated"].IsObject() ? index["migrated"] : Json::Object();
                Json files = migrated[source].IsObject() ? migrated[source] : Json::Object();
                for (const auto& [name, target] : moved)
                    files.Set(name, target);
                migrated.Set(source, std::move(files));
                index.Set("migrated", std::move(migrated));
            });
        }
        if (result.copied + result.matched + result.failed != 0)
            fprintf(stderr, "[collection] migrated %s: %d copied, %d already there, %d failed (originals kept)\n",
                source.c_str(), result.copied, result.matched, result.failed);
        return result;
    }

    std::string MigratedPath(const std::string& path)
    {
        Json index = LoadJson(PathOf(IndexName));
        const Json& target = index["migrated"][Directory(path)][FileName(path)];
        return target.IsString() && Exists(PathOf(target.AsString())) ? PathOf(target.AsString()) : path;
    }
}

namespace collection
{
    std::string SeatTag(const Seat& seat, bool french)
    {
        if (seat.id.size() == 2 && seat.id[0] == 'p')
            return (french ? "J" : "P") + seat.id.substr(1);
        if (seat.id.rfind("vehicle", 0) == 0)
            return "V" + seat.id.substr(7);
        if (seat.id == "item")
            return french ? "Objet" : "Item";
        if (seat.id == "adventure")
            return french ? "Aventure" : "Adventure";
        if (seat.id == "trap")
            return french ? "Piège" : "Trap";
        return seat.id;
    }

    std::string SeatName(const Seat& seat, bool french)
    {
        if (seat.id.size() == 2 && seat.id[0] == 'p')
            return (french ? "Joueur " : "Player ") + seat.id.substr(1);
        if (seat.id.rfind("vehicle", 0) == 0)
            return (french ? "Véhicule " : "Vehicle ") + seat.id.substr(7);
        if (seat.id == "item")
            return french ? "Objet magique" : "Magic item";
        return SeatTag(seat, french);
    }

    std::vector<PortalEntry> PortalOverview(const Game& game, const std::vector<std::string>& paths,
        const std::vector<std::string>& names, const std::vector<int>& slots, bool french)
    {
        std::vector<PortalEntry> entries;
        for (size_t i = 0; i < game.seats.size() && i < paths.size(); i++)
        {
            if (paths[i].empty())
                continue;
            PortalEntry entry;
            entry.seat = i;
            entry.slot = i < slots.size() ? slots[i] : -1;
            entry.tag = SeatTag(game.seats[i], french);
            entry.seatName = SeatName(game.seats[i], french);
            entry.path = paths[i];
            entry.name = i < names.size() && !names[i].empty() ? names[i] : paths[i].substr(paths[i].find_last_of("/\\") + 1);
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    std::string PortalOverviewLine(const std::vector<PortalEntry>& entries, const std::string& emptyText)
    {
        std::string line;
        for (const PortalEntry& entry : entries)
            line += (line.empty() ? "" : " \xC2\xB7 ") + entry.tag + " " + entry.name;
        return line.empty() ? emptyText : line;
    }
}
