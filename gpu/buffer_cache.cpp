#include "gpu/buffer_cache.h"
#include "gpu/context.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include "../host/env.h"

#define XXH_INLINE_ALL
#include <xxhash.h>
#include "gpu/range_hash.h"

using namespace plume;
using namespace gpu::detail;
using gpu::BufferRange;

namespace
{
    // Resident copies live in blocks of this size (larger ranges get a block of their own): one Metal buffer and one
    // residency-set update per block rather than per range.
    constexpr uint64_t BlockSize = 32ull << 20;
    constexpr uint64_t RangeAlignment = 256;
    // Eviction (least recently used first) starts above this much resident data.
    constexpr uint64_t CacheLimit = 256ull << 20;

    // --- Suballocation of resident ranges -----------------------------------------------------------------------------

    struct Block
    {
        std::unique_ptr<RenderBuffer> buffer;
        uint8_t* memory;
        uint64_t size;
        std::map<uint64_t, uint64_t> free; // offset -> size, coalesced
    };

    struct Allocation
    {
        Block* block = nullptr;
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    std::vector<std::unique_ptr<Block>> g_blocks;
    uint64_t g_residentBytes = 0;

    // Freed ranges the GPU may still read: reusable once the submission that last used them has completed.
    struct PendingFree
    {
        Allocation allocation;
        uint64_t frame;
    };
    std::vector<PendingFree> g_pendingFrees;

    Allocation Allocate(uint64_t size)
    {
        size = AlignUp(std::max<uint64_t>(size, 1), RangeAlignment);
        for (const std::unique_ptr<Block>& block : g_blocks)
        {
            for (auto it = block->free.begin(); it != block->free.end(); ++it)
            {
                if (it->second < size)
                    continue;
                Allocation allocation{ block.get(), it->first, size };
                uint64_t remaining = it->second - size;
                uint64_t offset = it->first + size;
                block->free.erase(it);
                if (remaining != 0)
                    block->free.emplace(offset, remaining);
                g_residentBytes += size;
                return allocation;
            }
        }

        auto block = std::make_unique<Block>();
        block->size = std::max(BlockSize, size);
        block->buffer = g_context->device->createBuffer(RenderBufferDesc::UploadBuffer(
            block->size, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::CONSTANT |
                             RenderBufferFlag::DEVICE_ADDRESSABLE));
        if (block->buffer == nullptr)
            return {};
        block->memory = static_cast<uint8_t*>(block->buffer->map());
        if (block->memory == nullptr)
        {
            gpu::detail::CheckDeviceLost();
            return {};
        }
        if (block->size > size)
            block->free.emplace(size, block->size - size);
        g_blocks.push_back(std::move(block));
        g_residentBytes += size;
        return { g_blocks.back().get(), 0, size };
    }

    void Release(const Allocation& allocation)
    {
        std::map<uint64_t, uint64_t>& free = allocation.block->free;
        uint64_t offset = allocation.offset, size = allocation.size;
        g_residentBytes -= allocation.size;
        auto next = free.lower_bound(offset);
        if (next != free.end() && offset + size == next->first)
        {
            size += next->second;
            next = free.erase(next);
        }
        if (next != free.begin())
        {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset)
            {
                previous->second += size;
                return;
            }
        }
        free.emplace(offset, size);
    }

    // --- Cache entries ------------------------------------------------------------------------------------------------

    struct Key
    {
        uint32_t address;
        uint32_t bytes;
        uint32_t wordSize; // 2: 8-in-32 (vertices), 1: 8-in-16 (indices)

        bool operator==(const Key& other) const
        {
            return address == other.address && bytes == other.bytes && wordSize == other.wordSize;
        }
    };

    struct KeyHash
    {
        size_t operator()(const Key& key) const
        {
            uint64_t value = uint64_t(key.address) * 0x9E3779B97F4A7C15ull;
            value ^= uint64_t(key.bytes) * 0xC2B2AE3D27D4EB4Full;
            value ^= uint64_t(key.wordSize) * 0x165667B19E3779F9ull;
            value ^= value >> 29;
            return size_t(value);
        }
    };

    struct Entry
    {
        Allocation resident;      // none until a later frame draws from the range again
        BufferRange staged;       // the first frame's upload while not resident
        uint64_t firstFrame = 0;  // frame that first drew from the range
        uint64_t usedFrame = 0;
        uint64_t validatedFrame = 0;
        uint64_t contentHash = 0;
        uint32_t minIndex = 0, maxIndex = 0;
    };

    std::unordered_map<Key, Entry, KeyHash> g_entries;
    // Ranges staged during the previous and the current frame, for pruning.
    std::vector<Key> g_stagedPrevious, g_stagedCurrent;
    uint64_t g_frame = 1;

