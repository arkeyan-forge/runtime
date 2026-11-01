#include "host/gamemenu.h"
#include "gpu/edge_smoothing.h"
#include "gpu/gpu.h"
#include "game.h"
#include "game_hooks.h"
#include "host/audio.h"
#include "host/collection/datadir.h"
#include "host/env.h"
#include "host/launcher.h"
#include "host/log.h"
#include "host/report.h"
#include "host/settings.h"
#include "kernel/video.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    gamemenu::Values g_values;
    SDL_Window* g_window;
    std::string g_version;
    bool g_restart;

    // The FPS counter: frames presented over the last half second.
    uint64_t g_countStart, g_countFrames;
    std::string g_fpsText = "-- FPS";

    // Writes one key of the game's own section in launcher.ini (the file the launcher shows). The file is read and written whole:
    // every other key is kept, comments are not.
    bool Save(const char* sectionSuffix, const char* key, const std::string& value)
    {
        std::string path = LauncherConfigPath();
        LauncherConfig config = LoadLauncherConfig(path);
        config[std::string(SSA_GAME_ID) + sectionSuffix][key] = value;
        if (SaveLauncherConfig(path, config))
            return true;
        fprintf(stderr, "[gamemenu] cannot write %s: %s\n", path.c_str(), SDL_GetError());
        return false;
    }

    // Every change is in the log: a report then says what the player did.
    void Logged(const char* key, const std::string& value)
    {
        fprintf(stderr, "[gamemenu] %s = %s\n", key, value.c_str());
    }

    bool SaveVideo(const char* key, const std::string& value) { return Save(".video", key, value); }

    bool SaveForNextStart(const char* key, const std::string& value)
    {
        g_restart = true;
        Logged(key, value + " (next start)");
        return SaveVideo(key, value);
    }
}

void gamemenu::Init(const RuntimeSettings& settings, SDL_Window* window, const char* gameVersion)
{
    g_window = window;
    g_version = gameVersion != nullptr ? gameVersion : "";
    g_values = { settings.volume, settings.showFps, settings.fullscreen, settings.fpsLimit, settings.resolutionScale,
        settings.anisotropy, settings.vsync, settings.skipIntroMovies, settings.edgeSmoothing, settings.fpsAutoFallback,
        ::game::HostKernelPixelShader != 0 };
    HostAudioSetVolume(settings.volume);
}

const gamemenu::Values& gamemenu::Current() { return g_values; }

bool gamemenu::SetVolume(uint32_t percent)
{
    g_values.volume = percent > 100 ? 100 : percent;
    HostAudioSetVolume(g_values.volume);
    Logged("volume", std::to_string(g_values.volume));
    return Save(".audio", "volume", std::to_string(g_values.volume));
}

bool gamemenu::SetShowFps(bool on)
{
    g_values.showFps = on;
    g_countStart = 0;
    Logged("show_fps", on ? "1" : "0");
    return SaveVideo("show_fps", on ? "1" : "0");
}

bool gamemenu::SetFullscreen(bool on)
{
    g_values.fullscreen = on;
    Logged("fullscreen", on ? "1" : "0");
    if (g_window != nullptr)
        SDL_SetWindowFullscreen(g_window, on);
    return SaveVideo("fullscreen", on ? "1" : "0");
}

bool gamemenu::SetFpsLimit(uint32_t fps)
{
    g_values.fpsLimit = CapFrameRate(NormalFrameRate(fps), game::Current().maxFpsLimit);
    SetFrameRateLimit(g_values.fpsLimit);
    Logged("fps_limit", std::to_string(g_values.fpsLimit));
    return SaveVideo("fps_limit", std::to_string(g_values.fpsLimit));
}

bool gamemenu::SetFpsAutoFallback(bool on)
{
    g_values.fpsAutoFallback = on;
    ::SetFpsAutoFallback(on);
    Logged("fps_auto_fallback", on ? "1" : "0");
    return SaveVideo("fps_auto_fallback", on ? "1" : "0");
}

