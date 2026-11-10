// ArkeyanRecompGpuTest [output directory]: draws test images through the present pass into an offscreen target (no
// window), reads the result back and compares it with a CPU reference: bilinear, clamped sampling inside the
// letterboxed rectangle, black elsewhere. Writes <dir>/gpu_<case>_{actual,expected}.ppm when given a directory.
// Then two producer threads at once ("producers"), and FSR 3 through the running backend ("fsr"). Exit code 0 when every
// case matches.
#include "d3d/upscale_math.h"
#include "gpu/buffer_cache.h"
#include "gpu/draw.h"
#include "gpu/fsr/framegen.h"
#include "gpu/gpu.h"
#include "gpu/upscale.h"
#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Per channel, for the GPU's filtering and rounding precision.
    constexpr int Tolerance = 3;

    struct Image
    {
        uint32_t width;
        uint32_t height;
        std::vector<uint32_t> rgba;
    };

    Image Pattern(uint32_t width, uint32_t height, uint32_t seed)
    {
        Image image{ width, height, std::vector<uint32_t>(width * height) };
        for (uint32_t y = 0; y < height; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t r = x * 255 / (width - 1);
                uint32_t g = y * 255 / (height - 1);
                uint32_t b = ((x / 4 + y / 4 + seed) & 1) ? 255 : 0;
                image.rgba[y * width + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        }
        return image;
    }

    uint8_t Channel(uint32_t pixel, int channel) { return uint8_t(pixel >> (channel * 8)); }

    struct Rect
    {
        uint32_t left, top, width, height;
    };

    Image Expected(const Image& image, uint32_t outputWidth, uint32_t outputHeight, Rect& rect)
    {
        float scale = std::min(float(outputWidth) / image.width, float(outputHeight) / image.height);
        uint32_t width = std::min(outputWidth, uint32_t(std::lround(image.width * scale)));
        uint32_t height = std::min(outputHeight, uint32_t(std::lround(image.height * scale)));
        uint32_t left = (outputWidth - width) / 2;
        uint32_t top = (outputHeight - height) / 2;
        rect = { left, top, width, height };

        Image expected{ outputWidth, outputHeight, std::vector<uint32_t>(outputWidth * outputHeight) };
        auto texel = [&](int x, int y) {
            x = std::clamp(x, 0, int(image.width) - 1);
            y = std::clamp(y, 0, int(image.height) - 1);
            return image.rgba[y * image.width + x];
        };
        for (uint32_t y = 0; y < height; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                float u = (x + 0.5f) / width * image.width - 0.5f;
                float v = (y + 0.5f) / height * image.height - 0.5f;
                int x0 = int(std::floor(u)), y0 = int(std::floor(v));
                float fx = u - x0, fy = v - y0;
                uint32_t pixel = 0;
                for (int channel = 0; channel < 3; channel++)
                {
                    float top0 = Channel(texel(x0, y0), channel) * (1 - fx) + Channel(texel(x0 + 1, y0), channel) * fx;
                    float bottom =
                        Channel(texel(x0, y0 + 1), channel) * (1 - fx) + Channel(texel(x0 + 1, y0 + 1), channel) * fx;
                    pixel |= uint32_t(std::lround(top0 * (1 - fy) + bottom * fy)) << (channel * 8);
                }
                expected.rgba[(top + y) * outputWidth + left + x] = pixel;
            }
        }
        return expected;
    }

    void WritePpm(const std::string& path, const Image& image)
    {
        if (FILE* file = fopen(path.c_str(), "wb"))
        {
            fprintf(file, "P6\n%u %u\n255\n", image.width, image.height);
            for (uint32_t pixel : image.rgba)
            {
                uint8_t rgb[3] = { Channel(pixel, 0), Channel(pixel, 1), Channel(pixel, 2) };
                fwrite(rgb, 1, 3, file);
            }
            fclose(file);
        }
    }

    // Uploads each image in turn into one texture, presents it, and checks the output of the last.
    bool RunCase(const char* name, uint32_t outputWidth, uint32_t outputHeight, const std::vector<Image>& images,
        const char* directory)
    {
        if (!gpu::Init(nullptr, outputWidth, outputHeight))
            return false;

        const Image& last = images.back();
        gpu::Texture* texture =
            gpu::CreateTexture({ .width = last.width, .height = last.height, .format = gpu::Format::R8G8B8A8_UNORM });
        for (const Image& image : images)
        {
            gpu::UploadTexture(texture, image.rgba.data(), image.width * 4);
            gpu::Present(texture);
        }

        Image actual;
        bool readBack = gpu::ReadbackOutput(actual.rgba, actual.width, actual.height);
        gpu::DestroyTexture(texture);
        gpu::Shutdown();
        if (!readBack || actual.width != outputWidth || actual.height != outputHeight)
        {
            printf("%s: no readback\n", name);
            return false;
        }

        Rect rect;
        Image expected = Expected(last, outputWidth, outputHeight, rect);
        int maxError = 0;
        uint32_t mismatches = 0;
        for (uint32_t y = 0; y < outputHeight; y++)
        {
            for (uint32_t x = 0; x < outputWidth; x++)
            {
                uint32_t i = y * outputWidth + x;
                int error = 0;
                for (int channel = 0; channel < 3; channel++)
                    error = std::max(
                        error, std::abs(Channel(actual.rgba[i], channel) - Channel(expected.rgba[i], channel)));
                // The bars must be exactly black.
                bool inside = x - rect.left < rect.width && y - rect.top < rect.height;
                mismatches += error > (inside ? Tolerance : 0);
                maxError = std::max(error, maxError);
            }
        }

        if (directory != nullptr)
        {
            WritePpm(std::string(directory) + "/gpu_" + name + "_actual.ppm", actual);
            WritePpm(std::string(directory) + "/gpu_" + name + "_expected.ppm", expected);
        }
        printf("%s: %ux%u image in %ux%u output at (%u, %u) %ux%u, max channel error %d, %u mismatching pixels\n", name,
            last.width, last.height, outputWidth, outputHeight, rect.left, rect.top, rect.width, rect.height, maxError,
            mismatches);
        return mismatches == 0;
    }

    // Two producers at once, as before the game's first frame (docs/runtime/gpu/renderer_threads.md): the main thread
    // presents movie frames while the game's render thread draws, so gpu::Present ends frames (BufferCacheFrameDone)
    // during gpu::CacheVertices / CacheIndices. New ranges every call keep the cache staging (its vectors growing and
    // swapped). Passes when nothing crashes; a ThreadSanitizer build reports any unlocked access.
    bool RunProducers()
    {
        if (!gpu::Init(nullptr, 64, 64))
            return false;
        Image image = Pattern(16, 16, 0);
        gpu::Texture* texture =
            gpu::CreateTexture({ .width = image.width, .height = image.height, .format = gpu::Format::R8G8B8A8_UNORM });
        gpu::UploadTexture(texture, image.rgba.data(), image.width * 4);
        std::vector<uint8_t> data(4096, 0x5A);
        std::atomic<bool> stop = false;
        std::atomic<uint32_t> ranges = 0;
        std::thread drawer([&] {
            for (uint32_t address = 0x40000000; !stop; address += 0x1000)
            {
                gpu::CacheVertices(data.data(), address, 2048);
                uint32_t minIndex, maxIndex;
                gpu::CacheIndices(data.data(), address + 0x800, 1024, minIndex, maxIndex);
                ranges++;
            }
        });
        uint32_t presents = 0;
        for (; presents < 300; presents++)
        {
            gpu::Present(texture);
            // The lock is not fair: without a pause the drawer would wait for the end.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stop = true;
        drawer.join();
        gpu::DestroyTexture(texture);
        gpu::Shutdown();
        printf("producers: %u presents during %u vertex and index ranges\n", presents, ranges.load());
        return true;
    }
}

