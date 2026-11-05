#pragma once

// Host GPU copies of guest vertex and index data, byte-swapped once instead of on every draw. A range seen for the
// first time is converted into the frame's upload memory; when a later frame draws from it again it becomes resident:
// suballocated from large persistent buffers, re-validated with an XXH3 hash at most once per submission, and
// converted again only when the guest data changed. Data the game streams through a ring buffer (new ranges every
// frame) therefore never fills the cache. Within one submission the game is assumed not to rewrite a range it has
// drawn from (dynamic draws go through BeginVertices).
#include <cstdint>

namespace plume
{
    struct RenderBuffer;
}

namespace gpu
{
    // Converted guest data on the GPU: `buffer` at `offset`. Valid for the submission being recorded.
    struct BufferRange
    {
        plume::RenderBuffer* buffer = nullptr;
        uint64_t offset = 0;
    };

    // `data` points at `bytes` guest bytes starting at `guestAddress`, keyed by (guestAddress, bytes, stride, pairWords).
    // Vertex data is swapped 8-in-32, except the words of `pairWords` (bit i: word i of each `stride`-byte vertex,
    // d3d::PairWords), swapped 8-in-16: 16-bit pairs that are not texture coordinates (d3d/vertex_pairs.h).
    BufferRange CacheVertices(
        const void* data, uint32_t guestAddress, uint32_t bytes, uint32_t stride = 0, uint64_t pairWords = 0);
    // 16-bit indices, swapped 8-in-16; also gives the smallest and largest index of the range.
    BufferRange CacheIndices(
        const void* data, uint32_t guestAddress, uint32_t bytes, uint32_t& minIndex, uint32_t& maxIndex);

    // Called by gpu::Present, under the producer lock like the two above, at the end of each producer frame (the frames
    // these calls belong to, worker.cpp): frees what the GPU no longer needs and evicts for space. The GPU has finished
    // frames up to `completedFrame`.
    void BufferCacheFrameDone(uint64_t completedFrame);

    // Drops every buffer before the device goes away.
    void BufferCacheShutdown();
}
