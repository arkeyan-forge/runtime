// The settings the console uses: LocalState\launcher.ini ([video], [input] and [ssa] as on PC, docs/getting-started/settings.md, and
// [portal] mode) over console defaults (the gamepad, Xbox One glyphs, a resolution scale chosen by the host). The file is
// written with those defaults at the first start, so it can be edited through Device Portal. Compiled for the desktop API family
// like the other files that include SDL's headers (gpu/gpu.h).
#include "game.h"
#include "gpu/gpu.h"
#include "host/glyphs.h"
#include "host/settings.h"
#include "host/uwp/uwp.h"
#include "language.h"
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
#include <sstream>

namespace
{
    std::string Trim(std::string text)
    {
        size_t begin = text.find_first_not_of(" \t\r");
        size_t end = text.find_last_not_of(" \t\r");
        return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
    }

    // The same format as LoadLauncherConfig (host/launcher.cpp, not linked on UWP).
    bool LoadConfig(const std::string& path, LauncherConfig& config)
    {
        std::ifstream file(path);
        if (!file)
            return false;
        std::string line, section;
        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty() || line.front() == '#' || line.front() == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = Trim(line.substr(1, line.size() - 2));
                continue;
            }
            if (size_t equals = line.find('='); equals != std::string::npos)
                config[section][Trim(line.substr(0, equals))] = Trim(line.substr(equals + 1));
        }
        return true;
    }
}

void UwpApplySettings(const std::string& localState, uint32_t defaultScale)
{
    RuntimeSettings settings;
    settings.resolutionScale = defaultScale;
    settings.inputDevice = "gamepad";
    settings.glyphs = "xboxone";

    std::string path = localState + "launcher.ini";
    LauncherConfig config;
    if (!LoadConfig(path, config))
    {
        std::ofstream(path) << fmt::format(
            "# SSARecomp settings (docs/getting-started/settings.md). Edit through Device Portal, then restart the "
            "app.\n"
            "\n[video]\nresolution_scale={}\nanisotropy=1\nvsync=1\n"
            "\n[ssa]\n# language=en\n# voice_language=en\n"
            "\n[portal]\n# auto: a real portal plugged in at start, else the emulated one; real: wait for a real one\n"
            "mode=auto\n"
            "\n[gpu]\n# thread: the GPU thread (off: as 1.0.2488.555, which ran on this console); pipeline_library: "
            "keep\n"
            "# the pipelines built (pipelines.d3d12; on, it removed the device on an Xbox Series X). Turn one on to "
            "try it.\n"
            "thread=off\npipeline_library=off\n",
            defaultScale);
        fprintf(stderr, "[settings] wrote %s\n", path.c_str());
    }

    // [portal] mode (auto, real, emulated): read by PortalInit as SKY_PORTAL.
    auto portal = config.find("portal");
    std::string portalMode =
        portal != config.end() && portal->second.count("mode") ? portal->second.at("mode") : "auto";
    if (portalMode != "auto" && portalMode != "real" && portalMode != "emulated")
    {
        fprintf(stderr, "[settings] unknown [portal] mode \"%s\" (auto, real or emulated): auto\n", portalMode.c_str());
        portalMode = "auto";
    }
    _putenv_s("SKY_PORTAL", portalMode.c_str());
    fprintf(stderr, "[settings] portal %s\n", portalMode.c_str());

    std::vector<std::string> warnings;
    std::string error;
    ParseSettings(config, "ssa", getenv, 0, nullptr, settings, warnings, error);
    for (const std::string& warning : warnings)
        fprintf(stderr, "[settings] %s\n", warning.c_str());
    fprintf(stderr, "[settings] %s\n", DescribeSettings(settings).c_str());

    if (!settings.language.empty())
    {
        bool known = SetTextLanguage(settings.language);
        if (game::Current().SetTextLanguage != nullptr)
            known = game::Current().SetTextLanguage(settings.language) || known;
        if (!known)
            fprintf(stderr, "unknown text language \"%s\"\n", settings.language.c_str());
    }
    if (!settings.voiceLanguage.empty() &&
        (game::Current().SetVoiceLanguage == nullptr || !game::Current().SetVoiceLanguage(settings.voiceLanguage)))
        fprintf(stderr, "unknown voice language \"%s\"\n", settings.voiceLanguage.c_str());
    gpu::g_settings = {
        .resolutionScale = settings.resolutionScale, .anisotropy = settings.anisotropy, .vsync = settings.vsync
    };
    // [gpu] thread and pipeline_library (on/off). The GPU thread is off by default on the console: its first run there
    // lost the device at start (docs/history/handoffs/handoff_d3d12.md); the single-threaded renderer showed the game.
    auto flag = [&](const char* key, bool fallback) {
        auto section = config.find("gpu");
        if (section == config.end() || section->second.find(key) == section->second.end())
            return fallback;
        const std::string& value = section->second.at(key);
        return !(value == "off" || value == "0" || value == "false" || value == "no");
    };
    gpu::g_settings.gpuThread = flag("thread", false);
    // The pipeline library removed the device at start on an Xbox Series X (run 8): off unless asked for.
    gpu::g_settings.pipelineLibrary = flag("pipeline_library", false);
    fprintf(stderr, "[settings] gpu thread %s, pipeline library %s\n", gpu::g_settings.gpuThread ? "on" : "off",
        gpu::g_settings.pipelineLibrary ? "on" : "off");
    // The console's pad is always an Xbox One / Series one, and no device events reach glyphs::OnEvent here.
    if (settings.glyphs == "auto" || settings.glyphs == "xbox")
        settings.glyphs = "xboxone";
    glyphs::Init(settings);
}
