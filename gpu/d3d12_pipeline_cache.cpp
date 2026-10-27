// D3D12 pipeline cache (Windows and Xbox): building a pipeline from DXIL takes ~12 ms on the console and the game lists
// ~1600, so they are kept in an ID3D12PipelineLibrary saved next to pipelines.bin (pipelines.d3d12) and loaded by the
// next run. plume loads a pipeline from the library by its cache key, or builds and stores it (patches/plume.patch).
// The file starts with our header (the shader cache it was built with); D3D12 itself rejects a library from another
// driver or adapter, and a stored pipeline whose description changed is not loaded.
#include "gpu/context.h"
#include <plume_d3d12.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
    constexpr uint32_t Magic = 0x4C505353; // "SSPL"
    constexpr uint32_t Version = 1;

    struct Header
    {
        uint32_t magic, version;
        uint64_t identity; // of the shader cache (and so of the shaders the pipelines use)
    };

    std::filesystem::path g_path;
    uint64_t g_identity = 0;
    std::vector<char> g_blob; // the library reads from it for its whole life
    std::mutex g_saveMutex;

    plume::D3D12Device& Device(plume::RenderDevice& device)
    {
        return static_cast<plume::D3D12Device&>(device);
    }
}

namespace gpu::detail
{
    void OpenPipelineLibrary(plume::RenderDevice& renderDevice, const std::filesystem::path& path, uint64_t identity);

    // Opening the library removed the device on the Xbox (run 8, 1.0.2488.831: with a saved file or empty). A marker
    // file is written before opening and removed after: a marker left by an earlier run means that run did not get
    // past it, so the cache is skipped from then on (its file renamed .failed) until the marker is deleted.
    void D3D12OpenPipelineLibrary(plume::RenderDevice& renderDevice, const std::filesystem::path& path, uint64_t identity)
    {
        if (!gpu::g_settings.pipelineLibrary)
        {
            fprintf(stderr, "[gpu] D3D12 pipeline cache off ([gpu] pipeline_library=off)\n");
            return;
        }
        std::filesystem::path marker = path;
        marker += ".loading";
        std::error_code error;
        if (std::filesystem::exists(marker, error))
        {
            std::filesystem::path failed = path;
            failed += ".failed";
            std::filesystem::rename(path, failed, error);
            fprintf(stderr, "[gpu] D3D12 pipeline cache skipped: an earlier run did not get past opening it (%s is left; "
                "delete it to try again)\n", marker.filename().string().c_str());
            return;
        }
        std::ofstream(marker) << "opening the pipeline cache\n";
        OpenPipelineLibrary(renderDevice, path, identity);
        if (D3D12RemovedReason(renderDevice) != S_OK)
        {
            fprintf(stderr, "[gpu] the device was removed while opening the pipeline cache: the next start skips it\n");
            return;
        }
        std::filesystem::remove(marker, error);
    }

    void OpenPipelineLibrary(plume::RenderDevice& renderDevice, const std::filesystem::path& path, uint64_t identity)
    {
        plume::D3D12Device& device = Device(renderDevice);
        // Only where the driver says it has pipeline libraries.
        D3D12_FEATURE_DATA_SHADER_CACHE shaderCache = {};
        if (FAILED(device.d3d->CheckFeatureSupport(D3D12_FEATURE_SHADER_CACHE, &shaderCache, sizeof(shaderCache))) ||
            (shaderCache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY) == 0)
        {
            fprintf(stderr, "[gpu] D3D12 pipeline cache: the driver has no pipeline libraries (support flags 0x%X)\n",
                unsigned(shaderCache.SupportFlags));
            return;
        }
        g_path = path;
        g_identity = identity;
        std::ifstream in(path, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Header header{};
        if (data.size() > sizeof(header))
            memcpy(&header, data.data(), sizeof(header));

        HRESULT result = E_FAIL;
        const char* why = "none saved";
        if (header.magic == Magic && header.version == Version && header.identity == identity)
        {
            g_blob.assign(data.begin() + sizeof(header), data.end());
            result = device.d3d->CreatePipelineLibrary(g_blob.data(), g_blob.size(), IID_PPV_ARGS(&device.pipelineLibrary));
            why = SUCCEEDED(result) ? nullptr : "rejected (another driver or adapter, or damaged)";
        }
        else if (!data.empty())
        {
            why = "for another shader cache";
        }
        if (FAILED(result))
        {
            g_blob.clear();
            device.pipelineLibrary = nullptr;
            result = device.d3d->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&device.pipelineLibrary));
            if (FAILED(result))
            {
                device.pipelineLibrary = nullptr;
                fprintf(stderr, "[gpu] D3D12 pipeline library unavailable (0x%08lX): every run builds its pipelines\n",
                    static_cast<unsigned long>(result));
                return;
            }
            fprintf(stderr, "[gpu] D3D12 pipeline cache: starting empty (%s)\n", why);
            return;
        }
        fprintf(stderr, "[gpu] D3D12 pipeline cache: loaded %.1f MB\n", double(g_blob.size()) / (1 << 20));
    }

    // Writes the library when pipelines were added since the last save (to a temporary file, then renamed). Called from
    // a pipeline thread when the list is built, and every 30 s after draw-time builds; the stores wait meanwhile.
    void D3D12SavePipelineLibrary(plume::RenderDevice& renderDevice, const char* when)
    {
        plume::D3D12Device& device = Device(renderDevice);
        if (device.pipelineLibrary == nullptr || g_path.empty())
            return;
        std::lock_guard saveLock(g_saveMutex);
        std::vector<char> data;
        uint32_t hits, misses;
        {
            std::lock_guard lock(device.pipelineLibraryMutex);
            hits = device.pipelineLibraryHits;
            misses = device.pipelineLibraryMisses;
            if (!device.pipelineLibraryChanged)
            {
                fprintf(stderr, "[gpu] D3D12 pipeline cache %s: %u loaded, %u built\n", when, hits, misses);
                return;
            }
            size_t size = device.pipelineLibrary->GetSerializedSize();
            data.resize(sizeof(Header) + size);
            Header header{ Magic, Version, g_identity };
            memcpy(data.data(), &header, sizeof(header));
            if (FAILED(device.pipelineLibrary->Serialize(data.data() + sizeof(header), size)))
            {
                fprintf(stderr, "[gpu] D3D12 pipeline cache: Serialize failed\n");
                return;
            }
            device.pipelineLibraryChanged = false;
        }
        std::filesystem::path temporary = g_path;
        temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(data.data(), std::streamsize(data.size()));
        }
        std::error_code error;
        std::filesystem::rename(temporary, g_path, error);
        fprintf(stderr, "[gpu] D3D12 pipeline cache %s: %u loaded, %u built; saved %.1f MB%s\n", when, hits, misses,
            double(data.size()) / (1 << 20), error ? " (rename failed)" : "");
    }
}
