// The Direct3D replacement ("Option B"). The game's own D3D keeps running: its setters only write the device structure,
// and its draw/resolve/swap functions build a command ring nobody executes. Each draw, resolve and swap is mirrored
// with host GPU work built from the device state at that moment (d3d/device.h), using the game's shaders as
// XenosRecomp translated them. EDRAM surfaces become host render targets; resolves copy them into host textures that
// stand in for the guest textures they were resolved to.
#include "d3d/capture.h"
#include "d3d/command_buffers.h"
#include "d3d/depth_readback.h"
#include "d3d/device.h"
#include "d3d/render_scale.h"
#include "d3d/ring.h"
#include "d3d/shaders.h"
#include "d3d/ultrawide.h"
#include "d3d/xenos.h"
#include "gpu/buffer_cache.h"
#include "gpu/draw.h"
#include "gpu/edge_smoothing.h"
#include "host/display.h"
#include "host/dds.h"
#include "host/art/xenos_texture.h"
#include "host/glyphs.h"
#include "host/texture_pack.h"
#include "kernel/video.h"
#include "game.h"
#include "game_hooks.h"
#include "memory.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include "byteswap.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ppc_config.h>
#include <ppc_context.h>
#include "../host/env.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

using namespace d3d;
using plume::RenderPrimitiveTopology;

namespace
{
    uint32_t Read32(uint32_t address) { return g_memory.Read<uint32_t>(address); }

    std::recursive_mutex g_mutex;
    uint64_t g_frame = 1;
    std::chrono::steady_clock::time_point g_swapEnd; // SKY_FRAME_TIMES
    const auto g_processStart = std::chrono::steady_clock::now(); // set at start-up: SKY_FRAME_TIMES's origin

    // Resolution scale (settings): host render targets, and the textures resolved from them, are this many times the
    // guest's surfaces, so guest pixel coordinates (viewports, scissors, resolve and clear rectangles) are scaled by it.
    int32_t Scale() { return int32_t(gpu::g_settings.resolutionScale); }
    // The effective edge smoothing: the setting, or by the resolution scale when it was not chosen.
    edgesmoothing::Mode EdgeSmoothing()
    {
        return edgesmoothing::Resolve(
            edgesmoothing::Mode(gpu::g_settings.edgeSmoothing), uint32_t(gpu::g_settings.resolutionScale));
    }

    int32_t TargetScale(const gpu::TextureDesc& desc) { return d3d::TargetScale(desc.width, desc.height, Scale()); }

    struct Stats
    {
        uint32_t draws, skippedShader, skippedTarget, resolves;
    } g_stats;

    // --- Render targets: one host texture per EDRAM surface (base tile, format, size). ---------------------------------

    std::unordered_map<uint64_t, gpu::Texture*> g_targets;

    // --- Ultrawide: the frame (the game's 1280x720 back buffer and its depth) is wider than the game thinks. -------------
    // The game composes the scene and draws the HUD for 1280x720 guest pixels. With a wide window the host target is
    // ultrawide::FrameWidth() wide: the composite (the pass that reads the scene) fills it, every other draw into it is
    // moved to the middle, so the HUD keeps its 16:9 layout.

    bool IsFrameSize(uint32_t guestWidth, uint32_t guestHeight)
    {
        return ultrawide::Active() && guestWidth == ultrawide::FrameWidth16x9 && guestHeight == ultrawide::FrameHeight;
    }

    // A host target made for the frame: the wide frame at the current scale.
    bool IsFrameTexture(const gpu::TextureDesc& desc)
    {
        return ultrawide::Active() && desc.width == ultrawide::FrameWidth() * Scale() &&
               desc.height == ultrawide::FrameHeight * Scale();
    }

    // How far the guest's frame sits from the wide frame's left edge, in host pixels.
    int32_t FrameShift() { return int32_t(ultrawide::FrameWidth() - ultrawide::FrameWidth16x9) / 2 * Scale(); }

    // D3DSurface: +0x1C RB_COLOR_INFO / RB_DEPTH_INFO (EDRAM base tile, format), +0x24 size (width-1 << 18 | height-1 << 3).
    gpu::Texture* TargetFor(uint32_t device, uint32_t surface, bool depth)
    {
        if (surface == 0)
            return nullptr;
        uint32_t info = Read32(surface + 0x1C);
        uint32_t size = Read32(surface + 0x24);
        uint32_t width = ((size >> 18) & 0x3FFF) + 1;
        uint32_t height = ((size >> 3) & 0x1FFF) + 1;

        // Predicated tiling (Giants' 1280x720 MSAA frontend pass): the surface is one tile, the GPU replays the draws per
        // tile with a window offset nobody executes here. The host target covers all the tiles, so the draws land once at
        // their screen positions and each tile's resolve reads its part.
        uint32_t tiles = Read32(device + Device::TileCount);
        if (game::ExpandTiledTargets && tiles > 1 && tiles <= 16)
        {
            uint32_t right = 0, bottom = 0;
            for (uint32_t i = 0; i < tiles; i++)
            {
                right = std::max(right, Read32(device + Device::TileRects + i * 16 + 8));
                bottom = std::max(bottom, Read32(device + Device::TileRects + i * 16 + 12));
            }
            uint32_t tileWidth = Read32(device + Device::TileRects + 8) - Read32(device + Device::TileRects);
            uint32_t tileHeight = Read32(device + Device::TileRects + 12) - Read32(device + Device::TileRects + 4);
            if (right <= 4096 && bottom <= 4096 &&
                ((width == tileWidth && right > width) || (height == tileHeight && bottom > height)))
            {
                width = std::max(width, right);
                height = std::max(height, bottom);
            }
        }
        uint32_t format = depth ? 0x10 : (info >> 16) & 0xF;
        uint64_t key = (info & 0xFFF) | uint64_t(format) << 12 | uint64_t(width) << 20 | uint64_t(height) << 36;

        gpu::Texture*& texture = g_targets[key];
        if (texture == nullptr)
        {
            const int32_t scale = d3d::TargetScale(width, height, Scale());
            gpu::TextureDesc desc{ .width = (IsFrameSize(width, height) ? ultrawide::FrameWidth() : width) * scale,
                .height = height * scale };
            desc.format = depth ? xenos::DepthTargetFormat : xenos::ColorTargetFormat(format);
            desc.usage =
                depth ? gpu::TextureUsage::DepthStencil : gpu::TextureUsage::RenderTarget | gpu::TextureUsage::Sampled;
            texture = gpu::CreateTexture(desc);
        }
        return texture;
    }

    // --- Textures: host copies of guest textures, by base address, format and size. ------------------------------------

    struct HostTexture
    {
        gpu::Texture* texture = nullptr;
        bool resolved = false; // written by resolves: the host copy is the only valid one
        bool swapRB =
            false; // resolved with red and blue swapped (RB_COPY_DEST_INFO.copy_dest_swap): the host copy is not
        uint64_t checkedFrame = 0;
        uint64_t contentHash = 0;
        uint64_t guestHash = 0; // GuestContentHash at the last full check (contentHash may be a glyph's host hash)
        uint64_t sampleHash = 0; // GuestSampleHash then
        uint64_t fullFrame = 0; // the frame of the next full check
    };
    std::unordered_map<uint64_t, HostTexture> g_textures;

    uint64_t TextureKey(const xenos::TextureFetch& fetch)
    {
        return uint64_t(fetch.BaseAddress() & 0x1FFFF000) >> 12 | uint64_t(fetch.Format()) << 17 |
               uint64_t(fetch.Width()) << 23 | uint64_t(fetch.Height()) << 37 | uint64_t(fetch.Dimension()) << 51;
    }

    uint32_t BlocksLog2(uint32_t bytes) { return uint32_t(std::countr_zero(bytes)); }

    // 16-bit texels with no host format: expanded to R8G8B8A8 (component order as Xenos fetches them: X in the low bits).
    uint32_t ExpandTexel(uint32_t format, uint16_t texel)
    {
        auto scale = [](uint32_t value, uint32_t bits) { return value * 255 / ((1u << bits) - 1); };
        switch (format)
        {
        case 3: // k_1_5_5_5
            return scale(texel & 31, 5) | scale((texel >> 5) & 31, 5) << 8 | scale((texel >> 10) & 31, 5) << 16 |
                   (texel >> 15 ? 0xFF000000u : 0);
        case 4: // k_5_6_5
            return scale(texel & 31, 5) | scale((texel >> 5) & 63, 6) << 8 | scale((texel >> 11) & 31, 5) << 16 |
                   0xFF000000u;
        default: // k_4_4_4_4
            return scale(texel & 15, 4) | scale((texel >> 4) & 15, 4) << 8 | scale((texel >> 8) & 15, 4) << 16 |
                   scale(texel >> 12, 4) << 24;
        }
    }

    // The fetch constant's swizzle: 3 bits per output component, 0-3 = X-W (X is the format's first component), 4 = 0, 5 = 1.
    // `swapRB`: the guest texture holds the host texture's red in Z and its blue in X.
    plume::RenderComponentMapping SwizzleFor(const xenos::TextureFetch& fetch, bool swapRB)
    {
        uint32_t swizzle = fetch.Swizzle();
        if (swizzle == 0x688 && !swapRB) // XYZW
            return {};
        auto component = [&](uint32_t c) {
            static const plume::RenderSwizzle Table[8] = { plume::RenderSwizzle::R, plume::RenderSwizzle::G,
                plume::RenderSwizzle::B, plume::RenderSwizzle::A, plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ONE,
                plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ZERO };
            static const plume::RenderSwizzle Swapped[8] = { plume::RenderSwizzle::B, plume::RenderSwizzle::G,
                plume::RenderSwizzle::R, plume::RenderSwizzle::A, plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ONE,
                plume::RenderSwizzle::ZERO, plume::RenderSwizzle::ZERO };
            return (swapRB ? Swapped : Table)[(swizzle >> (c * 3)) & 7];
        };
        return { component(0), component(1), component(2), component(3) };
    }

    // Reads one level of one array slice of a guest 2D texture into host layout (untiled, byte-swapped, converted).
    // `source`: the slice's guest storage (level.sliceBytes from level.address + slice * level.sliceBytes).
    void DecodeTexture(const uint8_t* source, const xenos::TextureFetch& fetch, const xenos::TextureFormatInfo& info,
        const xenos::TextureLevel& level, std::vector<uint8_t>& out, uint32_t& rowPitch)
    {
        uint32_t width = level.width, height = level.height;
        uint32_t blocksX = (width + info.blockWidth - 1) / info.blockWidth;
        uint32_t blocksY = (height + info.blockWidth - 1) / info.blockWidth;

        std::vector<uint8_t> blocks(size_t(blocksX) * blocksY * info.blockBytes);
        if (fetch.Tiled())
        {
            uint32_t log2 = BlocksLog2(info.blockBytes);
            for (uint32_t y = 0; y < blocksY; y++)
                for (uint32_t x = 0; x < blocksX; x++)
                    memcpy(&blocks[(size_t(y) * blocksX + x) * info.blockBytes],
                        source + xenos::Tiled2D(level.x + x, level.y + y, level.pitchBlocks, log2), info.blockBytes);
        }
        else
        {
            size_t sourcePitch = size_t(level.pitchBlocks) * info.blockBytes;
            for (uint32_t y = 0; y < blocksY; y++)
                memcpy(&blocks[size_t(y) * blocksX * info.blockBytes],
                    source + (level.y + y) * sourcePitch + size_t(level.x) * info.blockBytes,
                    size_t(blocksX) * info.blockBytes);
        }
        xenos::SwapEndian(blocks.data(), blocks.size(), fetch.Endian());

        if (info.convertToRgba8)
        {
            out.resize(size_t(width) * height * 4);
            auto texels = reinterpret_cast<uint32_t*>(out.data());
            auto in = reinterpret_cast<const uint16_t*>(blocks.data());
            for (size_t i = 0; i < size_t(width) * height; i++)
                texels[i] = ExpandTexel(fetch.Format(), in[i]);
            rowPitch = width * 4;
            return;
        }

        out = std::move(blocks);
        rowPitch = blocksX * info.blockBytes;
    }

    // The bytes GuestContentHash reads: the base level, all slices (mips are assumed to change with it).
    uint64_t GuestContentBytes(const xenos::TextureFetch& fetch, const xenos::TextureFormatInfo& info)
    {
        return uint64_t(xenos::TextureLevelLayout(fetch, info, 0).sliceBytes) * xenos::TextureArraySize(fetch);
    }

    uint64_t GuestContentHash(const xenos::TextureFetch& fetch, const xenos::TextureFormatInfo& info)
    {
        uint32_t guest = PhysicalToGuest(fetch.BaseAddress());
        if (guest == 0)
            return 0;
        return XXH3_64bits(g_memory.Translate(guest), GuestContentBytes(fetch, info));
    }

    // Checking a big texture every frame (docs/runtime/gpu/renderer_threads.md, "Textures"): from SampledBytes, a frame
    // hashes 64 lines of 64 bytes spread over the storage (one in each 1/64th, at a place that differs from one 1/64th to
    // the next), and the whole storage only when that sample changed, every FullCheckFrames frames (staggered per
    // texture) and the frame after a change. A write the sample misses shows at most FullCheckFrames frames late. SKY_TEXTURE_STATS=1 also hashes the
    // whole storage at every sampled check and counts the changes the sample missed.
    constexpr uint64_t SampledBytes = 64 * 1024;
    constexpr uint64_t FullCheckFrames = 8;

