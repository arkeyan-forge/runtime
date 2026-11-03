#pragma once

// What the F3 menu changes and builds (docs/runtime/host/gamemenu.md): the settings a player changes while playing, written
// to the game's own sections of launcher.ini ([<game>.video], [<game>.audio]) and applied at once where the runtime can, the
// FPS counter's text, and the "Report a problem" file. The menu itself (look, input) is host/toybox/menu.cpp.
#include <cstdint>
#include <string>

struct RuntimeSettings;
struct SDL_Window;
struct ImFont;

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
        uint32_t edgeSmoothing = 0; // edgesmoothing::Mode as chosen (0: not chosen, by the resolution scale)
        bool fpsAutoFallback = true;
        bool debugScreen = false;
        bool edgeSmoothingShown = false; // the game has a known FXAA pass; without one the setting does nothing
        std::string version; // the game's version (games/<id>/game.txt), shown in the menu's corner
        bool reduceMotion = false; // [video] reduce_motion: messages fade in and out without sliding (not a menu row)
        bool portalSound = true;       // [<game>.audio] portal_sound: the trap's voice (host/portal_speaker.h)
        bool portalSoundShown = false; // the game's portal has a speaker (PortalGameHasSpeaker)
    };

    // The frame rates the menu offers, as the launcher does (0: no limit), and the rate a value stands for: a rate of the list as it
    // is, another number the one below it (30 under 60), so that opening the menu never lowers a limit.
    inline constexpr uint32_t FrameRates[] = { 30, 60, 120, 144, 240, 0 };
    inline uint32_t NormalFrameRate(uint32_t fps)
    {
        uint32_t best = 30;
        for (uint32_t rate : FrameRates)
            if (rate == fps)
                return fps;
            else if (rate != 0 && rate <= fps)
                best = rate;
        return best;
    }

    // `fps` under the game's cap (0: none; no limit counts as above every cap): game::Adapter::maxFpsLimit.
    inline uint32_t CapFrameRate(uint32_t fps, uint32_t cap) { return cap != 0 && (fps == 0 || fps > cap) ? cap : fps; }

    // After the window: remembers how the game started and applies the saved volume.
    void Init(const RuntimeSettings& settings, SDL_Window* window, const char* gameVersion);

    const Values& Current();

    // Each applies the new value now where it can (volume, FPS counter, fullscreen, frame rate), writes it to launcher.ini, and
    // returns false when the file could not be written (the value still applies for this run). The others apply at the next start.
    bool SetVolume(uint32_t percent);
    bool SetPortalSound(bool on);
    bool SetShowFps(bool on);
    bool SetDebugScreen(bool on);
    // Copies trusted game identity and warms the shared system identity before the frame loop.
    void InitDebugScreen(const char* game, const char* version);
    // Main-thread overlay integration: refresh at 2 Hz, returning true when its image needs drawing again.
    bool UpdateDebugScreen();
    // Inside the Toy Box's existing ImGui frame, before rasterizing its shared overlay.
    void DrawDebugScreen(ImFont* font);
    bool SetFullscreen(bool on);
    bool SetFpsLimit(uint32_t fps);
    bool SetFpsAutoFallback(bool on);
    bool SetScale(uint32_t scale);
    bool SetAnisotropy(uint32_t level);
    bool SetVsync(bool on);
    bool SetSkipIntroMovies(bool on);
    // Applies at once (the FXAA pass reads it every draw).
    bool SetEdgeSmoothing(uint32_t mode);
    // The mode in force: the chosen one, or by the resolution scale and the level when none was chosen (edgesmoothing::Resolve).
    uint32_t EdgeSmoothingNow();

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
