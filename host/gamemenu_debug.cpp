// Diagnostic panels in the Toy Box's existing ImGui overlay, on every GPU backend.
#include "host/gamemenu.h"
#include "host/audio.h"
#include "host/log.h"
#include "gpu/gpu.h"
#include "game.h"
#include "d3d/render_stats.h"
#include "kernel/video.h"
#include <SDL3/SDL.h>
#include <cstring>
#include "host/debug_screen.h"
#include "host/portal.h"
#include "host/toybox/model.h"
#include <imgui.h>
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    debugscreen::Screen g_debug;
    bool g_debugWasOn = false;
    char g_debugGame[192]{}, g_debugVersion[64]{}, g_debugOs[192]{}, g_debugCpu[128]{};

    uint64_t DebugResidentBytes(bool& known)
    {
        known = false;
#ifdef __APPLE__
        mach_task_basic_info_data_t info{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
            KERN_SUCCESS)
        {
            known = true;
            return info.resident_size;
        }
#elif defined(_WIN32)
#if !defined(PLUME_D3D12_COREWINDOW)
        PROCESS_MEMORY_COUNTERS info{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)))
        {
            known = true;
            return info.WorkingSetSize;
        }
#endif
#else
        int fd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
        {
            char text[128]{};
            ssize_t size = read(fd, text, sizeof(text) - 1);
            close(fd);
            unsigned long long pages = 0, resident = 0;
            long pageSize = sysconf(_SC_PAGESIZE);
            if (size > 0 && pageSize > 0 && sscanf(text, "%llu %llu", &pages, &resident) == 2)
            {
                known = true;
                return resident * uint64_t(pageSize);
            }
        }
#endif
        return 0;
    }

}

void gamemenu::InitDebugScreen(const char* game, const char* version)
{
    debugscreen::Label(g_debugGame, sizeof(g_debugGame), game ? game : "n/a");
    debugscreen::Label(g_debugVersion, sizeof(g_debugVersion), version ? version : "n/a");
#if defined(PLUME_D3D12_COREWINDOW)
    // This paused target has no host log implementation; keep unavailable identity explicit.
    debugscreen::Label(g_debugOs, sizeof(g_debugOs), SDL_GetPlatform());
    snprintf(g_debugCpu, sizeof(g_debugCpu), "%d logical cores", SDL_GetNumLogicalCPUCores());
#else
    debugscreen::Label(g_debugOs, sizeof(g_debugOs), SystemOsName());
    // @ in a CPU brand denotes its clock frequency; use the model before it for a compact, safe label.
    std::string_view cpu(SystemCpuName());
    debugscreen::Label(g_debugCpu, sizeof(g_debugCpu), cpu.substr(0, cpu.find('@')));
#endif
    g_debug = {};
    g_debugWasOn = false;
}