    uint64_t GuestSampleHash(const xenos::TextureFetch& fetch, uint64_t bytes)
    {
        constexpr uint64_t Lines = 64, Line = 64;
        auto data = static_cast<const uint8_t*>(g_memory.Translate(PhysicalToGuest(fetch.BaseAddress())));
        uint64_t stride = bytes / Lines, slots = stride / Line;
        uint8_t sample[Lines * Line];
        for (uint64_t i = 0; i < Lines; i++)
            memcpy(sample + i * Line, data + i * stride + (i * 37 % slots) * Line, Line);
        return XXH3_64bits(sample, sizeof(sample));
    }

    struct TextureStats
    {
        uint64_t checks = 0, full = 0, bytes = 0, sampled = 0, missed = 0, changed = 0;
    } g_textureStats;
    const bool g_textureStatsOn = skyenv::Get("SKY_TEXTURE_STATS") != nullptr;

    // The texture's guest hash this frame: GuestContentHash, or the last one when its sample says it did not change.
    uint64_t CheckGuestContent(
        HostTexture& host, uint64_t key, const xenos::TextureFetch& fetch, const xenos::TextureFormatInfo& info)
    {
        g_textureStats.checks++;
        uint64_t bytes = GuestContentBytes(fetch, info);
        if (bytes < SampledBytes || PhysicalToGuest(fetch.BaseAddress()) == 0)
        {
            g_textureStats.full++;
            g_textureStats.bytes += bytes;
            return GuestContentHash(fetch, info);
        }
        uint64_t sample = GuestSampleHash(fetch, bytes);
        if (host.texture != nullptr && sample == host.sampleHash && g_frame < host.fullFrame)
        {
            g_textureStats.sampled++;
            if (g_textureStatsOn && GuestContentHash(fetch, info) != host.guestHash)
            {
                g_textureStats.missed++;
                fprintf(stderr,
                    "[textures] the sample missed a change: %08X format %u %ux%u, %llu bytes, frame %llu (full check "
                    "at %llu)\n",
                    fetch.BaseAddress(), fetch.Format(), fetch.Width(), fetch.Height(), (unsigned long long)bytes,
                    (unsigned long long)g_frame, (unsigned long long)host.fullFrame);
            }
            return host.guestHash;
        }
        g_textureStats.full++;
        g_textureStats.bytes += bytes;
        uint64_t hash = GuestContentHash(fetch, info);
        bool changed = host.texture != nullptr && hash != host.guestHash;
        g_textureStats.changed += changed;
        // The first full check spreads the textures of a level over the FullCheckFrames frames; a texture that just changed
        // (one the game rewrites every frame, SSA's 1120x704 RGBA8 at 0x06A60000) is hashed whole again the next frame.
        host.fullFrame = g_frame + (host.fullFrame == 0 ? 1 + key % FullCheckFrames : changed ? 1 : FullCheckFrames);
        host.guestHash = hash;
        host.sampleHash = sample;
        return hash;
    }

