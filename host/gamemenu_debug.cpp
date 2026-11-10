// The debug screen in the Toy Box's existing ImGui overlay, on every GPU backend, and the performance log
// (docs/runtime/host/gamemenu.md, "Debug screen"). Laid out as Minecraft's F3 screen: one line per stat on its own dark
// strip, grouped under headings in the game's colour, left (frame, guest, render, audio and input) and right (machine,
// session); in Full, the frame-time graph bottom left and the per-thread CPU bars bottom right.
#include "host/gamemenu.h"
#include "host/audio.h"
#include "host/log.h"
#include "host/machine_state.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu.h"
#include "game.h"
#include "kernel/video.h"
#include <SDL3/SDL.h>
#include <cfloat>
#include <cstring>
#include <utility>
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
    namespace stats = gpu::framestats;
    using debugscreen::Group;
    using debugscreen::Hitch;

    debugscreen::Screen g_debug;
    uint32_t g_debugLevel = debugscreen::Off; // the level the panels were collected for
    char g_debugGame[192]{}, g_debugVersion[64]{}, g_debugOs[192]{}, g_debugCpu[128]{};
    SDL_Window* g_debugWindow = nullptr;
    uint64_t g_debugStart = 0; // the session's start (SDL time)
    bool g_showGraphs = true, g_showThreads = true;

    // Full: the finished frames of the last ten seconds or so, read at each refresh.
    std::array<stats::Sample, 600> g_recent;
    // Full: every finished frame once (its own cursor), counted into one-second buckets for the last minute.
    struct Second
    {
        uint64_t second = 0; // SDL seconds; a bucket of another second is stale
        uint32_t missed = 0, builds = 0;
    };
    std::array<Second, 60> g_seconds;
    uint64_t g_screenAfter = 0;
    std::array<stats::Sample, 128> g_screenBatch;
    // CPU times at the refresh before, for the use since.
    machine::Cpu g_cpuBefore;
    machine::System g_systemBefore;
    uint64_t g_cpuWallBefore = 0;

    // The performance log (SwapPerfLog): its file, the last frame written, the bytes so far, its time origin, its end.
    std::FILE* g_log = nullptr;
    std::FILE* g_logStopped = nullptr;
    uint64_t g_logAfter = 0, g_logBytes = 0, g_logStart = 0, g_logUntil = 0;
    std::array<stats::Sample, 128> g_logBatch;

    // The Full screen's cost: the measurement (frame_stats.h) and the panels, logged every ten seconds while it shows.
    uint64_t g_panelNs = 0, g_costWindow = 0, g_costMeasureBefore = 0, g_costPanelBefore = 0, g_costFramesBefore = 0;

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

    const char* Words(const char* english, const char* french) { return toybox::L(english, french); }

    const char* HitchWords(Hitch hitch)
    {
        switch (hitch)
        {
        case Hitch::Pipeline: return Words("pipeline built", "pipeline créé");
        case Hitch::Stall: return Words("GPU read-back wait", "attente de lecture GPU");
        case Hitch::Gpu: return Words("GPU work", "travail GPU");
        case Hitch::Load: return Words("loading", "chargement");
        case Hitch::Uploads: return Words("texture uploads", "envoi de textures");
        default: return Words("unknown", "inconnue");
        }
    }

    const char* Heading(Group group)
    {
        switch (group)
        {
        case Group::Frame: return Words("FRAME", "IMAGE");
        case Group::Guest: return Words("GUEST", "JEU");
        case Group::Render: return Words("RENDER", "RENDU");
        case Group::AudioInput: return Words("AUDIO AND INPUT", "AUDIO ET ENTRÉES");
        case Group::Machine: return "MACHINE";
        default: return "SESSION";
        }
    }

    const char* FallbackWords(const gamemenu::Values& values)
    {
        return !values.fpsAutoFallback     ? Words("off", "non")
               : FrameRateFallbackActive() ? Words("active", "actif")
                                           : Words("ready", "prêt");
    }

    // Writes the frames finished since the last call; stops the log at its size limit or its end time (`now`, 0:
    // none).
    void WritePerfLog(uint64_t now)
    {
        char line[256];
        if (g_log != nullptr && g_logUntil != 0 && now >= g_logUntil)
        {
            WritePerfLog(0);
            if (g_log == nullptr)
                return;
            fprintf(
                stderr, "[gamemenu] performance log: its time is up (%llu bytes)\n", (unsigned long long)g_logBytes);
            g_logStopped = std::exchange(g_log, nullptr);
            return;
        }
        while (g_log != nullptr)
        {
            size_t count = stats::ReadNew(g_logAfter, g_logBatch);
            if (count == 0)
                return;
            for (size_t i = 0; i < count; i++)
            {
                if (g_logStart == 0)
                    g_logStart = g_logBatch[i].presentNs - g_logBatch[i].intervalNs;
                size_t length = debugscreen::PerfLogLine(line, sizeof(line), g_logBatch[i], g_logStart);
                if (g_logBytes + length > debugscreen::PerfLogMaxBytes)
                {
                    fprintf(stderr, "[gamemenu] performance log full (%llu bytes): stopped\n",
                        (unsigned long long)g_logBytes);
                    g_logStopped = std::exchange(g_log, nullptr);
                    return;
                }
                fwrite(line, 1, length, g_log);
                g_logBytes += length;
            }
        }
    }

    // Per-frame means over the recent finished frames (Full); the *Frames counts say how many had each value.
    struct Means
    {
        size_t frames = 0, gpuFrames = 0, latencyFrames = 0, padFrames = 0, cpuFrames = 0;
        double frameMs = 0, rendererMs = 0, gameCpuMs = 0, gpuMs = 0, presentWaitMs = 0, latencyMs = 0, padMs = 0,
               draws = 0, resolves = 0, uploads = 0, uploadMb = 0, faults = 0, switches = 0;
        uint64_t builds = 0, fileReadsLastSecond = 0;
    };
    Means MeansOf(std::span<const stats::Sample> samples)
    {
        Means m;
        for (const stats::Sample& s : samples)
        {
            m.frames++;
            m.frameMs += double(s.intervalNs) / 1e6;
            m.rendererMs += double(s.rendererNs) / 1e6;
            m.presentWaitMs += double(uint64_t(s.vblankNs) + s.presentCallNs) / 1e6;
            if (s.gameCpuNs != stats::Unknown)
            {
                m.cpuFrames++;
                m.gameCpuMs += double(s.gameCpuNs) / 1e6;
            }
            if (s.gpuNs != stats::Unknown)
            {
                m.gpuFrames++;
                m.gpuMs += double(s.gpuNs) / 1e6;
            }
            if (s.latencyNs != stats::Unknown)
            {
                m.latencyFrames++;
                m.latencyMs += double(s.latencyNs) / 1e6;
            }
            if (s.padToGameNs != stats::Unknown)
            {
                m.padFrames++;
                m.padMs += double(s.padToGameNs) / 1e6;
            }
            m.draws += s.draws;
            m.resolves += s.resolves;
            m.uploads += s.uploads;
            m.uploadMb += double(s.uploadBytes) / double(1 << 20);
            m.faults += s.faults;
            m.switches += s.targetSwitches;
            m.builds += s.builds;
            if (s.presentNs + 1'000'000'000ull >= samples.back().presentNs)
                m.fileReadsLastSecond += s.fileReads;
        }
        if (m.frames == 0)
            return m;
        double n = double(m.frames);
        for (double* mean : { &m.frameMs, &m.rendererMs, &m.presentWaitMs, &m.draws, &m.resolves, &m.uploads,
                 &m.uploadMb, &m.faults, &m.switches })
            *mean /= n;
        m.gameCpuMs /= double(std::max<size_t>(m.cpuFrames, 1));
        m.gpuMs /= double(std::max<size_t>(m.gpuFrames, 1));
        m.latencyMs /= double(std::max<size_t>(m.latencyFrames, 1));
        m.padMs /= double(std::max<size_t>(m.padFrames, 1));
        return m;
    }

    // Every finished frame once: a missed frame (over 1.5 times the limit's interval, or 50 ms without a limit) and
    // the pipelines built at draw time, per second of the last minute.
    void CountSeconds(uint32_t limitFps)
    {
        uint64_t missAt = limitFps != 0 ? 1'500'000'000ull / limitFps : debugscreen::HitchNs;
        while (size_t count = stats::ReadNew(g_screenAfter, g_screenBatch))
            for (size_t i = 0; i < count; i++)
            {
                const stats::Sample& s = g_screenBatch[i];
                uint64_t second = s.presentNs / 1'000'000'000ull;
                Second& bucket = g_seconds[second % g_seconds.size()];
                if (bucket.second != second)
                    bucket = { second, 0, 0 };
                bucket.missed += s.intervalNs > missAt;
                bucket.builds += s.builds;
            }
    }

    void LastMinute(uint64_t now, uint32_t& missed, uint32_t& builds)
    {
        missed = builds = 0;
        uint64_t second = now / 1'000'000'000ull;
        for (const Second& bucket : g_seconds)
            if (bucket.second != 0 && bucket.second + 60 > second && bucket.second <= second)
            {
                missed += bucket.missed;
                builds += bucket.builds;
            }
    }

    // Every ten seconds while the Full screen shows: what it cost per presented frame.
    void LogCost(uint64_t now, uint64_t frames)
    {
        if (g_costWindow == 0)
        {
            g_costWindow = now;
            g_costMeasureBefore = stats::CostNs();
            g_costPanelBefore = g_panelNs;
            g_costFramesBefore = frames;
            return;
        }
        if (now - g_costWindow < 10'000'000'000ull || frames <= g_costFramesBefore)
            return;
        double count = double(frames - g_costFramesBefore);
        double measure = double(stats::CostNs() - g_costMeasureBefore) / 1e6 / count;
        double panels = double(g_panelNs - g_costPanelBefore) / 1e6 / count;
        fprintf(stderr,
            "[debugscreen] full: %.4f ms a frame (measurement %.4f, panels and overlay %.4f) over %.0f frames\n",
            measure + panels, measure, panels, count);
        g_costWindow = 0;
    }

    void Bytes(char* out, size_t size, uint64_t bytes, bool known = true)
    {
        debugscreen::Bytes(out, size, bytes, known);
    }

    // The map's own name: a zone's label is a disc path (tutorial\tutorial_a\tutorial_a), which Label refuses.
    const char* LevelName(char* value, size_t size)
    {
        game::CopyCurrentLevel(value, size);
        const char* level = value;
        for (const char* c = value; *c != 0; c++)
            if (*c == '\\' || *c == '/')
                level = c + 1;
        return level[0] ? level : Words("loading", "chargement");
    }

    double MeanOf(std::span<const uint64_t> frames)
    {
        double sum = 0;
        for (uint64_t ns : frames)
            sum += double(ns);
        return frames.empty() ? 0 : sum / double(frames.size()) / 1e6;
    }

    // In % of one core since the refresh before (negative: unknown).
    double Percent(bool known, uint64_t now, uint64_t before, uint64_t wall)
    {
        return known && wall != 0 && now >= before ? debugscreen::CorePercent(now - before, wall) : -1;
    }

    void Compact(const gamemenu::Values& values, const gpu::DebugSnapshot& snapshot, std::span<const uint64_t> frames)
    {
        auto& left = g_debug.left;
        auto& right = g_debug.right;
        char value[192], used[32], total[32], level[192];
        if (g_debug.rate.ready && !frames.empty())
            snprintf(value, sizeof(value), "%.1f  1%% low %.1f", g_debug.rate.fps, debugscreen::LowOnePercent(frames));
        else
            snprintf(value, sizeof(value), "--");
        left.Add(Group::Frame, "fps", value);
        if (frames.empty())
            snprintf(value, sizeof(value), "--");
        else
            snprintf(value, sizeof(value), "%.2f ms", MeanOf(frames));
        left.Add(Group::Frame, Words("frame", "image"), value);
        if (values.fpsLimit == 0)
            snprintf(value, sizeof(value), "%s  %s %s", Words("none", "aucune"), Words("drop to 30", "passage à 30"),
                FallbackWords(values));
        else
            snprintf(value, sizeof(value), "%u  %s %s", values.fpsLimit, Words("drop to 30", "passage à 30"),
                FallbackWords(values));
        left.Add(Group::Frame, Words("cap", "limite"), value);
        snprintf(value, sizeof(value), "%ux%u  %s x%u", snapshot.width, snapshot.height, Words("scale", "échelle"),
            snapshot.scale);
        left.Add(Group::Frame, Words("output", "sortie"), value);
        left.Add(Group::Guest, Words("level", "niveau"), LevelName(level, sizeof(level)));
        snprintf(value, sizeof(value), "%s %s", g_debugGame, g_debugVersion);
        left.Add(Group::Guest, Words("game", "jeu"), value);

        right.Add(Group::Machine, "OS", g_debugOs);
        right.Add(Group::Machine, "CPU", g_debugCpu);
        snprintf(value, sizeof(value), "%s (%s)", snapshot.available ? snapshot.adapter : "n/a",
            snapshot.available ? snapshot.backend : "n/a");
        right.Add(Group::Machine, "GPU", value);
        Bytes(used, sizeof(used), snapshot.videoUsed, snapshot.videoUsageKnown);
        Bytes(total, sizeof(total), snapshot.videoBudget, snapshot.videoBudgetKnown);
        snprintf(value, sizeof(value), "%s %s %s", used, Words("of", "sur"), total);
        right.Add(Group::Machine, Words("GPU memory", "mémoire GPU"), value);
        bool residentKnown;
        uint64_t resident = DebugResidentBytes(residentKnown);
        Bytes(used, sizeof(used), resident, residentKnown);
        snprintf(value, sizeof(value), "%s %s", Words("game", "jeu"), used);
        right.Add(Group::Machine, "RAM", value);
    }

    void Full(const gamemenu::Values& values, const gpu::DebugSnapshot& snapshot, std::span<const uint64_t> frames,
        uint64_t now)
    {
        auto& left = g_debug.left;
        auto& right = g_debug.right;
        char value[192], used[32], total[32], level[192];
        CountSeconds(values.fpsLimit);
        size_t count = stats::ReadRecent(g_recent);
        auto recent = std::span<const stats::Sample>(g_recent).first(count);
        const Means m = MeansOf(recent);
        machine::Cpu cpu;
        machine::ReadCpu(cpu);
        machine::System system = machine::ReadSystem();
        machine::Runtime runtime;
        machine::ReadRuntime(runtime);
        uint64_t wall = g_cpuWallBefore != 0 ? now - g_cpuWallBefore : 0;

        // FRAME
        double slowest = 0, fastest = 0;
        for (uint64_t ns : frames)
            if (ns != 0)
            {
                slowest = std::max(slowest, double(ns));
                fastest = fastest == 0 ? double(ns) : std::min(fastest, double(ns));
            }
        if (g_debug.rate.ready && slowest > 0)
            snprintf(value, sizeof(value), "%.1f  1%% low %.1f  min %.1f  max %.1f", g_debug.rate.fps,
                debugscreen::LowOnePercent(frames), 1e9 / slowest, 1e9 / fastest);
        else
            snprintf(value, sizeof(value), "--");
        left.Add(Group::Frame, "fps", value);
        char cap[16];
        snprintf(cap, sizeof(cap), "%u", values.fpsLimit);
        snprintf(value, sizeof(value), "%s  vsync %s  %s %s", values.fpsLimit == 0 ? Words("none", "aucune") : cap,
            values.vsync ? Words("on", "oui") : Words("off", "non"), Words("drop to 30", "passage à 30"),
            FallbackWords(values));
        left.Add(Group::Frame, Words("cap", "limite"), value);
        if (m.frames == 0)
            snprintf(value, sizeof(value), "--");
        else
        {
            char cpuMs[16] = "n/a", gpuMs[16] = "n/a";
            if (m.cpuFrames != 0)
                snprintf(cpuMs, sizeof(cpuMs), "%.2f", m.gameCpuMs);
            if (m.gpuFrames != 0)
                snprintf(gpuMs, sizeof(gpuMs), "%.2f", m.gpuMs);
            snprintf(value, sizeof(value), "%.2f ms  cpu %s  gpu %s  %s %.2f", m.frameMs, cpuMs, gpuMs,
                Words("wait", "attente"), m.presentWaitMs);
        }
        left.Add(Group::Frame, Words("frame", "image"), value);
        // What limits the frame rate: the game thread's CPU a frame, or the GPU thread's (its CPU since the refresh
        // before, over the frames presented meanwhile), against the GPU busy time and the limit.
        double rendererMs = -1;
        if (cpu.threadKnown[1] && g_cpuBefore.threadKnown[1] && cpu.threadNs[1] >= g_cpuBefore.threadNs[1] &&
            g_debug.rate.ready && g_debug.rate.fps > 0 && wall != 0)
            rendererMs =
                double(cpu.threadNs[1] - g_cpuBefore.threadNs[1]) / 1e6 / (double(wall) / 1e9 * g_debug.rate.fps);
        debugscreen::Bound bound = debugscreen::Decide(m.frames != 0 ? m.frameMs : 0,
            std::max(m.cpuFrames != 0 ? m.gameCpuMs : -1, rendererMs), m.gpuFrames != 0 ? m.gpuMs : -1,
            values.fpsLimit);
        left.Add(Group::Frame, Words("limited by", "limité par"),
            bound == debugscreen::Bound::Gpu     ? "GPU"
            : bound == debugscreen::Bound::Cpu   ? (rendererMs > m.gameCpuMs ? Words("CPU, render thread", "CPU, rendu")
                                                                             : Words("CPU, game thread", "CPU, jeu"))
            : bound == debugscreen::Bound::Limit ? Words("the frame limit", "le plafond d'images")
            : m.frames == 0                      ? "--"
                                                 : Words("nothing clear", "rien de net"));
        debugscreen::Pacing pacing = debugscreen::PacingOf(frames);
        // The minute up to the newest finished frame, on the frames' own clock.
        uint32_t missed, builds60;
        LastMinute(count != 0 ? recent.back().presentNs : now, missed, builds60);
        if (pacing.count == 0)
            snprintf(value, sizeof(value), "--");
        else
            snprintf(value, sizeof(value), "%.1f ms  p99 %.1f  %s %u %s", pacing.meanMs, pacing.p99Ms,
                Words("missed", "ratées"), missed, Words("in 60 s", "en 60 s"));
        left.Add(Group::Frame, Words("pacing", "rythme"), value);
        if (m.latencyFrames == 0)
            snprintf(value, sizeof(value), "n/a");
        else if (m.padFrames == 0)
            snprintf(value, sizeof(value), "%s n/a  %s %.1f ms", Words("pad>game", "manette>jeu"),
                Words("game>present", "jeu>affichage"), m.latencyMs);
        else
            snprintf(value, sizeof(value), "%s %.1f  %s %.1f  ~%.1f ms", Words("pad>game", "manette>jeu"), m.padMs,
                Words("game>present", "jeu>affichage"), m.latencyMs, m.padMs + m.latencyMs);
        left.Add(Group::Frame, Words("latency", "latence"), value);
        // The newest hitch among the recent frames, with its cause.
        size_t hitches = 0;
        const stats::Sample* newest = nullptr;
        for (const stats::Sample& s : recent)
            if (debugscreen::Classify(s) != Hitch::None)
            {
                hitches++;
                newest = &s;
            }
        if (newest == nullptr)
            snprintf(value, sizeof(value), "0");
        else
            snprintf(value, sizeof(value), "%zu, %s %.0f ms %s, %.1f s", hitches, Words("last", "dernier"),
                double(newest->intervalNs) / 1e6, HitchWords(debugscreen::Classify(*newest)),
                double(recent.back().presentNs - newest->presentNs) / 1e9);
        left.Add(Group::Frame, Words("hitches 50 ms+", "à-coups 50 ms+"), value);

        // GUEST
        if (m.frames == 0)
            snprintf(value, sizeof(value), "--");
        else if (m.cpuFrames == 0)
            snprintf(value, sizeof(value), "%s n/a  %s %.2f ms", Words("game", "jeu"),
                Words("host calls", "appels hôte"), m.rendererMs);
        else
            snprintf(value, sizeof(value), "%s %.2f  %s %.2f ms", Words("game", "jeu"), m.gameCpuMs,
                Words("host calls", "appels hôte"), m.rendererMs);
        left.Add(Group::Guest, "cpu", value);
        snprintf(value, sizeof(value), "%u %s  %s n/a", runtime.guestThreads, Words("running", "actifs"),
            Words("waiting", "en attente"));
        left.Add(Group::Guest, "threads", value);
        Bytes(used, sizeof(used), runtime.heapUsed);
        Bytes(total, sizeof(total), runtime.heapSize);
        snprintf(value, sizeof(value), "%s %s %s  %s %.1f", used, Words("of", "sur"), total,
            Words("write faults", "fautes d'écriture"), m.faults);
        left.Add(Group::Guest, Words("heap", "tas"), value);
        snprintf(value, sizeof(value), "%s  %s %llu  %s", LevelName(level, sizeof(level)), Words("tick", "image"),
            (unsigned long long)gpu::FramesPresented(),
            m.fileReadsLastSecond != 0 ? Words("loading", "chargement") : "");
        left.Add(Group::Guest, Words("level", "niveau"), value);
        {
            int at = snprintf(value, sizeof(value), "%s",
                !PortalIsConnected() ? Words("disconnected", "déconnecté")
                : PortalIsEmulated() ? Words("emulated", "émulé")
                                     : "USB");
            if (runtime.seats == 0)
                snprintf(value + at, sizeof(value) - size_t(at), ", %s", Words("empty", "vide"));
            for (uint32_t i = 0; i < runtime.seats && at > 0 && size_t(at) < sizeof(value); i++)
                at += runtime.seatToys[i] != 0
                          ? snprintf(value + at, sizeof(value) - size_t(at), "  %s %u", Words("toy", "jouet"),
                                runtime.seatToys[i])
                          : snprintf(value + at, sizeof(value) - size_t(at), "  %s ?", Words("toy", "jouet"));
        }
        left.Add(Group::Guest, Words("portal", "portail"), value);

        // RENDER
        snprintf(value, sizeof(value), "x%u  %s %ux%u  %s %ux%u", snapshot.scale, Words("scene", "scène"),
            1120 * snapshot.scale, 704 * snapshot.scale, Words("output", "sortie"), snapshot.width, snapshot.height);
        left.Add(Group::Render, Words("scale", "échelle"), value);
        snprintf(value, sizeof(value), "%.0f  %s %.0f  %s %.0f", m.draws, Words("resolves", "résolutions"), m.resolves,
            Words("targets", "cibles"), m.switches);
        left.Add(Group::Render, Words("draws", "dessins"), value);
        snprintf(value, sizeof(value), "%zu  %s %zu  pipelines %zu  %s %u", snapshot.shaders,
            Words("missing", "manquants"), snapshot.missingShaders, snapshot.pipelinesReady,
            Words("built in 60 s", "créés en 60 s"), builds60);
        left.Add(Group::Render, "shaders", value);
        char live[24] = "n/a";
        if (runtime.texturesKnown)
            snprintf(live, sizeof(live), "%zu", runtime.textures);
        snprintf(value, sizeof(value), "%s  %s %.1f (%.2f MB)  HD %zu", live, Words("uploads", "envois"), m.uploads,
            m.uploadMb, runtime.packFiles);
        left.Add(Group::Render, "textures", value);
        Bytes(used, sizeof(used), snapshot.videoUsed, snapshot.videoUsageKnown);
        Bytes(total, sizeof(total), snapshot.videoBudget, snapshot.videoBudgetKnown);
        snprintf(value, sizeof(value), "%s %s %s", used, Words("of", "sur"), total);
        left.Add(Group::Render, Words("video memory", "mémoire vidéo"), value);

        // AUDIO AND INPUT
        if (HostAudioActive())
            snprintf(value, sizeof(value), "48 kHz  %s %u  %s %u ms  %s %u", Words("voices", "voix"), runtime.voices,
                Words("buffer", "tampon"),
                uint32_t(uint64_t(HostAudioQueuedFrames()) * AudioFrameSamples * 1000 / 48000),
                Words("underruns", "manques"), runtime.underruns);
        else
            snprintf(value, sizeof(value), "%s", Words("no output device", "aucune sortie"));
        left.Add(Group::AudioInput, "audio", value);
        snprintf(value, sizeof(value), "%s  %s %s", runtime.input, Words("glyphs", "boutons"), runtime.glyphs);
        left.Add(Group::AudioInput, Words("input", "entrée"), value);

        // MACHINE
        right.Add(Group::Machine, "OS", g_debugOs);
        snprintf(value, sizeof(value), "%s, %d %s", g_debugCpu, SDL_GetNumLogicalCPUCores(),
            Words("cores", "coeurs") /* the monospace face has no œ */);
        right.Add(Group::Machine, "CPU", value);
        double process =
            Percent(cpu.processKnown && g_cpuBefore.processKnown, cpu.processNs, g_cpuBefore.processNs, wall);
        double machineCpu = system.cpuKnown && g_systemBefore.cpuKnown && system.cpuTotal > g_systemBefore.cpuTotal
                                ? 100.0 * double(system.cpuBusy - g_systemBefore.cpuBusy) /
                                      double(system.cpuTotal - g_systemBefore.cpuTotal)
                                : -1;
        char processText[24] = "--", systemText[24] = "--";
        if (process >= 0)
            snprintf(processText, sizeof(processText), "%.0f %%", process);
        if (machineCpu >= 0)
            snprintf(systemText, sizeof(systemText), "%.0f %%", machineCpu);
        snprintf(value, sizeof(value), "%s %s  %s %s", Words("process", "processus"), processText,
            Words("system", "système"), systemText);
        right.Add(Group::Machine, Words("CPU use", "CPU utilisé"), value);
        char busy[24] = "n/a";
        if (m.gpuFrames != 0 && m.frameMs > 0)
            snprintf(busy, sizeof(busy), "%.0f %%", std::min(100.0, 100.0 * m.gpuMs / m.frameMs));
        snprintf(value, sizeof(value), "%s (%s)  %s %s", snapshot.available ? snapshot.adapter : "n/a",
            snapshot.available ? snapshot.backend : "n/a", Words("busy", "occupé"), busy);
        right.Add(Group::Machine, "GPU", value);
        Bytes(used, sizeof(used), snapshot.videoUsed, snapshot.videoUsageKnown);
        Bytes(total, sizeof(total), snapshot.videoBudget, snapshot.videoBudgetKnown);
        snprintf(value, sizeof(value), "%s %s %s", used, Words("of", "sur"), total);
        right.Add(Group::Machine, Words("GPU memory", "mémoire GPU"), value);
        bool residentKnown;
        uint64_t resident = DebugResidentBytes(residentKnown);
        char game[32], systemUsed[32], systemTotal[32];
        Bytes(game, sizeof(game), resident, residentKnown);
        Bytes(systemUsed, sizeof(systemUsed), system.ramUsed, system.ramKnown);
        Bytes(systemTotal, sizeof(systemTotal), system.ramTotal, system.ramKnown);
        snprintf(value, sizeof(value), "%s %s  %s %s %s %s", Words("game", "jeu"), game, Words("system", "système"),
            systemUsed, Words("of", "sur"), systemTotal);
        right.Add(Group::Machine, "RAM", value);
        if (g_debugWindow != nullptr)
        {
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_debugWindow));
            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(g_debugWindow, &width, &height);
            if (mode != nullptr)
                snprintf(value, sizeof(value), "%dx%d %.0f Hz  %s %dx%d %s", mode->w, mode->h,
                    double(mode->refresh_rate), Words("window", "fenêtre"), width, height,
                    values.fullscreen ? Words("fullscreen", "plein écran") : Words("windowed", "fenêtrée"));
            else
                snprintf(value, sizeof(value), "n/a  %s %dx%d", Words("window", "fenêtre"), width, height);
        }
        else
            snprintf(value, sizeof(value), "n/a");
        right.Add(Group::Machine, Words("display", "écran"), value);
        // The power source changes rarely and its query is the slowest here (macOS: IOKit): every ten seconds.
        static machine::Power power;
        static uint64_t powerRead = 0;
        if (powerRead == 0 || now - powerRead >= 10'000'000'000ull)
        {
            power = machine::ReadPower();
            powerRead = now;
        }
        machine::Heat heat = machine::ReadHeat();
        char heatText[32] = "n/a";
        if (heat.level != machine::Thermal::Unknown)
            snprintf(heatText, sizeof(heatText), "%s",
                heat.level == machine::Thermal::Nominal    ? Words("nominal", "normal")
                : heat.level == machine::Thermal::Moderate ? Words("moderate", "modéré")
                : heat.level == machine::Thermal::Heavy    ? Words("heavy", "élevé")
                                                           : Words("critical", "critique"));
        else if (heat.celsius > -274)
            snprintf(heatText, sizeof(heatText), "%d C", heat.celsius);
        char powerText[48] = "n/a";
        if (power.known && power.battery && power.percent >= 0)
            snprintf(powerText, sizeof(powerText), "%s %d %%", Words("battery", "batterie"), power.percent);
        else if (power.known)
            snprintf(powerText, sizeof(powerText), "%s",
                power.battery ? Words("battery", "batterie") : Words("plugged in", "secteur"));
        snprintf(value, sizeof(value), "%s  %s %s", powerText, Words("thermal", "thermique"), heatText);
        right.Add(Group::Machine, Words("power", "alimentation"), value);

        // SESSION
        uint64_t played = (now - g_debugStart) / 1'000'000'000ull;
        snprintf(value, sizeof(value), "%llu:%02llu:%02llu", (unsigned long long)(played / 3600),
            (unsigned long long)(played / 60 % 60), (unsigned long long)(played % 60));
        right.Add(Group::Session, Words("played", "joué"), value);
        snprintf(value, sizeof(value), "%s %s", g_debugGame, g_debugVersion);
        right.Add(Group::Session, Words("game", "jeu"), value);
#if defined(PLUME_D3D12_COREWINDOW)
        right.Add(Group::Session, "build", "n/a");
#else
        right.Add(Group::Session, "build", LogBuild());
#endif
        snprintf(value, sizeof(value), "%s n/a  %s n/a  %s n/a", Words("disc check", "disque vérifié"),
            Words("profile", "profil"), Words("log warnings", "alertes"));
        right.Add(Group::Session, Words("not measured", "non mesuré"), value);

        // The graph: the last frames, game-thread CPU stacked under GPU, and the slowest of them.
        g_debug.graphCount = std::min(count, debugscreen::GraphBars);
        g_debug.worstMs = g_debug.worstAgo = 0;
        for (size_t i = 0; i < g_debug.graphCount; i++)
        {
            const stats::Sample& s = recent[count - g_debug.graphCount + i];
            g_debug.graphFrameMs[i] = float(s.intervalNs) / 1e6f;
            g_debug.graphCpuMs[i] = s.gameCpuNs != stats::Unknown ? float(s.gameCpuNs) / 1e6f : -1.0f;
            g_debug.graphGpuMs[i] = s.gpuNs != stats::Unknown ? float(s.gpuNs) / 1e6f : -1.0f;
            if (g_debug.graphFrameMs[i] > g_debug.worstMs)
            {
                g_debug.worstMs = g_debug.graphFrameMs[i];
                g_debug.worstAgo = float(double(recent.back().presentNs - s.presentNs) / 1e9);
            }
        }
        // The thread bars: each kind's CPU use since the refresh before (a kind whose threads changed in number
        // waits a refresh); streaming has no thread of its own known.
        for (size_t i = 0; i < debugscreen::ThreadBars; i++)
        {
            bool kind = i < size_t(machine::ThreadCount);
            bool known =
                kind && cpu.threadKnown[i] && g_cpuBefore.threadKnown[i] && cpu.threads[i] == g_cpuBefore.threads[i];
            g_debug.threadPercent[i] =
                kind ? float(Percent(known, cpu.threadNs[i], g_cpuBefore.threadNs[i], wall)) : -1;
            g_debug.threadCount[i] = kind ? cpu.threads[i] : 0;
        }
        g_cpuBefore = cpu;
        g_systemBefore = system;
        g_cpuWallBefore = now;
    }
}

