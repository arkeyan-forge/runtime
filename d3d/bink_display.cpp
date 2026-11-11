// Shows Bink movies in the window before the renderer exists: after each decoded frame, convert Bink's own YUV
// frame buffers (which the game's textures point at) to RGBA and hand them to the display.
#include "d3d/ultrawide.h"
#include "d3d/bink_frame.h"
#include "d3d/bink_display.h"
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
#include <unordered_set>
#include <ppc_config.h>
#include <ppc_context.h>
#include "../host/env.h"

namespace
{
    std::mutex g_failedMovieMutex;
    std::unordered_set<uint32_t> g_failedMovies;
    std::atomic<bool> g_failedMoviePlaying{ false };
#if defined(GAME_HOOK_BinkOpen) || defined(GAME_HOOK_BinkClose)
    std::mutex g_binkLogMutex;
    uint32_t g_loggedMovie = 0;
#endif

    void FailMovie(uint32_t bink, const char* reason)
    {
#if defined(GAME_HOOK_BinkOpen) && defined(GAME_HOOK_BinkClose)
        {
            std::lock_guard lock(g_failedMovieMutex);
            g_failedMovies.insert(bink);
            g_failedMoviePlaying = true;
        }
#endif
        d3d::EndFailedBinkMovie(
            bink, [](uint32_t address, uint32_t value) { g_memory.Write<uint32_t>(address, value); });
        // Do not wait for a decoder clock or audio callback to reach the end of an unusable picture.
        static std::mutex mutex;
        static std::unordered_set<std::string> reported;
        std::lock_guard lock(mutex);
        if (reported.insert(reason).second)
            fprintf(stderr, "[bink] handle %08X: %s; showing black and ending the movie\n", bink, reason);
    }

    // The story movie that plays (game::Adapter::storyMovieSkipButton): its HBINK (+8 its frame count, +12 the frame
    // shown) and name, 0 when none.
    std::mutex g_storyMovieMutex;
    uint32_t g_storyMovie = 0;
    char g_storyMovieName[64] = {};

    // Opens (bink != 0) or forgets the story movie; a close forgets it only when `closed` is that movie.
    [[maybe_unused]] void SetStoryMovie(uint32_t bink, const std::string& name, uint32_t closed = 0)
    {
        std::lock_guard lock(g_storyMovieMutex);
        if (bink != 0)
        {
            g_storyMovie = bink;
            snprintf(g_storyMovieName, sizeof(g_storyMovieName), "%s", name.c_str());
        }
        else if (closed == g_storyMovie)
            g_storyMovie = 0;
    }
}

bool d3d::FailedBinkMoviePlaying() { return g_failedMoviePlaying.load(); }

void game::StoryMovieButtons(uint16_t buttons)
{
    static uint16_t last = 0;
    const uint16_t button = Current().storyMovieSkipButton;
    const bool pressed = button != 0 && (buttons & button) != 0 && (last & button) == 0;
    last = buttons;
    if (!pressed)
        return;
    std::lock_guard lock(g_storyMovieMutex);
    if (g_storyMovie == 0)
        return;
    // Bink ends a movie when the frame shown reaches its frame count: the game then goes on as at the movie's end
    // (SuperChargers' fmv01 cut at its first frame: the vehicle prompt 23 s after the menus, run skC).
    const uint32_t frame = g_memory.Read<uint32_t>(g_storyMovie + 12),
                   frames = g_memory.Read<uint32_t>(g_storyMovie + 8);
    if (frame + 1 < frames)
    {
        g_memory.Write<uint32_t>(g_storyMovie + 8, frame + 1);
        fprintf(stderr, "[movie] skipped %s at frame %u of %u (the skip button)\n", g_storyMovieName, frame, frames);
    }
}

