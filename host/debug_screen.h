#pragma once

// Allocation-free presentation logic, independent of the renderer and the host clock.
#include "gpu/frame_stats.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string_view>

namespace debugscreen
{
    constexpr size_t MaxFrames = 2048, GraphBars = 240;
    constexpr uint64_t RefreshNs = 500'000'000;

    // [<game>.video] debug_screen: 0 off, 1 compact, 2 full. Shift+F3 in play and the F3 row go round in this order.
    enum Level : uint32_t
    {
        Off,
        Compact,
        Full,
        LevelCount
    };
    inline uint32_t NextLevel(uint32_t level) { return (level + 1) % LevelCount; }

    struct Rate
    {
        uint64_t time = 0, count = 0;
        double fps = 0;
        bool started = false, ready = false;
        // Whether Update would refresh now (it changes nothing).
        bool Due(uint64_t now, uint64_t frames) const
        {
            return !started || now < time || frames < count || now - time >= RefreshNs;
        }
        bool Update(uint64_t now, uint64_t frames)
        {
            if (!started || now < time || frames < count)
            {
                time = now;
                count = frames;
                started = true;
                fps = 0;
                ready = false;
                return true;
            }
            if (now - time < RefreshNs)
                return false;
            fps = double(frames - count) * 1e9 / double(now - time);
            time = now;
            count = frames;
            ready = true;
            return true;
        }
    };

    // Present-to-present pacing over the available frames: the shortest, the mean and the 99th percentile (the frame
    // 99 % of the frames are at most as long as), in milliseconds.
    struct Pacing
    {
        double minMs = 0, meanMs = 0, p99Ms = 0;
        size_t count = 0;
    };
    inline Pacing PacingOf(std::span<const uint64_t> frames)
    {
        std::array<uint64_t, MaxFrames> sorted{};
        Pacing pacing;
        double sum = 0;
        for (uint64_t ns : frames.last(std::min(frames.size(), MaxFrames)))
            if (ns != 0)
            {
                sorted[pacing.count++] = ns;
                sum += double(ns);
            }
        if (pacing.count == 0)
            return pacing;
        std::sort(sorted.begin(), sorted.begin() + std::ptrdiff_t(pacing.count));
        pacing.minMs = double(sorted[0]) / 1e6;
        pacing.meanMs = sum / double(pacing.count) / 1e6;
        size_t at = (pacing.count * 99 + 99) / 100; // the ceil(99 %)th frame, 1-based
        pacing.p99Ms = double(sorted[std::min(at, pacing.count) - 1]) / 1e6;
        return pacing;
    }

    // Why a frame over 50 ms was late, in this order: a pipeline built at draw time, the game's render thread waiting for
    // GPU work it needed back (a resolve read back), a GPU busier than half the frame, the game reading files (a load),
    // more than 16 MB of texture uploads; else unknown.
    using gpu::framestats::Sample;
    constexpr uint64_t HitchNs = 50'000'000, HitchUploadBytes = 16ull << 20;
    enum class Hitch : uint8_t
    {
        None,
        Pipeline,
        Stall,
        Gpu,
        Load,
        Uploads,
        Unknown
    };
    inline Hitch Classify(const Sample& s)
    {
        if (s.intervalNs < HitchNs)
            return Hitch::None;
        if (s.builds != 0)
            return Hitch::Pipeline;
        if (uint64_t(s.stallNs) * 2 >= s.intervalNs)
            return Hitch::Stall;
        if (s.gpuNs != gpu::framestats::Unknown && uint64_t(s.gpuNs) * 2 >= s.intervalNs)
            return Hitch::Gpu;
        if (s.fileReads != 0)
            return Hitch::Load;
        if (s.uploadBytes >= HitchUploadBytes)
            return Hitch::Uploads;
        return Hitch::Unknown;
    }
    // The performance log's words (also the English screen's).
    inline const char* HitchName(Hitch hitch)
    {
        constexpr const char* Names[] = { "", "pipeline", "stall", "gpu", "load", "uploads", "unknown" };
        return Names[size_t(hitch)];
    }