    // The base level of a 2D DXT1, DXT5 or 16-bit (expanded) texture as RGBA8 (R in the low byte), for host/glyphs.cpp's
    // atlases; empty for any other format.
    std::vector<uint32_t> GuestBaseRgba(const xenos::TextureFetch& fetch, const xenos::TextureFormatInfo& info)
    {
        uint32_t format = fetch.Format();
        bool dxt1 = format == 18 || format == 51, dxt5 = format == 20 || format == 53;
        xenos::TextureLevel base = xenos::TextureLevelLayout(fetch, info, 0);
        uint32_t guest = PhysicalToGuest(base.address);
        if ((!dxt1 && !dxt5 && !info.convertToRgba8) || guest == 0)
            return {};
        std::vector<uint8_t> blocks;
        uint32_t rowPitch;
        DecodeTexture(static_cast<const uint8_t*>(g_memory.Translate(guest)), fetch, info, base, blocks, rowPitch);
        if (info.convertToRgba8)
        {
            std::vector<uint32_t> rgba(size_t(base.width) * base.height);
            memcpy(rgba.data(), blocks.data(), rgba.size() * 4);
            return rgba;
        }
        uint32_t width = base.width, height = base.height, blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
        std::vector<uint32_t> rgba(size_t(width) * height);
        uint32_t texels[16];
        for (uint32_t by = 0; by < blocksY; by++)
            for (uint32_t bx = 0; bx < blocksX; bx++)
            {
                art::DecodeBlock(&blocks[(size_t(by) * blocksX + bx) * info.blockBytes],
                    dxt5 ? art::TextureFormat::Dxt5 : art::TextureFormat::Dxt1, texels);
                for (uint32_t i = 0; i < 16; i++)
                {
                    uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < width && y < height)
                        rgba[size_t(y) * width + x] = texels[i];
                }
            }
        return rgba;
    }

    // --- Texture packs (docs/runtime/gpu/texture_packs.md) --------------------------------------------------------------------------

    // The DDS format a guest format is dumped as, and how many channels it has (1 and 2: dumped expanded to RGBA8).
    // Formats outside this table (16-bit and float, depth) are neither dumped nor replaced.
    bool PackFormat(const xenos::TextureFormatInfo& info, dds::Format& format, uint32_t& channels)
    {
        channels = 4;
        switch (info.host)
        {
        case gpu::Format::BC1_UNORM: format = dds::Format::BC1; return true;
        case gpu::Format::BC2_UNORM: format = dds::Format::BC2; return true;
        case gpu::Format::BC3_UNORM: format = dds::Format::BC3; return true;
        case gpu::Format::BC4_UNORM: format = dds::Format::BC4; return true;
        case gpu::Format::BC5_UNORM: format = dds::Format::BC5; return true;
        case gpu::Format::R8G8B8A8_UNORM: format = dds::Format::RGBA8; return true;
        case gpu::Format::R8G8_UNORM:
            format = dds::Format::RGBA8;
            channels = 2;
            return true;
        case gpu::Format::R8_UNORM:
            format = dds::Format::RGBA8;
            channels = 1;
            return true;
        default: return false;
        }
    }

    gpu::Format HostFormat(dds::Format format)
    {
        switch (format)
        {
        case dds::Format::BC1: return gpu::Format::BC1_UNORM;
        case dds::Format::BC2: return gpu::Format::BC2_UNORM;
        case dds::Format::BC3: return gpu::Format::BC3_UNORM;
        case dds::Format::BC4: return gpu::Format::BC4_UNORM;
        case dds::Format::BC5: return gpu::Format::BC5_UNORM;
        default: return gpu::Format::R8G8B8A8_UNORM;
        }
    }

    // What a texture's top level hashes to, from the guest hash of its storage: the untiled data is built (and hashed, and
    // dumped) once per distinct storage content, not per upload.
    std::unordered_map<uint64_t, uint64_t> g_contentHashes;
    // Where resolves wrote: a texture read from there with another format or size than the resolve's has guest memory the
    // GPU never wrote, so it is not a game texture to dump or replace.
    std::unordered_set<uint32_t> g_resolvedBases;

    // A texture the game rewrites every frame (a buffer it fills on the CPU) is not a texture to dump or replace, and
    // hashing it would cost every frame: an address whose content changes at 4 uploads in a row, each at most 2 frames
    // after the one before, is dynamic until it stays unchanged for 3 frames.
    struct Churn
    {
        uint64_t lastFrame = 0;
        uint32_t streak = 0;
    };
    std::unordered_map<uint32_t, Churn> g_churn;
    constexpr uint32_t DynamicStreak = 4;

    void NoteUpload(const xenos::TextureFetch& fetch)
    {
        if (!texpack::Active())
            return;
        Churn& churn = g_churn[fetch.BaseAddress()];
        churn.streak = g_frame - churn.lastFrame <= 2 ? churn.streak + 1 : 1;
        churn.lastFrame = g_frame;
    }

    struct PackLookup
    {
        const texpack::Replacement* replacement = nullptr;
    };

    PackLookup LookUpPack(const xenos::TextureFetch& fetch, const xenos::TextureFormatInfo& info, uint64_t guestHash)
    {
        dds::Format format;
        uint32_t channels;
        if (!texpack::Active() || guestHash == 0 || g_resolvedBases.contains(fetch.BaseAddress()) ||
            g_churn[fetch.BaseAddress()].streak >= DynamicStreak || fetch.Dimension() != 1 ||
            xenos::TextureArraySize(fetch) != 1 || !PackFormat(info, format, channels))
            return {};
        // The same storage read with another layout is another texture.
        const uint32_t layoutKey[] = { fetch.Format(), fetch.Endian(), fetch.Width(), fetch.Height(), fetch.Pitch(),
            fetch.Tiled() };
        uint64_t key = XXH3_64bits_withSeed(layoutKey, sizeof(layoutKey), guestHash);
        auto cached = g_contentHashes.find(key);
        if (cached == g_contentHashes.end())
        {
            xenos::TextureLevel base = xenos::TextureLevelLayout(fetch, info, 0);
            uint32_t guest = PhysicalToGuest(base.address);
            if (guest == 0)
                return {};
            std::vector<uint8_t> data;
            uint32_t rowPitch;
            DecodeTexture(static_cast<const uint8_t*>(g_memory.Translate(guest)), fetch, info, base, data, rowPitch);
            uint64_t hash =
                texpack::ContentHash(fetch.Format(), fetch.Width(), fetch.Height(), data.data(), data.size());
            if (texpack::ShouldDump(hash))
            {
                if (channels != 4 && !info.convertToRgba8)
                {
                    std::vector<uint8_t> rgba(size_t(fetch.Width()) * fetch.Height() * 4, 0);
                    for (size_t i = 0; i < size_t(fetch.Width()) * fetch.Height(); i++)
                    {
                        rgba[i * 4 + 3] = 0xFF;
                        for (uint32_t c = 0; c < channels; c++)
                            rgba[i * 4 + c] = data[i * channels + c];
                    }
                    data = std::move(rgba);
                }
                // Finding a game's glyph textures (host/glyphs.h): the dump file's name next to the guest hash.
                static const bool logGuest = skyenv::Get("SKY_TEXTURE_DUMP_LOG") != nullptr;
                if (logGuest)
                    fprintf(stderr, "[texpack] dumped %016llx: guest hash %016llX, %ux%u\n", (unsigned long long)hash,
                        (unsigned long long)guestHash, fetch.Width(), fetch.Height());
                texpack::Dump(hash, format, fetch.Width(), fetch.Height(), std::move(data));
            }
            cached = g_contentHashes.emplace(key, hash).first;
        }
        return { texpack::Find(cached->second) };
    }

    gpu::Texture* TextureForUntimed(
        const xenos::TextureFetch& fetch, const xenos::TextureFormatInfo& info, uint64_t key, HostTexture& host);

    // The perf log's texture time: hashing the guest data, copying it and (without the GPU thread) decoding it. A texture
    // already checked this frame (most draws' textures) returns before the timer: its two clock reads cost more than the
    // lookup.
    gpu::Texture* TextureFor(const xenos::TextureFetch& fetch)
    {
        if (fetch.Type() != 2 || fetch.Dimension() == 2)
            return nullptr; // 3D textures: not yet (a 1D one is a 2D texture of one row, as the translated shaders sample it)
        const xenos::TextureFormatInfo& info = xenos::TextureFormat(fetch.Format());
        uint64_t key = TextureKey(fetch);
        HostTexture& host = g_textures[key];
        if (host.resolved || info.host == gpu::Format::UNKNOWN || host.checkedFrame == g_frame)
            return host.texture;
        auto start = std::chrono::steady_clock::now();
        gpu::Texture* texture = TextureForUntimed(fetch, info, key, host);
        gpu::AddFrameTime(gpu::FrameTime::Textures,
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                    .count()));
        return texture;
    }

    gpu::Texture* TextureForUntimed(
        const xenos::TextureFetch& fetch, const xenos::TextureFormatInfo& info, uint64_t key, HostTexture& host)
    {
        // Guest textures can change at any time (movies, streaming): reload when the guest data differs.
        host.checkedFrame = g_frame;
        uint64_t hash = CheckGuestContent(host, key, fetch, info);
        if (glyphs::Replace(
                hash, fetch.Width(), fetch.Height(), xenos::TextureLevelCount(fetch),
                [&] { return GuestBaseRgba(fetch, info); }, host.texture, host.contentHash))
            return host.texture; // a button glyph drawn for the device in use (host/glyphs.cpp)
        if (host.texture != nullptr && hash == host.contentHash)
            return host.texture;

        uint32_t levels = xenos::TextureLevelCount(fetch);
        uint32_t arraySize = xenos::TextureArraySize(fetch);
        uint32_t width = fetch.Width(), height = fetch.Height();
        gpu::Format hostFormat = info.host;
        NoteUpload(fetch);
        const texpack::Replacement* pack = LookUpPack(fetch, info, hash).replacement;
        if (pack != nullptr)
        {
            width = pack->info.width;
            height = pack->info.height;
            levels = pack->info.levels;
            hostFormat = HostFormat(pack->info.format);
        }
        if (host.texture != nullptr)
        {
            const gpu::TextureDesc& desc = gpu::Desc(host.texture);
            if (desc.mipLevels != levels || desc.arraySize != arraySize || desc.width != width ||
                desc.height != height || desc.format != hostFormat)
            {
                gpu::DestroyTexture(host.texture);
                host.texture = nullptr;
            }
        }
        if (pack != nullptr)
        {
            // A replacement file: the texture threads read its levels.
            if (host.texture == nullptr)
                host.texture = gpu::CreateTexture(
                    { .width = width, .height = height, .mipLevels = levels, .arraySize = 1, .format = hostFormat });
            for (uint32_t level = 0; level < levels; level++)
                gpu::UploadTextureAsync(host.texture, level, 0, [pack, level](uint8_t* destination, uint32_t pitch) {
                    std::vector<uint8_t> data;
                    texpack::ReadLevel(*pack, level, data);
                    dds::LevelSize size =
                        dds::SizeOfLevel(pack->info.format, pack->info.width, pack->info.height, level);
                    for (uint32_t row = 0; row < size.rows; row++)
                        memcpy(destination + size_t(row) * pitch, data.data() + size_t(row) * size.rowBytes,
                            std::min(size.rowBytes, pitch));
                });
            host.contentHash = hash;
            return host.texture;
        }
        // Each subresource's guest storage is copied now (the guest may change it later) and decoded on a texture worker
        // thread, straight into upload memory.
        for (uint32_t level = 0; level < levels; level++)
        {
            xenos::TextureLevel layout = xenos::TextureLevelLayout(fetch, info, level);
            for (uint32_t slice = 0; slice < arraySize; slice++)
            {
                uint32_t guest = PhysicalToGuest(layout.address + slice * layout.sliceBytes);
                if (guest == 0)
                    return host.texture;
                if (host.texture == nullptr)
                    host.texture = gpu::CreateTexture({ .width = fetch.Width(),
                        .height = fetch.Height(),
                        .mipLevels = levels,
                        .arraySize = arraySize,
                        .format = info.host,
                        .cube = fetch.Dimension() == 3 });
                auto source = static_cast<const uint8_t*>(g_memory.Translate(guest));
                auto storage = std::make_shared<std::vector<uint8_t>>(source, source + layout.sliceBytes);
                gpu::UploadTextureAsync(host.texture, level, slice,
                    [storage, fetch, format = &info, layout](uint8_t* destination, uint32_t pitch) {
                        std::vector<uint8_t> data;
                        uint32_t rowPitch;
                        DecodeTexture(storage->data(), fetch, *format, layout, data, rowPitch);
                        for (size_t row = 0; row < data.size() / rowPitch; row++)
                            memcpy(destination + row * pitch, data.data() + row * rowPitch, std::min(rowPitch, pitch));
                    });
            }
        }
        host.contentHash = hash;
        return host.texture;
    }

    gpu::SamplerState SamplerFor(const xenos::TextureFetch& fetch)
    {
        gpu::SamplerState state{};
        state.minFilter = fetch.MinFilter() == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
        state.magFilter = fetch.MagFilter() == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
        state.mipFilter = fetch.MipFilter() == 0 ? plume::RenderMipmapMode::NEAREST : plume::RenderMipmapMode::LINEAR;
        state.addressU = xenos::AddressMode(fetch.ClampX());
        state.addressV = xenos::AddressMode(fetch.ClampY());
        state.addressW = xenos::AddressMode(fetch.ClampZ());
        state.borderColor = plume::RenderBorderColor::TRANSPARENT_BLACK;
        // The anisotropy setting only for textures with mipmaps: the games' own fetch constants never ask for it, and a
        // texture without mips is a render target read at computed coordinates or a colour table (Swap Force: anisotropic
        // taps along the coordinates' gradient smeared the whole frame and turned its lava and Eruptor green).
        bool mipmapped = xenos::TextureLevelCount(fetch) > 1;
        state.maxAnisotropy = state.minFilter == plume::RenderFilter::LINEAR &&
                                      state.magFilter == plume::RenderFilter::LINEAR && mipmapped
                                  ? gpu::g_settings.anisotropy
                                  : 1;
        state.minLod = float(fetch.MipMinLevel());
        state.maxLod = float(fetch.MipMaxLevel());
        return state;
    }

    // SKY_DUMP_FRAME=<seconds> (or f<N>: the frame after the N-th swap) and SKY_DUMP_PREFIX=<prefix>: saves every texture
    // resolved during the first frame after that time, then the presented one, as <prefix>_<n>_<width>x<height>.ppm
    // (with SKY_DUMP_DRAWS, also the target after every draw; R32_FLOAT ones as grey from their minimum to their maximum,
    // with the values logged). A debugging aid.
    struct FrameDump
    {
        bool active = false;
        bool done = false;
        uint32_t index = 0;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    } g_dump;

    void DumpResolve(const gpu::Texture* texture, char tag = 'r')
    {
        static const char* prefix = skyenv::Get("SKY_DUMP_PREFIX");
        static const bool draws = skyenv::Get("SKY_DUMP_DRAWS") != nullptr;
        if (!g_dump.active || prefix == nullptr || (tag == 'd' && !draws))
            return;
        std::vector<uint32_t> rgba;
        uint32_t width, height;
        if (!gpu::ReadbackTexture(texture, rgba, width, height))
            return;
        if (gpu::Desc(texture).format == gpu::Format::R32_FLOAT)
        {
            // A float target (depth resolves, linear depth): its values, and the picture as grey from min (black) to max.
            std::vector<uint32_t> bits;
            if (gpu::ReadbackTexture(texture, bits, width, height, true) && !bits.empty())
            {
                std::vector<float> values(bits.size());
                for (size_t i = 0; i < bits.size(); i++)
                    values[i] = std::bit_cast<float>(bits[i]);
                std::vector<float> sorted = values;
                std::sort(sorted.begin(), sorted.end());
                float low = sorted.front(), high = sorted.back();
                fprintf(stderr, "[dump] float file %02u: min %g, 10%% %g, median %g, 90%% %g, max %g\n", g_dump.index,
                    low, sorted[sorted.size() / 10], sorted[sorted.size() / 2], sorted[sorted.size() * 9 / 10], high);
                for (size_t i = 0; i < values.size(); i++)
                {
                    uint32_t grey =
                        high > low ? uint32_t(std::clamp((values[i] - low) / (high - low), 0.0f, 1.0f) * 255.0f) : 0;
                    rgba[i] = grey | grey << 8 | grey << 16 | 0xFF000000u;
                }
            }
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s_%02u%c_%ux%u.ppm", prefix, g_dump.index++, tag, width, height);
        if (FILE* file = fopen(path, "wb"))
        {
            fprintf(file, "P6\n%u %u\n255\n", width, height);
            for (uint32_t pixel : rgba)
            {
                uint8_t rgb[3] = { uint8_t(pixel), uint8_t(pixel >> 8), uint8_t(pixel >> 16) };
                fwrite(rgb, 1, 3, file);
            }
            fclose(file);
        }
        // SKY_DUMP_ALPHA=1: the alpha channel too, as grey (<file>a.ppm).
        static const bool alpha = skyenv::Get("SKY_DUMP_ALPHA") != nullptr;
        path[strlen(path) - 4] = 0;
        strcat(path, "a.ppm");
        if (FILE* file = alpha ? fopen(path, "wb") : nullptr)
        {
            fprintf(file, "P6\n%u %u\n255\n", width, height);
            for (uint32_t pixel : rgba)
            {
                uint8_t rgb[3] = { uint8_t(pixel >> 24), uint8_t(pixel >> 24), uint8_t(pixel >> 24) };
                fwrite(rgb, 1, 3, file);
            }
            fclose(file);
        }
    }

    // --- Draws --------------------------------------------------------------------------------------------------------

    gpu::Format AsUint(gpu::Format format)
    {
        switch (format)
        {
        case gpu::Format::R32_FLOAT: return gpu::Format::R32_UINT;
        case gpu::Format::R32G32_FLOAT: return gpu::Format::R32G32_UINT;
        case gpu::Format::R32G32B32_FLOAT: return gpu::Format::R32G32B32_UINT;
        case gpu::Format::R32G32B32A32_FLOAT: return gpu::Format::R32G32B32A32_UINT;
        default: return format;
        }
    }

    // A BeginVertices draw: built (state, shaders, constants) when the game asks for the buffer, sent once it has filled it.
    struct PendingVertices
    {
        bool pending = false;
        gpu::DrawDesc draw{};
        uint32_t guestData = 0; // the vertices, read again when sending
        uint32_t bytes = 0;
        uint32_t primitive = 0;
        uint32_t count = 0;
    } g_pendingVertices;

    std::vector<std::vector<uint8_t>> g_streamData;
    std::vector<uint8_t> g_indexData;
    std::vector<float> g_constants(2 * 256 * 4);
    // What PrepareDraw saw of the draw it built, for BuildDraw: it draws into the wide frame, may read the scene, VTE_CNTL.
    bool g_frameDraw, g_frameSceneRead, g_frameComposite;
    // The draw's guest shader hashes, for the frame dump's lines (0: none).
    uint64_t g_drawVertexShader, g_drawPixelShader;

    void ReadConstants(uint32_t device, float* out)
    {
        auto words = reinterpret_cast<uint32_t*>(out);
        memcpy(words, GpuFloatConstants(device), 2 * 256 * 16);
        for (uint32_t i = 0; i < 2 * 256 * 4; i++)
            words[i] = ByteSwap(words[i]);
    }

    gpu::BlendState BlendFor(uint32_t control, uint32_t mask)
    {
        gpu::BlendState blend{};
        blend.srcColor = xenos::BlendFactor(control & 31);
        blend.colorOp = xenos::BlendOp((control >> 5) & 7);
        blend.dstColor = xenos::BlendFactor((control >> 8) & 31);
        blend.srcAlpha = xenos::BlendFactor((control >> 16) & 31);
        blend.alphaOp = xenos::BlendOp((control >> 21) & 7);
        blend.dstAlpha = xenos::BlendFactor((control >> 24) & 31);
        // Xenos has no enable bit: ONE, ZERO, ADD for both colour and alpha is no blending.
        blend.enabled = (control & 0x1FFF1FFF) != 0x00010001;
        blend.writeMask = uint8_t(mask & 0xF);
        return blend;
    }

    // Fills everything except the vertex data and the draw range. False when the draw cannot be rendered (yet).
    // SKY_SKIP_PS=<hex hash>[,<hex hash>...]: drops the draws of these pixel shaders (a test aid: bisects a frame by pass).
    // A hash may be cut short, as the docs write them (123B15C5). The target keeps what it held: a post pass that writes
    // the target it read from is removed cleanly, one that writes another target leaves that target's old contents.
    bool SkipPixelShader(uint64_t hash)
    {
        struct Skip
        {
            uint64_t prefix;
            uint32_t shift;
            bool logged;
        };
        static std::vector<Skip> skips = [] {
            std::vector<Skip> list;
            const char* value = skyenv::Get("SKY_SKIP_PS");
            while (value != nullptr && *value != '\0')
            {
                char* end = nullptr;
                uint64_t prefix = strtoull(value, &end, 16);
                if (end == value)
                    break;
                list.push_back({ prefix, uint32_t(64 - 4 * std::min<ptrdiff_t>(end - value, 16)), false });
                value = *end == ',' ? end + 1 : end;
            }
            return list;
        }();
        for (Skip& skip : skips)
            if ((skip.shift == 0 ? hash : hash >> skip.shift) == skip.prefix)
            {
                if (!skip.logged)
                    fprintf(stderr, "[render] SKY_SKIP_PS: skipping pixel shader %016llX\n", (unsigned long long)hash);
                skip.logged = true;
                return true;
            }
        return false;
    }

    bool PrepareDraw(uint32_t device, uint32_t primitive, gpu::DrawDesc& draw)
    {
        memset(&draw, 0, sizeof(draw));
        g_frameDraw = g_frameSceneRead = g_frameComposite = false;

        // No pixel shader is allowed: depth-only passes.
        uint32_t pixelShaderObject = Read32(device + Device::PixelShader);
        const GuestShader* vs = FindGuestShader(Read32(device + Device::VertexShader));
        const GuestShader* ps = pixelShaderObject != 0 ? FindGuestShader(pixelShaderObject) : nullptr;
        g_drawVertexShader = vs != nullptr ? vs->hash : 0;
        g_drawPixelShader = ps != nullptr ? ps->hash : 0;
        if (ps != nullptr && SkipPixelShader(ps->hash))
        {
            g_stats.skippedShader++;
            return false;
        }
        draw.vertexShader = vs != nullptr ? gpu::FindXenosShader(vs->hash) : nullptr;
        draw.pixelShader = ps != nullptr ? gpu::FindXenosShader(ps->hash) : nullptr;
        if (draw.vertexShader == nullptr || (pixelShaderObject != 0 && draw.pixelShader == nullptr))
        {
            static std::vector<uint64_t> reported;
            for (const GuestShader* shader : { vs, ps })
                if (shader != nullptr && gpu::FindXenosShader(shader->hash) == nullptr &&
                    std::find(reported.begin(), reported.end(), shader->hash) == reported.end())
                {
                    reported.push_back(shader->hash);
                    fprintf(stderr, "[render] no translated %s shader %016llX (primitive %u): its draws are skipped\n",
                        shader->vertex ? "vertex" : "pixel", (unsigned long long)shader->hash, primitive);
                }
            g_stats.skippedShader++;
            return false;
        }

        switch (primitive)
        {
        case 1: draw.topology = RenderPrimitiveTopology::POINT_LIST; break;
        case 2: draw.topology = RenderPrimitiveTopology::LINE_LIST; break;
        case 3: draw.topology = RenderPrimitiveTopology::LINE_STRIP; break;
        case 5: draw.topology = RenderPrimitiveTopology::TRIANGLE_FAN; break;
        case 6: draw.topology = RenderPrimitiveTopology::TRIANGLE_STRIP; break;
        default: draw.topology = RenderPrimitiveTopology::TRIANGLE_LIST; break; // triangle, rectangle and quad lists
        }

        // Render targets.
        uint32_t colorMask = GpuRegister(device, Reg::RB_COLOR_MASK);
        const uint32_t blendRegisters[4] = { Reg::RB_BLENDCONTROL0, 0x2209, 0x220A, 0x220B };
        for (uint32_t i = 0; i < gpu::MaxRenderTargets; i++)
        {
            gpu::Texture* target = TargetFor(device, Read32(device + Device::RenderTargets + i * 4), false);
            if (target == nullptr)
                break;
            draw.colorTargets[i] = target;
            draw.blend[i] = BlendFor(GpuRegister(device, blendRegisters[i]), colorMask >> (i * 4));
            draw.colorTargetCount++;
        }
        for (uint32_t i = 0; i < 4; i++)
            draw.blendFactor[i] = std::bit_cast<float>(GpuRegister(device, Reg::RB_BLEND_RED + i));
        // SKY_LOG_SHADER=<hex hash>: says once where a pixel shader is first used (a test aid for shaders built at run time).
        static const uint64_t logShader =
            skyenv::Get("SKY_LOG_SHADER") != nullptr ? strtoull(skyenv::Get("SKY_LOG_SHADER"), nullptr, 16) : 0;
        static bool logged = false;
        if (logShader != 0 && !logged && ps != nullptr && ps->hash == logShader)
        {
            logged = true;
            const gpu::TextureDesc* target = draw.colorTargetCount != 0 ? &gpu::Desc(draw.colorTargets[0]) : nullptr;
            fprintf(stderr,
                "[render] shader %016llX first drawn: frame %llu, primitive %u, vertex shader %016llX, target %ux%u\n",
                (unsigned long long)logShader, (unsigned long long)g_frame, primitive, (unsigned long long)vs->hash,
                target != nullptr ? target->width : 0, target != nullptr ? target->height : 0);
        }
        draw.depthTarget = TargetFor(device, Read32(device + Device::DepthStencil), true);
        if (draw.colorTargetCount == 0 && draw.depthTarget == nullptr)
        {
            g_stats.skippedTarget++;
            return false;
        }
        gpu::Texture* sizeTarget = draw.colorTargetCount != 0 ? draw.colorTargets[0] : draw.depthTarget;
        for (uint32_t i = 0; i < draw.colorTargetCount; i++)
            if (gpu::Desc(draw.colorTargets[i]).width != gpu::Desc(sizeTarget).width ||
                gpu::Desc(draw.colorTargets[i]).height != gpu::Desc(sizeTarget).height)
                return false; // different sizes cannot share a host framebuffer; not seen so far

        // Depth, stencil and rasteriser state.
        uint32_t depthControl = GpuRegister(device, Reg::RB_DEPTHCONTROL);
        draw.depthStencil.depthEnabled = (depthControl >> 1) & 1;
        draw.depthStencil.depthWrite = (depthControl >> 2) & 1;
        draw.depthStencil.depthFunction = xenos::Compare((depthControl >> 4) & 7);
        draw.depthStencil.stencilEnabled = depthControl & 1;
        uint32_t stencilRef = GpuRegister(device, Reg::RB_STENCILREFMASK);
        draw.depthStencil.stencilReference = uint8_t(stencilRef);
        draw.depthStencil.stencilReadMask = uint8_t(stencilRef >> 8);
        draw.depthStencil.stencilWriteMask = uint8_t(stencilRef >> 16);
        draw.depthStencil.front = { xenos::StencilOp((depthControl >> 14) & 7),
            xenos::StencilOp((depthControl >> 11) & 7), xenos::StencilOp((depthControl >> 17) & 7),
            xenos::Compare((depthControl >> 8) & 7) };
        draw.depthStencil.back =
            (depthControl >> 7) & 1 ? plume::RenderStencilFaceDesc{ xenos::StencilOp((depthControl >> 26) & 7),
                xenos::StencilOp((depthControl >> 23) & 7), xenos::StencilOp((depthControl >> 29) & 7),
                xenos::Compare((depthControl >> 20) & 7) }
                                    : draw.depthStencil.front;

        uint32_t mode = GpuRegister(device, Reg::PA_SU_SC_MODE_CNTL);
        bool cullFront = mode & 1, cullBack = (mode >> 1) & 1;
        draw.raster.cullMode = cullFront && cullBack ? plume::RenderCullMode::NONE // both: nothing drawn; not handled
                               : cullFront           ? plume::RenderCullMode::FRONT
                               : cullBack            ? plume::RenderCullMode::BACK
                                                     : plume::RenderCullMode::NONE;
        draw.raster.frontFace =
            (mode >> 2) & 1 ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;

        // Polygon offset (depth bias): PA_SU_POLY_OFFSET_{FRONT,BACK}_{SCALE,OFFSET} (0x2380-0x2383), enabled by
        // PA_SU_SC_MODE_CNTL bit 11 (front faces), 12 (back faces) and 13 (points and lines, with the front values); the
        // host has one bias per draw, so the front one wins when both apply. Swap Force draws its lava canyon's and
        // crystal tunnel's G-buffer decals (vertex shader 1A20887C...) over the walls with a slope scale of 16 or 48 and
        // a GEQUAL test: without the offset they depth-fight the wall under them and the lava shimmers in patches from
        // frame to frame (games/swapforce/docs/shaders.md). The units follow Xenia's conversion (BSD): the slope scale
        // counts 1/16 subpixels, the constant offset is in units of 2^-19 of the 20e4 depth (2^-23 of a 24-bit one),
        // rounded away from zero; the slope is per guest pixel, so it grows with the host's scale. SKY_POLY_OFFSET=0
        // turns it off (to see the fault, or to check that tools/regress/analyze.py's shimmer check catches it).
        static const bool polyOffset = [] {
            const char* v = skyenv::Get("SKY_POLY_OFFSET");
            return v == nullptr || strcmp(v, "0") != 0;
        }();
        if (polyOffset)
        {
            const bool polygonal = primitive != 1 && primitive != 2 && primitive != 3 && primitive != 12;
            int face = -1; // 0 front, 1 back
            if (!polygonal)
                face = (mode >> 13) & 1 ? 0 : -1;
            else if ((mode >> 11) & 1 && !cullFront)
                face = 0;
            else if ((mode >> 12) & 1 && !cullBack)
                face = 1;
            if (face >= 0)
            {
                float scale = std::bit_cast<float>(GpuRegister(device, 0x2380 + 2 * face));
                float offset = std::bit_cast<float>(GpuRegister(device, 0x2381 + 2 * face));
                offset *= (GpuRegister(device, Reg::RB_DEPTH_INFO) >> 16) & 1 ? float(1 << 19) : float(1 << 23);
                draw.raster.depthBias = int32_t(std::ceil(std::fabs(offset))) * (offset < 0.0f ? -1 : 1);
                draw.raster.slopeScaledDepthBias = scale / 16.0f * float(Scale());
                static bool biasLogged = false;
                if (!biasLogged && (draw.raster.depthBias != 0 || draw.raster.slopeScaledDepthBias != 0.0f))
                {
                    biasLogged = true;
                    fprintf(stderr,
                        "[render] polygon offset first used: frame %llu, pixel shader %016llX, slope scale %g, offset "
                        "%g\n",
                        (unsigned long long)g_frame, (unsigned long long)g_drawPixelShader, scale,
                        std::bit_cast<float>(GpuRegister(device, 0x2381 + 2 * face)));
                }
            }
        }

        // Viewport from the PA_CL_VPORT registers (screen = ndc * scale + offset).
        auto regFloat = [&](uint32_t reg) { return std::bit_cast<float>(GpuRegister(device, reg)); };
        float xScale = regFloat(0x210F), xOffset = regFloat(0x2110), yScale = regFloat(0x2111),
              yOffset = regFloat(0x2112);
        float zScale = regFloat(0x2113), zOffset = regFloat(0x2114);
        uint32_t vte = GpuRegister(device, Reg::PA_CL_VTE_CNTL);
        const gpu::TextureDesc& size = gpu::Desc(sizeTarget);
        const bool frame = IsFrameTexture(size);
        const int32_t targetScale = TargetScale(size);
        draw.resolutionScale = float(targetScale);
        draw.positionScale[0] = draw.positionScale[1] = 1.0f;
        if ((vte & 1) == 0)
        {
            // Viewport transform disabled: shaders output pixel positions. Convert them to clip space in the shader
            // (g_PositionScale) and draw over the whole target.
            // The wide frame is still 1280 guest pixels for the game.
            float guestWidth = float(frame ? ultrawide::FrameWidth16x9 : size.width / targetScale);
            draw.positionScale[0] = 2.0f / guestWidth;
            draw.positionScale[1] = -2.0f / float(size.height / targetScale);
            draw.halfPixelOffset[0] = -1.0f;
            draw.halfPixelOffset[1] = 1.0f;
            xScale = guestWidth / 2;
            xOffset = xScale;
            yScale = -float(size.height / targetScale) / 2;
            yOffset = -yScale;
        }
        // An inverted depth range (MinZ > MaxZ) is kept as it is: Swap Force draws its scene with viewports 1..0.2 and
        // 1..0 into a depth buffer cleared to 0 with GEQUAL (reversed depth through the viewport). Swapping the two
        // inverted every depth: the sky dome, drawn last at the far plane, won every test and covered the scene.
        draw.viewport =
            plume::RenderViewport(xOffset - std::fabs(xScale), yOffset - std::fabs(yScale), 2 * std::fabs(xScale),
                2 * std::fabs(yScale), (vte & 0x20) ? zOffset : 0.0f, (vte & 0x10) ? zOffset + zScale : 1.0f);
        draw.viewport.x *= targetScale;
        draw.viewport.y *= targetScale;
        draw.viewport.width *= targetScale;
        draw.viewport.height *= targetScale;

        int32_t left = int32_t(Read32(device + Device::Scissor)), top = int32_t(Read32(device + Device::Scissor + 4));
        int32_t right = int32_t(Read32(device + Device::Scissor + 8)),
                bottom = int32_t(Read32(device + Device::Scissor + 12));
        left *= targetScale;
        top *= targetScale;
        right *= targetScale;
        bottom *= targetScale;
        draw.scissor =
            plume::RenderRect(std::clamp(left, 0, int32_t(size.width)), std::clamp(top, 0, int32_t(size.height)),
                std::clamp(right, 0, int32_t(size.width)), std::clamp(bottom, 0, int32_t(size.height)));

        // Alpha test (XenosRecomp clips alpha below the threshold).
        uint32_t colorControl = GpuRegister(device, Reg::RB_COLORCONTROL);
        if ((colorControl >> 3) & 1)
        {
            draw.specConstants |= gpu::SpecConstant::AlphaTest;
            draw.alphaThreshold = regFloat(Reg::RB_ALPHA_REF);
        }

        // Textures (fetch constants).
        uint32_t fetchConstants[gpu::MaxSamplers * 6];
        GpuRegisters(device, 0x4800, gpu::MaxSamplers * 6, fetchConstants);
        for (uint32_t slot = 0; slot < gpu::MaxSamplers; slot++)
        {
            xenos::TextureFetch fetch;
            for (uint32_t i = 0; i < 6; i++)
                fetch.dword[i] = fetchConstants[slot * 6 + i];
            if (fetch.Type() != 2)
                continue;
            draw.textures[slot] = TextureFor(fetch);
            auto host = g_textures.find(TextureKey(fetch));
            draw.swizzles[slot] = SwizzleFor(fetch, host != g_textures.end() && host->second.swapRB);
            if (host != g_textures.end() && host->second.resolved &&
                gpu::Desc(host->second.texture).width != fetch.Width())
                draw.scaledTextures |= 1u << slot; // a resolve: resolution-scale times its guest size
            draw.samplers[slot] = SamplerFor(fetch);
        }

        // A vertex shader's texture fetches (fetch constants 16-19), only for the samplers its constant table lists: the
        // slots keep their textures between draws.
        if (vs != nullptr && vs->samplers != 0)
        {
            uint32_t vertexFetch[gpu::MaxVertexSamplers * 6];
            GpuRegisters(device, 0x4800 + gpu::MaxSamplers * 6, gpu::MaxVertexSamplers * 6, vertexFetch);
            for (uint32_t slot = 0; slot < gpu::MaxVertexSamplers; slot++)
            {
                xenos::TextureFetch fetch;
                for (uint32_t i = 0; i < 6; i++)
                    fetch.dword[i] = vertexFetch[slot * 6 + i];
                if ((vs->samplers & (1u << slot)) == 0 || fetch.Type() != 2)
                    continue;
                draw.vertexTextures[slot] = TextureFor(fetch);
                auto host = g_textures.find(TextureKey(fetch));
                draw.vertexSwizzles[slot] = SwizzleFor(fetch, host != g_textures.end() && host->second.swapRB);
                draw.vertexSamplers[slot] = SamplerFor(fetch);
            }
        }

        // The wide frame (see BuildDraw): does the draw read the scene? (The slots hold stale textures too, so this only
        // says it can be the composite.)
        g_frameDraw = frame;
        g_frameSceneRead = false;
        if (frame)
            for (uint32_t slot = 0; slot < gpu::MaxSamplers; slot++)
                if ((game::UltrawideCompositeSlot < 0 || int(slot) == game::UltrawideCompositeSlot) &&
                    draw.textures[slot] != nullptr &&
                    gpu::Desc(draw.textures[slot]).width == ultrawide::SceneWidth() * Scale() &&
                    gpu::Desc(draw.textures[slot]).height == ultrawide::SceneHeight() * Scale())
                    g_frameSceneRead = true;

        draw.booleans = (GpuRegister(device, 0x4900) & 0xFFFF) | (GpuRegister(device, 0x4904) & 0xFFFF)
                                                                     << 16; // bool constants

        // Point sprites: SQ_PROGRAM_CNTL.param_gen (bit 18) puts the point coordinate in pixel shader register
        // SQ_CONTEXT_MISC.param_gen_pos (bits 8-15). Both come from the pixel shader's own header: the device's shadows of
        // those registers can still hold another shader's (D3D's FlushShaders sends them without updating the shadows).
        // A stale register made the level's ground saturated yellow; a stale enable bit replaced the sky dome's texture
        // coordinates with the pixel position (no clouds, or clouds every other frame).
        draw.paramGen = ps != nullptr ? ps->paramGen : 0;
        return true;
    }

    // Vertex declaration (header 0x34 bytes, element count at +0x18) and streams. `upData` != 0: one stream of
    // `upStride`-byte vertices at that guest address (BeginVertices).
    // Inputs a vertex shader declares but the vertex declaration lacks read a constant (as unbound fetches return): 0, or
    // 1 for colours. One vertex of each, given per instance.
    const float g_defaultInputs[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };

    // The input a vertex shader fetches at a computed index (GuestShader::pulled), as float4s: one per vertex of its
    // stream, `offset` bytes into each.
    std::vector<float> g_pulledData;

    void GatherPulled(
        gpu::DrawDesc& draw, uint32_t guest, uint32_t bytes, uint32_t stride, uint32_t offset, gpu::Format format)
    {
        uint32_t count = bytes / stride;
        g_pulledData.assign(size_t(count) * 4, 0.0f);
        uint32_t floats = format == gpu::Format::R32_FLOAT            ? 1
                          : format == gpu::Format::R32G32_FLOAT       ? 2
                          : format == gpu::Format::R32G32B32_FLOAT    ? 3
                          : format == gpu::Format::R32G32B32A32_FLOAT ? 4
                                                                      : 0;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t at = guest + i * stride + offset;
            float* out = &g_pulledData[size_t(i) * 4];
            uint32_t v = Read32(at);
            uint8_t bytes4[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16),
                uint8_t(v >> 24) }; // as the GPU reads them
            switch (format)
            {
            case gpu::Format::R8G8B8A8_UINT:
                for (uint32_t c = 0; c < 4; c++)
                    out[c] = bytes4[c];
                break;
            case gpu::Format::R8G8B8A8_UNORM:
                for (uint32_t c = 0; c < 4; c++)
                    out[c] = bytes4[c] / 255.0f;
                break;
            case gpu::Format::B8G8R8A8_UNORM:
                for (uint32_t c = 0; c < 4; c++)
                    out[c] = bytes4[c == 3 ? 3 : 2 - c] / 255.0f;
                break;
            default:
                for (uint32_t c = 0; c < floats; c++)
                    out[c] = g_memory.Read<float>(at + c * 4);
                if (floats == 0)
                {
                    static bool reported;
                    if (!reported)
                        fprintf(stderr, "[render] computed-index fetch of vertex format %d: not supported, reads 0\n",
                            int(format));
                    reported = true;
                }
                break;
            }
        }
        draw.pulledVertices = g_pulledData.data();
        draw.pulledCount = count;
    }

    // Streams kept on the GPU (gpu::CacheVertices) hold only the vertices the draw reads, [vertexStart, vertexEnd): a
    // stream can describe much more, such as the rest of the ring buffer the game streams vertices through.
    bool PrepareVertices(uint32_t device, gpu::DrawDesc& draw, uint32_t vertexShaderInputs, uint32_t pulledInputs,
        uint32_t vertexCount, uint32_t upData, uint32_t upStride, bool expand, uint32_t vertexStart, uint32_t vertexEnd)
    {
        draw.pulledCount = 0;
        uint32_t declaration = Read32(device + Device::VertexDeclaration);
        if (declaration == 0)
            return false;
        uint32_t elementCount = std::min(Read32(declaration + 0x18), gpu::MaxVertexElements);

        uint32_t provided = 0;
        uint32_t streamSlots[16];
        std::fill(std::begin(streamSlots), std::end(streamSlots), ~0u);
        uint32_t streamGuest[16], streamBytes[16], streamStride[16];
        g_streamData.resize(gpu::MaxVertexStreams);
        for (uint32_t i = 0; i < elementCount; i++)
        {
            uint32_t element = declaration + 0x34 + i * 12;
            uint32_t stream = g_memory.Read<uint16_t>(element);
            uint32_t offset = g_memory.Read<uint16_t>(element + 2);
            uint32_t type = Read32(element + 4);
            uint32_t usage = g_memory.Read<uint8_t>(element + 9);
            uint32_t usageIndex = g_memory.Read<uint8_t>(element + 10);
            if (stream >= 16)
                break;

            xenos::VertexFormatInfo format = xenos::VertexFormat(type);
            uint32_t location = InputLocation(usage, usageIndex);
            if (format.host == gpu::Format::UNKNOWN || location == ~0u)
                continue;

            bool normal = usage == 3 || usage == 6 || usage == 7;
            if (normal && format.packedNormal)
                draw.specConstants |= gpu::SpecConstant::R11G11B10Normal;

            // Blend indices are uint4 in the translated shaders: UBYTE4 (SSA) feeds them by value, float formats (Giants'
            // FLOAT4) as their bits, which the shader reinterprets.
            bool floatIndices = usage == 2 && AsUint(format.host) != format.host;
            if (floatIndices)
                draw.specConstants |= gpu::SpecConstant::BlendIndicesFloat;

            // Vertex data is byte-swapped in 32-bit words, which swaps the halves of 16-bit component pairs; for
            // texture coordinates the translated shaders swap them back (.yxwz) when told to.
            bool sixteenBitPairs =
                format.host == gpu::Format::R16G16_FLOAT || format.host == gpu::Format::R16G16_SINT ||
                format.host == gpu::Format::R16G16_SNORM || format.host == gpu::Format::R16G16_UNORM ||
                format.host == gpu::Format::R16G16B16A16_FLOAT || format.host == gpu::Format::R16G16B16A16_SINT ||
                format.host == gpu::Format::R16G16B16A16_SNORM || format.host == gpu::Format::R16G16B16A16_UNORM;
            if (usage == 5 && sixteenBitPairs)
                draw.swappedTexcoords |= 1u << usageIndex;

            if (streamSlots[stream] == ~0u)
            {
                uint32_t slot = draw.streamCount++;
                streamSlots[stream] = slot;
                std::vector<uint8_t>& data = g_streamData[slot];
                uint32_t stride, guest, bytes;
                if (upData != 0)
                {
                    stride = upStride;
                    guest = upData;
                    bytes = vertexCount * stride;
                }
                else
                {
                    // Stream s fetches through vertex fetch constant 95 - s.
                    stride = uint32_t(g_memory.Read<uint8_t>(device + Device::StreamStrides + stream)) * 4;
                    uint32_t fetch = 0x4800 + (95 - stream) * 2;
                    guest = PhysicalToGuest(GpuRegister(device, fetch) & ~3u);
                    bytes = ((GpuRegister(device, fetch + 1) >> 2) & 0xFFFFFF) * 4;
                }
                if (guest == 0 || stride == 0)
                    return false;
                streamGuest[stream] = guest;
                streamBytes[stream] = bytes;
                streamStride[stream] = stride;
                if (upData == 0 && !expand && !g_dump.active)
                {
                    // Keep the converted vertices on the GPU: level geometry is shared by many draws.
                    uint32_t begin = uint32_t(std::min<uint64_t>(uint64_t(vertexStart) * stride, bytes));
                    uint32_t end = uint32_t(std::clamp<uint64_t>(uint64_t(vertexEnd) * stride, begin, bytes));
                    draw.streams[slot] = { nullptr, end - begin, stride, false,
                        gpu::CacheVertices(g_memory.Translate(guest + begin), guest + begin, end - begin) };
                }
                else
                {
                    data.assign(static_cast<uint8_t*>(g_memory.Translate(guest)),
                        static_cast<uint8_t*>(g_memory.Translate(guest)) + bytes);
                    xenos::SwapEndian(data.data(), data.size(), 2);
                    draw.streams[slot] = { data.data(), bytes, stride, false, {} };
                }
            }

            gpu::VertexInput& input = draw.inputs[draw.inputCount++];
            input = { location, streamSlots[stream], offset,
                normal || floatIndices ? AsUint(format.host) : format.host };
            provided |= 1u << location;
            if (((pulledInputs >> location) & 1) && upData == 0) // (BeginVertices data arrives later; not seen pulled)
                GatherPulled(draw, streamGuest[stream], streamBytes[stream], streamStride[stream], offset, format.host);
        }
        if (draw.streamCount == 0)
            return false;

        uint32_t missing = vertexShaderInputs & ~provided;
        if (missing != 0 && draw.streamCount < gpu::MaxVertexStreams)
        {
            uint32_t slot = draw.streamCount++;
            draw.streams[slot] = { g_defaultInputs, sizeof(g_defaultInputs), sizeof(g_defaultInputs), true, {} };
            for (uint32_t location = 0; location < 32 && draw.inputCount < gpu::MaxVertexElements; location++)
            {
                if ((missing >> location) & 1)
                {
                    bool color = location == 8 || location == 11;
                    bool integer =
                        (location >= 1 && location <= 3) || location == 9; // normals and blend indices: uint4
                    draw.inputs[draw.inputCount++] = { location, slot, color ? 16u : 0u,
                        integer ? gpu::Format::R32G32B32A32_UINT : gpu::Format::R32G32B32A32_FLOAT };
                }
            }
        }
        return true;
    }

    // Quad lists: four vertices per quad, drawn as two triangles through an index list.
    void ExpandQuads(gpu::DrawDesc& draw, uint32_t vertexCount)
    {
        g_indexData.clear();
        for (uint32_t base = 0; base + 4 <= vertexCount; base += 4)
        {
            const uint32_t quad[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
            g_indexData.insert(g_indexData.end(), reinterpret_cast<const uint8_t*>(quad),
                reinterpret_cast<const uint8_t*>(quad) + sizeof(quad));
        }
        draw.indices = g_indexData.data();
        draw.indexBytes = uint32_t(g_indexData.size());
        draw.indices32 = true;
        draw.count = uint32_t(g_indexData.size() / 4);
        draw.first = 0;
    }

    uint32_t FloatComponents(gpu::Format format)
    {
        switch (format)
        {
        case gpu::Format::R32_FLOAT: return 1;
        case gpu::Format::R32G32_FLOAT: return 2;
        case gpu::Format::R32G32B32_FLOAT: return 3;
        case gpu::Format::R32G32B32A32_FLOAT: return 4;
        default: return 0;
        }
    }

    // Rectangle lists give three corners per rectangle (stream 0): the fourth is v1 + v2 - v0, attribute by attribute
    // (float attributes; others repeat v0). Each rectangle becomes two triangles.
    std::vector<uint8_t> g_rectangleVertices;

    void ExpandRectangles(gpu::DrawDesc& draw, uint32_t vertexCount)
    {
        const gpu::VertexStream& stream = draw.streams[0];
        uint32_t stride = stream.stride;
        auto source = static_cast<const uint8_t*>(stream.data) + size_t(draw.first) * stride;
        vertexCount = std::min(vertexCount, stream.size / stride - std::min(draw.first, stream.size / stride));
        uint32_t rectangles = vertexCount / 3;

        g_rectangleVertices.resize(size_t(rectangles) * 4 * stride);
        g_indexData.clear();
        for (uint32_t r = 0; r < rectangles; r++)
        {
            const uint8_t* v = source + size_t(r) * 3 * stride;
            uint8_t* out = &g_rectangleVertices[size_t(r) * 4 * stride];
            memcpy(out, v, size_t(3) * stride);
            memcpy(out + 3 * stride, v, stride);
            for (uint32_t i = 0; i < draw.inputCount; i++)
            {
                const gpu::VertexInput& input = draw.inputs[i];
                uint32_t components = input.stream == 0 ? FloatComponents(input.format) : 0;
                for (uint32_t c = 0; c < components; c++)
                {
                    float corner[3];
                    for (uint32_t k = 0; k < 3; k++)
                        memcpy(&corner[k], v + k * stride + input.offset + c * 4, 4);
                    float fourth = corner[1] + corner[2] - corner[0];
                    memcpy(out + 3 * stride + input.offset + c * 4, &fourth, 4);
                }
            }
            const uint32_t base = r * 4;
            const uint32_t triangles[6] = { base, base + 1, base + 2, base + 2, base + 1, base + 3 };
            g_indexData.insert(g_indexData.end(), reinterpret_cast<const uint8_t*>(triangles),
                reinterpret_cast<const uint8_t*>(triangles) + sizeof(triangles));
        }
        draw.streams[0].data = g_rectangleVertices.data();
        draw.streams[0].size = uint32_t(g_rectangleVertices.size());
        draw.indices = g_indexData.data();
        draw.indexBytes = uint32_t(g_indexData.size());
        draw.indices32 = true;
        draw.count = rectangles * 6;
        draw.first = 0;
    }

    // Builds the host draw from the device state now. The data it points to (g_streamData, g_indexData, g_constants)
    // stays valid until the next BuildDraw.
    bool BuildDraw(uint32_t device, uint32_t primitive, uint32_t first, uint32_t count, uint32_t upData,
        uint32_t upStride, bool indexed, int32_t baseVertex, gpu::DrawDesc& draw)
    {
        if (!PrepareDraw(device, primitive, draw))
            return false;
        if (g_frameDraw)
        {
            // The wide frame. The composite (read from the scene: SSA's one triangle, Giants' one quad) fills it; the rest
            // (HUD, menus, movies) is centred.
            const gpu::TextureDesc& size =
                gpu::Desc(draw.colorTargetCount != 0 ? draw.colorTargets[0] : draw.depthTarget);
#ifdef GAME_ULTRAWIDE_COMPOSITE_INDEXED
            g_frameComposite = g_frameSceneRead && indexed && count == game::UltrawideCompositeVertices;
#else
            g_frameComposite = g_frameSceneRead && !indexed && count == game::UltrawideCompositeVertices;
#endif
            if (g_frameComposite && ultrawide::MoviePlaying())
            {
                // A movie is on: the scene behind it stays 16:9 (centred below) and the sides are black bars.
                const float black[4] = { 0, 0, 0, 1 };
                gpu::ClearColor(draw.colorTargets[0], black, nullptr);
                g_frameComposite = false;
            }
            if (g_frameComposite)
            {
                draw.viewport = plume::RenderViewport(
                    0, 0, float(size.width), float(size.height), draw.viewport.minDepth, draw.viewport.maxDepth);
                draw.scissor = plume::RenderRect(0, 0, int32_t(size.width), int32_t(size.height));
            }
            else
            {
                const int32_t shift = FrameShift();
                draw.viewport.x += float(shift);
                draw.scissor.left = std::min(draw.scissor.left + shift, int32_t(size.width));
                draw.scissor.right = std::min(draw.scissor.right + shift, int32_t(size.width));
            }
        }

        uint32_t vertexCount = indexed ? 0 : first + count;
        uint32_t vertexStart = 0, vertexEnd = UINT32_MAX; // the vertices the draw reads
        if (indexed)
        {
            // Index buffer: +0x18 address; 16-bit indices (32-bit index buffers not seen yet).
            uint32_t indices = Read32(device + Device::Indices);
            uint32_t guest = indices != 0 ? PhysicalToGuest(Read32(indices + Resource::IndexBufferAddress)) : 0;
            if (guest == 0)
                return false;
            uint32_t bytes = count * 2;
            if (!g_dump.active)
            {
                uint32_t minIndex, maxIndex;
                draw.indicesConverted = gpu::CacheIndices(
                    g_memory.Translate(guest + first * 2), guest + first * 2, bytes, minIndex, maxIndex);
                if (draw.indicesConverted.buffer != nullptr && baseVertex >= 0)
                {
                    vertexStart = uint32_t(baseVertex);
                    vertexEnd = uint32_t(baseVertex) + maxIndex + 1;
                }
            }
            else
            {
                g_indexData.resize(bytes);
                memcpy(g_indexData.data(), g_memory.Translate(guest + first * 2), bytes);
                xenos::SwapEndian(g_indexData.data(), g_indexData.size(), 1);
                draw.indices = g_indexData.data();
            }
            draw.indexBytes = bytes;
            draw.count = count;
            draw.baseVertex = baseVertex;
        }
        else
        {
            draw.count = count;
            draw.first = first;
            vertexStart = first;
            vertexEnd = first + count;
        }

        bool expand = !indexed && (primitive == 13 || (primitive == 8 && upData == 0));
        const GuestShader* vs = FindGuestShader(Read32(device + Device::VertexShader));
        if (!PrepareVertices(device, draw, vs != nullptr ? vs->inputs : 0, vs != nullptr ? vs->pulled : 0, vertexCount,
                upData, upStride, expand, vertexStart, vertexEnd))
            return false;
        if (upData == 0 && !expand && !g_dump.active)
        {
            // The cached streams start at vertexStart.
            if (indexed)
                draw.baseVertex -= int32_t(vertexStart);
            else
                draw.first -= vertexStart;
        }
        if (!indexed && primitive == 13)
            ExpandQuads(draw, count);
        if (!indexed && primitive == 8 && upData == 0)
            ExpandRectangles(draw, count);

        // FXAA's corner taps (fetch offsets of +-0.5 guest texels) and steps (c0, c1, from the guest's frame size) land N host
        // pixels apart at scale N: an edge filter that wide also softens what the higher resolution added. Measured at
        // scale 2: 2.9 % of the pixels changed and 20 % of the sharpness lost, against 1.4 % and 13 % in host pixels.
        const bool fxaaPass = game::HostKernelPixelShader != 0 && g_drawPixelShader == game::HostKernelPixelShader;
        const bool hostKernel = fxaaPass && Scale() > 1;
        // The "Edge smoothing" setting: Game leaves the pass's contrast thresholds, Light and Off raise them (gpu/edge_smoothing.h).
        const edgesmoothing::Mode smoothing = fxaaPass ? EdgeSmoothing() : edgesmoothing::Game;
        if (upData != 0 || g_dump.active || (g_frameComposite && game::UltrawideCompositeVpos) || hostKernel ||
            smoothing != edgesmoothing::Game)
        {
            // A BeginVertices draw captures the constants when the buffer is requested, before the game fills the
            // vertices (and may change state); the debug dump wants the same host-order copy.
            ReadConstants(device, g_constants.data());
            draw.vertexConstants = g_constants.data();
            draw.pixelConstants = g_constants.data() + 256 * 4;
            if (hostKernel)
            {
                HostKernel kernel{ .scaledTextures = draw.scaledTextures };
                std::copy_n(draw.pixelConstants, kernel.constants.size(), kernel.constants.begin());
                kernel = ScaleHostKernel(kernel, Scale());
                std::copy(kernel.constants.begin(), kernel.constants.end(), g_constants.data() + 256 * 4);
                draw.scaledTextures = kernel.scaledTextures;
            }
            if (smoothing != edgesmoothing::Game)
            {
                // The threshold and the minimum are two consecutive vec4 of the pixel constants (game::HostKernelThreshold).
                const bool off = smoothing == edgesmoothing::Off;
                float* constants = g_constants.data() + 256 * 4 + game::HostKernelThreshold;
                for (uint32_t i = 0; i < 4; i++)
                {
                    constants[i] = off ? edgesmoothing::OffThreshold : edgesmoothing::LightThreshold;
                    constants[4 + i] = off ? edgesmoothing::OffMinimum : edgesmoothing::LightMinimum;
                }
            }
            if (g_frameComposite && game::UltrawideCompositeVpos)
            {
                // The composite reads the scene at (pixel position) x VposParams.xy + VposParams.zw (pixel constant c0),
                // set for 1280 pixels across: the same picture over the wider frame.
                float* vpos = g_constants.data() + 256 * 4;
                const float ratio = float(ultrawide::FrameWidth16x9) / float(ultrawide::FrameWidth());
                vpos[0] *= ratio;
                vpos[2] *= ratio;
            }
        }
        else
        {
            draw.guestConstants = GpuFloatConstants(device, &draw.guestConstantsId,
                &draw.guestConstantGroups); // valid until the next call: gpu::Draw copies it
        }
        if (g_dump.active)
        {
            fprintf(stderr,
                "[dump] prim %u count %u first %u idx %d vp %.1f %.1f %.1f %.1f z %.2f-%.2f sc %d %d %d %d pos %.3f "
                "%.3f blend %d %d/%d mask %x spec %x "
                "depthctl %08x stencil %08x targets %u%s vs %016llx ps %016llx sumode %08x polyoffset %g %g %g %g\n",
                primitive, draw.count, draw.first, draw.indices != nullptr, draw.viewport.x, draw.viewport.y,
                draw.viewport.width, draw.viewport.height, draw.viewport.minDepth, draw.viewport.maxDepth,
                draw.scissor.left, draw.scissor.top, draw.scissor.right, draw.scissor.bottom, draw.positionScale[0],
                draw.positionScale[1], draw.blend[0].enabled, int(draw.blend[0].srcColor), int(draw.blend[0].dstColor),
                draw.blend[0].writeMask, draw.specConstants, GpuRegister(device, Reg::RB_DEPTHCONTROL),
                GpuRegister(device, Reg::RB_STENCILREFMASK), draw.colorTargetCount,
                draw.depthTarget != nullptr ? "+depth" : "", static_cast<unsigned long long>(g_drawVertexShader),
                static_cast<unsigned long long>(g_drawPixelShader), GpuRegister(device, Reg::PA_SU_SC_MODE_CNTL),
                std::bit_cast<float>(GpuRegister(device, 0x2380)), std::bit_cast<float>(GpuRegister(device, 0x2381)),
                std::bit_cast<float>(GpuRegister(device, 0x2382)), std::bit_cast<float>(GpuRegister(device, 0x2383)));
            for (uint32_t i = 0; i < draw.inputCount; i++)
                fprintf(stderr, "[dump]   input loc %u stream %u off %u fmt %d\n", draw.inputs[i].location,
                    draw.inputs[i].stream, draw.inputs[i].offset, int(draw.inputs[i].format));
            for (uint32_t i = 0; i < draw.streamCount; i++)
            {
                const float* f = static_cast<const float*>(draw.streams[i].data);
                fprintf(stderr, "[dump]   stream %u size %u stride %u inst %d first floats %g %g %g %g %g %g\n", i,
                    draw.streams[i].size, draw.streams[i].stride, draw.streams[i].perInstance, f[0], f[1], f[2], f[3],
                    f[4], f[5]);
            }
            static std::vector<const gpu::Texture*> dumped;
            for (uint32_t i = 0; i < gpu::MaxSamplers; i++)
                if (draw.textures[i] != nullptr)
                {
                    // The sampler and the fetch constant's dwords 3-5 (filters, mip levels, LOD bias, mip address).
                    uint32_t fetch[6];
                    GpuRegisters(device, 0x4800 + i * 6, 6, fetch);
                    const gpu::SamplerState& sampler = draw.samplers[i];
                    fprintf(stderr,
                        "[dump]   tex %u %ux%u fmt %d levels %u filter %d%d%d aniso %u lod %g-%g bias %g fetch %08x "
                        "%08x %08x\n",
                        i, gpu::Desc(draw.textures[i]).width, gpu::Desc(draw.textures[i]).height,
                        int(gpu::Desc(draw.textures[i]).format), gpu::Desc(draw.textures[i]).mipLevels,
                        int(sampler.minFilter), int(sampler.magFilter), int(sampler.mipFilter), sampler.maxAnisotropy,
                        sampler.minLod, sampler.maxLod, sampler.mipLodBias, fetch[3], fetch[4], fetch[5]);
                    if (std::find(dumped.begin(), dumped.end(), draw.textures[i]) == dumped.end() &&
                        gpu::Desc(draw.textures[i]).format != gpu::Format::BC3_UNORM)
                    {
                        dumped.push_back(draw.textures[i]);
                        DumpResolve(draw.textures[i], 't');
                    }
                }
            fprintf(stderr, "[dump]   vs c0 %g %g %g %g c4 %g %g %g %g\n", g_constants[0], g_constants[1],
                g_constants[2], g_constants[3], g_constants[16], g_constants[17], g_constants[18], g_constants[19]);
            // SKY_DUMP_VS_CONSTANTS=<n>: the vertex shader's c0 to c<n-1> too (a debugging aid: matrices, bone palettes,
            // motion-blur inputs).
            static const uint32_t vsConstants =
                skyenv::Get("SKY_DUMP_VS_CONSTANTS") != nullptr
                    ? std::clamp(uint32_t(atoi(skyenv::Get("SKY_DUMP_VS_CONSTANTS"))), 1u, 256u)
                    : 0;
            if (vsConstants != 0)
            {
                fprintf(stderr, "[dump]   vs c0-c%u", vsConstants - 1);
                for (uint32_t i = 0; i < vsConstants * 4; i++)
                    fprintf(stderr, " %g", g_constants[i]);
                fprintf(stderr, "\n");
            }
            const float* ps = g_constants.data() + 256 * 4;
            fprintf(stderr, "[dump]   ps c0-c5");
            for (uint32_t i = 0; i < 24; i++)
                fprintf(stderr, " %g", ps[i]);
            fprintf(stderr, "\n");
        }
        return true;
    }

    void IssueDraw(
        uint32_t device, uint32_t primitive, uint32_t first, uint32_t count, bool indexed, int32_t baseVertex)
    {
        gpu::DrawDesc draw;
        if (BuildDraw(device, primitive, first, count, 0, 0, indexed, baseVertex, draw))
        {
            gpu::Draw(draw);
            g_stats.draws++;
            if (draw.colorTargetCount != 0)
                DumpResolve(draw.colorTargets[0], 'd');
        }
    }

    // BeginVertices hands the game a buffer it fills after the call: the draw is issued at the next D3D call.
    void FlushPendingVertices()
    {
        PendingVertices& p = g_pendingVertices;
        if (!p.pending)
            return;
        p.pending = false;

        // The vertex stream of a BeginVertices draw is always the first one (PrepareVertices creates it first).
        std::vector<uint8_t>& data = g_streamData[0];
        data.assign(static_cast<uint8_t*>(g_memory.Translate(p.guestData)),
            static_cast<uint8_t*>(g_memory.Translate(p.guestData)) + p.bytes);
        xenos::SwapEndian(data.data(), data.size(), 2);
        p.draw.streams[0].data = data.data();
        if (p.primitive == 8)
            ExpandRectangles(p.draw, p.count);
        gpu::Draw(p.draw);
        g_stats.draws++;
        if (p.draw.colorTargetCount != 0)
            DumpResolve(p.draw.colorTargets[0], 'd');
    }

    // --- Resolves and the frame ---------------------------------------------------------------------------------------

    void AdvanceFrameDump()
    {
        static const char* value = skyenv::Get("SKY_DUMP_FRAME");
        if (value == nullptr || g_dump.done)
            return;
        // SKY_DUMP_FRAMES=<n>: n consecutive frames (default 1), each opened by a "[dump] frame <swap> at <seconds>" line.
        static const int frames =
            std::max(skyenv::Get("SKY_DUMP_FRAMES") != nullptr ? atoi(skyenv::Get("SKY_DUMP_FRAMES")) : 1, 1);
        static int dumped = 0;
        if (g_dump.active)
        {
            if (++dumped < frames)
            {
                fprintf(stderr, "[dump] frame %llu at %.4f s\n", static_cast<unsigned long long>(g_frame),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - g_dump.start).count());
                return;
            }
            g_dump.active = false;
            g_dump.done = true;
        }
        else if (value[0] == 'f'
                     ? g_frame > strtoull(value + 1, nullptr, 10) // "f<N>": the frame after the N-th swap
                     : std::chrono::duration<double>(std::chrono::steady_clock::now() - g_dump.start).count() >=
                           atof(value))
        {
            g_dump.active = true;
            fprintf(stderr, "[dump] frame %llu at %.4f s\n", static_cast<unsigned long long>(g_frame),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - g_dump.start).count());
        }
    }

    // D3DDevice_Resolve's arguments. Flags: bits 0-2 source (0-3 colour target, 4 depth), 4-6 samples, 0x100 clear the
    // colour target and 0x200 the depth-stencil surface afterwards (with clearColor, clearZ, clearStencil), bits 26-31
    // exponent bias (not applied yet). The guest resets its RB_COPY_* shadows before returning: nothing is read from them.
    struct ResolveArgs
    {
        uint32_t flags, rect, destination, point, slice, clearColor, clearStencil;
        float clearZ;
    };

    // Xenos 20e4 depth (D24FS8): 4-bit exponent with bias 15, 20-bit mantissa, values in [0, 2).
    uint32_t Float32To20e4(float value)
    {
        uint32_t bits = std::bit_cast<uint32_t>(value);
        if (!(value > 0.0f))
            return 0;
        if (bits >= 0x3FFFFFF8) // rounds to 2 or more (or NaN, infinity)
            return 0xFFFFFF;
        if (bits < 0x38800000) // below 2^-14: denormal
            return uint32_t(value * 17179869184.0f + 0.5f); // * 2^34
        bits -= 112u << 23; // exponent bias 127 -> 15
        return (bits + 3 + ((bits >> 3) & 1)) >> 3; // to 20 mantissa bits, nearest even
    }

    // The game's CPU reads the scene depth it resolves into a k_24_8_FLOAT texture: the light code classifies each
    // 32x32 screen tile from it (sub_821404D8, 4 KB per tile: which sun pass, with which shadow filter, draws it).
    // Resolves stay on the host GPU, so that copy is also written to guest memory as the 360 stores it: tiled, depth
    // in bits 31-8 as 20e4 (stencil 0), big-endian, one texel per guest pixel at higher resolution scales. Only the
    // resolve the job reads is written back (g_cpuReadDepth, a physical address), and two frames late, so that the
    // read-back never stalls (waiting for the frame before cost 6-11 ms per frame at scale 2): the job runs on its own
    // thread, loosely tied to the frame (before or after the next swap), and reads whatever is in memory.
    std::atomic<uint32_t> g_cpuReadDepth;

    void WriteBackDepth(const xenos::TextureFetch& fetch, const gpu::Texture* texture, uint32_t x, uint32_t y,
        uint32_t width, uint32_t height)
    {
        const xenos::TextureFormatInfo& info = xenos::TextureFormat(fetch.Format());
        xenos::TextureLevel level = xenos::TextureLevelLayout(fetch, info, 0);
        uint32_t guest = PhysicalToGuest(level.address);
        if (guest == 0)
            return;
        // The guest image, built on the GPU thread (texels the resolve did not cover are 0), copied here, where the
        // resolve says the memory is still that texture's.
        bool tiled = fetch.Tiled();
        uint32_t pitch = level.pitchBlocks, rows = tiled ? (level.height + 31) & ~31u : level.height;
        std::vector<uint32_t> image;
        if (gpu::CollectReadback(image) && image.size() == size_t(pitch) * rows) // two frames old
            memcpy(g_memory.Translate(guest), image.data(), image.size() * 4);

        uint32_t scale = uint32_t(Scale());
        uint32_t left = x / scale, top = y / scale;
        uint32_t right = std::min((x + width) / scale, level.width),
                 bottom = std::min((y + height) / scale, level.height);
        gpu::RequestReadback(texture, scale,
            [=](const uint32_t* depth, uint32_t depthWidth, uint32_t depthHeight, std::vector<uint32_t>& out) {
                out.assign(size_t(pitch) * rows, 0);
                for (uint32_t row = top; row < std::min(bottom, depthHeight); row++)
                    for (uint32_t column = left; column < std::min(right, depthWidth); column++)
                    {
                        uint32_t offset = tiled ? xenos::Tiled2D(column, row, pitch, 2) / 4 : row * pitch + column;
                        out[offset] = ByteSwap(
                            Float32To20e4(std::bit_cast<float>(depth[size_t(row) * depthWidth + column])) << 8);
                    }
            });
    }

    void Resolve(uint32_t device, const ResolveArgs& args)
    {
        g_stats.resolves++;
        if ((args.flags >> 26) != 0)
        {
            static bool reported;
            if (!reported)
                fprintf(stderr, "[render] resolve with exponent bias %d: not applied\n", int32_t(args.flags) >> 26);
            reported = true;
        }
        uint32_t source = args.flags & 7;
        bool depth = source == 4;
        gpu::Texture* target = TargetFor(
            device, Read32(device + (depth ? Device::DepthStencil : Device::RenderTargets + source * 4)), depth);
        if (target == nullptr)
            return;

        const bool frame = !depth && IsFrameTexture(gpu::Desc(target));
        const int32_t scale = TargetScale(gpu::Desc(target));
        int32_t left = 0, top = 0, right = int32_t(gpu::Desc(target).width), bottom = int32_t(gpu::Desc(target).height);
        if (args.rect != 0)
        {
            left = std::max(int32_t(Read32(args.rect)) * scale, 0);
            top = std::max(int32_t(Read32(args.rect + 4)) * scale, 0);
            // A rectangle over the guest's whole frame takes the whole wide one. The tiles of a predicated-tiling frame
            // (Giants: 0-864 and 864-1280) take the wide frame from its left edge, or to its right edge, and meet at the
            // centred position of their common edge.
            int32_t guestRight = int32_t(Read32(args.rect + 8)) * scale;
            if (frame && left == 0 && guestRight >= int32_t(ultrawide::FrameWidth16x9) * scale)
                ; // the whole wide frame
            else if (frame && game::ExpandTiledTargets)
            {
                if (guestRight >= int32_t(ultrawide::FrameWidth16x9) * scale)
                    guestRight = right;
                else
                    guestRight += FrameShift();
                if (left > 0)
                    left += FrameShift();
                right = std::min(guestRight, right);
            }
            else
                right = std::min(guestRight, right);
            bottom = std::min(int32_t(Read32(args.rect + 12)) * scale, bottom);
        }

        if (args.destination != 0)
        {
            xenos::TextureFetch fetch;
            for (uint32_t i = 0; i < 6; i++)
                fetch.dword[i] = Read32(args.destination + Resource::TextureFetch + i * 4);
            HostTexture& host = g_textures[TextureKey(fetch)];
            // Depth (k_24_8, k_24_8_FLOAT textures) is sampled as its value in X, as Xenia converts it.
            gpu::Format format = depth ? gpu::Format::R32_FLOAT : gpu::Desc(target).format;
            // The frame's copy is as wide as the frame: that is what the swap presents.
            const uint32_t hostWidth = frame ? gpu::Desc(target).width : fetch.Width() * scale;
            // A cube texture receives one resolve per face (`slice`): the level's ambient cube map is built this way.
            bool cube = !depth && fetch.Dimension() == 3;
            if (host.texture == nullptr || gpu::Desc(host.texture).format != format ||
                gpu::Desc(host.texture).cube != cube || gpu::Desc(host.texture).width != hostWidth)
            {
                gpu::DestroyTexture(host.texture);
                host.texture = gpu::CreateTexture({ .width = hostWidth,
                    .height = fetch.Height() * scale,
                    .arraySize = cube ? 6u : 1u,
                    .format = format,
                    .usage = gpu::TextureUsage::Sampled | gpu::TextureUsage::RenderTarget,
                    .cube = cube });
            }
            host.resolved = true;
            g_resolvedBases.insert(fetch.BaseAddress());
            // The guest swaps red and blue (RB_COPY_DEST_INFO.copy_dest_swap) when the destination's swizzle does not
            // start with X, as for ARGB textures (ZYXW): the host copy keeps the target's order, so sampling swaps back.
            host.swapRB = !depth && ((fetch.dword[3] >> 1) & 7) != 0;

            uint32_t x = args.point != 0 ? Read32(args.point) * scale : 0,
                     y = args.point != 0 ? Read32(args.point + 4) * scale : 0;
            if (frame && game::ExpandTiledTargets && ultrawide::Active())
                x = uint32_t(
                    left); // a tile of the wide frame lands where it was read (the copy is as wide as the frame)
            int32_t copyRight = std::min(right, left + int32_t(gpu::Desc(host.texture).width) - int32_t(x));
            int32_t copyBottom = std::min(bottom, top + int32_t(gpu::Desc(host.texture).height) - int32_t(y));
            if (copyRight > left && copyBottom > top)
            {
                if (depth)
                {
                    gpu::ResolveDepth(host.texture, x, y, target, plume::RenderRect(left, top, copyRight, copyBottom));
                    if ((fetch.BaseAddress() & 0x1FFFFFFF) == g_cpuReadDepth.load(std::memory_order_relaxed))
                        WriteBackDepth(
                            fetch, host.texture, x, y, uint32_t(copyRight - left), uint32_t(copyBottom - top));
                }
                else
                    gpu::CopyTexture(host.texture, x, y, target, plume::RenderRect(left, top, copyRight, copyBottom),
                        cube ? args.slice % 6 : 0);
            }
            DumpResolve(host.texture);
        }

        if (g_dump.active)
            fprintf(stderr, "[dump] resolve flags %x src %ux%u rect %d %d %d %d dest %08x depthsurf %08x (file %02u)\n",
                args.flags, gpu::Desc(target).width, gpu::Desc(target).height, left, top, right, bottom,
                args.destination, Read32(device + Device::DepthStencil), g_dump.index - 1);
        // A resolve can also clear what it copied: the colour target, and/or the bound depth-stencil surface.
        if (right <= left || bottom <= top)
            return;
        plume::RenderRect region(left, top, right, bottom);
        const plume::RenderRect* clearRect = left == 0 && top == 0 && right == int32_t(gpu::Desc(target).width) &&
                                                     bottom == int32_t(gpu::Desc(target).height)
                                                 ? nullptr
                                                 : &region;
        if ((args.flags & 0x100) && !depth)
        {
            float rgba[4] = {};
            if (args.clearColor != 0)
                for (uint32_t i = 0; i < 4; i++)
                    rgba[i] = g_memory.Read<float>(args.clearColor + i * 4);
            gpu::ClearColor(target, rgba, clearRect);
        }
        if (args.flags & 0x200)
        {
            gpu::Texture* depthTarget = depth ? target : TargetFor(device, Read32(device + Device::DepthStencil), true);
            if (depthTarget != nullptr)
                gpu::ClearDepthStencil(
                    depthTarget, true, true, args.clearZ, uint8_t(args.clearStencil), depth ? clearRect : nullptr);
        }
    }

    // D3DDevice_Clear/ClearF (and BeginTiling, BeginZPass) end in 0x822ADDE0, which clears with a rectangle draw of its
    // own that never reaches the draw hooks. Flags: bits 0-3 colour targets 0-3, 0x10 depth, 0x20 stencil. Like the
    // guest, the rectangle is clipped to the viewport and, with the scissor test on, to the scissor rectangle.
    void Clear(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, uint32_t stencil, float z)
    {
        uint32_t depthSurface = Read32(device + Device::DepthStencil);
        if (depthSurface == 0)
            flags &= ~0xF0u;
        auto viewport = [&](uint32_t i) { return int32_t(g_memory.Read<float>(device + Device::Viewport + i * 4)); };
        int32_t vx = viewport(0), vy = viewport(1);
        int32_t left = std::max(int32_t(Read32(rect)), vx), top = std::max(int32_t(Read32(rect + 4)), vy);
        int32_t right = std::min(int32_t(Read32(rect + 8)), vx + viewport(2));
        int32_t bottom = std::min(int32_t(Read32(rect + 12)), vy + viewport(3));
        if (Read32(device + Device::ScissorTestEnable) != 0)
        {
            left = std::max(left, int32_t(Read32(device + Device::Scissor)));
            top = std::max(top, int32_t(Read32(device + Device::Scissor + 4)));
            right = std::min(right, int32_t(Read32(device + Device::Scissor + 8)));
            bottom = std::min(bottom, int32_t(Read32(device + Device::Scissor + 12)));
        }
        if (right <= left || bottom <= top)
            return;

        float rgba[4] = {};
        if (color != 0)
            for (uint32_t i = 0; i < 4; i++)
                rgba[i] = g_memory.Read<float>(color + i * 4);

        // The whole target: a load-action clear, cheaper than plume's rectangle clear.
        auto clearRect = [&](const gpu::Texture* target, plume::RenderRect& out) -> const plume::RenderRect* {
            const gpu::TextureDesc& desc = gpu::Desc(target);
            const int32_t scale = TargetScale(desc);
            int32_t l = left * scale, r = right * scale;
            if (IsFrameTexture(desc))
            {
                // The wide frame: a clear of the guest's whole frame clears all of it, a smaller one is centred.
                if (l <= 0 && r >= int32_t(ultrawide::FrameWidth16x9) * Scale())
                    r = int32_t(desc.width);
                else
                    l += FrameShift(), r += FrameShift();
            }
            out = plume::RenderRect(std::max(l, 0), std::max(top * scale, 0), std::min(r, int32_t(desc.width)),
                std::min(bottom * scale, int32_t(desc.height)));
            if (out.left == 0 && out.top == 0 && out.right == int32_t(desc.width) && out.bottom == int32_t(desc.height))
                return nullptr;
            return &out;
        };
        plume::RenderRect region;
        for (uint32_t i = 0; i < 4; i++)
        {
            gpu::Texture* target =
                (flags >> i) & 1 ? TargetFor(device, Read32(device + Device::RenderTargets + i * 4), false) : nullptr;
            if (target != nullptr)
            {
                const plume::RenderRect* r = clearRect(target, region);
                if (r == nullptr || (r->right > r->left && r->bottom > r->top))
                    gpu::ClearColor(target, rgba, r);
            }
        }
        gpu::Texture* depthTarget = flags & 0x30 ? TargetFor(device, depthSurface, true) : nullptr;
        if (depthTarget != nullptr)
        {
            const plume::RenderRect* r = clearRect(depthTarget, region);
            if (r == nullptr || (r->right > r->left && r->bottom > r->top))
                gpu::ClearDepthStencil(depthTarget, flags & 0x10, flags & 0x20, z, uint8_t(stencil), r);
        }
    }
}

