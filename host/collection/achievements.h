#pragma once

// A game's achievements as the player's own default.xex holds them: the title's XDBF resource lists each achievement
// (id, name, descriptions, gamerscore, picture) with its texts in every console language. Read on the player's machine
// and kept in memory (or in the launcher's cache): nothing from the disc is in git. Bounds-checked: the file comes from
// the player. The runtime answers the games' achievement calls from it and Skylauncher lists it; the same file is in
// both (tools/sync_skylauncher.sh).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace achievements
{
    struct Info
    {
        uint32_t id = 0;
        std::string name;                // the title as shown in the list
        std::string unlockedDescription; // shown once unlocked
        std::string lockedDescription;   // shown while locked ("unachieved" text); empty for a hidden one
        uint32_t gamerscore = 0;
        uint32_t imageId = 0;
        uint32_t flags = 0;              // the XDBF flags: type in the low three bits, 0x8 = shown while locked
        bool hidden = false;             // the game does not show it until unlocked
        std::vector<uint8_t> imagePng;   // when asked for
    };

    struct List
    {
        uint32_t titleId = 0;
        std::vector<Info> items; // in the title's order
        uint32_t TotalGamerscore() const;
    };

    // The XDBF language number (1 English, 3 German, 4 French, 5 Spanish, 6 Italian, 9 Portuguese...) of a game language
    // code (en, fr, de, es, es-419, it, pt, ...). The languages the console has no number for (nl, sv, no, da, fi) and
    // unknown codes give English (1).
    int XdbfLanguage(std::string_view code);

    // The title's XDBF block of a default.xex (the resource named after the title ID); empty when there is none.
    std::vector<uint8_t> ReadTitleXdbf(const std::vector<uint8_t>& xex);

    // The achievements of an XDBF block; a text missing in `language` is taken from English. Empty list when the block is
    // not an XDBF or holds none.
    List Parse(const uint8_t* xdbf, size_t size, int language, bool withImages);

    // ReadTitleXdbf then Parse.
    List Read(const std::vector<uint8_t>& xex, int language, bool withImages);

    // --- The launcher's cache ----------------------------------------------------------------------------------------
    //
    // Skylauncher reads the list once, when it checks the disc, and keeps it in a folder of the game's data (never in git):
    // list.tsv (the achievements with their texts in the cached languages: en, fr, de, es, it, pt) and one <image id>.png
    // each. `key` is any text that changes with the disc (the default.xex's SHA-256): a cache with the same key is kept.
    bool CacheIsCurrent(const std::filesystem::path& folder, std::string_view key);
    // Writes the cache from a default.xex. False when it holds no achievements or a file cannot be written.
    bool WriteCache(const std::filesystem::path& folder, std::string_view key, const std::vector<uint8_t>& xex);
    // The cached list in a language code (English where a text is missing); no pictures: see CachedImage. Empty when
    // there is no cache.
    List ReadCache(const std::filesystem::path& folder, std::string_view languageCode);
    std::filesystem::path CachedImage(const std::filesystem::path& folder, uint32_t imageId);
}
