// CPU-only overlay smoke test: synthetic metrics, the real fonts, software rasterizer and frame_stats ring; no game or
// GPU device. Compact and Full screens in English and French, the clear gameplay area, collection stopping when off,
// no C++ heap allocation in collection, the text for the clipboard, and the performance log's lines, end and size limit.
#include "host/gamemenu.h"
#include "host/debug_screen.h"
#include "host/embedded_fonts.h"
#include "host/imgui_software.h"
#include "host/machine_state.h"
#include "gpu/frame_stats.h"
#include "gpu/gpu.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

namespace
{
    gamemenu::Values g_values;
    bool g_french = false;
    unsigned g_snapshots = 0, g_levelReads = 0, g_cpuReads = 0;
    uint64_t g_presented = 120;
    bool g_rendererBusy = false; // ReadDebugSnapshot without waiting fails, as while the GPU thread holds the renderer
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

    // The value of the panel row labelled `label` ("" when there is none).
    std::string_view Value(const debugscreen::Panel& panel, std::string_view label)
    {
        for (size_t i = 0; i < panel.count; i++)
            if (label == panel.rows[i].label)
                return panel.rows[i].value;
        return {};
    }
    bool Has(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

    // Frames start..start+count through the real ring: 2 ms of renderer, 3 ms of game-thread CPU, 4 ms of GPU, 1 ms of
    // vblank wait and 0.5 ms of present, 7 target changes, 16.7 ms apart; frame `slow` lasts 80 ms and built a pipeline.
    uint64_t g_renderer = 0, g_gameCpu = 0, g_vblank = 0, g_builds = 0, g_switches = 0, g_clock = 1'000'000'000;
    void Frames(uint64_t start, uint64_t count, uint64_t slow)
    {
        namespace stats = gpu::framestats;
        for (uint64_t frame = start; frame < start + count; frame++)
        {
            stats::GuestFrame(400, 12);
            g_renderer += 2'000'000;
            g_gameCpu += 3'000'000;
            g_vblank += 1'000'000;
            stats::ProducerFrame(frame, { g_renderer, frame * 3, frame << 20, frame, g_vblank, g_gameCpu });
            uint64_t interval = frame == slow ? 80'000'000 : 16'666'667;
            g_clock += interval;
            g_builds += frame == slow;
            g_switches += 7;
            stats::GpuSpan(frame, 100, 2'000'100, true, false);
            stats::GpuSpan(frame, 1'000'100, 4'000'100, true, true); // overlaps the first: 4 ms busy in all
            stats::Presented(frame, g_clock, interval, { 500'000, g_builds, g_switches });
        }
    }
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
        snprintf(text, capacity, "area\\test_level");
    }
    uint32_t Accent() { return 0xDE0018; } // Trap Team's red: made readable
}
bool FrameRateFallbackActive() { return true; }
bool HostAudioActive() { return true; }
uint32_t HostAudioVolume() { return 80; }
uint32_t HostAudioQueuedFrames() { return 3; }
bool PortalIsConnected() { return true; }
bool PortalIsEmulated() { return true; }
// Half a core for the process and each kind of thread since the refresh before (the clock is the real one); the
// audio thread unknown, the texture workers two threads.
void machine::ReadCpu(Cpu& cpu)
{
    g_cpuReads++;
    uint64_t half = SDL_GetTicksNS() / 2;
    cpu = {};
    cpu.processKnown = true;
    cpu.processNs = half;
    for (int i = 0; i < ThreadCount; i++)
    {
        cpu.threadKnown[i] = i != 5;
        cpu.threadNs[i] = i == 3 ? 2 * half : half;
        cpu.threads[i] = i == 3 ? 2 : 1;
    }
}
// A quarter of the machine busy since the start, 8 of 32 GiB in use.
machine::System machine::ReadSystem()
{
    uint64_t ticks = SDL_GetTicksNS() / 1000;
    return { true, true, ticks / 4, ticks, 8ull << 30, 32ull << 30 };
}
void machine::ReadRuntime(Runtime& runtime)
{
    runtime = {};
    runtime.guestThreads = 21;
    runtime.heapUsed = 300ull << 20;
    runtime.heapSize = 512ull << 20;
    runtime.texturesKnown = true;
    runtime.textures = 812;
    runtime.voices = 3;
    snprintf(runtime.input, sizeof(runtime.input), "keyboard and mouse");
    snprintf(runtime.glyphs, sizeof(runtime.glyphs), "keyboard");
    runtime.seatToys[0] = 14;
    runtime.seats = 1;
}
machine::Heat machine::ReadHeat() { return { Thermal::Moderate, -1000 }; }
machine::Power machine::ReadPower() { return { true, true, 54 }; }
uint64_t gpu::FramesPresented() { return g_presented; }
bool gpu::ReadDebugSnapshot(DebugSnapshot& snapshot, bool wait)
{
    if (g_rendererBusy && !wait)
        return false;
    g_snapshots++;
    snapshot = {};
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
    return true;
}

namespace
{
    gamemenu::DebugFonts g_fonts;

    // Draws the screen as it stands: both columns drawn, nothing over the gameplay area (the centre, above the
    // bottom boxes and the key hints).
    void DrawAndCheck(imgui_software::Texture& image, const char* name, int argc, char** argv)
    {
        ImGui::NewFrame();
        gamemenu::DrawDebugScreen(g_fonts);
        ImGui::Render();
        std::fill(image.pixels.begin(), image.pixels.end(), 0);
        imgui_software::Render(ImGui::GetDrawData(), image);
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 100);
        size_t left = 0, right = 0, centre = 0;
        for (int y = 0; y < image.height; y++)
            for (int x = 0; x < image.width; x++)
            {
                uint32_t pixel = image.pixels[size_t(y) * image.width + x];
                if (x < 500)
                    left += pixel != 0;
                else if (x > 780)
                    right += pixel != 0;
                else if (y < 560)
                    centre += pixel != 0;
            }
        CHECK(left > 10000 && right > 5000 && centre == 0);
        if (argc > 1)
        {
            char filename[512];
            snprintf(filename, sizeof(filename), "%s-%s.rgba", argv[1], name);
            FILE* file = fopen(filename, "wb");
            CHECK(file != nullptr);
            if (file)
            {
                CHECK(fwrite(image.pixels.data(), sizeof(uint32_t), image.pixels.size(), file) == image.pixels.size());
                fclose(file);
            }
        }
    }
}

int main(int argc, char** argv)
{
    using namespace debugscreen;
    // Warm the SDL timer outside the measured path. InitDebugScreen does no per-frame identity work.
    SDL_GetTicksNS();
    gamemenu::InitDebugScreen("Test game", "1.2.3", nullptr);
    for (int i = 0; i < 100; i++)
        CHECK(!gamemenu::UpdateDebugScreen());
    CHECK(g_snapshots == 0 && g_levelReads == 0 && g_cpuReads == 0 && !gpu::framestats::Enabled());
    CHECK(gamemenu::DebugScreenText().empty());
    g_values.debugScreen = Compact;
    g_values.fpsLimit = 60;
    g_values.vsync = true;
    size_t before = g_allocations;
    CHECK(gamemenu::UpdateDebugScreen());
    CHECK(g_allocations == before);
    CHECK(g_snapshots == 1 && g_levelReads == 1 && g_cpuReads == 0); // compact reads no CPU times
    CHECK(!gpu::framestats::Enabled()); // compact measures no frame
    const debugscreen::Screen& screen = gamemenu::DebugScreenState();
    CHECK(!screen.full && Value(screen.left, "level") == "test_level"); // the map's name, not the zone's path
    CHECK(Value(screen.left, "cap") == "60  drop to 30 active");
    CHECK(!gamemenu::UpdateDebugScreen());
    CHECK(g_snapshots == 1); // no resampling inside the half-second refresh interval
    // A busy renderer: the refresh waits for an update that finds it free, without blocking.
    SDL_Delay(510);
    g_rendererBusy = true;
    CHECK(!gamemenu::UpdateDebugScreen() && !gamemenu::UpdateDebugScreen());
    g_rendererBusy = false;
    CHECK(gamemenu::UpdateDebugScreen() && g_snapshots == 2);
    // Busy for a quarter of a second: it waits for the renderer rather than never refreshing.
    SDL_Delay(510);
    g_rendererBusy = true;
    CHECK(!gamemenu::UpdateDebugScreen());
    SDL_Delay(260);
    CHECK(gamemenu::UpdateDebugScreen() && g_snapshots == 3);
    g_rendererBusy = false;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 30;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    g_fonts.text = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(g_embeddedOutfitBold), int(g_embeddedOutfitBoldSize), 16, &config);
    g_fonts.heading = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(g_embeddedBungee), int(g_embeddedBungeeSize), 16, &config);
    g_fonts.mono = io.Fonts->AddFontDefaultBitmap();
    imgui_software::Init();
    imgui_software::Texture image{ 1280, 720 };
    image.pixels.resize(1280 * 720);
    for (int language = 0; language < 2; language++)
    {
        g_values.debugScreen = Off;
        CHECK(gamemenu::UpdateDebugScreen());
        g_french = language != 0;
        g_values.debugScreen = Compact;
        CHECK(gamemenu::UpdateDebugScreen());
        DrawAndCheck(image, language ? "compact-fr" : "compact-en", argc, argv);
    }

    // Full: the measurement starts, the frames go through the real ring, the screen carries what they measured.
    g_french = false;
    g_values.debugScreen = Full;
    CHECK(gamemenu::UpdateDebugScreen()); // a new level collects at once
    CHECK(gpu::framestats::Enabled());
    Frames(1, 200, 150);
    SDL_Delay(510); // the next refresh, with CPU use since this one
    before = g_allocations;
    g_presented += 30;
    CHECK(gamemenu::UpdateDebugScreen());
    CHECK(g_allocations == before);
    CHECK(screen.full && screen.graphCount == 199); // frame 1 began before the measurement
    CHECK(Has(Value(screen.left, "frame"), "cpu 3.00  gpu 4.00  wait 1.50"));
    CHECK(Value(screen.left, "cpu") == "game 3.00  host calls 2.00 ms");
    CHECK(Value(screen.left, "draws") == "400  resolves 12  targets 7");
    CHECK(Value(screen.left, "shaders") == "1234  missing 1  pipelines 234  built in 60 s 1");
    CHECK(Value(screen.left, "textures") == "812  uploads 3.0 (1.00 MB)  HD 0");
    CHECK(Value(screen.left, "hitches 50 ms+").starts_with("1, last 80 ms pipeline built, "));
    CHECK(Value(screen.left, "pacing") == "17.2 ms  p99 16.7  missed 1 in 60 s"); // 80 ms is over 1.5 x 16.7
    CHECK(Value(screen.left, "cap") == "60  vsync on  drop to 30 active");
    CHECK(Value(screen.left, "latency") == "n/a"); // no input read
    CHECK(Value(screen.left, "limited by") == "the frame limit"); // 60 FPS limit, 17 ms frames
    CHECK(Value(screen.left, "threads") == "21 running  waiting n/a");
    CHECK(Value(screen.left, "heap") == "300 MiB of 512 MiB  write faults 1.0");
    CHECK(Value(screen.left, "portal") == "emulated  toy 14");
    CHECK(Value(screen.left, "audio") == "48 kHz  voices 3  buffer 16 ms  underruns 0");
    CHECK(Value(screen.right, "power") == "battery 54 %  thermal moderate");
    CHECK(Value(screen.right, "GPU") == "Test GPU (Test backend)  busy 24 %");
    CHECK(Value(screen.right, "RAM").starts_with("game ") &&
          Has(Value(screen.right, "RAM"), "system 8.00 GiB of 32.00 GiB"));
    CHECK(Value(screen.right, "CPU use") == "process 50 %  system 25 %");
    CHECK(Value(screen.right, "display") == "n/a");
    CHECK(Value(screen.right, "build") == "test-build");
    for (size_t i = 0; i < screen.graphCount; i++)
        CHECK(screen.graphGpuMs[i] == 4.0f && screen.graphCpuMs[i] == 3.0f);
    CHECK(screen.worstMs == 80.0f);
    CHECK(std::abs(screen.threadPercent[0] - 50) < 2 && std::abs(screen.threadPercent[3] - 100) < 4 &&
          screen.threadCount[3] == 2);
    CHECK(screen.threadPercent[5] < 0 && screen.threadPercent[6] < 0); // audio unknown, streaming not measured
    std::string text = gamemenu::DebugScreenText();
    CHECK(
        text.starts_with("FRAME\nfps: ") && Has(text, "\nMACHINE\n") && Has(text, "worst frame of the last 199: 80.0"));
    DrawAndCheck(image, "full-en", argc, argv);
    CHECK(gamemenu::ToggleDebugGraphs() && gamemenu::ToggleDebugThreads()); // off
    DrawAndCheck(image, "full-text-en", argc, argv);
    CHECK(gamemenu::ToggleDebugGraphs() && gamemenu::ToggleDebugThreads()); // on again
    g_french = true;
    SDL_Delay(510);
    CHECK(gamemenu::UpdateDebugScreen());
    CHECK(Value(screen.right, "alimentation") == "batterie 54 %  thermique modéré");
    DrawAndCheck(image, "full-fr", argc, argv);
    g_french = false;

    // The performance log: a header, then one line per finished frame from the next one on, without allocating.
    FILE* log = tmpfile();
    CHECK(log != nullptr);
    CHECK(gamemenu::SwapPerfLog(log) == nullptr);
    Frames(201, 50, 220);
    before = g_allocations;
    gamemenu::UpdateDebugScreen();
    CHECK(g_allocations == before);
    CHECK(gamemenu::SwapPerfLog(nullptr) == log);
    rewind(log);
    char line[512];
    int lines = 0, hitches = 0;
    bool header = fgets(line, sizeof(line), log) != nullptr && std::string_view(line) == PerfLogHeader;
    CHECK(header);
    while (fgets(line, sizeof(line), log) != nullptr)
    {
        lines++;
        std::string_view row(line);
        hitches += row.ends_with(",pipeline\n");
        if (lines == 1) // frame 201: 16.667 ms, 2 ms renderer, 4 ms GPU, no latency
            CHECK(row.starts_with("0.0167,16.667,2.000,4.000,,400,12,3,1.000,1,0,0,\n"));
    }
    CHECK(lines == 50 && hitches == 1);
    fclose(log);

    // A timed log (F3+R) stops at its end time and hands its file back once.
    log = tmpfile();
    CHECK(gamemenu::SwapPerfLog(log) == nullptr);
    gamemenu::StopPerfLogAt(SDL_GetTicksNS() + 50'000'000);
    Frames(251, 10, 0);
    gamemenu::UpdateDebugScreen();
    CHECK(gamemenu::PerfLogStopped() == nullptr);
    SDL_Delay(60);
    gamemenu::UpdateDebugScreen();
    CHECK(gamemenu::PerfLogStopped() == log && gamemenu::SwapPerfLog(nullptr) == nullptr);
    fclose(log);

    // The log stops by itself at its size limit, and hands its file back once.
    log = tmpfile();
    CHECK(gamemenu::SwapPerfLog(log) == nullptr);
    uint64_t frame = 261;
    while (frame < 261 + 1'000'000)
    {
        Frames(frame, 500, 0);
        frame += 500;
        gamemenu::UpdateDebugScreen();
        if (FILE* stopped = gamemenu::PerfLogStopped())
        {
            CHECK(stopped == log);
            break;
        }
    }
    fseek(log, 0, SEEK_END);
    long size = ftell(log);
    CHECK(size > long(PerfLogMaxBytes) - 512 && size <= long(PerfLogMaxBytes));
    CHECK(gamemenu::PerfLogStopped() == nullptr && gamemenu::SwapPerfLog(nullptr) == nullptr);
    fclose(log);

    // Off: no more collection or measurement.
    g_values.debugScreen = Off;
    CHECK(gamemenu::UpdateDebugScreen());
    CHECK(!gpu::framestats::Enabled());
    unsigned reads = g_snapshots, cpuReads = g_cpuReads;
    before = g_allocations;
    for (int i = 0; i < 100; i++)
        CHECK(!gamemenu::UpdateDebugScreen());
    CHECK(g_snapshots == reads && g_cpuReads == cpuReads && g_allocations == before);
    ImGui::NewFrame();
    gamemenu::DrawDebugScreen(g_fonts);
    ImGui::Render();
    CHECK(ImGui::GetDrawData()->TotalVtxCount == 0);
    ImGui::DestroyContext();
    printf("debug screen: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