namespace
{
    // FSR 3 through the backend of the running GPU API (gpu/fsr/): a still, smooth scene "rendered" on the CPU at 96x48
    // with FSR's jitter (the picture each jitter gives), 64 frames, upscaled to 192x96. The output must be the scene at
    // the output's own pixels: a wrong binding, format or constant gives a far larger error.
    // `pan`: the scene moves by this many render pixels a frame (x right, y down), and `motion` gives FSR the camera's
    // reprojection for it (else the identity). Returns the mean channel error, or -1.
    // A float in [0, 1) as IEEE half (the HDR case's RGBA16F upload); normal numbers and 0 only.
    uint16_t Half(float value)
    {
        if (value <= 0.0f)
            return 0;
        const uint32_t bits = std::bit_cast<uint32_t>(value);
        const int32_t exponent = int32_t((bits >> 23) & 0xFF) - 127 + 15;
        return exponent <= 0 ? 0 : uint16_t(uint32_t(exponent) << 10 | (bits >> 13 & 0x3FF));
    }

    // `hdr`: the picture is RGBA16F (FSR's HDR mode and the sanitize pass), else RGBA8.
    double FsrError(const char* name, float panX, float panY, bool motion, const char* directory, bool hdr = false)
    {
        constexpr uint32_t Width = 96, Height = 48, OutputWidth = 192, OutputHeight = 96, Frames = 64;
        if (!gpu::Init(nullptr, OutputWidth, OutputHeight))
            return -1.0;
        if (!gpu::UpscalerAvailable(gpu::Upscaler::Fsr3))
        {
            gpu::Shutdown();
            printf("fsr: FSR 3 is not available on this backend\n");
            return -1.0;
        }
        // Periodic, so that a panned scene is defined everywhere.
        auto scene = [](float u, float v) {
            auto level = [](float t) {
                return uint32_t(std::lround((0.5f + 0.5f * std::sin(6.2831853f * t)) * 255.0f));
            };
            return level(u) | level(v + 0.25f) << 8 | level(1.5f * u + v) << 16 | 0xFF000000u;
        };
        gpu::Texture* color = gpu::CreateTexture({ .width = Width,
            .height = Height,
            .format = hdr ? gpu::Format::R16G16B16A16_FLOAT : gpu::Format::R8G8B8A8_UNORM });
        std::vector<uint16_t> halves(size_t(Width) * Height * 4);
        gpu::Texture* depth = gpu::CreateTexture({ .width = Width,
            .height = Height,
            .format = gpu::Format::D32_FLOAT_S8_UINT,
            .usage = gpu::TextureUsage::DepthStencil });
        gpu::Texture* output = gpu::CreateTexture({ .width = OutputWidth,
            .height = OutputHeight,
            .format = gpu::Format::R8G8B8A8_UNORM,
            .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTarget | gpu::TextureUsage::Storage });
        gpu::ClearDepthStencil(depth, true, false, 0.5f, 0);
        std::vector<uint32_t> picture(Width * Height);
        const uint32_t phases = upscale::JitterPhases(float(OutputWidth) / float(Width));
        for (uint32_t frame = 0; frame < Frames; frame++)
        {
            // The shaders move the geometry by the jitter (x right, y down): a pixel shows the scene at its centre
            // minus the jitter.
            const std::array<float, 2> jitter = upscale::Jitter(frame, phases);
            const float moveX = panX * float(frame), moveY = panY * float(frame);
            for (uint32_t y = 0; y < Height; y++)
                for (uint32_t x = 0; x < Width; x++)
                    picture[y * Width + x] = scene(
                        (float(x) + 0.5f - jitter[0] - moveX) / Width, (float(y) + 0.5f - jitter[1] - moveY) / Height);
            if (hdr)
            {
                for (size_t i = 0; i < picture.size(); i++)
                    for (int channel = 0; channel < 4; channel++)
                        halves[i * 4 + channel] = Half(Channel(picture[i], channel) / 255.0f);
                gpu::UploadTexture(color, halves.data(), Width * 8);
            }
            else
                gpu::UploadTexture(color, picture.data(), Width * 4);
            gpu::UpscaleDesc desc{ .upscaler = gpu::Upscaler::Fsr3,
                .color = color,
                .depth = depth,
                .output = output,
                .jitter = { jitter[0], jitter[1] },
                .reprojection = {},
                .cameraFound = true,
                .depthMin = 0.0f,
                .depthMax = 1.0f,
                .depthReversed = false,
                .reset = frame == 0,
                .sharpness = 0.0f,
                .cameraNear = 0.1f,
                .cameraFar = 1000.0f,
                .fovY = 1.0f };
            // A point now at clip (x, y) was, the frame before, panX pixels left and panY pixels lower on screen
            // (clip y is up).
            upscale::Matrix reprojection = upscale::Identity;
            if (motion)
                reprojection[3] = -2.0 * panX / Width, reprojection[7] = 2.0 * panY / Height;
            std::copy(reprojection.begin(), reprojection.end(), desc.reprojection);
            gpu::Upscale(desc);
            gpu::Present(output);
        }
        Image actual, input;
        const bool readBack = gpu::ReadbackTexture(output, actual.rgba, actual.width, actual.height);
        if (directory != nullptr && gpu::ReadbackTexture(color, input.rgba, input.width, input.height))
            WritePpm(std::string(directory) + "/gpu_" + name + "_input.ppm", input);
        gpu::DestroyTexture(color);
        gpu::DestroyTexture(depth);
        gpu::DestroyTexture(output);
        gpu::Shutdown();
        if (!readBack || actual.width != OutputWidth || actual.height != OutputHeight)
        {
            printf("%s: no readback\n", name);
            return -1.0;
        }
        Image expected{ OutputWidth, OutputHeight, std::vector<uint32_t>(OutputWidth * OutputHeight) };
        double sum = 0.0;
        int maxError = 0;
        uint32_t samples = 0;
        for (uint32_t y = 0; y < OutputHeight; y++)
            for (uint32_t x = 0; x < OutputWidth; x++)
            {
                const float lastX = panX * float(Frames - 1) / Width, lastY = panY * float(Frames - 1) / Height;
                const uint32_t want = scene((x + 0.5f) / OutputWidth - lastX, (y + 0.5f) / OutputHeight - lastY);
                expected.rgba[y * OutputWidth + x] = want;
                if (x < 4 || y < 4 || x >= OutputWidth - 4 || y >= OutputHeight - 4)
                    continue; // the edges: clamped sampling
                for (int channel = 0; channel < 3; channel++)
                {
                    const int error =
                        std::abs(Channel(actual.rgba[y * OutputWidth + x], channel) - Channel(want, channel));
                    sum += error;
                    maxError = std::max(maxError, error);
                    samples++;
                }
            }
        if (directory != nullptr)
        {
            WritePpm(std::string(directory) + "/gpu_" + name + "_actual.ppm", actual);
            WritePpm(std::string(directory) + "/gpu_" + name + "_expected.ppm", expected);
        }
        const double mean = sum / std::max(samples, 1u);
        printf("%s: %ux%u -> %ux%u, %u frames, pan %.2f, %.2f px%s, mean channel error %.2f, max %d\n", name, Width,
            Height, OutputWidth, OutputHeight, Frames, panX, panY, motion ? " with its motion" : "", mean, maxError);
        return mean;
    }

