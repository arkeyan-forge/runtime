#include "kernel/video.h"
#include "game.h"
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
#include <vector>

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
    uint64_t g_lastFlip;

    uint32_t g_fpsLimit = 30;

    // SSA_VBLANK_LOG=1: every 600 vertical blanks, how late the interrupt thread delivered them and how long the guest's
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
            fprintf(stderr, "[vblank] late median %.2f p95 %.2f max %.2f ms; callback median %.2f p95 %.2f max %.2f ms\n",
                l[0], l[1], l[2], c[0], c[1], c[2]);
            late.clear();
            callback.clear();
        }
    };

    // One thread delivers every graphics interrupt, in order, like the console's interrupt context.
    void InterruptThread(PPCContext& ctx)
    {
        PlatformSetThreadTimingCritical(uint32_t(VBlankPeriod.count()));
        static const bool logVBlanks = getenv("SSA_VBLANK_LOG") != nullptr;
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
            (PPC_LOOKUP_FUNC(g_memory.base, callback))(ctx, g_memory.base);
            if (logVBlanks && due != std::chrono::steady_clock::time_point{})
                log.Add(called - due, std::chrono::steady_clock::now() - called);
        }
    }

    void WriteVideoMode(uint32_t mode)
    {
        memset(g_memory.Translate(mode), 0, 0x30);
        g_memory.Write<uint32_t>(mode + 0x00, 1280); // display width
        g_memory.Write<uint32_t>(mode + 0x04, 720);  // display height
        g_memory.Write<uint32_t>(mode + 0x08, 0);    // interlaced
        g_memory.Write<uint32_t>(mode + 0x0C, 1);    // widescreen
        g_memory.Write<uint32_t>(mode + 0x10, 1);    // high definition
        g_memory.Write<uint32_t>(mode + 0x14, std::bit_cast<uint32_t>(60.0f));
        g_memory.Write<uint32_t>(mode + 0x18, 1);    // NTSC
        g_memory.Write<uint32_t>(mode + 0x1C, 0x8A);
        g_memory.Write<uint32_t>(mode + 0x20, 0x01);
    }
}

void WaitForFlip(uint32_t presentInterval)
{
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
    // A frame that missed its vertical blank is shown at once, where the console would wait for the next one: there is
    // no scan-out to tear here, and a frame a little over 2 blanks (33 ms) would otherwise take 3 (50 ms).
    uint64_t target = vblanks == 0 ? g_vblanks : g_lastFlip + vblanks;
    g_vblankSignal.wait(lock, [&] { return g_vblanks >= target; });
    g_lastFlip = g_vblanks;
}

void SetFrameRateLimit(uint32_t fps)
{
    g_fpsLimit = fps;
}

uint32_t FrameRateLimit()
{
    return g_fpsLimit;
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

PPC_FUNC(__imp__VdQueryVideoMode)
{
    WriteVideoMode(ctx.r3.u32);
}

PPC_FUNC(__imp__XGetVideoMode)
{
    WriteVideoMode(ctx.r3.u32);
}
