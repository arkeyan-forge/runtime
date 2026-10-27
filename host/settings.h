#pragma once

#include "host/launcher.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Keyboard actions of the settings file's [input] key_<action> keys, in the contract's order (docs/getting-started/settings.md).
struct KeyAction
{
    const char* name;         // "a", "left_up"...
    const char* defaultKey;   // SDL key name
};
extern const KeyAction g_keyActions[22];

// How the game starts: the [video] and [input] sections of launcher.ini, the game section's languages, then the
// command line. Precedence: command line > SSA_* variables > launcher.ini > defaults.
struct RuntimeSettings
{
    uint32_t resolutionScale = 0; // 1-10: host render targets are this multiple of the guest's; 0: auto (gpu::Init
                                  // picks it from the GPU: 1 on integrated GPUs, 2 on discrete ones and Apple's)
    bool vsync = true;
    bool fullscreen = false;      // desktop fullscreen (no mode change)
    uint32_t anisotropy = 1;      // 1, 2, 4, 8, 16
    uint32_t fpsLimit = 30;       // 30 or 60 (experimental)
    bool aspectAuto = true;       // [video] aspect: auto follows a window wider than 16:9 (Hor+ camera, wide scene and
                                  // frame, HUD centred); 16:9 keeps the game's 16:9 picture, with bars on a wider window
    bool skipIntroMovies = false; // skip the start-up logos (legal, atvilogo, xpeclogo, tfb); story movies still play
    bool textureDump = false;     // dump the game's textures as DDS files (host/texture_pack.h)
    bool texturePacks = true;     // replace textures with the files of the game's textures folder, when there is one
    std::string inputDevice = "auto"; // auto, keyboard, gamepad
    std::string gamepad;          // SDL GUID, empty: the first one connected
    std::string keys[22];         // SDL key name per g_keyActions entry
    std::string glyphs = "auto";  // button glyphs: auto (the last device used), xbox (auto among the next two), xbox360,
                                  // xboxone, playstation, nintendo, keyboard
    bool toyboxAutoOpen = true;   // the Toy Box opens by itself when the game asks for a figure ([portal] toybox_auto_open)
    std::string language;         // text language code, empty: the game's choice
    std::string voiceLanguage;

    bool help = false;            // --help
    bool version = false;         // --version
    bool listGamepads = false;    // --list-gamepads
    bool portalProbe = false;     // --portal-probe
    std::vector<std::string> arguments; // what the settings don't consume: the disc image, --select-iso, --launcher
};

// Fills `settings` from `config`, the environment (`getenv`) and the command line (argv[1..]). Bad values are reported
// in `warnings` and leave the previous source's value. Returns false on an unknown option or a missing value (`error`).
using GetEnv = std::function<const char*(const char*)>;
bool ParseSettings(const LauncherConfig& config, const std::string& gameSection, const GetEnv& getEnv, int argc,
    char** argv, RuntimeSettings& settings, std::vector<std::string>& warnings, std::string& error);

// The text of --help.
std::string SettingsHelp(const char* program);

// The effective configuration on one line, for the log.
std::string DescribeSettings(const RuntimeSettings& settings);

// Whether the last ParseSettings turned on [video] skip_intro_movies. The Bink hook (Movie_Update) uses it to end the
// start-up logos only; story movies always play.
bool SkipIntroMovies();
