// Calls the real hooks with synthetic handles and frame buffers; opens no files or windows.
#include "d3d/bink_display.h"
#include "d3d/bink_frame.h"
#include "d3d/ultrawide.h"
#include "game.h"
#include "game_hooks.h"
#include "memory.h"
#include "platform/platform.h"
#include <ppc_config.h>
#include <ppc_context.h>
#include <cstdio>

PPC_EXTERN_FUNC(GAME_HOOK(BinkOpen));
PPC_EXTERN_FUNC(GAME_HOOK(BinkClose));
PPC_EXTERN_FUNC(GAME_HOOK(BinkDoFrame));

namespace
{
    int g_failures = 0, g_openMovies = 0;
    uint32_t g_openResult = 0;
    bool g_rendering = false;
    std::vector<uint32_t> g_picture;
    int g_width = 0, g_height = 0;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            fprintf(stderr, "FAIL: %s\n", message);
            g_failures++;
        }
    }

    void Call(void (*hook)(PPCContext&, uint8_t*), uint32_t argument)
    {
        PPCContext ctx{};
        ctx.r3.u64 = argument;
        hook(ctx, g_memory.base);
        Check(ctx.r3.u32 == argument, "decode and close preserve the original's result");
    }

    void Open(uint32_t handle, uint32_t path)
    {
        g_openResult = handle;
        PPCContext ctx{};
        ctx.r3.u64 = path;
        GAME_HOOK(BinkOpen)(ctx, g_memory.base);
        Check(ctx.r3.u32 == handle, "open preserves the original's result");
    }

    bool Black()
    {
        return !g_picture.empty() &&
               std::all_of(g_picture.begin(), g_picture.end(), [](uint32_t pixel) { return pixel == d3d::BinkBlack; });
    }
}

// No OS mappings are needed: these tests use only the small buffer supplied below.
uint8_t* PlatformReserveMemory(size_t) { return nullptr; }
bool PlatformProtectNoAccess(void*, size_t) { return true; }

bool IsGameRendering() { return g_rendering; }
void SubmitDisplayFrame(std::vector<uint32_t> rgba, int width, int height)
{
    g_picture = std::move(rgba);
    g_width = width;
    g_height = height;
}

bool SkipIntroMovies() { return false; }
void ultrawide::MovieOpened() { g_openMovies++; }
void ultrawide::MovieClosed() { g_openMovies--; }

const GameInfo g_gameInfo{};
const game::Adapter& game::Current()
{
    static const Adapter adapter{ .info = g_gameInfo };
    return adapter;
}
std::string game::MovieName(std::string_view) { return "synthetic_movie"; }
bool game::IsIntroMovie(std::string_view) { return false; }

PPC_FUNC_IMPL(GAME_IMP(BinkOpen)) { ctx.r3.u64 = g_openResult; }
PPC_FUNC_IMPL(GAME_IMP(BinkClose)) {}
PPC_FUNC_IMPL(GAME_IMP(BinkDoFrame)) {}

