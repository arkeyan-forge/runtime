// The F3 menu on Xbox (UWP): no launcher.ini to write and no log folder to report from yet, so the values only apply to this
// run and "Report a problem" says it cannot. Not built or run with the other builds: the UWP target is paused (docs/runtime/host/gamemenu.md).
#include "host/gamemenu.h"
#include "host/audio.h"
#include "host/settings.h"
#include "kernel/video.h"

namespace
{
    gamemenu::Values g_values;
}

void gamemenu::Init(const RuntimeSettings& settings, SDL_Window*, const char*)
{
    g_values = { settings.volume, settings.showFps, settings.fullscreen, settings.fpsLimit, settings.resolutionScale,
        settings.anisotropy, settings.vsync, settings.skipIntroMovies };
    HostAudioSetVolume(settings.volume);
}

const gamemenu::Values& gamemenu::Current() { return g_values; }
bool gamemenu::SetVolume(uint32_t percent) { g_values.volume = percent > 100 ? 100 : percent; HostAudioSetVolume(g_values.volume); return true; }
bool gamemenu::SetShowFps(bool on) { g_values.showFps = on; return true; }
bool gamemenu::SetFullscreen(bool on) { g_values.fullscreen = on; return true; }
bool gamemenu::SetFpsLimit(uint32_t fps) { g_values.fpsLimit = fps >= 60 ? 60 : 30; SetFrameRateLimit(g_values.fpsLimit); return true; }
bool gamemenu::SetScale(uint32_t scale) { g_values.scale = scale; return true; }
bool gamemenu::SetAnisotropy(uint32_t level) { g_values.anisotropy = level; return true; }
bool gamemenu::SetVsync(bool on) { g_values.vsync = on; return true; }
bool gamemenu::SetSkipIntroMovies(bool on) { g_values.skipIntroMovies = on; return true; }
bool gamemenu::RestartNeeded() { return false; }
std::string gamemenu::FpsText() { return {}; }

gamemenu::ReportResult gamemenu::WriteProblemReport()
{
    ReportResult result;
    result.error = "not available on this build";
    return result;
}