    // SKY_BUFFER_STATS: cache activity every 60 submissions.
    struct Stats
    {
        uint64_t acquire, staged, stagedBytes, promoted, hashed, hashedBytes, hashedDistinctBytes, rewritten, evicted;
    } g_stats;
    const bool g_statsOn = skyenv::Get("SKY_BUFFER_STATS") != nullptr;
    // The ranges hashed this frame, when g_statsOn: how much of the hashing overlaps (the same bytes in several keys).
    std::vector<std::pair<uint32_t, uint32_t>> g_hashedRanges;
    gpu::RangeHasher g_hasher;
    // SKY_RANGE_HASH=0: every range hashed whole, as before the chunks (to measure).
    const bool g_chunkedHash = [] {
        const char* value = skyenv::Get("SKY_RANGE_HASH");
        return value == nullptr || value[0] != '0';
    }();

    uint32_t Swap32(uint32_t value)
    {
#if defined(_MSC_VER) && !defined(__clang__)
        return _byteswap_ulong(value);
#else
        return __builtin_bswap32(value);
#endif
    }

    uint16_t Swap16(uint16_t value)
    {
#if defined(_MSC_VER) && !defined(__clang__)
        return _byteswap_ushort(value);
#else
        return __builtin_bswap16(value);
#endif
    }

    // Byte-swaps `bytes` guest bytes into `destination`; for indices (`wordSize` 1) also records the smallest and
    // largest index in `entry`.
    void Convert(const uint8_t* source, uint8_t* destination, uint32_t bytes, uint32_t wordSize, Entry& entry)
    {
        if (wordSize == 2)
        {
            uint32_t i = 0;
            for (; i + 4 <= bytes; i += 4)
            {
                uint32_t value;
                memcpy(&value, source + i, 4);
                value = Swap32(value);
                memcpy(destination + i, &value, 4);
            }
            memcpy(destination + i, source + i, bytes - i);
        }
        else
        {
            uint32_t minIndex = 0xFFFF, maxIndex = 0;
            uint32_t i = 0;
            for (; i + 2 <= bytes; i += 2)
            {
                uint16_t value;
                memcpy(&value, source + i, 2);
                value = Swap16(value);
                memcpy(destination + i, &value, 2);
                minIndex = std::min<uint32_t>(minIndex, value);
                maxIndex = std::max<uint32_t>(maxIndex, value);
            }
            memcpy(destination + i, source + i, bytes - i);
            entry.minIndex = std::min(minIndex, maxIndex);
            entry.maxIndex = maxIndex;
        }
    }

    // Converts into a new resident range; the previous one is released once the GPU has finished with it.
    bool WriteResident(Entry& entry, const Key& key, const void* data)
    {
        Allocation allocation = Allocate(key.bytes);
        if (allocation.block == nullptr)
            return false;
        Convert(static_cast<const uint8_t*>(data), allocation.block->memory + allocation.offset, key.bytes,
            key.wordSize, entry);
        if (entry.resident.block != nullptr)
            g_pendingFrees.push_back({ entry.resident, entry.usedFrame });
        entry.resident = allocation;
        return true;
    }

    BufferRange Acquire(const void* data, uint32_t guestAddress, uint32_t bytes, uint32_t wordSize, uint32_t* minIndex,
        uint32_t* maxIndex)
    {
        if (data == nullptr || bytes == 0 || g_context == nullptr)
            return {};
        g_stats.acquire++;

        Key key{ guestAddress, bytes, wordSize };
        auto [it, inserted] = g_entries.try_emplace(key);
        Entry& entry = it->second;
        if (inserted)
            entry.firstFrame = g_frame;

        BufferRange result;
        if (entry.resident.block == nullptr && entry.firstFrame == g_frame)
        {
            // First frame drawing from this range: convert it into this frame's upload memory, as it may never be
            // drawn from again (a ring buffer the game streams vertices through).
            if (entry.usedFrame != g_frame)
            {
                RenderBuffer* buffer;
                uint64_t offset = AllocateUpload(bytes, buffer, 16);
                auto mapped = static_cast<uint8_t*>(buffer->map());
                if (mapped == nullptr)
                {
                    gpu::detail::CheckDeviceLost();
                    return {};
                }
                Convert(static_cast<const uint8_t*>(data), mapped + offset, bytes, wordSize, entry);
                buffer->unmap();
                entry.staged = { buffer, offset };
                g_stagedCurrent.push_back(key);
                g_stats.staged++;
                g_stats.stagedBytes += bytes;
            }
            result = entry.staged;
        }
        else
        {
            // Drawn from in an earlier frame too: keep it resident, converted again only when the guest data changed.
            if (entry.validatedFrame != g_frame)
            {
                uint64_t hash = g_chunkedHash
                                    ? g_hasher.Hash(static_cast<const uint8_t*>(data), guestAddress, bytes, g_frame)
                                    : XXH3_64bits(data, bytes);
                g_stats.hashed++;
                g_stats.hashedBytes += bytes;
                if (g_statsOn)
                    g_hashedRanges.emplace_back(guestAddress, guestAddress + bytes);
                if (entry.resident.block == nullptr || hash != entry.contentHash)
                {
                    g_stats.promoted += entry.resident.block == nullptr;
                    g_stats.rewritten += entry.resident.block != nullptr;
                    if (!WriteResident(entry, key, data))
                        return {};
                    entry.contentHash = hash;
                }
                entry.validatedFrame = g_frame;
            }
            result = { entry.resident.block->buffer.get(), entry.resident.offset };
        }
        entry.usedFrame = g_frame;
        if (minIndex != nullptr)
        {
            *minIndex = entry.minIndex;
            *maxIndex = entry.maxIndex;
        }
        return result;
    }
}

