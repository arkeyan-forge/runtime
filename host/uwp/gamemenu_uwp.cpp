// The F3 menu on Xbox (UWP): no launcher.ini to write and no log folder to report from yet, so the values only apply to this
// run and "Report a problem" says it cannot. Not built or run with the other builds: the UWP target is paused (docs/runtime/host/gamemenu.md).
#include "game.h"
#include "host/gamemenu.h"
#include "host/audio.h"
#include "host/portal.h"
#include "host/settings.h"
#include "kernel/video.h"

namespace
{
    gamemenu::Values g_values;
}

void gamemenu::Init(const RuntimeSettings& settings, SDL_Window*, const char* version)
{
    g_values = { settings.volume, settings.showFps, settings.fullscreen, settings.fpsLimit, settings.resolutionScale,
        settings.anisotropy, settings.vsync, settings.skipIntroMovies };
    g_values.fpsAutoFallback = settings.fpsAutoFallback;
    g_values.debugScreen = settings.debugScreen;
    InitDebugScreen(g_gameInfo.name, version);
    g_values.reduceMotion = settings.reduceMotion;
    g_values.portalSound = settings.portalSound;
    g_values.portalSoundShown = PortalGameHasSpeaker();
    g_values.speakers = settings.speakers;
    g_values.speakersNow = HostAudioLayout();
    HostAudioSetVolume(settings.volume);
    PortalSetSound(settings.portalSound);
}

const gamemenu::Values& gamemenu::Current() { return g_values; }
bool gamemenu::SetVolume(uint32_t percent)
{
    g_values.volume = percent > 100 ? 100 : percent;
    HostAudioSetVolume(g_values.volume);
    return true;
}
bool gamemenu::SetPortalSound(bool on)
{
    g_values.portalSound = on;
    PortalSetSound(on);
    return true;
}
bool gamemenu::SetSpeakers(uint32_t mode)
{
    g_values.speakers = mode;
    return true;
}
bool gamemenu::SetGpuBackend(uint32_t backend) // one backend here: the row is hidden
{
    g_values.gpuBackend = backend;
    return true;
}
bool gamemenu::SetShowFps(bool on)
{
    g_values.showFps = on;
    return true;
}
bool gamemenu::SetDebugScreen(uint32_t level)
{
    g_values.debugScreen = level % 3;
    return true;
}
bool gamemenu::CycleDebugScreen() { return SetDebugScreen(g_values.debugScreen + 1); }
// No logs folder to write to on this target.
bool gamemenu::SetPerfLog(bool) { return false; }
bool gamemenu::SetFullscreen(bool on)
{
    g_values.fullscreen = on;
    return true;
}
bool gamemenu::SetFpsLimit(uint32_t fps)
{
    g_values.fpsLimit = CapFrameRate(NormalFrameRate(fps), game::Current().maxFpsLimit);
    SetFrameRateLimit(g_values.fpsLimit);
    return true;
}
bool gamemenu::SetFpsAutoFallback(bool on)
{
    g_values.fpsAutoFallback = on;
    ::SetFpsAutoFallback(on);
    return true;
}
bool gamemenu::SetScale(uint32_t scale)
{
    g_values.scale = scale;
    return true;
}
bool gamemenu::SetAnisotropy(uint32_t level)
{
    g_values.anisotropy = level;
    return true;
}
bool gamemenu::SetVsync(bool on)
{
    g_values.vsync = on;
    return true;
}
bool gamemenu::SetSkipIntroMovies(bool on)
{
    g_values.skipIntroMovies = on;
    return true;
}
bool gamemenu::SetEdgeSmoothing(uint32_t mode)
{
    g_values.edgeSmoothing = mode;
    return true;
}
bool gamemenu::SetUpscaler(uint32_t upscaler)
{
    g_values.upscaler = upscaler;
    return true;
}
bool gamemenu::SetFsrQuality(uint32_t quality)
{
    g_values.fsrQuality = quality;
    return true;
}
uint32_t gamemenu::EdgeSmoothingNow() { return g_values.edgeSmoothing; }
bool gamemenu::RestartNeeded() { return false; }
std::string gamemenu::FpsText() { return {}; }

gamemenu::ReportResult gamemenu::WriteProblemReport()
{
    ReportResult result;
    result.error = "not available on this build";
    return result;
}
