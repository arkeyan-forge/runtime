// CPU-only overlay smoke test: synthetic metrics, the real fonts and software rasterizer, no game or GPU device.
#include "host/gamemenu.h"
#include "host/embedded_fonts.h"
#include "host/imgui_software.h"
#include "gpu/gpu.h"
#include "d3d/render_stats.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
    gamemenu::Values g_values;
    bool g_french = false;
    unsigned g_snapshots = 0, g_renderReads = 0, g_levelReads = 0;
    size_t g_allocations = 0;
    int g_checks = 0, g_failures = 0;
    void Check(bool ok, int line)
    {
        g_checks++;
        if (!ok)
        {
            g_failures++;
            fprintf(stderr, "FAIL line %d\n", line);
        }
    }
#define CHECK(test) Check((test), __LINE__)
}

void* operator new(size_t size)
{
    g_allocations++;
    if (void* p = malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { free(p); }
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { free(p); }

const gamemenu::Values& gamemenu::Current() { return g_values; }
const char* LogBuild() { return "test-build"; }
const char* SystemOsName() { return "Test OS"; }
const char* SystemCpuName() { return "Test CPU @ 3.0 GHz"; }
namespace toybox
{
    bool French() { return g_french; }
}
namespace game
{
    void CopyCurrentLevel(char* text, size_t capacity)
    {
        g_levelReads++;
        snprintf(text, capacity, "test_level");
    }
}
bool FrameRateFallbackActive() { return true; }
bool HostAudioActive() { return true; }
uint32_t HostAudioVolume() { return 80; }
uint32_t HostAudioQueuedFrames() { return 3; }
bool PortalIsConnected() { return true; }
bool PortalIsEmulated() { return true; }
uint64_t gpu::FramesPresented() { return 120; }
gpu::DebugSnapshot gpu::ReadDebugSnapshot()
{
    g_snapshots++;
    DebugSnapshot snapshot;
    snprintf(snapshot.adapter, sizeof(snapshot.adapter), "Test GPU");
    snprintf(snapshot.backend, sizeof(snapshot.backend), "Test backend");
    snprintf(snapshot.driver, sizeof(snapshot.driver), "1.2.3");
    snapshot.width = 1920;
    snapshot.height = 1080;
    snapshot.scale = 2;
    snapshot.frameCount = 120;
    snapshot.frameNs.fill(16'666'667);
    snapshot.frameNs[119] = 80'000'000;
    snapshot.shaders = 1234;
    snapshot.pipelinesReady = 234;
    snapshot.drawTimeBuilds = 2;
    snapshot.missingShaders = 1;
    snapshot.videoUsed = 3ull << 29;
    snapshot.videoBudget = 4ull << 30;
    snapshot.available = snapshot.videoUsageKnown = snapshot.videoBudgetKnown = true;
    return snapshot;
}
d3d::RenderStats d3d::ReadRenderStats()
{
    g_renderReads++;
    return { 123, 12 };
}

int main(int argc, char** argv)
{
    // Warm the SDL timer outside the measured path. InitDebugScreen does no per-frame identity work.
    SDL_GetTicksNS();
    gamemenu::InitDebugScreen("Test game", "1.2.3");
    for (int i = 0; i < 100; i++)
        CHECK(!gamemenu::UpdateDebugScreen());
    CHECK(g_snapshots == 0 && g_renderReads == 0 && g_levelReads == 0);
    g_values.debugScreen = true;
    g_values.fpsLimit = 60;
    size_t before = g_allocations;
    CHECK(gamemenu::UpdateDebugScreen());
    CHECK(g_allocations == before);
    CHECK(g_snapshots == 1 && g_renderReads == 1 && g_levelReads == 1);
    CHECK(!gamemenu::UpdateDebugScreen());
    CHECK(g_snapshots == 1); // no resampling inside the half-second refresh interval

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 30;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    ImFont* font = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(g_embeddedOutfitBold), int(g_embeddedOutfitBoldSize), 16, &config);
    imgui_software::Init();
    imgui_software::Texture image{ 1280, 720 };
    image.pixels.resize(1280 * 720);
    for (int language = 0; language < 2; language++)
    {
        g_values.debugScreen = false;
        CHECK(gamemenu::UpdateDebugScreen());
        g_french = language != 0;
        g_values.debugScreen = true;
        CHECK(gamemenu::UpdateDebugScreen());
        ImGui::NewFrame();
        gamemenu::DrawDebugScreen(font);
        ImGui::Render();
        std::fill(image.pixels.begin(), image.pixels.end(), 0);
        imgui_software::Render(ImGui::GetDrawData(), image);
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 100);
        size_t left = 0, right = 0, centre = 0;
        for (int y = 0; y < image.height; y++)
        {
            for (int x = 0; x < image.width; x++)
            {
                uint32_t pixel = image.pixels[size_t(y) * image.width + x];
                if (x < 500)
                    left += pixel != 0;
                else if (x > 780)
                    right += pixel != 0;
                else
                    centre += pixel != 0; // clear gameplay centre
            }
        }
        CHECK(left > 10000 && right > 10000 && centre == 0);
        if (argc > 1)
        {
            char filename[512];
            snprintf(filename, sizeof(filename), "%s-%s.rgba", argv[1], language ? "fr" : "en");
            FILE* file = fopen(filename, "wb");
            CHECK(file != nullptr);
            if (file)
            {
                CHECK(fwrite(image.pixels.data(), sizeof(uint32_t), image.pixels.size(), file) == image.pixels.size());
                fclose(file);
            }
        }
    }
    g_values.debugScreen = false;
    CHECK(gamemenu::UpdateDebugScreen());
    unsigned reads = g_snapshots;
    before = g_allocations;
    for (int i = 0; i < 100; i++)
        CHECK(!gamemenu::UpdateDebugScreen());
    CHECK(g_snapshots == reads && g_allocations == before);
    ImGui::NewFrame();
    gamemenu::DrawDebugScreen(font);
    ImGui::Render();
    CHECK(ImGui::GetDrawData()->TotalVtxCount == 0);
    ImGui::DestroyContext();
    printf("debug screen: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