    // A still scene must come out as the scene; a panning one must too with its motion, and better than without it
    // (the motion's sign and units are right).
    bool RunFsr(const char* directory)
    {
        const double still = FsrError("fsr_still", 0.0f, 0.0f, false, directory);
        const double moving = FsrError("fsr_pan", 0.37f, 0.23f, true, directory);
        const double unmoved = FsrError("fsr_pan_no_motion", 0.37f, 0.23f, false, directory);
        const double hdr = FsrError("fsr_hdr_pan", 0.37f, 0.23f, true, directory, true);
        return still >= 0.0 && still < 3.0 && moving >= 0.0 && moving < 4.0 && unmoved > moving && hdr >= 0.0 &&
               hdr < 4.0;
    }
}

namespace
{
    // FSR 3's frame generation through the running backend (gpu/fsr/framegen.h): the panned scene of FsrError, upscaled
    // 480x270 -> 960x540 with its camera motion, a white box drawn after the composite copy as the HUD, 48 frames. Each
    // generated frame must be the scene halfway between the two real frames around it (nearer to it than to either real
    // frame), with the box where it is (the HUD path, not interpolated). `debugDir` keeps the last pair.
    bool RunFrameGeneration(const char* directory)
    {
        constexpr uint32_t Width = 480, Height = 270, OutputWidth = 960, OutputHeight = 540, Frames = 48;
        constexpr float PanX = 1.5f, PanY = 0.75f; // render pixels a frame
        constexpr int32_t BoxLeft = 40, BoxTop = 30, BoxRight = 200, BoxBottom = 90; // the HUD, output pixels
        gpu::g_settings.vsync = false;
        if (!gpu::Init(nullptr, OutputWidth, OutputHeight))
            return false;
        if (!gpu::UpscalerAvailable(gpu::Upscaler::Fsr3) || !gpu::framegen::Available())
        {
            gpu::Shutdown();
            printf("framegen: not available on this backend\n");
            return false;
        }
        auto scene = [](float u, float v) {
            auto level = [](float t) {
                return uint32_t(std::lround((0.5f + 0.5f * std::sin(6.2831853f * t)) * 255.0f));
            };
            return level(2.0f * u) | level(v + 0.25f) << 8 | level(1.5f * u + v) << 16 | 0xFF000000u;
        };
        // The last generated frame (index 40) and the real one after it, from the snapshot hook.
        struct Kept
        {
            Image generated, real;
            uint64_t index = 0;
        } kept;
        gpu::framegen::SetSnapshotHook(
            [&kept](const char* kind, uint64_t index, const std::vector<uint32_t>& rgba, uint32_t w, uint32_t h) {
                Image& image = std::string(kind) == "gen" ? kept.generated : kept.real;
                image = { w, h, rgba };
                kept.index = index;
            });
#if defined(_WIN32)
        _putenv_s("SKY_FSR_FG_SNAPSHOT_FROM", "40");
        _putenv_s("SKY_FSR_FG_SNAPSHOT_EVERY", "1000");
#else
        setenv("SKY_FSR_FG_SNAPSHOT_FROM", "40", 1);
        setenv("SKY_FSR_FG_SNAPSHOT_EVERY", "1000", 1);
#endif
        gpu::framegen::SetEnabled(true);
        gpu::Texture* color =
            gpu::CreateTexture({ .width = Width, .height = Height, .format = gpu::Format::R8G8B8A8_UNORM });
        gpu::Texture* depth = gpu::CreateTexture({ .width = Width,
            .height = Height,
            .format = gpu::Format::D32_FLOAT_S8_UINT,
            .usage = gpu::TextureUsage::DepthStencil });
        gpu::Texture* output = gpu::CreateTexture({ .width = OutputWidth,
            .height = OutputHeight,
            .format = gpu::Format::R8G8B8A8_UNORM,
            .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTarget | gpu::TextureUsage::Storage });
        gpu::ClearDepthStencil(depth, true, false, 0.5f, 0);
        std::vector<uint32_t> picture(Width * Height);
        const uint32_t phases = upscale::JitterPhases(float(OutputWidth) / float(Width));
        const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        const plume::RenderRect box(BoxLeft, BoxTop, BoxRight, BoxBottom);
        for (uint32_t frame = 0; frame < Frames; frame++)
        {
            const std::array<float, 2> jitter = upscale::Jitter(frame, phases);
            const float moveX = PanX * float(frame), moveY = PanY * float(frame);
            for (uint32_t y = 0; y < Height; y++)
                for (uint32_t x = 0; x < Width; x++)
                    picture[y * Width + x] = scene(
                        (float(x) + 0.5f - jitter[0] - moveX) / Width, (float(y) + 0.5f - jitter[1] - moveY) / Height);
            gpu::UploadTexture(color, picture.data(), Width * 4);
            gpu::UpscaleDesc desc{ .upscaler = gpu::Upscaler::Fsr3,
                .color = color,
                .depth = depth,
                .output = output,
                .jitter = { jitter[0], jitter[1] },
                .reprojection = {},
                .cameraFound = true,
                .depthMin = 0.0f,
                .depthMax = 1.0f,
                .depthReversed = false,
                .reset = frame == 0,
                .sharpness = 0.0f,
                .cameraNear = 0.1f,
                .cameraFar = 1000.0f,
                .fovY = 1.0f };
            upscale::Matrix reprojection = upscale::Identity;
            reprojection[3] = -2.0 * PanX / Width, reprojection[7] = 2.0 * PanY / Height;
            std::copy(reprojection.begin(), reprojection.end(), desc.reprojection);
            gpu::Upscale(desc);
            gpu::framegen::CaptureHudless(output); // the composite's picture, before the HUD
            gpu::ClearColor(output, white, &box);
            gpu::Present(output);
        }
        const gpu::framegen::Stats counts = gpu::framegen::Counts();
        gpu::framegen::SetEnabled(false);
        gpu::framegen::SetSnapshotHook(nullptr);
        gpu::DestroyTexture(color);
        gpu::DestroyTexture(depth);
        gpu::DestroyTexture(output);
        gpu::Shutdown();
        if (kept.generated.rgba.size() != size_t(OutputWidth) * OutputHeight ||
            kept.real.rgba.size() != kept.generated.rgba.size())
        {
            printf("framegen: no generated frame kept (%llu real, %llu generated, %llu reset)\n",
                (unsigned long long)counts.real, (unsigned long long)counts.generated,
                (unsigned long long)counts.reset);
            return false;
        }
        // Generated frame n (1-based) comes before real frame n: the scene at frame n - 0.5.
        const float at = float(kept.index) - 0.5f;
        auto errorTo = [&](const Image& image, float position, bool insideBox) {
            double sum = 0.0;
            uint32_t samples = 0;
            for (uint32_t y = 16; y < OutputHeight - 16; y++)
                for (uint32_t x = 16; x < OutputWidth - 16; x++)
                {
                    const bool inBox = int32_t(x) >= BoxLeft && int32_t(x) < BoxRight && int32_t(y) >= BoxTop &&
                                       int32_t(y) < BoxBottom;
                    // Keep a margin around the box: its edge is where HUD and scene meet.
                    const bool nearBox = int32_t(x) >= BoxLeft - 8 && int32_t(x) < BoxRight + 8 &&
                                         int32_t(y) >= BoxTop - 8 && int32_t(y) < BoxBottom + 8;
                    if (insideBox ? !inBox : nearBox)
                        continue;
                    const uint32_t want = insideBox ? 0xFFFFFFFFu
                                                    : scene((x + 0.5f) / OutputWidth - PanX * position / Width,
                                                          (y + 0.5f) / OutputHeight - PanY * position / Height);
                    for (int channel = 0; channel < 3; channel++)
                        sum += std::abs(Channel(image.rgba[y * OutputWidth + x], channel) - Channel(want, channel));
                    samples += 3;
                }
            return sum / std::max(samples, 1u);
        };
        const double middle = errorTo(kept.generated, at, false);
        const double before = errorTo(kept.generated, at - 0.5f, false);
        const double after = errorTo(kept.generated, at + 0.5f, false);
        const double hud = errorTo(kept.generated, at, true);
        const double real = errorTo(kept.real, at + 0.5f, false);
        if (directory != nullptr)
        {
            WritePpm(std::string(directory) + "/gpu_framegen_generated.ppm", kept.generated);
            WritePpm(std::string(directory) + "/gpu_framegen_real.ppm", kept.real);
        }
        printf("framegen: %ux%u -> %ux%u, %u frames: %llu generated, %llu reset; generated frame %llu: mean channel "
               "error %.2f to the halfway scene (%.2f to the frame before, %.2f to the frame after), %.2f in the HUD "
               "box; the real frame %.2f\n",
            Width, Height, OutputWidth, OutputHeight, Frames, (unsigned long long)counts.generated,
            (unsigned long long)counts.reset, (unsigned long long)kept.index, middle, before, after, hud, real);
        return counts.generated + 1 >= Frames - 1 && middle < 6.0 && middle < before && middle < after && hud < 2.0;
    }
}