#ifdef GAME_HOOK_BinkDoFrame
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

    uint32_t AllocatedBytes(uint32_t address)
    {
        const bool physical = address >= 0xA0000000;
        const uint32_t offset = physical ? address & 0x1FFFFFFF : address;
        const GuestHeap& heap = physical                             ? g_physicalHeap
                                : g_virtualHeap64K.Contains(address) ? g_virtualHeap64K
                                                                     : g_virtualHeap4K;
        uint32_t size = 0;
        const uint32_t allocation = heap.FindAllocation(offset, &size);
        return allocation != 0 ? size - (offset - allocation) : 0;
    }

    // HBINK points at the BINKFRAMEBUFFERS the game registered (BinkRegisterFrameBuffers); find that field by its
    // contents: the buffer width, then a height rounded up from the movie's.
    uint32_t FindFrameBuffers(uint32_t bink, uint32_t width, uint32_t height)
    {
        static thread_local uint32_t offset;
        auto matches = [&](uint32_t pointer) {
            if (AllocatedBytes(pointer) < BuffersFrames)
                return false;
            const uint32_t bufferHeight = g_memory.Read<uint32_t>(pointer + 8);
            return g_memory.Read<uint32_t>(pointer + 4) == width && bufferHeight >= height &&
                   bufferHeight < height + 64;
        };
        const uint32_t bytes = std::min(AllocatedBytes(bink), 0x400u);
        if (offset != 0 && (offset + 4 > bytes || !matches(g_memory.Read<uint32_t>(bink + offset))))
            offset = 0;
        if (offset == 0)
        {
            for (uint32_t candidate = 8; candidate + 4 <= bytes; candidate += 4)
            {
                uint32_t pointer = g_memory.Read<uint32_t>(bink + candidate);
                if (matches(pointer))
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

    void ShowFrame(uint32_t bink)
    {
        if (IsGameRendering())
            return; // the game draws the movie frames itself
        uint32_t width = g_memory.Read<uint32_t>(bink + BinkWidth);
        uint32_t height = g_memory.Read<uint32_t>(bink + BinkHeight);
        const uint32_t buffers = d3d::BinkDimensionsValid(width, height) ? FindFrameBuffers(bink, width, height) : 0;
        const char* error = "invalid dimensions";
        std::vector<uint32_t> rgba;
        if (buffers != 0)
        {
            const uint32_t frame = g_memory.Read<uint32_t>(buffers + BuffersFrameNum);
            const uint32_t count = g_memory.Read<uint32_t>(buffers);
            if (frame < count &&
                uint64_t(BuffersFrames) + uint64_t(frame + 1) * PlaneSetSize <= AllocatedBytes(buffers))
            {
                const uint32_t set = buffers + BuffersFrames + frame * PlaneSetSize;
                auto plane = [&](uint32_t index) -> d3d::BinkPlane {
                    const uint32_t address = g_memory.Read<uint32_t>(set + index * PlaneSize + 4);
                    const uint32_t pitch = g_memory.Read<uint32_t>(set + index * PlaneSize + 8);
                    const uint32_t bytes = AllocatedBytes(address);
                    if (bytes == 0)
                        return {};
                    return { { static_cast<const uint8_t*>(g_memory.Translate(address)), bytes }, pitch };
                };
                error = d3d::ConvertBinkFrame(width, height, plane(0), plane(1), plane(2), rgba);
            }
            else
                error = "invalid frame buffer index";
        }
        else if (d3d::BinkDimensionsValid(width, height))
            error = "missing frame buffers";
        if (error != nullptr)
        {
            FailMovie(bink, error);
            rgba.assign(1, d3d::BinkBlack);
            width = height = 1;
        }
        SubmitDisplayFrame(std::move(rgba), int(width), int(height));
    }
}

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
// game goes on (as tsapZ/GiantsRecomp does with the header's frame count): every movie with SKY_SKIP_MOVIES=1 (test
// aid), the game's start-up logos with [video] skip_intro_movies=1 (game::IsIntroMovie; the story movies still play).
// Each skipped logo is logged as "[movie] skipped <name>".
PPC_FUNC(GAME_HOOK(BinkOpen))
{
    static const bool skipAll = skyenv::Get("SKY_SKIP_MOVIES") != nullptr && atoi(skyenv::Get("SKY_SKIP_MOVIES")) != 0;
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
    {
        fprintf(stderr, "[bink] open failed %s (no handle)\n", game::MovieName(path).c_str());
        SubmitDisplayFrame({ d3d::BinkBlack }, 1, 1);
        return;
    }
    ultrawide::MovieOpened();
    if (g_memory.Read<uint32_t>(bink) == 0 || g_memory.Read<uint32_t>(bink + 4) == 0 ||
        g_memory.Read<uint32_t>(bink + 8) == 0)
    {
        fprintf(stderr, "[bink] invalid movie header %s\n", game::MovieName(path).c_str());
        FailMovie(bink, "invalid movie header");
        SubmitDisplayFrame({ d3d::BinkBlack }, 1, 1);
        return;
    }
    bool intro = SkipIntroMovies() && game::IsIntroMovie(path);
    fprintf(stderr, "[bink] open %s %ux%u, %u frames%s\n", game::MovieName(path).c_str(), g_memory.Read<uint32_t>(bink),
        g_memory.Read<uint32_t>(bink + 4), g_memory.Read<uint32_t>(bink + 8), skipAll ? " (skipped)" : "");
    if (intro)
        fprintf(stderr, "[movie] skipped %s\n", game::MovieName(path).c_str());
    if (skipAll || intro)
        g_memory.Write<uint32_t>(bink + 8, 1);
    else if (game::Current().storyMovieSkipButton != 0)
        SetStoryMovie(bink, game::MovieName(path));

    // SKY_BINK_LOG=1 (test aid): once a second, the last opened movie's frame number (HBINK+12) against the wall clock,
    // to check that movies play at their own rate (Bink paces the picture on its sound track).
    {
        std::lock_guard lock(g_binkLogMutex);
        g_loggedMovie = bink;
    }
    static std::once_flag started;
    if (skyenv::Get("SKY_BINK_LOG") != nullptr)
        std::call_once(started, [] {
            std::thread([] {
                auto start = std::chrono::steady_clock::now();
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    std::lock_guard lock(g_binkLogMutex);
                    const uint32_t current = g_loggedMovie;
                    if (current == 0)
                        continue;
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
    {
        std::lock_guard lock(g_binkLogMutex);
        if (g_loggedMovie == ctx.r3.u32)
            g_loggedMovie = 0;
    }
    {
        std::lock_guard lock(g_failedMovieMutex);
        g_failedMovies.erase(ctx.r3.u32);
        g_failedMoviePlaying = !g_failedMovies.empty();
    }
    if (ctx.r3.u32 != 0)
        ultrawide::MovieClosed();
    SetStoryMovie(0, {}, ctx.r3.u32);
    GAME_IMP(BinkClose)(ctx, base);
}
#endif
