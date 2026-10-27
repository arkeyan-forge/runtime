#pragma once

#include "launcher.h"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// A game tile shown in the launcher sidebar. Only entries backed by a runtime in the game table can be played;
// the rest are placeholders for the other Skylanders games.
struct LauncherGameEntry
{
    const char* id;
    const char* name;
    const char* tagline;
    uint32_t accent; // 0xRRGGBB
    bool enabled;
};

// The six console Skylanders games, in release order. Enabled entries match g_games.
std::span<const LauncherGameEntry> LauncherGameCatalog();

// Shows the launcher window until the player starts a verified disc image, quits, or the screenshot
// test script finishes. Returns the chosen image path (empty when nothing was chosen).
// `config` is updated for settings, and `[<game>] iso=` is written when a valid image is chosen.
std::string RunLauncherWindow(const GameInfo& game, const std::string& configPath, LauncherConfig& config,
    const std::string& previous);

// Small pure helpers, also used by SSALauncherTest.
std::string LauncherLanguageLabel(const LauncherConfig& config, const GameInfo& game);
std::string LauncherVoiceLabel(const LauncherConfig& config, const GameInfo& game);
bool LauncherFullscreen(const LauncherConfig& config);
std::string LauncherFormatSize(uint64_t bytes);
std::string LauncherMiddleEllipsis(std::string_view text, size_t maxCharacters);
std::string LauncherDisplayName(std::string_view xdbfName, const GameInfo& game);