    // What limits the frame rate, from per-frame means in milliseconds (negative: unknown): the frame limit when the
    // frames already last what it asks (then nothing else does: a thread may still spin while it waits), else the GPU
    // when it is busy for most of a frame, else a CPU thread when one is (the game's render thread or the renderer's);
    // else nothing clear.
    enum class Bound
    {
        Unknown,
        Gpu,
        Cpu,
        Limit
    };
    inline Bound Decide(double frameMs, double cpuMs, double gpuMs, uint32_t limitFps)
    {
        if (frameMs <= 0)
            return Bound::Unknown;
        if (limitFps != 0 && 1000.0 / double(limitFps) >= 0.95 * frameMs)
            return Bound::Limit;
        if (gpuMs >= 0.85 * frameMs)
            return Bound::Gpu;
        if (cpuMs >= 0.85 * frameMs)
            return Bound::Cpu;
        return Bound::Unknown;
    }

    // The game's glow made readable on the screen's dark strips (0x15171C), as Skylauncher does for Trap Team's red: mixed
    // toward white until its contrast ratio is at least 4.5:1.
    inline double Luminance(uint32_t rgb)
    {
        auto channel = [](uint32_t c) {
            double v = double(c) / 255.0;
            return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(rgb >> 16 & 0xFF) + 0.7152 * channel(rgb >> 8 & 0xFF) + 0.0722 * channel(rgb & 0xFF);
    }
    inline uint32_t ReadableAccent(uint32_t rgb, uint32_t background = 0x15171C)
    {
        double dark = Luminance(background) + 0.05;
        for (int step = 0; step <= 20; step++)
        {
            auto mix = [&](int shift) {
                uint32_t c = rgb >> shift & 0xFF;
                return uint32_t(c + (255 - c) * uint32_t(step) / 20) << shift;
            };
            uint32_t mixed = mix(16) | mix(8) | mix(0);
            if ((Luminance(mixed) + 0.05) / dark >= 4.5)
                return mixed;
        }
        return 0xFFFFFF;
    }

    // CPU time over wall time, in % of one core (100 %: a whole core; a process can exceed it).
    inline double CorePercent(uint64_t cpuNs, uint64_t wallNs)
    {
        return wallNs == 0 ? 0 : double(cpuNs) * 100.0 / double(wallNs);
    }

    // The performance log: a CSV file, one line per presented frame, at most PerfLogMaxBytes (then it stops).
    constexpr uint64_t PerfLogMaxBytes = 32ull << 20;
    constexpr const char* PerfLogHeader = "time_s,frame_ms,renderer_ms,gpu_ms,latency_ms,draws,resolves,uploads,"
                                          "upload_mb,write_faults,pipelines_built,file_reads,hitch\n";
    // One line for `s` (`startNs`: the present clock when the log started); unknown values are empty. Returns its
    // length, 0 if it did not fit.
    inline size_t PerfLogLine(char* out, size_t capacity, const Sample& s, uint64_t startNs)
    {
        char gpuMs[24] = "", latencyMs[24] = "";
        if (s.gpuNs != gpu::framestats::Unknown)
            snprintf(gpuMs, sizeof(gpuMs), "%.3f", double(s.gpuNs) / 1e6);
        if (s.latencyNs != gpu::framestats::Unknown)
            snprintf(latencyMs, sizeof(latencyMs), "%.3f", double(s.latencyNs) / 1e6);
        int length = snprintf(out, capacity, "%.4f,%.3f,%.3f,%s,%s,%u,%u,%u,%.3f,%u,%u,%u,%s\n",
            s.presentNs >= startNs ? double(s.presentNs - startNs) / 1e9 : 0.0, double(s.intervalNs) / 1e6,
            double(s.rendererNs) / 1e6, gpuMs, latencyMs, s.draws, s.resolves, s.uploads,
            double(s.uploadBytes) / double(1 << 20), s.faults, s.builds, s.fileReads, HitchName(Classify(s)));
        return length > 0 && size_t(length) < capacity ? size_t(length) : 0;
    }

    // Reciprocal of the mean duration of the slowest ceil(1%) frames, not a percentile of FPS.
    inline double LowOnePercent(std::span<const uint64_t> frames)
    {
        std::array<uint64_t, MaxFrames> sorted{};
        size_t count = 0;
        for (uint64_t ns : frames.last(std::min(frames.size(), MaxFrames)))
            if (ns != 0)
                sorted[count++] = ns;
        if (count == 0)
            return 0;
        std::sort(sorted.begin(), sorted.begin() + count, std::greater<uint64_t>());
        size_t slow = (count + 99) / 100;
        double sum = 0;
        for (size_t i = 0; i < slow; i++)
            sum += double(sorted[i]);
        return double(slow) * 1e9 / sum;
    }

    inline void Bytes(char* out, size_t capacity, uint64_t bytes, bool known = true)
    {
        if (!known)
            snprintf(out, capacity, "n/a");
        else if (bytes >= (1ull << 30))
            snprintf(out, capacity, "%.2f GiB", double(bytes) / double(1ull << 30));
        else
            snprintf(out, capacity, "%.0f MiB", double(bytes) / double(1ull << 20));
    }

    // Only display labels supplied by the runtime, never a path. Reject control characters too.
    inline void Label(char* out, size_t capacity, std::string_view value)
    {
        if (capacity == 0)
            return;
        // A slash is only allowed in "n/a" (a composite value's unknown part).
        for (size_t at = value.find_first_of("/\\@"); at != std::string_view::npos;
            at = value.find_first_of("/\\@", at + 1))
            if (value[at] != '/' || at == 0 || value[at - 1] != 'n' || at + 1 == value.size() || value[at + 1] != 'a' ||
                (at >= 2 && value[at - 2] != ' ') || (at + 2 < value.size() && value[at + 2] != ' '))
            {
                value = "n/a";
                break;
            }
        size_t length = 0;
        for (unsigned char c : value)
        {
            if (length + 1 >= capacity)
                break;
            out[length++] = c >= 32 && c != 127 ? char(c) : ' ';
        }
        // Do not leave the leading bytes of a truncated UTF-8 character at the buffer boundary.
        if (length < value.size())
            while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80)
                length--;
        out[length] = 0;
    }

