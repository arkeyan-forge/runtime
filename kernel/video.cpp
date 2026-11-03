#include "kernel/video.h"
#include "game.h"
#include "kernel/frame_limiter.h"
#include "host/settings.h"
#include "host/settings_fallback.h"
#include "host/toybox.h"
#include "memory.h"
#include "platform/platform.h"
#include "thread.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "../host/env.h"

namespace
{
    constexpr uint32_t VBlankInterrupt = 0;
    constexpr uint32_t VBlankStatusRegister = 0x7FC86544; // D3D's handler only handles a vblank when bit 0 is set
    constexpr auto VBlankPeriod = std::chrono::microseconds(16667);

    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::deque<std::pair<uint32_t, std::function<void()>>> g_pending;
    uint32_t g_callback;
    uint32_t g_callbackContext;

    // Vertical blanks since the interrupt thread started, and the one the last frame was shown at.
    std::condition_variable g_vblankSignal;
    bool g_vblanksRunning;
    uint64_t g_vblanks;
    uint64_t
        g_interruptsHandled; // interrupts (blanks and others) whose guest callback has returned (InterruptsHandled)
    uint64_t g_lastFlip;

    uint32_t g_fpsLimit = 30;
    FrameLimiter g_limiter; // above 60 (and unlimited): the host paces the frames, the blanks stay 60 Hz
    constexpr uint64_t SpinNanoseconds = 1'500'000; // a sleep wakes up up to about a millisecond late

    bool FasterThanBlanks(uint32_t fps) { return fps == 0 || fps > 60; }

    // Engines step by measured time: holding the flip for two blanks gives a steady 30 FPS.
    FrameRateFallback g_halfRate;

    // SKY_VBLANK_LOG=1: every 600 vertical blanks, how late the interrupt thread delivered them and how long the guest's
    // interrupt callback took (median, p95, max), to tell the emulated blanks' jitter from the game's.
    struct VBlankLog
    {
        std::vector<float> late, callback;
        void Add(std::chrono::steady_clock::duration lateBy, std::chrono::steady_clock::duration took)
        {
            late.push_back(std::chrono::duration<float, std::milli>(lateBy).count());
            callback.push_back(std::chrono::duration<float, std::milli>(took).count());
            if (late.size() < 600)
                return;
            auto stats = [](std::vector<float>& v) {
                std::sort(v.begin(), v.end());
                return std::array<float, 3>{ v[v.size() / 2], v[v.size() * 95 / 100], v.back() };
            };
            auto l = stats(late), c = stats(callback);
            fprintf(stderr,
                "[vblank] late median %.2f p95 %.2f max %.2f ms; callback median %.2f p95 %.2f max %.2f ms\n", l[0],
                l[1], l[2], c[0], c[1], c[2]);
            late.clear();
            callback.clear();
        }
    };

    // One thread delivers every graphics interrupt, in order, like the console's interrupt context.
    void InterruptThread(PPCContext& ctx)
    {
        PlatformSetThreadTimingCritical(uint32_t(VBlankPeriod.count()));
        static const bool logVBlanks = skyenv::Get("SKY_VBLANK_LOG") != nullptr;
        VBlankLog log;
        auto nextVBlank = std::chrono::steady_clock::now() + VBlankPeriod;
        while (true)
        {
            std::pair<uint32_t, std::function<void()>> interrupt;
            uint32_t callback, callbackContext;
            std::chrono::steady_clock::time_point due{};
            {
                std::unique_lock lock(g_mutex);
                if (g_wake.wait_until(lock, nextVBlank, [] { return !g_pending.empty(); }))
                {
                    interrupt = std::move(g_pending.front());
                    g_pending.pop_front();
                }
                else
                {
                    due = nextVBlank;
                    nextVBlank += VBlankPeriod;
                    g_vblanks++;
                    g_vblankSignal.notify_all();
                    interrupt = { VBlankInterrupt, [] { g_memory.Write<uint32_t>(VBlankStatusRegister, 1); } };
                }
                callback = g_callback;
                callbackContext = g_callbackContext;
            }

            if (interrupt.second)
                interrupt.second();
            if (callback == 0)
                continue;

            ctx.r3.u64 = interrupt.first;
            ctx.r4.u64 = callbackContext;
            auto called = std::chrono::steady_clock::now();
            CallGuest(ctx, callback);
            {
                std::lock_guard lock(g_mutex);
                g_interruptsHandled++;
            }
            g_vblankSignal.notify_all();
            if (logVBlanks && due != std::chrono::steady_clock::time_point{})
                log.Add(called - due, std::chrono::steady_clock::now() - called);
        }
    }