void gamemenu::InitDebugScreen(const char* game, const char* version, SDL_Window* window)
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
    cpu = cpu.substr(0, cpu.find('@'));
    while (!cpu.empty() && cpu.back() == ' ')
        cpu.remove_suffix(1);
    debugscreen::Label(g_debugCpu, sizeof(g_debugCpu), cpu);
#endif
    g_debugWindow = window;
    g_debugStart = SDL_GetTicksNS();
    g_debug = {};
    g_debugLevel = debugscreen::Off;
}

const debugscreen::Screen& gamemenu::DebugScreenState() { return g_debug; }

std::FILE* gamemenu::SwapPerfLog(std::FILE* file)
{
    std::FILE* before = g_log;
    if (before != nullptr)
        WritePerfLog(0); // what is finished already
    g_log = file;
    g_logStopped = nullptr;
    g_logBytes = g_logStart = g_logUntil = 0;
    if (file != nullptr)
    {
        // From the next finished frame on.
        stats::SetEnabled(true);
        std::array<stats::Sample, 1> skip;
        g_logAfter = 0;
        while (stats::ReadNew(g_logAfter, skip) != 0)
        {}
        g_logBytes = fwrite(debugscreen::PerfLogHeader, 1, strlen(debugscreen::PerfLogHeader), file);
    }
    return before;
}