    // The sections, each under its heading: left FRAME, GUEST, RENDER, AUDIO / INPUT; right MACHINE, SESSION.
    enum class Group
    {
        Frame,
        Guest,
        Render,
        AudioInput,
        Machine,
        Session
    };
    struct Row
    {
        Group group{};
        char label[32]{};
        char value[192]{};
    };
    struct Panel
    {
        std::array<Row, 32> rows{};
        size_t count = 0;
        void Add(Group group, const char* label, const char* value)
        {
            if (count == rows.size())
                return;
            Row& row = rows[count++];
            row.group = group;
            Label(row.label, sizeof(row.label), label);
            Label(row.value, sizeof(row.value), value);
        }
        bool StartsGroup(size_t row) const { return row == 0 || rows[row].group != rows[row - 1].group; }
    };

    // The per-thread CPU bars (Full): the kinds of gpu::framestats::Thread, then the streaming threads (none known).
    constexpr size_t ThreadBars = 7;

    struct Screen
    {
        Panel left, right;
        // Full: the frame-time graph, each frame's game-thread CPU and GPU milliseconds (negative: unknown), oldest first.
        std::array<float, GraphBars> graphCpuMs{}, graphGpuMs{}, graphFrameMs{};
        size_t graphCount = 0;
        float worstMs = 0, worstAgo = 0; // the slowest of them, and how long ago (seconds)
        // Full: each thread kind's CPU use in % of one core (negative: unknown) and how many threads it has.
        std::array<float, ThreadBars> threadPercent{};
        std::array<uint32_t, ThreadBars> threadCount{};
        bool full = false;
        Rate rate;
    };
}

namespace gamemenu
{
    // The panels as last collected (gamemenu_debug.cpp), for ArkeyanDebugScreenTest.
    const debugscreen::Screen& DebugScreenState();
}