    void WriteVideoMode(uint32_t mode)
    {
        memset(g_memory.Translate(mode), 0, 0x30);
        g_memory.Write<uint32_t>(mode + 0x00, 1280); // display width
        g_memory.Write<uint32_t>(mode + 0x04, 720); // display height
        g_memory.Write<uint32_t>(mode + 0x08, 0); // interlaced
        g_memory.Write<uint32_t>(mode + 0x0C, 1); // widescreen
        g_memory.Write<uint32_t>(mode + 0x10, 1); // high definition
        g_memory.Write<uint32_t>(mode + 0x14, std::bit_cast<uint32_t>(60.0f));
        g_memory.Write<uint32_t>(mode + 0x18, 1); // NTSC
        g_memory.Write<uint32_t>(mode + 0x1C, 0x8A);
        g_memory.Write<uint32_t>(mode + 0x20, 0x01);
    }
}

// SKY_CRASH_TEST=fault|abort (test aid, docs/porting/debugging.md): after the 400th flip the guest's render thread faults on a
// null host pointer, or aborts as the runtime's own fatal errors do, to check the crash report's "[crash]" lines.
static void CrashTest()
{
    static const char* mode = skyenv::Get("SKY_CRASH_TEST");
    static uint32_t flips;
    if (mode == nullptr || ++flips != 400)
        return;
    fprintf(stderr, "[crash-test] %s now\n", mode);
    if (std::string_view(mode) == "abort")
        abort();
    *static_cast<volatile int*>(nullptr) = 1;
}

// SKY_CLOCK_STEP=<guest address>: a float the title keeps its frame step in, in 60ths of a second (Lux's F, sub_827F6628): its
// sum over the flips against the wall time, logged every 5 s, says how many seconds of game pass in a second (1.00 is right).
static void ClockCheck()
{
    static const char* value = skyenv::Get("SKY_CLOCK_STEP");
    if (value == nullptr)
        return;
    static const uint32_t address = uint32_t(strtoul(value, nullptr, 16));
    static double game = 0;
    static auto start = std::chrono::steady_clock::now();
    game += double(g_memory.Read<float>(address)) / 60.0;
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (wall >= 5.0)
    {
        fprintf(stderr, "[clock] %.3f s of game in %.3f s: %.3f s/s\n", game, wall, game / wall);
        game = 0;
        start = std::chrono::steady_clock::now();
    }
}

