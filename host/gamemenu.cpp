#include "host/gamemenu.h"
#include "d3d/upscale.h"
#include "gpu/edge_smoothing.h"
#include "gpu/fsr/framegen.h"
#include "gpu/gpu.h"
#include "game.h"
#include "game_hooks.h"
#include "host/audio.h"
#include "host/collection/datadir.h"
#include "host/debug_screen.h"
#include "host/env.h"
#include "host/launcher.h"
#include "host/log.h"
#include "host/portal.h"
#include "host/report.h"
#include "host/settings.h"
#include "host/speakers.h"
#include "host/toybox.h"
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
    std::string g_perfLogName; // the performance log's file name

    // Writes one key of the game's own section in launcher.ini (the file the launcher shows). The file is read and written whole:
    // every other key is kept, comments are not.
    bool Save(const char* sectionSuffix, const char* key, const std::string& value)
    {
        std::string path = LauncherConfigPath();
        LauncherConfig config = LoadLauncherConfig(path);
        config[std::string(ARKEYAN_GAME_ID) + sectionSuffix][key] = value;
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
        settings.debugScreen, ::game::HostKernelPixelShader != 0, g_version, settings.reduceMotion,
        settings.portalSound, PortalGameHasSpeaker() };
    g_values.upscaler = uint32_t(settings.upscaler);
    g_values.fsrQuality = uint32_t(settings.fsrQuality);
    g_values.frameGeneration = settings.fsrFrameGeneration;
    g_values.gpuBackend = uint32_t(settings.gpuBackend);
#if defined(_WIN32) && !defined(PLUME_D3D12_COREWINDOW)
    g_values.gpuBackendShown = true;
#endif
    InitDebugScreen(g_gameInfo.name, g_version.c_str(), window);
    g_values.speakers = settings.speakers;
    g_values.speakersNow = HostAudioLayout();
    HostAudioSetVolume(settings.volume);
    PortalSetSound(settings.portalSound);
    ToyboxSetKeys(settings.toyboxKeys, settings.toyboxTab);
}

const gamemenu::Values& gamemenu::Current() { return g_values; }

bool gamemenu::SetVolume(uint32_t percent)
{
    g_values.volume = percent > 100 ? 100 : percent;
    HostAudioSetVolume(g_values.volume);
    Logged("volume", std::to_string(g_values.volume));
    return Save(".audio", "volume", std::to_string(g_values.volume));
}

bool gamemenu::SetPortalSound(bool on)
{
    g_values.portalSound = on;
    PortalSetSound(on);
    Logged("portal_sound", on ? "1" : "0");
    return Save(".audio", "portal_sound", on ? "1" : "0");
}

bool gamemenu::SetSpeakers(uint32_t mode)
{
    g_values.speakers = mode;
    g_restart = true;
    const char* name = speakers::Name(speakers::Mode(mode));
    Logged("speakers", std::string(name) + " (next start)");
    return Save(".audio", "speakers", name);
}

bool gamemenu::SetShowFps(bool on)
{
    g_values.showFps = on;
    g_countStart = 0;
    Logged("show_fps", on ? "1" : "0");
    return SaveVideo("show_fps", on ? "1" : "0");
}

bool gamemenu::SetDebugScreen(uint32_t level)
{
    g_values.debugScreen = level % debugscreen::LevelCount;
    g_countStart = 0;
    g_fpsText = "-- FPS";
    Logged("debug_screen", std::to_string(g_values.debugScreen));
    return SaveVideo("debug_screen", std::to_string(g_values.debugScreen));
}

bool gamemenu::CycleDebugScreen() { return SetDebugScreen(debugscreen::NextLevel(g_values.debugScreen)); }

