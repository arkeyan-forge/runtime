#pragma once

// What the F3 menu changes and builds (docs/runtime/host/gamemenu.md): the settings a player changes while playing, written
// to the game's own sections of launcher.ini ([<game>.video], [<game>.audio]) and applied at once where the runtime can, the
// FPS counter's text, and the "Report a problem" file. The menu itself (look, input) is host/toybox/menu.cpp.
#include <cstdint>
#include <cstdio>
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
        uint32_t debugScreen = 0; // debugscreen::Level: off, compact, full
        bool edgeSmoothingShown = false; // the game has a known FXAA pass; without one the setting does nothing
        std::string version; // the game's version (games/<id>/game.txt), shown in the menu's corner
        bool reduceMotion = false; // [video] reduce_motion: messages fade in and out without sliding (not a menu row)
        bool portalSound = true;       // [<game>.audio] portal_sound: the trap's voice (host/portal_speaker.h)
        bool portalSoundShown = false; // the game's portal has a speaker (PortalGameHasSpeaker)
        bool perfLog = false;          // the performance log records (not saved: for this run only)
        uint32_t speakers = 0; // speakers::Mode as chosen ([<game>.audio] speakers), for the next start
        uint32_t speakersNow = 1; // the layout of this run (HostAudioLayout): stereo or 5.1
        uint32_t upscaler = 0;         // upscalemode::Upscaler: off, FSR 3 (MetalFX: internal, SKY_UPSCALER only)
        uint32_t fsrQuality = 0;       // upscalemode::Quality
        bool frameGeneration = false; // [<game>.video] fsr_frame_generation (gpu/fsr/framegen.h)
        uint32_t gpuBackend = 0; // gpubackend::Backend as chosen ([<game>.video] gpu_backend), for the next start
        bool gpuBackendShown = false; // desktop Windows, the one host with two backends
    };

    // The frame rates the menu offers, as the launcher does, and the rate a value stands for: 60 for 60 and above and for
    // no limit (0), which older versions offered, 30 under 60.
    inline constexpr uint32_t FrameRates[] = { 30, 60 };
    inline uint32_t NormalFrameRate(uint32_t fps) { return fps == 0 || fps >= 60 ? 60 : 30; }

    // `fps` under the game's cap (0: none; no limit counts as above every cap): game::Adapter::maxFpsLimit.
    inline uint32_t CapFrameRate(uint32_t fps, uint32_t cap) { return cap != 0 && (fps == 0 || fps > cap) ? cap : fps; }

    // After the window: remembers how the game started and applies the saved volume.
    void Init(const RuntimeSettings& settings, SDL_Window* window, const char* gameVersion);

    const Values& Current();

    // Each applies the new value now where it can (volume, FPS counter, fullscreen, frame rate), writes it to launcher.ini, and
    // returns false when the file could not be written (the value still applies for this run). The others apply at the next start.
    bool SetVolume(uint32_t percent);
    bool SetPortalSound(bool on);
    // Applies at the next start: the device is opened and the title told once, at start.
    bool SetSpeakers(uint32_t mode);
    bool SetShowFps(bool on);
    bool SetDebugScreen(uint32_t level);
    // Shift+F3 in play: off, compact, full, off (saved as the row is).
    bool CycleDebugScreen();
    // "Record a performance log": a CSV file in the logs folder, one line per frame, while on (debug_screen.h).
    bool SetPerfLog(bool on);
    // Copies trusted game identity and warms the shared system identity before the frame loop; `window` gives the
    // display mode and window size (nullptr: n/a).
    void InitDebugScreen(const char* game, const char* version, SDL_Window* window = nullptr);
    // Main-thread overlay integration: refresh at 2 Hz, returning true when its image needs drawing again. Also writes
    // the performance log's new lines, every call while it records.
    bool UpdateDebugScreen();
    // Inside the Toy Box's existing ImGui frame, before rasterizing its shared overlay: Outfit for the keys, a
    // monospace face for the values, Bungee for the headings.
    struct DebugFonts
    {
        ImFont* text = nullptr;
        ImFont* mono = nullptr;
        ImFont* heading = nullptr;
    };
    void DrawDebugScreen(const DebugFonts& fonts);
    // F3 held with another key, in play (toybox.cpp): G the graphs, T the thread bars (Full), C the screen as text,
    // R ten seconds of the performance log. Each returns whether it did something.
    bool ToggleDebugGraphs();
    bool ToggleDebugThreads();
    std::string DebugScreenText(); // every heading and line, for the clipboard; empty while the screen is off
    bool RecordPerfLogFor(uint64_t seconds);
    // The performance log stops by itself at its size limit or the end of RecordPerfLogFor: returns its file's name
    // once when it did (closed, the menu's row off), else empty. Main thread, every update.
    std::string FinishedPerfLog();
    void StopPerfLogAt(uint64_t sdlNs); // gamemenu_debug.cpp: the log stops at that SDL time (0: never)
    // The performance log's file (nullptr: none), written by UpdateDebugScreen; returns the one it replaces, for the
    // caller to close. A full log (PerfLogMaxBytes) stops by itself: PerfLogStopped then returns its file once.
    std::FILE* SwapPerfLog(std::FILE* file);
    std::FILE* PerfLogStopped();
    // Time the overlay took to draw the screen (toybox.cpp), counted in the screen's cost while it measures.
    void AddDebugScreenCost(uint64_t ns);
    bool SetFullscreen(bool on);
    bool SetFpsLimit(uint32_t fps);
    bool SetFpsAutoFallback(bool on);
    bool SetScale(uint32_t scale);
    bool SetAnisotropy(uint32_t level);
    bool SetVsync(bool on);
    bool SetSkipIntroMovies(bool on);
    // Desktop Windows' graphics API (gpu/gpu_backend.h), from the next start.
    bool SetGpuBackend(uint32_t backend);
    // Applies at once (the FXAA pass reads it every draw).
    bool SetEdgeSmoothing(uint32_t mode);
    // Upscaling (gpu/upscale_mode.h): applied from the next frame on (d3d/upscale.h, Request).
    bool SetUpscaler(uint32_t upscaler);
    bool SetFsrQuality(uint32_t quality);
    // FSR 3's frame generation: from the next frame on, while FSR 3 upscales.
    bool SetFrameGeneration(bool on);
    // Why the frame generation row cannot change (the row greyed with the reason).
    enum class FrameGenerationBlock
    {
        None,
        Later, // held back for players (gpu/fsr/framegen_rules.h, HeldBack)
        Device, // this graphics card's backend has no frame generation
        Game, // not clean in this game yet (game::Adapter::frameGeneration)
        Upscaler, // FSR 3 does not upscale
    };
    FrameGenerationBlock FrameGenerationBlocked();
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
        std::string path; // the file written, for the message
        std::string error; // why not
    };
    ReportResult WriteProblemReport();
}