void gamemenu::StopPerfLogAt(uint64_t sdlNs) { g_logUntil = g_log != nullptr ? sdlNs : 0; }

std::FILE* gamemenu::PerfLogStopped() { return std::exchange(g_logStopped, nullptr); }

void gamemenu::AddDebugScreenCost(uint64_t ns) { g_panelNs += ns; }

bool gamemenu::ToggleDebugGraphs()
{
    if (Current().debugScreen != debugscreen::Full)
        return false;
    g_showGraphs = !g_showGraphs;
    fprintf(stderr, "[gamemenu] debug screen graphs %s\n", g_showGraphs ? "on" : "off");
    return true;
}

bool gamemenu::ToggleDebugThreads()
{
    if (Current().debugScreen != debugscreen::Full)
        return false;
    g_showThreads = !g_showThreads;
    fprintf(stderr, "[gamemenu] debug screen threads %s\n", g_showThreads ? "on" : "off");
    return true;
}

std::string gamemenu::DebugScreenText()
{
    if (Current().debugScreen == debugscreen::Off)
        return {};
    std::string text;
    for (const debugscreen::Panel* panel : { &g_debug.left, &g_debug.right })
        for (size_t i = 0; i < panel->count; i++)
        {
            const debugscreen::Row& row = panel->rows[i];
            if (panel->StartsGroup(i))
                text += std::string(text.empty() ? "" : "\n") + Heading(row.group) + "\n";
            text += std::string(row.label) + ": " + row.value + "\n";
        }
    if (g_debug.full && g_debug.graphCount != 0)
    {
        char line[96];
        snprintf(line, sizeof(line), "worst frame of the last %zu: %.1f ms, %.1f s ago\n", g_debug.graphCount,
            double(g_debug.worstMs), double(g_debug.worstAgo));
        text += line;
    }
    return text;
}