void d3d::Draw(uint32_t device, uint32_t primitive, uint32_t first, uint32_t count, bool indexed, int32_t baseVertex)
{
    std::lock_guard lock(g_mutex);
    FlushPendingVertices();
    IssueDraw(device, primitive, first, count, indexed, baseVertex);
}

namespace
{
    uint64_t NowNs()
    {
        return uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // The perf log's renderer time (gpu::AddFrameTime) for one hooked D3D call: all of it but what Exclude runs (the
    // guest's own function, the vblank wait, the present), which nested hooks count themselves.
    class RendererTimer
    {
    public:
        template <typename F>
        void Exclude(F&& function)
        {
            uint64_t start = NowNs();
            function();
            m_excluded += NowNs() - start;
        }
        ~RendererTimer() { gpu::AddFrameTime(gpu::FrameTime::Renderer, NowNs() - m_start - m_excluded); }

    private:
        uint64_t m_start = NowNs();
        uint64_t m_excluded = 0;
    };
}

#ifdef GAME_HOOK_D3DDevice_DrawVertices
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_DrawVertices));
// D3DDevice_DrawVertices(device, primitiveType, startVertex, vertexCount)
// Both draw calls are drawn once the guest's call has written the state it sends into the ring (d3d/ring.h).
PPC_FUNC(GAME_HOOK(D3DDevice_DrawVertices))
{
    RendererTimer timer;
    uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
    CaptureDrawVertices(ctx);
    bool recorded;
    {
        std::lock_guard lock(g_mutex);
        FlushPendingVertices();
        recorded = RecordDraw(device, primitive, start, count, false, 0);
    }
    timer.Exclude([&] { GAME_IMP(D3DDevice_DrawVertices)(ctx, base); });
    ScanRing(device);
    std::lock_guard lock(g_mutex);
    if (recorded)
        FinishRecordedDraw();
    else
        IssueDraw(device, primitive, start, count, false, 0);
    DrawConsumed();
}
#endif