int main()
{
    std::vector<uint8_t> memory(0x100000);
    g_memory.base = memory.data();
    const uint32_t path = g_virtualHeap4K.Alloc(0x1000);
    memcpy(g_memory.Translate(path), "synthetic.bik", sizeof("synthetic.bik"));
    const uint32_t bink = g_virtualHeap4K.Alloc(0x1000);
    const uint32_t buffers = g_virtualHeap4K.Alloc(0x1000);
    const uint32_t planes = g_virtualHeap4K.Alloc(0x1000);
    auto header = [&](uint32_t width = 2, uint32_t height = 2, uint32_t frames = 10) {
        g_memory.Write<uint32_t>(bink, width);
        g_memory.Write<uint32_t>(bink + 4, height);
        g_memory.Write<uint32_t>(bink + 8, frames);
        g_memory.Write<uint32_t>(bink + 12, 1);
    };

    Open(0, path);
    Check(g_openMovies == 0 && !d3d::FailedBinkMoviePlaying(), "failed open starts no movie lifetime");
    Check(Black(), "failed open supplies black to the host display");

    // An empty header must reach completion even without a decoder clock or a decoded frame.
    header(2, 2, 0);
    Open(bink, path);
    Check(
        g_memory.Read<uint32_t>(bink + 8) == 1 && g_memory.Read<uint32_t>(bink + 12) == 1, "empty header is terminal");
    Check(Black() && d3d::FailedBinkMoviePlaying(), "failed header requests black on the GPU path too");
    const uint32_t other = g_virtualHeap4K.Alloc(0x1000);
    g_memory.Write<uint32_t>(other, 2);
    g_memory.Write<uint32_t>(other + 4, 2);
    g_memory.Write<uint32_t>(other + 8, 10);
    g_memory.Write<uint32_t>(other + 12, 1);
    Open(other, path);
    Call(GAME_HOOK(BinkClose), other);
    Check(d3d::FailedBinkMoviePlaying(), "closing another movie does not clear a pending failure");
    Call(GAME_HOOK(BinkClose), bink);
    Check(g_openMovies == 0 && !d3d::FailedBinkMoviePlaying(), "closing restores normal presentation");

    header(0, 2);
    Open(bink, path);
    Check(d3d::FailedBinkMoviePlaying(), "invalid dimensions request black");
    Call(GAME_HOOK(BinkClose), bink);

    header();
    g_memory.Write<uint32_t>(bink + 0x100, buffers);
    g_memory.Write<uint32_t>(buffers, 1);
    g_memory.Write<uint32_t>(buffers + 4, 2);
    g_memory.Write<uint32_t>(buffers + 8, 2);
    for (uint32_t i = 0; i < 3; i++)
    {
        g_memory.Write<uint32_t>(buffers + 24 + i * 12 + 4, planes + i * 4);
        g_memory.Write<uint32_t>(buffers + 24 + i * 12 + 8, i == 0 ? 2 : 1);
    }
    Open(bink, path);
    Call(GAME_HOOK(BinkDoFrame), bink);
    Check(Black() && g_width == 1 && g_height == 1, "zero YUV becomes an opaque black fallback");
    Check(g_memory.Read<uint32_t>(bink + 8) == 1, "zero YUV ends the movie");
    Call(GAME_HOOK(BinkClose), bink);

    // A legitimate decoded black frame is not a failed movie.
    header();
    memset(g_memory.Translate(planes), 16, 4);
    memset(g_memory.Translate(planes + 4), 128, 8);
    Open(bink, path);
    Call(GAME_HOOK(BinkDoFrame), bink);
    Check(Black() && g_width == 2 && g_height == 2, "decoded black retains its dimensions");
    Check(g_memory.Read<uint32_t>(bink + 8) == 10 && !d3d::FailedBinkMoviePlaying(), "decoded black keeps playing");
    g_rendering = true;
    memset(g_memory.Translate(planes), 0, 12);
    Call(GAME_HOOK(BinkDoFrame), bink);
    Check(g_memory.Read<uint32_t>(bink + 8) == 10, "host fallback leaves GPU-decoded frames alone");
    g_rendering = false;
    g_memory.Write<uint32_t>(buffers + 28, 0);
    Call(GAME_HOOK(BinkDoFrame), bink);
    Check(Black() && g_memory.Read<uint32_t>(bink + 8) == 1, "missing luma plane is black and terminal");
    g_memory.Write<uint32_t>(buffers + 28, planes);
    header();
    g_memory.Write<uint32_t>(buffers + 20, UINT32_MAX);
    Call(GAME_HOOK(BinkDoFrame), bink);
    Check(Black() && g_memory.Read<uint32_t>(bink + 8) == 1, "bad frame index is black and terminal");
    Call(GAME_HOOK(BinkClose), bink);

    // A cached layout must be checked again when the handle is reused.
    header();
    g_memory.Write<uint32_t>(bink + 0x100, 0);
    Open(bink, path);
    Call(GAME_HOOK(BinkDoFrame), bink);
    Check(Black() && g_memory.Read<uint32_t>(bink + 8) == 1, "missing frame buffers are black and terminal");
    Call(GAME_HOOK(BinkClose), bink);
    Check(g_openMovies == 0 && !d3d::FailedBinkMoviePlaying(), "no failed lifetime leaks into later scenes");

    if (g_failures == 0)
        printf("ArkeyanBinkDisplayTest: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