bool gamemenu::UpdateDebugScreen()
{
    const auto& values = gamemenu::Current();
    // Measured only while something reads it: the Full screen or the log.
    bool measure = values.debugScreen == debugscreen::Full || g_log != nullptr;
    if (measure != stats::Enabled())
        stats::SetEnabled(measure);
    if (g_log != nullptr)
        WritePerfLog(SDL_GetTicksNS());
    if (values.debugScreen == debugscreen::Off)
    {
        bool changed = g_debugLevel != debugscreen::Off;
        g_debugLevel = debugscreen::Off;
        return changed;
    }
    if (g_debugLevel != values.debugScreen)
    {
        g_debug = {};
        g_cpuWallBefore = 0;
        g_costWindow = 0;
        g_screenAfter = 0;
        g_seconds.fill({});
        g_debugLevel = values.debugScreen;
    }
    uint64_t now = SDL_GetTicksNS();
    uint64_t presented = gpu::FramesPresented();
    // The snapshot first: when the renderer is busy, the refresh waits for the next update rather than the main thread.
    static gpu::DebugSnapshot snapshot;
    static uint64_t dueSince = 0;
    if (!g_debug.rate.Due(now, presented))
    {
        dueSince = 0;
        return false;
    }
    if (dueSince == 0)
        dueSince = now;
    if (!gpu::ReadDebugSnapshot(snapshot, now - dueSince >= 250'000'000))
        return false;
    dueSince = 0;
    g_debug.rate.Update(now, presented);
    g_debug.left.count = g_debug.right.count = 0;
    g_debug.full = values.debugScreen == debugscreen::Full;
    auto frames = std::span(snapshot.frameNs).first(snapshot.frameCount);
    if (g_debug.full)
    {
        // The cost is the work, from here: a wait for the renderer above is the main thread idle, not the frame's.
        uint64_t start = SDL_GetTicksNS();
        Full(values, snapshot, frames, now);
        g_panelNs += SDL_GetTicksNS() - start;
        LogCost(now, presented);
    }
    else
        Compact(values, snapshot, frames);
    return true;
}