namespace
{
    // ArkeyanRecompGpuTest --fsr-bench <render w> <render h> <output w> <output h>: the time per frame of FSR's work alone
    // (its inputs pass and passes, a still picture, 300 frames after 30), with and without it: a measure of its GPU cost.
    int BenchFsr(uint32_t width, uint32_t height, uint32_t outputWidth, uint32_t outputHeight)
    {
        gpu::g_settings.vsync = false;
        if (!gpu::Init(nullptr, outputWidth, outputHeight) || !gpu::UpscalerAvailable(gpu::Upscaler::Fsr3))
            return 1;
        gpu::Texture* color =
            gpu::CreateTexture({ .width = width, .height = height, .format = gpu::Format::R8G8B8A8_UNORM });
        gpu::Texture* depth = gpu::CreateTexture({ .width = width,
            .height = height,
            .format = gpu::Format::D32_FLOAT_S8_UINT,
            .usage = gpu::TextureUsage::DepthStencil });
        gpu::Texture* output = gpu::CreateTexture({ .width = outputWidth,
            .height = outputHeight,
            .format = gpu::Format::R8G8B8A8_UNORM,
            .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTarget | gpu::TextureUsage::Storage });
        std::vector<uint32_t> picture(size_t(width) * height, 0xFF808080u);
        gpu::UploadTexture(color, picture.data(), width * 4);
        gpu::ClearDepthStencil(depth, true, false, 0.5f, 0);
        double ms[2] = {};
        size_t wrong = 0;
        for (int pass = 0; pass < 2; pass++)
        {
            auto start = std::chrono::steady_clock::now();
            for (uint32_t frame = 0; frame < 330; frame++)
            {
                if (frame == 30)
                    start = std::chrono::steady_clock::now();
                if (pass == 0)
                {
                    gpu::UpscaleDesc desc{ .upscaler = gpu::Upscaler::Fsr3,
                        .color = color,
                        .depth = depth,
                        .output = output,
                        .jitter = { 0.0f, 0.0f },
                        .reprojection = {},
                        .cameraFound = true,
                        .depthMin = 0.0f,
                        .depthMax = 1.0f,
                        .depthReversed = false,
                        .reset = frame == 0,
                        .sharpness = 0.5f,
                        .cameraNear = 0.1f,
                        .cameraFar = 1000.0f,
                        .fovY = 1.0f };
                    std::copy(upscale::Identity.begin(), upscale::Identity.end(), desc.reprojection);
                    gpu::Upscale(desc);
                }
                gpu::Present(output);
            }
            ms[pass] =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 300.0;
            if (pass == 0)
            {
                // A still grey picture stays grey: a pixel far from it is a pass that read what it should not have.
                std::vector<uint32_t> rgba;
                uint32_t w = 0, h = 0;
                if (!gpu::ReadbackTexture(output, rgba, w, h))
                    wrong = ~size_t(0);
                for (const uint32_t pixel : rgba)
                    for (int shift = 0; shift < 24; shift += 8)
                        if (std::abs(int((pixel >> shift) & 0xFF) - 0x80) > 8)
                        {
                            wrong++;
                            break;
                        }
            }
        }
        gpu::DestroyTexture(color);
        gpu::DestroyTexture(depth);
        gpu::DestroyTexture(output);
        gpu::Shutdown();
        printf("fsr bench: %ux%u -> %ux%u: %.2f ms a frame with FSR, %.2f without: FSR %.2f ms; %zu pixels off grey\n",
            width, height, outputWidth, outputHeight, ms[0], ms[1], ms[0] - ms[1], wrong);
        return wrong == 0 ? 0 : 1;
    }
}