bool gamemenu::UpdateDebugScreen()
{
    const auto& values = gamemenu::Current();
    if (!values.debugScreen)
    {
        bool changed = g_debugWasOn;
        g_debugWasOn = false;
        return changed;
    }
    if (!g_debugWasOn)
    {
        g_debug = {};
        g_debugWasOn = true;
    }
    if (!g_debug.rate.Update(SDL_GetTicksNS(), gpu::FramesPresented()))
        return false;
    auto snapshot = gpu::ReadDebugSnapshot();
    using namespace debugscreen;
    auto& left = g_debug.left;
    auto& right = g_debug.right;
    left.count = right.count = 0;
    char value[192], used[32], total[32];
    auto words = [](const char* en, const char* fr) { return toybox::L(en, fr); };
    left.Add(Group::Game, words("Game", "Jeu"), g_debugGame);
    left.Add(Group::Game, words("Version", "Version"), g_debugVersion[0] ? g_debugVersion : "n/a");
#if defined(PLUME_D3D12_COREWINDOW)
    left.Add(Group::Game, "Build", "n/a");
#else
    left.Add(Group::Game, "Build", LogBuild());
#endif
    game::CopyCurrentLevel(value, sizeof(value));
    left.Add(Group::Game, words("Level", "Niveau"), value[0] ? value : words("Loading", "Chargement"));
    if (g_debug.rate.ready)
        snprintf(value, sizeof(value), "%6.1f", g_debug.rate.fps);
    else
        snprintf(value, sizeof(value), "--");
    left.Add(Group::Frame, "FPS", value);
    auto frames = std::span(snapshot.frameNs).first(snapshot.frameCount);
    if (frames.empty())
        snprintf(value, sizeof(value), "--");
    else
        snprintf(value, sizeof(value), "%6.1f FPS", LowOnePercent(frames));
    left.Add(Group::Frame, words("1% low", "1 % bas"), value);
    double sum = 0;
    for (uint64_t ns : frames)
        sum += double(ns);
    if (frames.empty())
        snprintf(value, sizeof(value), "--");
    else
        snprintf(value, sizeof(value), "%6.2f ms", sum / double(frames.size()) / 1e6);
    left.Add(Group::Frame, words("Frame time", "Durée image"), value);
    if (values.fpsLimit == 0)
        snprintf(value, sizeof(value), "%s", words("Unlimited", "Illimité"));
    else
        snprintf(value, sizeof(value), "%u FPS", values.fpsLimit);
    left.Add(Group::Frame, words("Limit", "Limite"), value);
    left.Add(Group::Frame, words("30 FPS drop", "Passage à 30 FPS"),
        !values.fpsAutoFallback     ? words("Disabled", "Désactivé")
        : FrameRateFallbackActive() ? words("Active", "Actif")
                                    : words("Inactive", "Inactif"));
    snprintf(value, sizeof(value), "%u x %u  (x%u)", snapshot.width, snapshot.height, snapshot.scale);
    left.Add(Group::Frame, words("Output and scale", "Sortie et échelle"), value);
    const auto render = d3d::ReadRenderStats();
    snprintf(value, sizeof(value), "%u", render.draws);
    left.Add(Group::Frame, words("Draws so far", "Dessins en cours"), value);
    snprintf(value, sizeof(value), "%u", render.resolves);
    left.Add(Group::Frame, words("Resolves so far", "Résolutions en cours"), value);

    right.Add(Group::Machine, "OS", g_debugOs);
    right.Add(Group::Machine, "CPU", g_debugCpu);
    bool residentKnown;
    uint64_t resident = DebugResidentBytes(residentKnown);
    Bytes(used, sizeof(used), resident, residentKnown);
    int totalRam = SDL_GetSystemRAM();
    Bytes(total, sizeof(total), uint64_t(std::max(totalRam, 0)) << 20, totalRam > 0);
    snprintf(value, sizeof(value), "%s %s %s", used, words("of", "sur"), total);
    right.Add(Group::Machine, words("Game RAM of total", "RAM jeu sur total"), value);
    right.Add(Group::Renderer, "GPU", snapshot.available ? snapshot.adapter : "n/a");
    right.Add(Group::Renderer, words("Backend", "Moteur graphique"), snapshot.available ? snapshot.backend : "n/a");
    right.Add(Group::Renderer, words("Driver", "Pilote"), snapshot.available ? snapshot.driver : "n/a");
    Bytes(used, sizeof(used), snapshot.videoUsed, snapshot.videoUsageKnown);
    Bytes(total, sizeof(total), snapshot.videoBudget, snapshot.videoBudgetKnown);
    snprintf(value, sizeof(value), "%s %s %s", used, words("of", "sur"), total);
    right.Add(Group::Renderer, words("VRAM of budget", "VRAM sur budget"), value);
    snprintf(value, sizeof(value), "%zu", snapshot.shaders);
    right.Add(Group::Renderer, words("Cached shaders", "Shaders en cache"), value);
    snprintf(value, sizeof(value), "%zu", snapshot.missingShaders);
    right.Add(Group::Renderer, words("Missing shaders", "Shaders manquants"), value);
    snprintf(value, sizeof(value), "%zu", snapshot.pipelinesReady);
    right.Add(Group::Renderer, words("Pipelines ready", "Pipelines prêts"), value);
    snprintf(value, sizeof(value), "%u", snapshot.drawTimeBuilds);
    right.Add(Group::Renderer, words("Built at draw", "Créés au dessin"), value);
    right.Add(Group::Devices, words("Portal", "Portail"),
        !PortalIsConnected() ? words("Disconnected", "Déconnecté")
        : PortalIsEmulated() ? words("Emulated", "Émulé")
                             : "USB");
    snprintf(value, sizeof(value), "48 kHz stereo  %u%%  %u ms", HostAudioVolume(),
        uint32_t(uint64_t(HostAudioQueuedFrames()) * AudioFrameSamples * 1000 / 48000));
    right.Add(Group::Devices, "Audio", HostAudioActive() ? value : words("No output device", "Aucune sortie"));
    g_debug.graphCount = std::min(frames.size(), GraphBars);
    for (size_t i = 0; i < g_debug.graphCount; i++)
        g_debug.graphMs[i] = float(frames[frames.size() - g_debug.graphCount + i]) / 1e6f;
    return true;
}

