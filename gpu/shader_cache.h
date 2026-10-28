#pragma once

// Layout of ppc/shader_cache.cpp, which tools/shaders/build_cache.sh generates with XenosRecomp from the game disc.
// Entries are sorted by hash; each locates the shader's code in the zstd-compressed blob of every backend.
#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry
{
    uint64_t hash; // XXH3-64 of the guest shader container
    uint32_t dxilOffset;
    uint32_t dxilSize;
    uint32_t spirvOffset;
    uint32_t spirvSize;
    uint32_t airOffset;
    uint32_t airSize;
    uint32_t specConstantsMask;
    const char* filename;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

#if defined(_WIN32)
// Per entry: flags (bit 0: the vertex shader outputs a point size), the variant count, each variant's size, then the
// signed DXIL containers, one per combination of the entry's specialization constants in increasing order (D3D12 has
// no specialization constants; patches/xenosrecomp.patch).
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;
#endif

#if defined(SSA_METAL)
extern const uint8_t g_compressedAirCache[];
extern const size_t g_airCacheCompressedSize;
extern const size_t g_airCacheDecompressedSize;
#else
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
#endif