#ifdef GAME_HOOK_D3DDevice_DrawIndexedVertices
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_DrawIndexedVertices));
// D3DDevice_DrawIndexedVertices(device, primitiveType, baseVertex, startIndex, indexCount)
PPC_FUNC(GAME_HOOK(D3DDevice_DrawIndexedVertices))
{
    RendererTimer timer;
    // (device, primitive, base vertex, start index, index count); newer XDKs drop the base vertex (Giants).
    uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32;
    uint32_t start = game::DrawIndexedBaseVertex ? ctx.r6.u32 : ctx.r5.u32;
    uint32_t count = game::DrawIndexedBaseVertex ? ctx.r7.u32 : ctx.r6.u32;
    int32_t baseVertex = game::DrawIndexedBaseVertex ? int32_t(ctx.r5.u32) : 0;
    CaptureDrawIndexedVertices(ctx);
    bool recorded;
    {
        std::lock_guard lock(g_mutex);
        FlushPendingVertices();
        recorded = RecordDraw(device, primitive, start, count, true, baseVertex);
    }
    timer.Exclude([&] { GAME_IMP(D3DDevice_DrawIndexedVertices)(ctx, base); });
    ScanRing(device);
    std::lock_guard lock(g_mutex);
    if (recorded)
        FinishRecordedDraw();
    else
        IssueDraw(device, primitive, start, count, true, baseVertex);
    DrawConsumed();
}
#endif

