// Shows Bink movies in the window before the renderer exists: after each decoded frame, convert Bink's own YUV
// frame buffers (which the game's textures point at) to RGBA and hand them to the display.
#include "d3d/ultrawide.h"
#include "host/display.h"
#include "host/settings.h"
#include "game.h"
#include "game_hooks.h"
#include "memory.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    // HBINK
    constexpr uint32_t BinkWidth = 0;
    constexpr uint32_t BinkHeight = 4;

    // BINKFRAMEBUFFERS: TotalFrames, YA width/height, CrCb width/height, FrameNum, then per frame set four
    // BINKPLANEs (Y, cR, cB, A), each { Allocate, Buffer, BufferPitch }.
    constexpr uint32_t BuffersFrameNum = 20;
    constexpr uint32_t BuffersFrames = 24;
    constexpr uint32_t PlaneSize = 12;
    constexpr uint32_t PlaneSetSize = 4 * PlaneSize;

    // HBINK points at the BINKFRAMEBUFFERS the game registered (BinkRegisterFrameBuffers); find that field by its
    // contents: the buffer width, then a height rounded up from the movie's.
    uint32_t FindFrameBuffers(uint32_t bink, uint32_t width, uint32_t height)
    {
        static uint32_t offset;
        if (offset == 0)
        {
            for (uint32_t candidate = 8; candidate < 0x400; candidate += 4)
            {
                uint32_t pointer = g_memory.Read<uint32_t>(bink + candidate);
                if (pointer < 0x10000 || pointer > 0xFFFF0000)
                    continue;
                uint32_t bufferHeight = g_memory.Read<uint32_t>(pointer + 8);
                if (g_memory.Read<uint32_t>(pointer + 4) == width && bufferHeight >= height && bufferHeight < height + 64)
                {
                    offset = candidate;
                    break;
                }
            }
            if (offset == 0)
                return 0;
        }
        return g_memory.Read<uint32_t>(bink + offset);
    }

    uint8_t Clamp(int value)
    {
        return uint8_t(std::clamp(value, 0, 255));
    }

    void ShowFrame(uint32_t bink)
    {
        if (IsGameRendering())
            return; // the game draws the movie frames itself
        uint32_t width = g_memory.Read<uint32_t>(bink + BinkWidth);
        uint32_t height = g_memory.Read<uint32_t>(bink + BinkHeight);
        uint32_t buffers = FindFrameBuffers(bink, width, height);
        if (buffers == 0 || width == 0 || height == 0 || width > 4096 || height > 4096)
            return;

        uint32_t set = buffers + BuffersFrames + g_memory.Read<uint32_t>(buffers + BuffersFrameNum) * PlaneSetSize;
        auto plane = [&](uint32_t index, uint32_t& pitch)
        {
            pitch = g_memory.Read<uint32_t>(set + index * PlaneSize + 8);
            return static_cast<const uint8_t*>(g_memory.Translate(g_memory.Read<uint32_t>(set + index * PlaneSize + 4)));
        };

        uint32_t yPitch, crPitch, cbPitch;
        const uint8_t* y = plane(0, yPitch);
        const uint8_t* cr = plane(1, crPitch);
        const uint8_t* cb = plane(2, cbPitch);

        // BT.601, limited range.
        std::vector<uint32_t> rgba(width * height);
        for (uint32_t row = 0; row < height; row++)
        {
            for (uint32_t column = 0; column < width; column++)
            {
                int luma = (y[row * yPitch + column] - 16) * 298;
                int v = cr[(row / 2) * crPitch + column / 2] - 128;
                int u = cb[(row / 2) * cbPitch + column / 2] - 128;
                uint8_t r = Clamp((luma + 409 * v + 128) >> 8);
                uint8_t g = Clamp((luma - 100 * u - 208 * v + 128) >> 8);
                uint8_t b = Clamp((luma + 516 * u + 128) >> 8);
                rgba[row * width + column] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        }
        SubmitDisplayFrame(std::move(rgba), int(width), int(height));
    }
}

#ifdef GAME_HOOK_BinkDoFrame
PPC_FUNC_IMPL(GAME_IMP(BinkDoFrame));
// BinkDoFrame(HBINK)
PPC_FUNC(GAME_HOOK(BinkDoFrame))
{
    uint32_t bink = ctx.r3.u32;
    GAME_IMP(BinkDoFrame)(ctx, base);
    ShowFrame(bink);
}
#endif

#ifdef GAME_HOOK_BinkOpen
PPC_FUNC_IMPL(GAME_IMP(BinkOpen));
// BinkOpen(name, flags) -> HBINK (Width, Height, Frames, FrameNum...). A movie made one frame long ends at once and the
// game goes on (as tsapZ/GiantsRecomp does with the header's frame count): every movie with SSA_SKIP_MOVIES=1 (test
// aid), the game's start-up logos with [video] skip_intro_movies=1 (game::IsIntroMovie; the story movies still play).
// Each skipped logo is logged as "[movie] skipped <name>".
PPC_FUNC(GAME_HOOK(BinkOpen))
{
    static const bool skipAll = getenv("SSA_SKIP_MOVIES") != nullptr && atoi(getenv("SSA_SKIP_MOVIES")) != 0;
    std::string path;
    if (ctx.r3.u32 >= 0x10000)
        for (uint32_t i = 0; i < 260; i++)
        {
            uint8_t c = g_memory.Read<uint8_t>(ctx.r3.u32 + i);
            if (c == 0)
                break;
            path.push_back(char(c));
        }
    GAME_IMP(BinkOpen)(ctx, base);
    uint32_t bink = ctx.r3.u32;
    if (bink == 0)
        return;
    bool intro = SkipIntroMovies() && game::IsIntroMovie(path);
    fprintf(stderr, "[bink] open %s %ux%u, %u frames%s\n", game::MovieName(path).c_str(), g_memory.Read<uint32_t>(bink),
        g_memory.Read<uint32_t>(bink + 4), g_memory.Read<uint32_t>(bink + 8), skipAll ? " (skipped)" : "");
    if (intro)
        fprintf(stderr, "[movie] skipped %s\n", game::MovieName(path).c_str());
    if (skipAll || intro)
        g_memory.Write<uint32_t>(bink + 8, 1);
    ultrawide::MovieOpened();

    // SSA_BINK_LOG=1 (test aid): once a second, the last opened movie's frame number (HBINK+12) against the wall clock,
    // to check that movies play at their own rate (Bink paces the picture on its sound track).
    static std::atomic<uint32_t> s_current{ 0 };
    s_current = bink;
    static std::once_flag started;
    if (getenv("SSA_BINK_LOG") != nullptr)
        std::call_once(started, [] {
            std::thread([] {
                auto start = std::chrono::steady_clock::now();
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    uint32_t current = s_current.load();
                    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    fprintf(stderr, "[bink] t=%.2f frame %u of %u\n", seconds, g_memory.Read<uint32_t>(current + 12),
                        g_memory.Read<uint32_t>(current + 8));
                }
            }).detach();
        });
}
#endif

#ifdef GAME_HOOK_BinkClose
PPC_FUNC_IMPL(GAME_IMP(BinkClose));
// BinkClose(HBINK): the movie is over (ultrawide's bars, d3d/ultrawide.h).
PPC_FUNC(GAME_HOOK(BinkClose))
{
    if (ctx.r3.u32 != 0)
        ultrawide::MovieClosed();
    GAME_IMP(BinkClose)(ctx, base);
}
#endif
