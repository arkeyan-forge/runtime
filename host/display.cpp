#include "host/display.h"
#include "gpu/gpu.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    std::mutex g_mutex;
    std::vector<uint32_t> g_pixels;
    int g_width;
    int g_height;
    bool g_fresh;

    bool g_gpuReady;
    std::atomic<bool> g_gameRendering;
    gpu::Texture* g_texture;
    int g_textureWidth;
    int g_textureHeight;
}

static void WritePpm(const std::string& path, const std::vector<uint32_t>& rgba, int width, int height)
{
    if (FILE* file = fopen(path.c_str(), "wb"))
    {
        fprintf(file, "P6\n%d %d\n255\n", width, height);
        for (uint32_t pixel : rgba)
        {
            uint8_t rgb[3] = { uint8_t(pixel), uint8_t(pixel >> 8), uint8_t(pixel >> 16) };
            fwrite(rgb, 1, 3, file);
        }
        fclose(file);
    }
}

// SSA_SNAPSHOT=<prefix> saves every 100th frame as <prefix>_<n>.ppm (up to 20), to check output without a window.
static void Snapshot(const std::vector<uint32_t>& rgba, int width, int height)
{
    static const char* prefix = getenv("SSA_SNAPSHOT");
    static int frame;
    if (prefix == nullptr || frame++ % 100 != 0 || frame > 2000)
        return;
    WritePpm(std::string(prefix) + "_" + std::to_string(frame / 100) + ".ppm", rgba, width, height);
}

// SSA_GPU_SNAPSHOT=<prefix> saves every 300th presented image (SSA_GPU_SNAPSHOT_EVERY=<n>: every nth; starting at
// frame SSA_GPU_SNAPSHOT_FROM=<n>, default 0) for 36000 frames, read back from the GPU. Offscreen output only
// (SDL_VIDEO_DRIVER=offscreen), as swapchain images cannot be read.
// SSA_GPU_SNAPSHOT_SECONDS=<s>,<s>,... also saves the first image presented at or after each of these seconds since
// the start, as <prefix>_t<s>.ppm: the same moments at any frame rate (the game speed check, docs/engines/lux/frame_pacing.md).
void SnapshotPresentedFrame()
{
    static const char* prefix = getenv("SSA_GPU_SNAPSHOT");
    static std::vector<double> seconds = [] {
        std::vector<double> list;
        if (const char* text = getenv("SSA_GPU_SNAPSHOT_SECONDS"))
            for (const char* at = text; *at != '\0';)
            {
                char* end;
                double value = strtod(at, &end);
                if (end == at)
                    break;
                list.push_back(value);
                at = *end == ',' ? end + 1 : end;
            }
        return list;
    }();
    static const auto start = std::chrono::steady_clock::now();
    static size_t nextSecond = 0;
    if (prefix != nullptr && nextSecond < seconds.size()
        && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= seconds[nextSecond])
    {
        std::vector<uint32_t> rgba;
        uint32_t width, height;
        if (gpu::ReadbackOutput(rgba, width, height))
        {
            char name[32];
            snprintf(name, sizeof(name), "_t%g.ppm", seconds[nextSecond]);
            WritePpm(std::string(prefix) + name, rgba, int(width), int(height));
        }
        nextSecond++;
    }
    static const int every = std::max(getenv("SSA_GPU_SNAPSHOT_EVERY") != nullptr ? atoi(getenv("SSA_GPU_SNAPSHOT_EVERY")) : 300, 1);
    static const int from = getenv("SSA_GPU_SNAPSHOT_FROM") != nullptr ? atoi(getenv("SSA_GPU_SNAPSHOT_FROM")) : 0;
    static int frame = -from;
    if (prefix == nullptr || frame < 0 || frame++ % every != 0 || frame > 36000)
    {
        frame += frame < 0;
        return;
    }
    std::vector<uint32_t> rgba;
    uint32_t width, height;
    if (gpu::ReadbackOutput(rgba, width, height))
        WritePpm(std::string(prefix) + "_" + std::to_string(frame / every) + ".ppm", rgba, int(width), int(height));
}

void SubmitDisplayFrame(std::vector<uint32_t> rgba, int width, int height)
{
    Snapshot(rgba, width, height);
    std::lock_guard lock(g_mutex);
    g_pixels = std::move(rgba);
    g_width = width;
    g_height = height;
    g_fresh = true;
}

SDL_Window* CreateDisplayWindow(const char* title, int width, int height, bool fullscreen)
{
    // SSA_WINDOW_SIZE=<width>x<height>: the window at that size (test runs on a display of another shape, e.g.
    // 2560x1080 offscreen); the settings decide the normal size.
    if (const char* size = getenv("SSA_WINDOW_SIZE"))
        if (int w = 0, h = 0; sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
        {
            width = w;
            height = h;
        }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    SDL_Window* window = SDL_CreateWindow(title, width, height, flags | gpu::WindowFlags());
    if (window == nullptr)
    {
        // SDL's offscreen driver has no Metal view: the GPU then renders to an offscreen target. Without a graphics
        // flag or this property, SDL would load OpenGL for the window, which that driver lacks on macOS.
        SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
        SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, Sint64(flags));
        SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
        window = SDL_CreateWindowWithProperties(properties);
        SDL_DestroyProperties(properties);
    }
    g_gpuReady = window != nullptr && gpu::Init(window);
    return window;
}

void SetGameRendering(bool active)
{
    g_gameRendering = active;
}

bool IsGameRendering()
{
    return g_gameRendering;
}

void PresentDisplayFrame()
{
    if (!g_gpuReady || g_gameRendering)
    {
        SDL_Delay(16);
        return;
    }

    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
    {
        std::lock_guard lock(g_mutex);
        if (g_fresh)
        {
            pixels.swap(g_pixels);
            width = g_width;
            height = g_height;
            g_fresh = false;
        }
    }

    if (!pixels.empty())
    {
        if (g_texture == nullptr || g_textureWidth != width || g_textureHeight != height)
        {
            gpu::DestroyTexture(g_texture);
            g_texture = gpu::CreateTexture({ .width = uint32_t(width), .height = uint32_t(height), .format = gpu::Format::R8G8B8A8_UNORM });
            g_textureWidth = width;
            g_textureHeight = height;
        }
        gpu::UploadTexture(g_texture, pixels.data(), uint32_t(width) * 4);
    }
    gpu::Present(g_texture);
    SnapshotPresentedFrame();
}

void ServiceGamePresents(uint32_t timeoutMs)
{
    if (!g_gpuReady)
    {
        SDL_Delay(timeoutMs);
        return;
    }
    gpu::ServicePresents(timeoutMs);
}