void gamemenu::DrawDebugScreen(const DebugFonts& fonts)
{
    if (gamemenu::Current().debugScreen == debugscreen::Off || fonts.text == nullptr)
        return;
    ImFont* mono = fonts.mono != nullptr ? fonts.mono : fonts.text;
    ImFont* heading = fonts.heading != nullptr ? fonts.heading : fonts.text;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float width = ImGui::GetIO().DisplaySize.x, height = ImGui::GetIO().DisplaySize.y;
    // The look approved for it (Skylauncher's): greys 15171C, 1E1E24, 26262C, 3A3A42, and the game's glow as the accent.
    auto rgb = [](uint32_t c, int alpha = 255) {
        return IM_COL32(int(c >> 16 & 0xFF), int(c >> 8 & 0xFF), int(c & 0xFF), alpha);
    };
    const ImU32 strip = rgb(0x15171C, 205), panelFill = rgb(0x1E1E24, 220), track = rgb(0x26262C), rule = rgb(0x3A3A42),
                key = rgb(0xA6A8B4), ink = rgb(0xF2F3F7), accent = rgb(debugscreen::ReadableAccent(game::Accent())),
                cpuInk = rgb(0x4C8DFF), gpuInk = rgb(0xFF8A3D);
    constexpr float Size = 13, Line = 15, Margin = 6, Pad = 3;

    // One line per stat on its own strip: the key in grey, the value in white monospace.
    auto column = [&](const debugscreen::Panel& rows, bool right) {
        float y = Margin;
        for (size_t i = 0; i < rows.count; i++)
        {
            const auto& row = rows.rows[i];
            if (rows.StartsGroup(i))
            {
                if (i != 0)
                    y += 5;
                const char* title = Heading(row.group);
                float w = heading->CalcTextSizeA(Size, FLT_MAX, 0, title).x;
                float x = right ? width - Margin - w - 2 * Pad : Margin;
                draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w + 2 * Pad, y + Line), strip);
                draw->AddText(heading, Size, ImVec2(x + Pad, y + 1), accent, title);
                y += Line;
            }
            float keyWidth = fonts.text->CalcTextSizeA(Size, FLT_MAX, 0, row.label).x;
            float valueWidth = mono->CalcTextSizeA(Size, FLT_MAX, 0, row.value).x;
            float w = keyWidth + 6 + valueWidth;
            float x = right ? width - Margin - w - 2 * Pad : Margin;
            draw->AddRectFilled(ImVec2(x, y), ImVec2(x + w + 2 * Pad, y + Line), strip);
            draw->AddText(fonts.text, Size, ImVec2(x + Pad, y + 1), key, row.label);
            draw->AddText(mono, Size, ImVec2(x + Pad + keyWidth + 6, y + 1), ink, row.value);
            y += Line;
        }
    };
    column(g_debug.left, false);
    column(g_debug.right, true);
    if (!g_debug.full)
        return;

    constexpr float BoxHeight = 128;
    const float boxTop = height - Margin - BoxHeight;
    if (g_showGraphs)
    {
        // Bottom left: the frame-time graph, game-thread CPU (blue) under GPU (orange), 16.7 and 33.3 ms lines.
        const float left = Margin, graphWidth = 2.0f * float(debugscreen::GraphBars), plotTop = boxTop + 22,
                    plotHeight = BoxHeight - 28, msToPx = plotHeight / 50.0f;
        draw->AddRectFilled(ImVec2(left, boxTop), ImVec2(left + graphWidth + 2 * Pad, boxTop + BoxHeight), panelFill);
        const char* title = Words("FRAME TIME", "TEMPS D'IMAGE");
        float at = left + Pad;
        draw->AddText(heading, Size, ImVec2(at, boxTop + 4), accent, title);
        at += heading->CalcTextSizeA(Size, FLT_MAX, 0, title).x + 10;
        for (auto [colour, words] : { std::pair{ cpuInk, "cpu" }, std::pair{ gpuInk, "gpu" } })
        {
            draw->AddRectFilled(ImVec2(at, boxTop + 7), ImVec2(at + 8, boxTop + 15), colour);
            draw->AddText(fonts.text, Size, ImVec2(at + 11, boxTop + 4), key, words);
            at += 11 + fonts.text->CalcTextSizeA(Size, FLT_MAX, 0, words).x + 10;
        }
        char worst[64];
        snprintf(worst, sizeof(worst), "%s %.1f ms, %.1f s", Words("worst", "pire"), double(g_debug.worstMs),
            double(g_debug.worstAgo));
        draw->AddText(mono, Size, ImVec2(at, boxTop + 4), ink, worst);
        const float base = plotTop + plotHeight, x0 = left + Pad;
        draw->AddRectFilled(ImVec2(x0, plotTop), ImVec2(x0 + graphWidth, base), track);
        for (size_t i = 0; i < g_debug.graphCount; i++)
        {
            float x = x0 + 2.0f * float(debugscreen::GraphBars - g_debug.graphCount + i);
            float cpuMs = std::max(g_debug.graphCpuMs[i], 0.0f), gpuMs = std::max(g_debug.graphGpuMs[i], 0.0f);
            float cpuTop = std::max(plotTop, base - cpuMs * msToPx);
            float gpuTop = std::max(plotTop, cpuTop - gpuMs * msToPx);
            // A frame with neither known shows its interval, dim.
            if (g_debug.graphCpuMs[i] < 0 && g_debug.graphGpuMs[i] < 0)
                draw->AddRectFilled(
                    ImVec2(x, std::max(plotTop, base - g_debug.graphFrameMs[i] * msToPx)), ImVec2(x + 2, base), rule);
            if (cpuMs > 0)
                draw->AddRectFilled(ImVec2(x, cpuTop), ImVec2(x + 2, base), cpuInk);
            if (gpuMs > 0 && gpuTop < cpuTop)
                draw->AddRectFilled(ImVec2(x, gpuTop), ImVec2(x + 2, cpuTop), gpuInk);
        }
        for (float ms : { 16.7f, 33.3f })
        {
            float y = base - ms * msToPx;
            draw->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + graphWidth, y + 1), rgb(0xF2F3F7, 110));
            char label[16];
            snprintf(label, sizeof(label), "%.1f", double(ms));
            draw->AddText(mono, 11, ImVec2(x0 + graphWidth - 28, y - 12), key, label);
        }
    }
    if (g_showThreads)
    {
        // Bottom right: each kind of thread's CPU use, in % of one core (the bar is full at one core).
        constexpr float BoxWidth = 300, Bar = 12;
        const float left = width - Margin - BoxWidth;
        draw->AddRectFilled(ImVec2(left, boxTop), ImVec2(left + BoxWidth, boxTop + BoxHeight), panelFill);
        draw->AddText(heading, Size, ImVec2(left + Pad, boxTop + 4), accent, "THREADS");
        static constexpr const char* Names[debugscreen::ThreadBars][2] = { { "game", "jeu" }, { "render", "rendu" },
            { "host main", "hôte" }, { "texture workers", "textures" }, { "shader build", "shaders" },
            { "audio", "audio" }, { "streaming", "flux" } };
        for (size_t i = 0; i < debugscreen::ThreadBars; i++)
        {
            float y = boxTop + 22 + float(i) * 14.5f;
            draw->AddText(fonts.text, 12, ImVec2(left + Pad, y), key, Words(Names[i][0], Names[i][1]));
            const float barLeft = left + 108, barRight = left + BoxWidth - 70;
            draw->AddRectFilled(ImVec2(barLeft, y + 2), ImVec2(barRight, y + Bar), track);
            float percent = g_debug.threadPercent[i];
            char text[24];
            if (percent < 0)
                snprintf(text, sizeof(text), "n/a");
            else
            {
                draw->AddRectFilled(ImVec2(barLeft, y + 2),
                    ImVec2(barLeft + (barRight - barLeft) * std::min(percent, 100.0f) / 100.0f, y + Bar), cpuInk);
                if (g_debug.threadCount[i] > 1)
                    snprintf(text, sizeof(text), "%.0f%% x%u", double(percent), g_debug.threadCount[i]);
                else
                    snprintf(text, sizeof(text), "%.0f%%", double(percent));
            }
            draw->AddText(mono, 12, ImVec2(barRight + 6, y), ink, text);
        }
    }
    // Bottom centre: the keys.
    const char* hint = Words("F3+G graphs   F3+T threads   F3+C copy   F3+R record 10 s",
        "F3+G graphiques   F3+T threads   F3+C copier   F3+R enregistrer 10 s");
    float w = fonts.text->CalcTextSizeA(12, FLT_MAX, 0, hint).x;
    // Between the two bottom boxes (the graph 6..492, the thread bars from width - 306).
    float x = (2 * Margin + 2.0f * float(debugscreen::GraphBars) + width - Margin - 300) / 2 - w / 2,
          y = height - Margin - 15;
    draw->AddRectFilled(ImVec2(x - Pad, y), ImVec2(x + w + Pad, y + 15), strip);
    draw->AddText(fonts.text, 12, ImVec2(x, y + 1), key, hint);
}