#ifdef GAME_HOOK_D3DDevice_BeginVertices
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_BeginVertices));
// D3DDevice_BeginVertices(device, primitiveType, vertexCount, stride) -> buffer the game fills afterwards
PPC_FUNC(GAME_HOOK(D3DDevice_BeginVertices))
{
    RendererTimer timer;
    CaptureBeginVertices(ctx);
    uint32_t device = ctx.r3.u32, primitive = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
    {
        std::lock_guard lock(g_mutex);
        FlushPendingVertices();
    }
    timer.Exclude([&] { GAME_IMP(D3DDevice_BeginVertices)(ctx, base); });
    std::lock_guard lock(g_mutex);
    ScanRing(device); // includes the state this draw sent
    PendingVertices& p = g_pendingVertices;
    p.guestData = ctx.r3.u32;
    p.bytes = count * stride;
    p.primitive = primitive;
    p.count = count;
    p.pending = p.guestData != 0 && BuildDraw(device, primitive, 0, count, p.guestData, stride, false, 0, p.draw);
    DrawConsumed();
}
#endif

#ifdef GAME_HOOK_D3DDevice_Resolve
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_Resolve));
// D3DDevice_Resolve(device, flags, srcRect, destTexture, destPoint, level, slice, clearColor, f1 = clearZ, clearStencil, ...)
PPC_FUNC(GAME_HOOK(D3DDevice_Resolve))
{
    RendererTimer timer;
    CaptureResolve(ctx);
    // The tenth argument (clear stencil) is on the stack, in the caller's parameter area.
    uint32_t device = ctx.r3.u32;
    // (device, flags, source rect, destination, point, level, slice, clear color, clear Z, clear stencil on the stack);
    // newer XDKs (Giants) drop the level, which moves the slice, the clear color and the stencil up one place.
    ResolveArgs args = game::ResolveHasLevel ? ResolveArgs{ ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r9.u32,
        ctx.r10.u32, Read32(ctx.r1.u32 + 0x5C), float(ctx.f1.f64) }
                                             : ResolveArgs{ ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                                                   ctx.r9.u32, Read32(ctx.r1.u32 + 0x54), float(ctx.f1.f64) };
    {
        std::lock_guard lock(g_mutex);
        FlushPendingVertices();
    }
    timer.Exclude([&] { GAME_IMP(D3DDevice_Resolve)(ctx, base); });
    std::lock_guard lock(g_mutex);
    ScanRing(device);
    Resolve(device, args);
}
#endif

