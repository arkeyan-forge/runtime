#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "xex_title.h"

// A disc release a runtime was recompiled from, identified by its default.xex.
struct GameRelease
{
    const char* label;            // shown to the player, e.g. "USA, Europe"
    uint32_t titleId;
    uint32_t mediaId;
    uint32_t version;             // XEX execution info version
    const char* defaultXexSha256; // lowercase hex
};

struct GameInfo
{
    const char* id;             // settings file section
    const char* name;           // display name
    std::span<const GameRelease> releases;
    const char* runtime;        // executable that runs this game
};

// Supported games (host/launcher_games.cpp). Adding a game is adding a table entry.
extern const std::span<const GameInfo> g_games;
extern const char* const g_launcherName; // folder of the settings file in the user's data directory

const GameInfo* FindGame(std::string_view id);

enum class GameSourceStatus
{
    Ok,
    NotFound,
    Unreadable,
    Folder,         // extracted game folder: the disc reader only reads images for now
    NotXboxDisc,    // no XDVDFS volume, or the image is cut short before it
    NotXbox360Game, // XDVDFS volume without default.xex (original Xbox or data disc)
    Damaged,        // default.xex missing bytes or not an XEX2 executable
    WrongGame,      // another title ID
    WrongVersion,   // this title, but a default.xex no runtime was recompiled from
};

struct GameSourceCheck
{
    GameSourceStatus status{};
    std::string message; // why the source can't be used, for the player
};

std::string Sha256Hex(std::span<const uint8_t> data); // lowercase hex

// Checks that `path` is a disc image of one of `game`'s known releases. `progress`, when set, receives
// a 0..1 fraction and a short description of the current step; it may be called from a worker thread.
using GameSourceProgress = std::function<void(float, std::string_view)>;
GameSourceCheck CheckGameSource(const std::string& path, const GameInfo& game, const GameSourceProgress& progress = {});

// Settings file: "[section]" lines, "key=value" lines, '#' comment lines. Keys before any section go to "".
using LauncherConfig = std::map<std::string, std::map<std::string, std::string>>;
std::string LauncherConfigPath();
LauncherConfig LoadLauncherConfig(const std::string& path);
bool SaveLauncherConfig(const std::string& path, const LauncherConfig& config);

// The game's text and voice language codes (see language.h): SSA_LANGUAGE and SSA_VOICE_LANGUAGE, else the
// "language" and "voice_language" keys of the game's section in the settings file. Empty where neither picks one.
struct LanguageChoice
{
    std::string text;
    std::string voice;
};
LanguageChoice ResolveLanguages(const GameInfo& game);

// Picks the game's disc image: the command-line path, else the remembered one, else asks with a file dialog
// and remembers the answer. `--select-iso` or holding Option (macOS) at startup asks again.
// Returns an empty string when the program should quit. Never shows UI unless `interactive`.
std::string ResolveGameSource(int argc, char** argv, const GameInfo& game, bool interactive);