// Both take the producer lock, as every producer call: the cache and the upload memory belong to the producer frame,
// and until the game presents its first frame the main thread presents too (PresentDisplayFrame), ending frames
// (BufferCacheFrameDone) while the game's render thread draws.
BufferRange gpu::CacheVertices(const void* data, uint32_t guestAddress, uint32_t bytes)
{
    ProducerLock producer;
    if (gpu::detail::DeviceLost())
        return {};
    return Acquire(data, guestAddress, bytes, 2, nullptr, nullptr);
}

BufferRange gpu::CacheIndices(
    const void* data, uint32_t guestAddress, uint32_t bytes, uint32_t& minIndex, uint32_t& maxIndex)
{
    ProducerLock producer;
    if (gpu::detail::DeviceLost())
        return {};
    return Acquire(data, guestAddress, bytes, 1, &minIndex, &maxIndex);
}

void gpu::BufferCacheFrameDone(uint64_t completed)
{
    // g_frame follows the producer frames (gpu::Present): the GPU has finished every one up to `completed`.
    uint64_t recorded = g_frame;
    g_frame++;

    for (size_t i = 0; i < g_pendingFrees.size();)
    {
        if (g_pendingFrees[i].frame <= completed)
        {
            Release(g_pendingFrees[i].allocation);
            g_pendingFrees[i] = g_pendingFrees.back();
            g_pendingFrees.pop_back();
        }
        else
        {
            i++;
        }
    }

    // Ranges first drawn from in the frame before and not drawn from since are forgotten.
    for (const Key& key : g_stagedPrevious)
    {
        auto it = g_entries.find(key);
        if (it != g_entries.end() && it->second.resident.block == nullptr && it->second.usedFrame != recorded)
            g_entries.erase(it);
    }
    g_stagedPrevious.swap(g_stagedCurrent);
    g_stagedCurrent.clear();

    if (g_residentBytes > CacheLimit)
    {
        std::vector<std::pair<uint64_t, Key>> candidates;
        for (auto& [key, entry] : g_entries)
            if (entry.resident.block != nullptr && entry.usedFrame < completed)
                candidates.emplace_back(entry.usedFrame, key);
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [usedFrame, key] : candidates)
        {
            if (g_residentBytes <= CacheLimit)
                break;
            auto it = g_entries.find(key);
            Release(it->second.resident);
            g_entries.erase(it);
            g_stats.evicted++;
        }
    }

    if (g_statsOn)
    {
        std::sort(g_hashedRanges.begin(), g_hashedRanges.end());
        uint32_t end = 0;
        for (const auto& [from, to] : g_hashedRanges)
        {
            if (to > std::max(from, end))
                g_stats.hashedDistinctBytes += to - std::max(from, end);
            end = std::max(end, to);
        }
        g_hashedRanges.clear();
    }
    if (g_statsOn && g_frame % 60 == 0)
    {
        fprintf(stderr,
            "[buffers] frame %llu, per frame: acquire %.0f staged %.1f (%.2f MB) promoted %.1f hashed %.0f (%.2f MB, "
            "%.2f MB distinct, %.2f MB read) rewritten %.1f evicted %.1f; entries %zu, resident %.1f MB in %zu "
            "blocks\n",
            (unsigned long long)g_frame, g_stats.acquire / 60.0, g_stats.staged / 60.0,
            g_stats.stagedBytes / 60.0 / 1048576.0, g_stats.promoted / 60.0, g_stats.hashed / 60.0,
            g_stats.hashedBytes / 60.0 / 1048576.0, g_stats.hashedDistinctBytes / 60.0 / 1048576.0,
            g_hasher.hashedBytes / 60.0 / 1048576.0, g_stats.rewritten / 60.0, g_stats.evicted / 60.0, g_entries.size(),
            g_residentBytes / 1048576.0, g_blocks.size());
        g_stats = {};
        g_hasher.hashedBytes = 0;
    }
}

void gpu::BufferCacheShutdown()
{
    g_entries.clear();
    g_stagedPrevious.clear();
    g_stagedCurrent.clear();
    g_pendingFrees.clear();
    g_blocks.clear();
    g_residentBytes = 0;
    g_frame = 1;
}
