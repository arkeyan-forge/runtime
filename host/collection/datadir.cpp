#include "datadir.h"
#include "../env.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <cwchar>
#elif defined(__APPLE__)
#include <pwd.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace datadir
{
    namespace
    {
        bool Set(const char* name)
        {
            const char* value = skyenv::Get(name);
            return value != nullptr && *value != '\0';
        }

        // The user's data folder, where "Skylanders" goes: what SDL_GetPrefPath("", ...) would use.
        fs::path DataHome()
        {
#if defined(_WIN32)
            if (const wchar_t* roaming = _wgetenv(L"APPDATA"); roaming != nullptr && *roaming != L'\0')
                return fs::path(roaming);
            return {};
#elif defined(__APPLE__)
            // The passwd entry, not $HOME: it is where SDL looks, and a changed $HOME must not redirect the data.
            if (const passwd* entry = getpwuid(getuid()); entry != nullptr && entry->pw_dir != nullptr)
                return fs::path(entry->pw_dir) / "Library" / "Application Support";
            return {};
#else
            if (Set("XDG_DATA_HOME"))
                return fs::path(std::getenv("XDG_DATA_HOME"));
            if (Set("HOME"))
                return fs::path(std::getenv("HOME")) / ".local" / "share";
            return {};
#endif
        }

        std::string Stamp()
        {
            std::time_t now = std::time(nullptr);
            char text[32] = {};
            std::tm parts{};
#if defined(_WIN32)
            localtime_s(&parts, &now);
#else
            localtime_r(&now, &parts);
#endif
            std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &parts);
            return text;
        }

        std::string LogTime()
        {
            std::string stamp = Stamp();
            return stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2) + " " + stamp.substr(9, 2) + ":" +
                stamp.substr(11, 2) + ":" + stamp.substr(13, 2);
        }
    }

    fs::path Root()
    {
        if (Set("SKYLANDERS_DATA"))
        {
            std::error_code error;
            fs::path path = FromText(std::getenv("SKYLANDERS_DATA"));
            fs::path absolute = fs::absolute(path, error);
            return error ? path : absolute;
        }
        fs::path home = DataHome();
        return home.empty() ? fs::path() : home / "Skylanders";
    }

    std::string GameFolderName(std::string_view id)
    {
        if (id == "swapforce")
            return "SwapForce"; // the one exception: two words
        std::string name(id);
        if (name.size() <= 3)
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        else if (!name.empty())
            name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
        return name;
    }

    fs::path Game(std::string_view id)
    {
        fs::path root = Root();
        return root.empty() ? fs::path() : root / GameFolderName(id);
    }

    namespace
    {
        fs::path Below(const char* name)
        {
            fs::path root = Root();
            return root.empty() ? fs::path() : root / name;
        }
    }

    fs::path LauncherIni() { return Below("launcher.ini"); }
    fs::path Kits() { return Below("kit"); }
    fs::path Toolchains() { return Below("toolchains"); }
    fs::path Logs() { return Below("logs"); }

    std::string Text(const fs::path& path)
    {
        std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    fs::path FromText(std::string_view utf8)
    {
        return fs::path(std::u8string(utf8.begin(), utf8.end()));
    }

    uint64_t TrimFolder(const fs::path& folder, uint64_t maxBytes)
    {
        struct Item { fs::path path; uint64_t size; fs::file_time_type time; };
        std::vector<Item> items;
        uint64_t total = 0;
        std::error_code error;
        for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        {
            std::error_code entryError;
            if (!it->is_regular_file(entryError))
                continue;
            uint64_t size = it->file_size(entryError);
            fs::file_time_type time = it->last_write_time(entryError);
            if (entryError)
                continue;
            items.push_back({ it->path(), size, time });
            total += size;
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.time < b.time; });
        uint64_t freed = 0;
        for (const Item& item : items)
        {
            if (total <= maxBytes)
                break;
            std::error_code removeError;
            if (fs::remove(item.path, removeError))
            {
                total -= item.size;
                freed += item.size;
            }
        }
        return freed;
    }

    // --- Migration -----------------------------------------------------------------------------------------------------

    namespace
    {
        bool SameBytes(const fs::path& a, const fs::path& b)
        {
            std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
            if (!x || !y)
                return false;
            char bufferX[65536], bufferY[65536];
            for (;;)
            {
                x.read(bufferX, sizeof(bufferX));
                y.read(bufferY, sizeof(bufferY));
                if (x.gcount() != y.gcount() || !std::equal(bufferX, bufferX + x.gcount(), bufferY))
                    return false;
                if (x.gcount() == 0)
                    return true;
            }
        }

        // Every file (and folder) of `source` is in `target`, byte for byte.
        bool Covered(const fs::path& source, const fs::path& target)
        {
            std::error_code error;
            fs::file_status status = fs::symlink_status(source, error);
            if (fs::is_directory(status))
            {
                if (!fs::is_directory(target, error))
                    return false;
                for (fs::directory_iterator it(source, error), end; !error && it != end; it.increment(error))
                    if (!Covered(it->path(), target / it->path().filename()))
                        return false;
                return !error;
            }
            if (fs::is_symlink(status))
                return fs::is_symlink(target, error) && fs::read_symlink(source, error) == fs::read_symlink(target, error);
            return fs::is_regular_file(target, error) && fs::file_size(source, error) == fs::file_size(target, error) &&
                SameBytes(source, target);
        }

        // Makes a folder's files writable so remove_all can delete a tree holding read-only files (Windows).
        void RemoveTree(const fs::path& path)
        {
            std::error_code error;
            fs::remove_all(path, error);
        }

        struct Move { fs::path from, to; };

        class Migration
        {
        public:
            Migration(const OldRoots& old, const fs::path& root, const MigrationOptions& options)
                : m_old(old), m_root(root), m_options(options)
            {
                if (m_options.stamp.empty())
                    m_options.stamp = Stamp();
            }

            MigrationReport Run()
            {
                Plan();
                for (const Move& move : m_plan)
                    Apply(move);
                RewriteIni();
                for (const fs::path& folder : m_cleanup)
                    RemoveIfEmpty(folder);
                return m_report;
            }

        private:
            void Add(const fs::path& from, const fs::path& to)
            {
                std::error_code error;
                if (!from.empty() && fs::exists(fs::symlink_status(from, error)))
                    m_plan.push_back({ from, to });
            }

            // Moves every entry of `from` into `to`, one by one.
            void AddEntries(const fs::path& from, const fs::path& to)
            {
                std::error_code error;
                if (from.empty() || !fs::is_directory(from, error))
                    return;
                std::vector<fs::path> entries;
                for (fs::directory_iterator it(from, error), end; !error && it != end; it.increment(error))
                    entries.push_back(it->path());
                std::sort(entries.begin(), entries.end());
                for (const fs::path& entry : entries)
                    Add(entry, to / entry.filename());
            }

            void Plan()
            {
                fs::path game = m_root / "SSA";
                fs::path logs = m_root / "logs";
                const fs::path& a = m_old.ssarecomp;
                if (!a.empty())
                {
                    Add(a / "launcher.ini", m_root / "launcher.ini");
                    for (const char* name : { "SSARecomp.log", "SSARecomp.prev.log", "figures", "portraits", "launcher-cache" })
                        Add(a / name, game / name);
                    for (const char* name : { "skylauncher-game.log", "skylauncher-game.prev.log" })
                        Add(a / name, logs / name);
                    AddEntries(a / "SSARecomp", game); // SDL_GetPrefPath("SSARecomp", "SSARecomp")
                    m_cleanup.push_back(a / "SSARecomp");
                    m_cleanup.push_back(a);
                }
                const fs::path& s = m_old.skylauncher;
                if (!s.empty())
                {
                    // One game, one folder: the first built game (by hash) becomes SSA/. Others stay where they are.
                    std::error_code error;
                    std::vector<fs::path> built;
                    for (fs::directory_iterator it(s / "games", error), end; !error && it != end; it.increment(error))
                        if (it->is_directory(error))
                            built.push_back(it->path());
                    std::sort(built.begin(), built.end());
                    if (!built.empty())
                    {
                        AddEntries(built.front(), game);
                        m_folders.push_back({ built.front(), game });
                        m_cleanup.push_back(built.front());
                        m_cleanup.push_back(s / "games");
                    }
                    AddEntries(s / "kit", m_root / "kit");
                    m_cleanup.push_back(s / "kit");
                    m_cleanup.push_back(s);
                }
                if (!m_old.localSsarecomp.empty())
                {
                    AddEntries(m_old.localSsarecomp / "toolchains", m_root / "toolchains");
                    m_cleanup.push_back(m_old.localSsarecomp / "toolchains");
                    m_cleanup.push_back(m_old.localSsarecomp);
                }
            }

            void Log(const std::string& what, const fs::path& from, const fs::path& to)
            {
                std::error_code error;
                fs::create_directories(m_root / "logs", error);
                std::ofstream log(m_root / "logs" / "migration.log", std::ios::app | std::ios::binary);
                log << LogTime() << " " << what << " " << Text(from);
                if (!to.empty())
                    log << " -> " << Text(to);
                log << "\n";
            }

            void Apply(const Move& move)
            {
                std::error_code error;
                if (!fs::exists(fs::symlink_status(move.from, error)))
                    return; // done by an earlier run
                fs::path to = move.to;
                if (fs::exists(fs::symlink_status(to, error)))
                {
                    if (move.from.filename() == "launcher-cache")
                    {
                        // A cache made again by a game built before this layout: the one in place wins.
                        RemoveTree(move.from);
                        Log("drop (a cache, already in place)", move.from, to);
                        m_report.dropped++;
                        return;
                    }
                    if (Covered(move.from, to))
                    {
                        // An earlier run copied it and was stopped before deleting the original.
                        RemoveTree(move.from);
                        Log("drop (already there byte for byte)", move.from, to);
                        m_report.dropped++;
                        return;
                    }
                    fs::path kept = to;
                    kept += ".old-" + m_options.stamp;
                    for (int n = 2; fs::exists(fs::symlink_status(kept, error)); n++)
                    {
                        kept = to;
                        kept += ".old-" + m_options.stamp + "-" + std::to_string(n);
                    }
                    to = kept;
                    m_report.kept++;
                }
                fs::create_directories(to.parent_path(), error);
                if (error)
                {
                    Log("FAILED (cannot create " + Text(to.parent_path()) + "): " + error.message(), move.from, {});
                    m_report.failed++;
                    if (to != move.to)
                        m_report.kept--;
                    return;
                }
                if (!m_options.forceCopy)
                {
                    fs::rename(move.from, to, error);
                    if (!error)
                    {
                        Log("move", move.from, to);
                        m_report.moved++;
                        m_done.push_back({ move.from, to });
                        return;
                    }
                }
                if (CopyThrough(move.from, to))
                {
                    Log("copy, verify, delete", move.from, to);
                    m_report.copied++;
                    m_done.push_back({ move.from, to });
                    return;
                }
                Log("FAILED (left in place)", move.from, to);
                m_report.failed++;
                if (to != move.to)
                    m_report.kept--;
            }

            // Copy beside the target, check, rename into place, then delete the original. False: the original stays.
            bool CopyThrough(const fs::path& from, const fs::path& to)
            {
                std::error_code error;
                fs::path temporary = to;
                temporary += ".migrating";
                RemoveTree(temporary);
                fs::copy(from, temporary, fs::copy_options::recursive | fs::copy_options::copy_symlinks, error);
                if (error || !Covered(from, temporary))
                {
                    RemoveTree(temporary);
                    return false;
                }
                fs::rename(temporary, to, error);
                if (error)
                {
                    RemoveTree(temporary);
                    return false;
                }
                RemoveTree(from); // a failure here leaves a duplicate that the next run recognises and drops
                return true;
            }

            void RemoveIfEmpty(const fs::path& folder)
            {
                std::error_code error;
                if (fs::is_directory(folder, error) && fs::is_empty(folder, error) && fs::remove(folder, error))
                    Log("remove empty folder", folder, {});
            }

            // launcher.ini holds absolute paths (the built game): follow the moves, including those of an earlier run.
            void RewriteIni()
            {
                fs::path ini = m_root / "launcher.ini";
                std::error_code error;
                if (!fs::is_regular_file(ini, error))
                    return;
                std::vector<Move> pairs = m_plan;
                pairs.insert(pairs.end(), m_folders.begin(), m_folders.end());
                std::stable_sort(pairs.begin(), pairs.end(), [](const Move& a, const Move& b) {
                    return a.from.native().size() > b.from.native().size();
                });
                std::vector<std::pair<std::string, fs::path>> table;
                for (const Move& pair : pairs)
                    table.emplace_back(Slashes(Text(pair.from)), pair.to);
                for (const Move& pair : m_done) // moved with a suffix: follow where it really went
                    table.emplace_back(Slashes(Text(pair.from)), pair.to);
                if (table.empty())
                    return;

                std::ifstream in(ini, std::ios::binary);
                std::stringstream buffer;
                buffer << in.rdbuf();
                in.close();
                std::string text = buffer.str(), out;
                int changed = 0;
                size_t position = 0;
                while (position < text.size())
                {
                    size_t end = text.find('\n', position);
                    std::string line = text.substr(position, end == std::string::npos ? std::string::npos : end - position);
                    position = end == std::string::npos ? text.size() : end + 1;
                    size_t equals = line.find('=');
                    size_t first = line.find_first_not_of(" \t");
                    if (equals != std::string::npos && first != std::string::npos && line[first] != '#' && line[first] != ';' && line[first] != '[')
                    {
                        size_t start = line.find_first_not_of(" \t", equals + 1);
                        size_t stop = line.find_last_not_of(" \t\r");
                        if (start != std::string::npos && stop >= start)
                        {
                            std::string value = line.substr(start, stop - start + 1);
                            std::string rewritten = Follow(value, table);
                            if (rewritten != value)
                            {
                                line = line.substr(0, start) + rewritten + line.substr(stop + 1);
                                changed++;
                            }
                        }
                    }
                    out += line;
                    if (end != std::string::npos)
                        out += '\n';
                }
                if (changed == 0)
                    return;
                fs::path temporary = ini;
                temporary += ".tmp";
                {
                    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                    file << out;
                    if (!file)
                    {
                        file.close();
                        RemoveTree(temporary);
                        return;
                    }
                }
                fs::rename(temporary, ini, error);
                if (error)
                {
                    RemoveTree(temporary);
                    return;
                }
                Log("rewrote " + std::to_string(changed) + " path(s) in launcher.ini", ini, {});
                m_report.rewritten += changed;
            }

            static std::string Slashes(std::string text)
            {
                std::replace(text.begin(), text.end(), '\\', '/');
                return text;
            }

            static std::string Follow(const std::string& value, const std::vector<std::pair<std::string, fs::path>>& table)
            {
                std::string slashed = Slashes(value);
                for (const auto& [from, to] : table)
                {
                    if (slashed.compare(0, from.size(), from) != 0)
                        continue;
                    if (slashed.size() != from.size() && slashed[from.size()] != '/')
                        continue;
                    fs::path moved = FromText(std::string_view(slashed).substr(from.size() + (slashed.size() > from.size() ? 1 : 0)));
                    fs::path result = to;
                    if (!moved.empty())
                        result /= moved;
                    // Only a path that is gone from the old place and there in the new one.
                    std::error_code error;
                    if (fs::exists(fs::symlink_status(FromText(slashed), error)) || !fs::exists(fs::symlink_status(result, error)))
                        continue;
                    return Text(result.make_preferred());
                }
                return value;
            }

            OldRoots m_old;
            fs::path m_root;
            MigrationOptions m_options;
            MigrationReport m_report;
            std::vector<Move> m_plan, m_folders, m_done;
            std::vector<fs::path> m_cleanup;
        };
    }

    MigrationReport Migrate(const OldRoots& old, const fs::path& root, const MigrationOptions& options)
    {
        if (root.empty())
            return {};
        return Migration(old, root, options).Run();
    }

    OldRoots RealOldRoots()
    {
        OldRoots old;
        fs::path home = DataHome();
        if (home.empty())
            return old;
        old.ssarecomp = home / "SSARecomp";
        old.skylauncher = home / "Skylauncher";
#if defined(_WIN32)
        if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local != nullptr && *local != L'\0')
        {
            old.skylauncher = fs::path(local) / "Skylauncher";
            old.localSsarecomp = fs::path(local) / "SSARecomp";
        }
#endif
        return old;
    }

    MigrationReport MigrateOnce()
    {
        static bool done = false;
        if (done)
            return {};
        done = true;
        for (const char* name : { "SKYLANDERS_DATA", "SKY_LAUNCHER_DATA", "SKY_SAVE_DIR", "SKY_SETTINGS", "SKY_LAUNCHER_INI", "SKY_COLLECTION" })
            if (Set(name))
                return {};
        return Migrate(RealOldRoots(), Root());
    }
}