namespace
{
    // ArkeyanRecompGpuTest --framegen-bench <render w> <render h> <output w> <output h>: the time per frame of frame
    // generation's work (optical flow, interpolation, the extra present) over FSR's upscale alone, with
    // SKY_FSR_FG_NO_PACING=1 (no wait between the two presents): its GPU cost.
    int BenchFrameGeneration(uint32_t width, uint32_t height, uint32_t outputWidth, uint32_t outputHeight)
    {
        gpu::g_settings.vsync = false;
        if (!gpu::Init(nullptr, outputWidth, outputHeight) || !gpu::UpscalerAvailable(gpu::Upscaler::Fsr3) ||
            !gpu::framegen::Available())
            return 1;
        gpu::Texture* color =
            gpu::CreateTexture({ .width = width, .height = height, .format = gpu::Format::R8G8B8A8_UNORM });
        gpu::Texture* depth = gpu::CreateTexture({ .width = width,
            .height = height,
            .format = gpu::Format::D32_FLOAT_S8_UINT,
            .usage = gpu::TextureUsage::DepthStencil });
        gpu::Texture* output = gpu::CreateTexture({ .width = outputWidth,
            .height = outputHeight,
            .format = gpu::Format::R8G8B8A8_UNORM,
            .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTarget | gpu::TextureUsage::Storage });
        std::vector<uint32_t> picture(size_t(width) * height, 0xFF808080u);
        gpu::UploadTexture(color, picture.data(), width * 4);
        gpu::ClearDepthStencil(depth, true, false, 0.5f, 0);
        double ms[2] = {};
        uint64_t generated = 0;
        for (int pass = 0; pass < 2; pass++)
        {
            gpu::framegen::SetEnabled(pass == 0);
            auto start = std::chrono::steady_clock::now();
            for (uint32_t frame = 0; frame < 330; frame++)
            {
                if (frame == 30)
                    start = std::chrono::steady_clock::now();
                gpu::UpscaleDesc desc{ .upscaler = gpu::Upscaler::Fsr3,
                    .color = color,
                    .depth = depth,
                    .output = output,
                    .jitter = { 0.0f, 0.0f },
                    .reprojection = {},
                    .cameraFound = true,
                    .depthMin = 0.0f,
                    .depthMax = 1.0f,
                    .depthReversed = false,
                    .reset = frame == 0,
                    .sharpness = 0.5f,
                    .cameraNear = 0.1f,
                    .cameraFar = 1000.0f,
                    .fovY = 1.0f };
                std::copy(upscale::Identity.begin(), upscale::Identity.end(), desc.reprojection);
                gpu::Upscale(desc);
                gpu::framegen::CaptureHudless(output);
                gpu::Present(output);
            }
            ms[pass] =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 300.0;
            if (pass == 0)
                generated = gpu::framegen::Counts().generated;
        }
        gpu::DestroyTexture(color);
        gpu::DestroyTexture(depth);
        gpu::DestroyTexture(output);
        gpu::Shutdown();
        printf("framegen bench: %ux%u -> %ux%u: %.2f ms a frame with frame generation (%llu generated), %.2f with FSR "
               "alone: frame generation %.2f ms\n",
            width, height, outputWidth, outputHeight, ms[0], (unsigned long long)generated, ms[1], ms[0] - ms[1]);
        return generated >= 300 ? 0 : 1;
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // every result line reaches the log, even if the process dies later
    if (argc == 6 && std::string(argv[1]) == "--framegen-bench")
        return BenchFrameGeneration(
            uint32_t(atoi(argv[2])), uint32_t(atoi(argv[3])), uint32_t(atoi(argv[4])), uint32_t(atoi(argv[5])));
    if (argc == 6 && std::string(argv[1]) == "--fsr-bench")
        return BenchFsr(
            uint32_t(atoi(argv[2])), uint32_t(atoi(argv[3])), uint32_t(atoi(argv[4])), uint32_t(atoi(argv[5])));
    const char* directory = argc > 1 ? argv[1] : nullptr;
    bool pass = true;
    // 4:3 movie in a 16:9 output (pillarbox), upscaled; the second upload must replace the first.
    pass &= RunCase("pillarbox", 320, 180, { Pattern(64, 48, 0), Pattern(64, 48, 1) }, directory);
    // Wide image in a tall output (letterbox), downscaled.
    pass &= RunCase("letterbox", 150, 260, { Pattern(200, 50, 0) }, directory);
    // Same aspect ratio, odd sizes (row pitch not a multiple of the upload alignment).
    pass &= RunCase("fit", 333, 111, { Pattern(99, 33, 1) }, directory);
    pass &= RunProducers();
    pass &= RunFsr(directory);
    pass &= RunFrameGeneration(directory);
    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