void gamemenu::DrawDebugScreen(ImFont* font)
{
    if (!gamemenu::Current().debugScreen)
        return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    constexpr float Size = 13, Line = 18, Gap = 9, Padding = 10, Width = 480;
    const ImU32 ink = IM_COL32(235, 240, 242, 255), dim = IM_COL32(172, 187, 199, 255);
    auto panel = [&](const debugscreen::Panel& rows, float x, bool graph) {
        float height = Padding * 2;
        for (size_t i = 0; i < rows.count; i++)
            height += Line + (i != 0 && rows.StartsGroup(i) ? Gap : 0);
        if (graph)
            height += 70;
        draw->AddRectFilled(ImVec2(x, 12), ImVec2(x + Width, 12 + height), IM_COL32(12, 18, 26, 185), 5);
        float y = 12 + Padding;
        for (size_t i = 0; i < rows.count; i++)
        {
            if (i != 0 && rows.StartsGroup(i))
            {
                y += Gap;
                draw->AddLine(
                    ImVec2(x + Padding, y - 5), ImVec2(x + Width - Padding, y - 5), IM_COL32(120, 140, 160, 60));
            }
            const auto& row = rows.rows[i];
            draw->AddText(font, Size, ImVec2(x + Padding, y), dim, row.label);
            ImVec2 size = font->CalcTextSizeA(Size, Width - 180, 0, row.value);
            draw->PushClipRect(ImVec2(x + 180, y), ImVec2(x + Width - Padding, y + Line), true);
            draw->AddText(font, Size, ImVec2(std::max(x + 180, x + Width - Padding - size.x), y), ink, row.value);
            draw->PopClipRect();
            y += Line;
        }
        if (!graph)
            return;
        y += 5;
        draw->AddText(font, Size, ImVec2(x + Padding, y), dim,
            toybox::L("Frame graph: 33 ms line, 67 ms ceiling", "Images : ligne à 33 ms, plafond à 67 ms"));
        y += 17;
        const float bar = (Width - 2 * Padding) / float(debugscreen::GraphBars);
        for (size_t i = 0; i < g_debug.graphCount; i++)
        {
            float ms = g_debug.graphMs[i];
            float at = x + Padding + float(i) * bar;
            draw->AddRectFilled(ImVec2(at, y + 44 - std::min(ms, 66.67f) * 44 / 66.67f), ImVec2(at + bar - 1, y + 44),
                ms > 33.34f ? IM_COL32(241, 150, 103, 230) : IM_COL32(129, 208, 177, 230));
        }
        draw->AddLine(ImVec2(x + Padding, y + 22), ImVec2(x + Width - Padding, y + 22), IM_COL32(235, 240, 242, 100));
    };
    panel(g_debug.left, 12, true);
    panel(g_debug.right, ImGui::GetIO().DisplaySize.x - Width - 12, false);
}