bool gamemenu::SetScale(uint32_t scale)
{
    g_values.scale = scale;
    return SaveForNextStart("resolution_scale", scale == 0 ? "auto" : std::to_string(scale));
}

bool gamemenu::SetAnisotropy(uint32_t level)
{
    g_values.anisotropy = level;
    return SaveForNextStart("anisotropy", std::to_string(level));
}

bool gamemenu::SetVsync(bool on)
{
    g_values.vsync = on;
    return SaveForNextStart("vsync", on ? "1" : "0");
}

bool gamemenu::SetSkipIntroMovies(bool on)
{
    g_values.skipIntroMovies = on;
    return SaveForNextStart("skip_intro_movies", on ? "1" : "0");
}

bool gamemenu::SetEdgeSmoothing(uint32_t mode)
{
    g_values.edgeSmoothing = mode;
    gpu::g_settings.edgeSmoothing = mode;
    const char* name = edgesmoothing::Name(edgesmoothing::Mode(mode));
    Logged("edge_smoothing", name);
    return SaveVideo("edge_smoothing", name);
}

uint32_t gamemenu::EdgeSmoothingNow()
{
    return edgesmoothing::Resolve(edgesmoothing::Mode(g_values.edgeSmoothing), gpu::g_settings.resolutionScale);
}

bool gamemenu::RestartNeeded() { return g_restart; }

std::string gamemenu::FpsText()
{
    if (!g_values.showFps)
        return {};
    uint64_t now = SDL_GetTicksNS();
    uint64_t frames = gpu::FramesPresented();
    if (g_countStart == 0)
    {
        g_countStart = now;
        g_countFrames = frames;
    }
    else if (now - g_countStart >= 500'000'000)
    {
        double fps = double(frames - g_countFrames) * 1e9 / double(now - g_countStart);
        char text[32];
        snprintf(text, sizeof(text), "%.0f FPS", fps);
        if (g_fpsText == "-- FPS")
            fprintf(stderr, "[gamemenu] fps counter shows %s\n", text);
        g_fpsText = text;
        g_countStart = now;
        g_countFrames = frames;
    }
    return g_fpsText;
}

gamemenu::ReportResult gamemenu::WriteProblemReport()
{
    ReportResult result;
    report::Sources sources;
    sources.game = g_gameInfo.name;
    sources.gameVersion = g_version;
    sources.log = datadir::FromText(LogPath());
    sources.previousLog = datadir::FromText(PreviousLogPath());
    fs::path logFolder = datadir::Logs();
    if (const char* chosen = skyenv::Get("SKY_LAUNCHER_LOG_DIR"))
        logFolder = datadir::FromText(chosen);
    sources.launcherLog = logFolder / "skylauncher-game.log";
    sources.settingsFile = datadir::FromText(LauncherConfigPath());
    std::string text = report::Build(sources, report::CurrentIdentity());

    // The Desktop, else the logs folder. SKY_REPORT_DIR (tests) is used alone, and the file manager is not opened.
    const char* forced = skyenv::Get("SKY_REPORT_DIR");
    std::vector<fs::path> folders;
    if (forced != nullptr)
        folders.push_back(datadir::FromText(forced));
    else
    {
        if (const char* desktop = SDL_GetUserFolder(SDL_FOLDER_DESKTOP))
            folders.push_back(datadir::FromText(desktop));
        folders.push_back(logFolder);
    }
    std::string name = report::FileName(SSA_GAME_ID);
    for (const fs::path& folder : folders)
    {
        std::error_code error;
        fs::create_directories(folder, error);
        fs::path file = folder / name;
        std::ofstream stream(file, std::ios::binary);
        if (!stream)
            continue;
        stream.write(text.data(), std::streamsize(text.size()));
        stream.close();
        if (!stream)
            continue;
        result.ok = true;
        result.path = datadir::Text(file);
        fprintf(stderr, "[report] written: %s (%zu bytes)\n", name.c_str(), text.size());
        if (forced == nullptr)
            SDL_OpenURL(("file://" + datadir::Text(folder)).c_str());
        return result;
    }
    result.error = "no folder could be written";
    return result;
}