void WaitForFlip(uint32_t presentInterval)
{
    // Test aid: delay real presents before sampling the actual vblank counter, never fabricate misses.
    static const uint32_t lateMs = [] {
        const char* value = skyenv::Get("SKY_FPS_TEST_LATE_MS");
        return value ? uint32_t(std::clamp(atoi(value), 0, 100)) : 0u;
    }();
    if (lateMs != 0)
    {
        static uint32_t samples = 0;
        if (samples++ % 120 == 0)
            fprintf(stderr, "[video-test] delaying presents %u ms; fps limit %u; auto fallback %s\n", lateMs,
                FrameRateLimit(), FpsAutoFallback() ? "on" : "off");
        std::this_thread::sleep_for(std::chrono::milliseconds(lateMs));
    }
    CrashTest();
    ClockCheck();
    static uint32_t seen[8];
    static uint32_t seenCount;
    if (std::find(seen, seen + seenCount, presentInterval) == seen + seenCount && seenCount < 8)
    {
        seen[seenCount++] = presentInterval;
        fprintf(stderr, "[video] present interval 0x%X seen (fps limit %u)\n", presentInterval, g_fpsLimit);
    }
    // D3DPRESENT_INTERVAL_DEFAULT (0) and ONE show the frame at the next vertical blank, TWO and FOUR that many blanks
    // after the previous frame at the earliest; IMMEDIATE (0x80000000) and THREE, which D3D sends as immediate
    // (sub_822A9088), don't wait.
    uint32_t vblanks = presentInterval <= 1 ? 1 : presentInterval == 2 || presentInterval == 4 ? presentInterval : 0;
    // At 60 FPS the title's interval 2 (NGL, sub_82631EA8) shows a frame at every blank. The game steps by the frame
    // time it measures (sub_827F6628), so it keeps its speed.
    if (g_fpsLimit == 60 && vblanks == 2)
        vblanks = 1;
    // A game that presents at every blank by itself (Giants) is held to 30 FPS by showing every second frame. Like SSA's,
    // it steps by the time it measures.
    else if (g_fpsLimit == 30 && vblanks == 1 && game::Current().presentsAtEveryBlank)
        vblanks = 2;

    std::unique_lock lock(g_mutex);
    if (!g_vblanksRunning)
        return;
    // fps_limit above 60 or 0: no wait for a blank (the guest's stay 60 Hz, in wall time), the frame is shown one period
    // after the previous one (g_limiter). Frames that are late are shown at once.
    if (FasterThanBlanks(g_fpsLimit))
    {
        g_lastFlip = g_vblanks;
        uint64_t due = g_limiter.Due(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
        lock.unlock();
        FrameLimiter::WaitUntil(
            due, SpinNanoseconds, [] { return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); },
            [](uint64_t ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); });
        return;
    }
    // fps_limit 60: halve the rate while the frames miss their blank, unless the player turned it off (g_halfRate).
    if (g_fpsLimit == 60 && vblanks == 1)
    {
        const auto change = g_halfRate.Sample(FpsAutoFallback(), g_vblanks > g_lastFlip + 1);
        static bool told = false;
        static uint32_t logged = 0;
        if (change == FrameRateFallback::Change::Halved)
        {
            if (!std::exchange(told, true))
                ToyboxNotice("30 FPS: 60 not held", "30 FPS : 60 non tenu");
            if (logged++ < 20)
                fprintf(stderr,
                    "[video] fps limit 60: frames miss their 16.7 ms blank, shown at 30 FPS until they fit again\n");
        }
        else if (change == FrameRateFallback::Change::Restored && logged++ < 20)
            fprintf(stderr, "[video] fps limit 60: frames fit in a blank again, back to 60 FPS\n");
        if (g_halfRate.active)
            vblanks = 2;
    }
    // A frame that missed its vertical blank is shown at once, where the console would wait for the next one: there is
    // no scan-out to tear here, and a frame a little over 2 blanks (33 ms) would otherwise take 3 (50 ms).
    uint64_t target = vblanks == 0 ? g_vblanks : g_lastFlip + vblanks;
    g_vblankSignal.wait(lock, [&] { return g_vblanks >= target; });
    g_lastFlip = g_vblanks;
}

uint64_t InterruptsHandled()
{
    std::lock_guard lock(g_mutex);
    return g_interruptsHandled;
}

void WaitForInterruptHandled(uint64_t since)
{
    std::unique_lock lock(g_mutex);
    if (!g_vblanksRunning)
        return;
    g_vblankSignal.wait_for(lock, 2 * VBlankPeriod, [&] { return g_interruptsHandled != since; });
}

void SetFrameRateLimit(uint32_t fps)
{
    std::lock_guard lock(g_mutex);
    g_halfRate = {};
    g_fpsLimit = fps;
    g_limiter.SetRate(fps);
}

uint32_t FrameRateLimit() { return g_fpsLimit; }

float FrameScale()
{
    constexpr uint32_t Fastest = 240; // "unlimited" is scaled as the fastest limit
    uint32_t fps = g_fpsLimit == 0 ? Fastest : std::min(g_fpsLimit, Fastest);
    return fps <= 30 ? 1.0f : 30.0f / float(fps);
}

void QueueGraphicsInterrupt(uint32_t source, std::function<void()> prepare)
{
    {
        std::lock_guard lock(g_mutex);
        g_pending.emplace_back(source, std::move(prepare));
    }
    g_wake.notify_one();
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    bool start;
    {
        std::lock_guard lock(g_mutex);
        start = g_callback == 0;
        g_vblanksRunning = true;
        g_callback = ctx.r3.u32;
        g_callbackContext = ctx.r4.u32;
    }

    static std::once_flag started;
    if (start)
        std::call_once(started, [] { StartHostDrivenThread(InterruptThread, 0); });
}

PPC_FUNC(__imp__VdQueryVideoMode) { WriteVideoMode(ctx.r3.u32); }

PPC_FUNC(__imp__XGetVideoMode) { WriteVideoMode(ctx.r3.u32); }

// Read by the debug screen; the pacing state is owned by the vblank mutex.
bool FrameRateFallbackActive()
{
    std::lock_guard lock(g_mutex);
    return g_fpsLimit == 60 && FpsAutoFallback() && g_halfRate.active;
}
