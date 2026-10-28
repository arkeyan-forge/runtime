#pragma once

// What a player has done in a game, kept in the game's data folder (docs/getting-started/settings.md): the achievements
// unlocked (achievements.txt) and the play time (playtime.txt). Plain C++ and the C library only: the runtime writes
// them, Skylauncher reads them (tools/sync_skylauncher.sh copies this file). Both files are small text files, replaced
// atomically (save_backup.h), so a crash leaves the previous version.
//
//   achievements.txt   one line per unlocked achievement: "<id> <unix time>"
//   playtime.txt       "total=<seconds>", "last_end=<unix time>", "sessions=<count>"
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace progress
{
    // The folder of a game's progress files: SKY_SAVE_DIR when set (test runs), else <root>/<Game>. Empty when there is
    // no data folder.
    std::filesystem::path Folder(std::string_view game);

    struct Unlock
    {
        uint32_t id = 0;
        int64_t time = 0; // unix time
    };
    // The unlocked achievements, in file order; empty when there is no file.
    std::vector<Unlock> ReadUnlocks(const std::filesystem::path& folder);
    // Keeps an unlock (the first time stays when the id is already there). `added` tells whether it was new. False when
    // the file cannot be written.
    bool WriteUnlock(const std::filesystem::path& folder, uint32_t id, int64_t time, bool* added = nullptr);

    struct PlayTime
    {
        int64_t totalSeconds = 0;
        int64_t lastEnd = 0; // unix time of the last session's end (updated while it runs); 0: never played
        int sessions = 0;
    };
    PlayTime ReadPlayTime(const std::filesystem::path& folder);
    bool WritePlayTime(const std::filesystem::path& folder, const PlayTime& playTime);

    // One run of the game. Begin adds a session and writes; Tick (called often) rewrites the total at most every
    // `interval` seconds; End writes the final total. Times: `wall` is unix time, `mono` a monotonic clock in seconds.
    class PlaySession
    {
    public:
        void Begin(const std::filesystem::path& folder, int64_t wall, double mono);
        bool Tick(int64_t wall, double mono, double interval = 60.0); // true when it wrote
        void End(int64_t wall, double mono);
        bool Running() const { return m_running; }

    private:
        void Write(int64_t wall, double mono);
        std::filesystem::path m_folder;
        PlayTime m_base; // what the file held at Begin (sessions already counts this one)
        double m_start = 0;
        double m_lastWrite = 0;
        bool m_running = false;
    };

    // The runtime's session (this process), with the real clocks.
    void BeginSession(std::string_view game);
    void TickSession();
    void EndSession();
}