#ifdef GAME_HOOK_D3DDevice_ClearF_Worker
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_ClearF_Worker));
// D3DDevice_ClearF's worker (device, flags, rect, color float4, -, stencil, f1 = z)
PPC_FUNC(GAME_HOOK(D3DDevice_ClearF_Worker))
{
    RendererTimer timer;
    ScanRing(ctx.r3.u32);
    CaptureClear(ctx);
    {
        std::lock_guard lock(g_mutex);
        FlushPendingVertices();
        Clear(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r8.u32, float(ctx.f1.f64));
    }
    timer.Exclude([&] { GAME_IMP(D3DDevice_ClearF_Worker)(ctx, base); });
}
#endif

#ifdef GAME_HOOK_D3DDevice_Swap
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_Swap));
// D3DDevice_Swap(device, frontBufferTexture, ...): presents the texture the frame was resolved into.
PPC_FUNC(GAME_HOOK(D3DDevice_Swap))
{
    RendererTimer timer;
    // A game that paces itself by the interval it finds in its device (game::Adapter::presentIntervalPacesGame) is shown
    // interval 1 at 60 FPS: the guest D3D then does not hold the frame to two blanks, and the engine times its frames.
    if (game::Current().presentIntervalPacesGame && (FrameRateLimit() >= 60 || FrameRateLimit() == 0) &&
        Read32(ctx.r3.u32 + Device::PresentInterval) == 2)
        g_memory.Write<uint32_t>(ctx.r3.u32 + Device::PresentInterval, 1);
    ScanRing(ctx.r3.u32);
    CaptureSwap(ctx);
    auto ready = std::chrono::steady_clock::now();
    timer.Exclude([&] {
        uint64_t waitStart = NowNs();
        WaitForFlip(
            Read32(ctx.r3.u32 +
                   Device::PresentInterval)); // D3DRS_PRESENTINTERVAL: the console shows frames at vertical blanks
        gpu::AddFrameTime(gpu::FrameTime::VblankWait, NowNs() - waitStart);
    });
    {
        std::lock_guard lock(g_mutex);
        FlushPendingVertices();
        xenos::TextureFetch fetch;
        uint32_t texture = ctx.r4.u32;
        for (uint32_t i = 0; i < 6; i++)
            fetch.dword[i] = texture != 0 ? Read32(texture + Resource::TextureFetch + i * 4) : 0;
        auto it = g_textures.find(TextureKey(fetch));
        // SKY_PRESENT_PROBE=<n>: every nth swap, what the present samples (fetch words, size, format) and a readback of
        // that texture; a swap whose texture is unknown presents nothing. Offscreen frames do not prove a window.
        static const int probeEvery =
            skyenv::Get("SKY_PRESENT_PROBE") != nullptr ? atoi(skyenv::Get("SKY_PRESENT_PROBE")) : 0;
        if (probeEvery > 0 && g_frame % probeEvery == 0)
        {
            bool known = it != g_textures.end() && it->second.texture != nullptr;
            fprintf(stderr, "[present] frame %llu: texture 0x%08X fetch %08X %08X %08X %08X %08X %08X, %s",
                (unsigned long long)g_frame, texture, fetch.dword[0], fetch.dword[1], fetch.dword[2], fetch.dword[3],
                fetch.dword[4], fetch.dword[5], known ? "found" : "NOT FOUND (nothing presented)");
            std::vector<uint32_t> pixels;
            uint32_t width = 0, height = 0;
            if (known && gpu::ReadbackTexture(it->second.texture, pixels, width, height))
            {
                uint64_t lit = 0, sum = 0;
                for (uint32_t p : pixels)
                {
                    uint32_t r = p & 0xFF, g = p >> 8 & 0xFF, b = p >> 16 & 0xFF;
                    lit += (r | g | b) > 8;
                    sum += r + g + b;
                }
                fprintf(stderr, ", read back %ux%u, %llu of %zu pixels lit, mean %.1f", width, height,
                    (unsigned long long)lit, pixels.size(),
                    pixels.empty() ? 0.0 : double(sum) / 3.0 / double(pixels.size()));
            }
            fprintf(stderr, "\n");
        }
        if (it != g_textures.end() && it->second.texture != nullptr)
        {
            SetGameRendering(true);
            DumpResolve(it->second.texture);
            timer.Exclude([&] { gpu::Present(it->second.texture); });
            SnapshotPresentedFrame();
        }
        if (skyenv::Get("SKY_RENDER_STATS") != nullptr && g_frame % 60 == 0)
        {
            // Frames per second over the last 60 frames.
            static auto last = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            double fps = 60.0 / std::chrono::duration<double>(now - last).count();
            last = now;
            fprintf(stderr,
                "[render] frame %llu: %u draws, %u resolves, skipped %u (shader) %u (target) [%0.1f fps], ring jumps "
                "%u\n",
                (unsigned long long)g_frame, g_stats.draws, g_stats.resolves, g_stats.skippedShader,
                g_stats.skippedTarget, fps, TakeRingJumps());
        }
        // SKY_FRAME_TIMES=<file>: one line per frame: milliseconds since start-up at the present (the origin of the
        // `[file] open` lines' times), the frame's work (from the previous swap's end to this swap, without the vblank
        // wait), its draws. tools/perf/frames.py and load_times.py read it.
        static FILE* frameTimes =
            skyenv::Get("SKY_FRAME_TIMES") != nullptr ? fopen(skyenv::Get("SKY_FRAME_TIMES"), "w") : nullptr;
        if (frameTimes != nullptr)
        {
            auto now = std::chrono::steady_clock::now();
            fprintf(frameTimes, "%.3f %.3f %u\n",
                std::chrono::duration<double, std::milli>(now - g_processStart).count(),
                std::chrono::duration<double, std::milli>(ready - g_swapEnd).count(), g_stats.draws);
            fflush(frameTimes);
        }
        if (g_textureStatsOn && g_frame % 60 == 0)
        {
            fprintf(stderr,
                "[textures] last 60 frames, per frame: %.1f checked, %.1f hashed whole (%.2f MB), %.1f by sample; "
                "%llu changes of sampled textures found, %llu missed by the sample\n",
                g_textureStats.checks / 60.0, g_textureStats.full / 60.0, g_textureStats.bytes / 60.0 / 1048576,
                g_textureStats.sampled / 60.0, (unsigned long long)g_textureStats.changed,
                (unsigned long long)g_textureStats.missed);
            g_textureStats = {};
        }
        g_stats = {};
        g_frame++;
        AdvanceFrameDump();
    }
    uint32_t device = ctx.r3.u32;
    timer.Exclude([&] { GAME_IMP(D3DDevice_Swap)(ctx, base); });
    ResyncRing(device); // the swap writes into the system command buffer
    g_swapEnd = std::chrono::steady_clock::now();
}
#endif

// The light tile job's scene depth (d3d/depth_readback.h).
void d3d::SetCpuReadDepth(uint32_t physicalAddress)
{
    g_cpuReadDepth.store(physicalAddress & 0x1FFFFFFF, std::memory_order_relaxed);
}
