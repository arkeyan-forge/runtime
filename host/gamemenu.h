#pragma once

// What the F3 menu changes and builds (docs/runtime/host/gamemenu.md): the settings a player changes while playing, written
// to the game's own sections of launcher.ini ([<game>.video], [<game>.audio]) and applied at once where the runtime can, the
// FPS counter's text, and the "Report a problem" file. The menu itself (look, input) is host/toybox/menu.cpp.
#include <cstdint>
#include <string>

struct RuntimeSettings;
struct SDL_Window;

namespace gamemenu
{
    // The values the menu shows. Scale 0 is auto; the others are the settings of the same names (host/settings.h).
    struct Values
    {
        uint32_t volume = 100;
        bool showFps = false;
        bool fullscreen = false;
        uint32_t fpsLimit = 30;
        uint32_t scale = 0;
        uint32_t anisotropy = 1;
        bool vsync = true;
        bool skipIntroMovies = false;
    };

    // After the window: remembers how the game started and applies the saved volume.
    void Init(const RuntimeSettings& settings, SDL_Window* window, const char* gameVersion);

    const Values& Current();

    // Each applies the new value now where it can (volume, FPS counter, fullscreen, frame rate), writes it to launcher.ini, and
    // returns false when the file could not be written (the value still applies for this run). The others apply at the next start.
    bool SetVolume(uint32_t percent);
    bool SetShowFps(bool on);
    bool SetFullscreen(bool on);
    bool SetFpsLimit(uint32_t fps);
    bool SetScale(uint32_t scale);
    bool SetAnisotropy(uint32_t level);
    bool SetVsync(bool on);
    bool SetSkipIntroMovies(bool on);

    // Whether the menu has saved a value that applies at the next start only.
    bool RestartNeeded();

    // The frame rate over the last half second, "30 FPS", from the frames the game presented; empty when the counter is off.
    // Called once per host frame.
    std::string FpsText();

    // "Report a problem": builds the report (report.h), writes it to the Desktop or, when that is not possible, the logs folder
    // (SKY_REPORT_DIR names the folder instead, for tests), and shows the file in the file manager (not with SKY_REPORT_DIR).
    struct ReportResult
    {
        bool ok = false;
        std::string path;  // the file written, for the message
        std::string error; // why not
    };
    ReportResult WriteProblemReport();
}
