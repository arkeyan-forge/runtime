#include "progress.h"
#include "datadir.h"
#include "save_backup.h"
#include "../env.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace progress
{
    namespace
    {
        constexpr const char* UnlocksFile = "achievements.txt";
        constexpr const char* PlayTimeFile = "playtime.txt";

        std::string Read(const fs::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }

        bool Write(const fs::path& file, const std::string& text)
        {
            std::error_code error;
            fs::create_directories(file.parent_path(), error);
            return savebackup::WriteFileAtomic(file, text.data(), text.size());
        }
    }

    fs::path Folder(std::string_view game)
    {
        if (const char* saves = skyenv::Get("SKY_SAVE_DIR"); saves != nullptr && *saves != '\0')
            return datadir::FromText(saves);
        return datadir::Game(game);
    }

    std::vector<Unlock> ReadUnlocks(const fs::path& folder)
    {
        std::vector<Unlock> unlocks;
        std::istringstream lines(Read(folder / UnlocksFile));
        std::string line;
        while (std::getline(lines, line))
        {
            char* end = nullptr;
            unsigned long long id = std::strtoull(line.c_str(), &end, 10);
            if (end == line.c_str() || id > 0xFFFFFFFFull)
                continue; // a comment, a blank line, or damage
            long long time = std::strtoll(end, nullptr, 10);
            unlocks.push_back({ uint32_t(id), time });
        }
        return unlocks;
    }

    bool WriteUnlock(const fs::path& folder, uint32_t id, int64_t time, bool* added)
    {
        std::vector<Unlock> unlocks = ReadUnlocks(folder);
        bool known = std::any_of(unlocks.begin(), unlocks.end(), [&](const Unlock& unlock) { return unlock.id == id; });
        if (added != nullptr)
            *added = !known;
        if (known)
            return true;
        unlocks.push_back({ id, time });
        std::string text = "# achievement id, unix time of the unlock\n";
        for (const Unlock& unlock : unlocks)
            text += std::to_string(unlock.id) + " " + std::to_string(unlock.time) + "\n";
        return Write(folder / UnlocksFile, text);
    }

    PlayTime ReadPlayTime(const fs::path& folder)
    {
        PlayTime playTime;
        std::istringstream lines(Read(folder / PlayTimeFile));
        std::string line;
        while (std::getline(lines, line))
        {
            size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            std::string key = line.substr(0, equals);
            long long value = std::strtoll(line.c_str() + equals + 1, nullptr, 10);
            if (value < 0)
                continue;
            if (key == "total")
                playTime.totalSeconds = value;
            else if (key == "last_end")
                playTime.lastEnd = value;
            else if (key == "sessions")
                playTime.sessions = int(value);
        }
        return playTime;
    }

    bool WritePlayTime(const fs::path& folder, const PlayTime& playTime)
    {
        std::string text = "total=" + std::to_string(playTime.totalSeconds) +
                           "\nlast_end=" + std::to_string(playTime.lastEnd) +
                           "\nsessions=" + std::to_string(playTime.sessions) + "\n";
        return Write(folder / PlayTimeFile, text);
    }

    void PlaySession::Begin(const fs::path& folder, int64_t wall, double mono)
    {
        m_folder = folder;
        if (m_folder.empty())
            return;
        m_base = ReadPlayTime(folder);
        m_base.sessions++;
        m_start = mono;
        m_running = true;
        Write(wall, mono);
    }

    void PlaySession::Write(int64_t wall, double mono)
    {
        PlayTime playTime = m_base;
        playTime.totalSeconds += int64_t(std::max(0.0, mono - m_start));
        playTime.lastEnd = wall;
        WritePlayTime(m_folder, playTime);
        m_lastWrite = mono;
    }

    bool PlaySession::Tick(int64_t wall, double mono, double interval)
    {
        if (!m_running || mono - m_lastWrite < interval)
            return false;
        Write(wall, mono);
        return true;
    }

    void PlaySession::End(int64_t wall, double mono)
    {
        if (!m_running)
            return;
        Write(wall, mono);
        m_running = false;
    }

    namespace
    {
        std::mutex g_lock;
        PlaySession g_session;

        int64_t Wall() { return int64_t(std::time(nullptr)); }
        double Mono()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }

    void BeginSession(std::string_view game)
    {
        std::lock_guard lock(g_lock);
        g_session.Begin(Folder(game), Wall(), Mono());
    }

    void TickSession()
    {
        std::lock_guard lock(g_lock);
        g_session.Tick(Wall(), Mono());
    }

    void EndSession()
    {
        std::lock_guard lock(g_lock);
        g_session.End(Wall(), Mono());
    }
}
