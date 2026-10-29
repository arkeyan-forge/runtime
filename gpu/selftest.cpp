// SSARecompGpuTest [output directory]: draws test images through the present pass into an offscreen target (no
// window), reads the result back and compares it with a CPU reference: bilinear, clamped sampling inside the
// letterboxed rectangle, black elsewhere. Writes <dir>/gpu_<case>_{actual,expected}.ppm when given a directory.
// Then two producer threads at once ("producers"). Exit code 0 when every case matches.
#include "gpu/buffer_cache.h"
#include "gpu/gpu.h"
#include <algorithm>
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

int main(int argc, char** argv)
{
    const char* directory = argc > 1 ? argv[1] : nullptr;
    bool pass = true;
    // 4:3 movie in a 16:9 output (pillarbox), upscaled; the second upload must replace the first.
    pass &= RunCase("pillarbox", 320, 180, { Pattern(64, 48, 0), Pattern(64, 48, 1) }, directory);
    // Wide image in a tall output (letterbox), downscaled.
    pass &= RunCase("letterbox", 150, 260, { Pattern(200, 50, 0) }, directory);
    // Same aspect ratio, odd sizes (row pitch not a multiple of the upload alignment).
    pass &= RunCase("fit", 333, 111, { Pattern(99, 33, 1) }, directory);
    pass &= RunProducers();
    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
