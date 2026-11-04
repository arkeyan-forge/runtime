#pragma once

// The buffer cache's content hash of a guest range (gpu/buffer_cache.cpp), with the 4 KB chunks inside the range
// hashed once a frame whatever the number of ranges that cover them. The keys are the ranges draws read, and the draws of
// one mesh read overlapping ranges of its buffer: SSA's level_008 validated 142 MB a frame for 12 MB of distinct bytes,
// 23 % of the render thread (docs/runtime/gpu/renderer_threads.md). The parts of the range outside whole chunks are
// hashed directly, so the hash depends only on the range's own bytes. A chunk's hash is reused within the frame: the
// cache's assumption that the game does not rewrite, within a submission, a range it has drawn from.
// Where ranges barely overlap (Trap Team: 27.9 MB hashed for 27.8 MB distinct) the table costs more than it saves, so a
// frame where under a quarter of the lookups found their chunk turns it off for the next 120 frames: the hash values are
// the same either way. Include after <xxhash.h>.
#include <cstdint>
#include <vector>

namespace gpu
{
    class RangeHasher
    {
    public:
        static constexpr uint32_t ChunkBytes = 4096;
        // Ranges of fewer whole chunks are hashed directly: no chunk would be shared often enough to pay its lookup.
        static constexpr uint32_t MinChunks = 4;
        static constexpr uint64_t FramesWithoutTable = 120;

        // `data` holds the `bytes` guest bytes at `address`; `frame` is the producer frame (chunks are kept for it).
        uint64_t Hash(const uint8_t* data, uint32_t address, uint32_t bytes, uint64_t frame)
        {
            const uint64_t begin = address, end = begin + bytes;
            const uint64_t first = (begin + ChunkBytes - 1) / ChunkBytes, last = end / ChunkBytes;
            if (last < first + MinChunks)
            {
                hashedBytes += bytes;
                return XXH3_64bits(data, bytes);
            }
            if (frame != m_frame)
                StartFrame(frame);
            const uint32_t head = uint32_t(first * ChunkBytes - begin), tail = uint32_t(end - last * ChunkBytes);
            uint64_t hash = XXH3_64bits_withSeed(data, head, bytes);
            hashedBytes += head + tail;
            for (uint64_t chunk = first; chunk < last; chunk++)
            {
                const uint8_t* chunkData = data + (chunk * ChunkBytes - begin);
                uint64_t chunkHash;
                if (m_table)
                {
                    Slot& slot = m_slots[(chunk * 0x9E3779B97F4A7C15ull) >> (64 - SlotBits)];
                    m_lookups++;
                    if (slot.frame != frame || slot.chunk != chunk)
                    {
                        slot = { XXH3_64bits(chunkData, ChunkBytes), chunk, frame };
                        hashedBytes += ChunkBytes;
                    }
                    else
                        m_reused++;
                    chunkHash = slot.hash;
                }
                else
                {
                    chunkHash = XXH3_64bits(chunkData, ChunkBytes);
                    hashedBytes += ChunkBytes;
                }
                // Each step is a bijection of the running hash: a changed chunk always changes the result.
                hash = (hash ^ chunkHash) * 0x9E3779B97F4A7C15ull;
                hash ^= hash >> 32;
            }
            return XXH3_64bits_withSeed(data + (last * ChunkBytes - begin), tail, hash);
        }

        bool UsesTable() const { return m_table; }

        uint64_t hashedBytes = 0; // bytes actually read by the hash (SKY_BUFFER_STATS)

    private:
        void StartFrame(uint64_t frame)
        {
            if (m_table && m_lookups != 0 && m_reused * 4 < m_lookups)
            {
                m_table = false;
                m_tableFrom = frame + FramesWithoutTable;
            }
            else if (!m_table && frame >= m_tableFrom)
                m_table = true; // one frame to measure the overlap again
            m_lookups = m_reused = 0;
            m_frame = frame;
        }

        // Direct-mapped by chunk index: a slot another chunk took is hashed again, never mistaken for it.
        static constexpr uint32_t SlotBits = 14;
        struct Slot
        {
            uint64_t hash = 0, chunk = 0, frame = ~0ull; // no frame yet
        };
        std::vector<Slot> m_slots = std::vector<Slot>(size_t(1) << SlotBits);
        bool m_table = true;
        uint64_t m_frame = 0, m_tableFrom = 0, m_lookups = 0, m_reused = 0;
    };
}