bool gamemenu::SetPerfLog(bool on)
{
    if (std::FILE* stopped = PerfLogStopped())
        fclose(stopped);
    if (!on)
    {
        if (std::FILE* file = SwapPerfLog(nullptr))
            fclose(file);
        g_values.perfLog = false;
        Logged("perf_log", "off");
        return true;
    }
    if (g_values.perfLog)
        return true;
    // The logs folder (SKY_LAUNCHER_LOG_DIR as for the report), a file per recording: <game>-perf-<date>-<time>.csv.
    fs::path folder = datadir::Logs();
    if (const char* chosen = skyenv::Get("SKY_LAUNCHER_LOG_DIR"))
        folder = datadir::FromText(chosen);
    std::error_code error;
    fs::create_directories(folder, error);
    std::string name = report::FileName(ARKEYAN_GAME_ID);
    if (size_t at = name.find("-report-"); at != std::string::npos)
        name.replace(at, 8, "-perf-");
    if (size_t dot = name.rfind(".txt"); dot != std::string::npos)
        name.replace(dot, 4, ".csv");
    fs::path path = folder / datadir::FromText(name.c_str());
#if defined(_WIN32)
    std::FILE* file = _wfopen(path.c_str(), L"wb");
#else
    std::FILE* file = fopen(path.c_str(), "wb");
#endif
    if (file == nullptr)
    {
        fprintf(stderr, "[gamemenu] cannot write the performance log %s\n", name.c_str());
        return false;
    }
    if (std::FILE* before = SwapPerfLog(file))
        fclose(before);
    g_values.perfLog = true;
    g_perfLogName = name;
    Logged("perf_log", name);
    return true;
}

bool gamemenu::RecordPerfLogFor(uint64_t seconds)
{
    if (g_values.perfLog || !SetPerfLog(true))
        return false;
    StopPerfLogAt(SDL_GetTicksNS() + seconds * 1'000'000'000ull);
    return true;
}

std::string gamemenu::FinishedPerfLog()
{
    std::FILE* stopped = PerfLogStopped();
    if (stopped == nullptr)
        return {};
    fclose(stopped);
    g_values.perfLog = false;
    Logged("perf_log", "off (" + g_perfLogName + " finished)");
    return g_perfLogName;
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

bool gamemenu::SetGpuBackend(uint32_t backend)
{
    g_values.gpuBackend = backend;
    return SaveForNextStart("gpu_backend", gpubackend::Name(gpubackend::Backend(backend)));
}

bool gamemenu::SetEdgeSmoothing(uint32_t mode)
{
    g_values.edgeSmoothing = mode;
    gpu::g_settings.edgeSmoothing = mode;
    const char* name = edgesmoothing::Name(edgesmoothing::Mode(mode));
    Logged("edge_smoothing", name);
    return SaveVideo("edge_smoothing", name);
}

bool gamemenu::SetUpscaler(uint32_t upscaler)
{
    g_values.upscaler = upscaler;
    const auto mode = upscalemode::Upscaler(upscaler);
    if (!upscale::SwitchesLive())
        return SaveForNextStart("upscaler", upscalemode::Name(mode));
    upscale::Request(mode, upscalemode::Quality(g_values.fsrQuality));
    Logged("upscaler", upscalemode::Name(mode));
    return SaveVideo("upscaler", upscalemode::Name(mode));
}

bool gamemenu::SetFsrQuality(uint32_t quality)
{
    g_values.fsrQuality = quality;
    const auto preset = upscalemode::Quality(quality);
    if (g_values.upscaler != uint32_t(upscalemode::Upscaler::Off))
        upscale::Request(upscalemode::Upscaler(g_values.upscaler), preset);
    Logged("fsr_quality", upscalemode::Name(preset));
    return SaveVideo("fsr_quality", upscalemode::Name(preset));
}

bool gamemenu::SetFrameGeneration(bool on)
{
    g_values.frameGeneration = on;
    gpu::framegen::SetEnabled(on);
    Logged("fsr_frame_generation", on ? "on" : "off");
    return SaveVideo("fsr_frame_generation", on ? "on" : "off");
}

gamemenu::FrameGenerationBlock gamemenu::FrameGenerationBlocked()
{
    if (!gpu::framegen::Preview())
        return FrameGenerationBlock::Later;
    if (!gpu::framegen::Available())
        return FrameGenerationBlock::Device;
    if (gpu::framegen::Blocked())
        return FrameGenerationBlock::Game;
    if (g_values.upscaler != uint32_t(upscalemode::Upscaler::Fsr3) || !upscale::Active())
        return FrameGenerationBlock::Upscaler;
    return FrameGenerationBlock::None;
}

uint32_t gamemenu::EdgeSmoothingNow()
{
    return edgesmoothing::Resolve(
        edgesmoothing::Mode(g_values.edgeSmoothing), gpu::g_settings.resolutionScale, ::game::InMenuLevel());
}

bool gamemenu::RestartNeeded() { return g_restart; }

std::string gamemenu::FpsText()
{
    if (!g_values.showFps || g_values.debugScreen)
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
    std::string name = report::FileName(ARKEYAN_GAME_ID);
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
