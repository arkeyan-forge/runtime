// Remembers which shader container every Direct3D shader object was created from, so draws can find the translated
// (XenosRecomp) version of the shaders bound on the device.
#include "d3d/shaders.h"
#include "game_hooks.h"
#include "memory.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <ppc_config.h>
#include <ppc_context.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace d3d
{
    namespace
    {
        std::mutex g_mutex;
        std::unordered_map<uint32_t, GuestShader> g_shaders; // by shader object

        // Container header (big-endian): flags (0x102A1101 vertex, 0x102A1100 pixel), virtual size, physical size.
        // Called before Create*Shader runs: it may rewrite parts of the container in place, and the cache is keyed by
        // the container as it is on disc. `physical` is where the physical part (the microcode) is: right after the
        // virtual part in a container, elsewhere when the engine builds the shader object itself (Alchemy).
        GuestShader Describe(uint32_t container, uint32_t physical, bool vertex)
        {
            uint32_t virtualSize = g_memory.Read<uint32_t>(container + 4);
            uint32_t size = virtualSize + g_memory.Read<uint32_t>(container + 8);
            std::vector<uint8_t> bytes(size);
            memcpy(bytes.data(), g_memory.Translate(container), virtualSize);
            memcpy(bytes.data() + virtualSize, g_memory.Translate(physical), size - virtualSize);
            GuestShader shader{ container, size, HashBytes(bytes.data(), size), vertex, 0, 0, 0, 0 };

            // SSA_DUMP_SHADERS=<dir>: keep every container the game creates (some are built at run time, so the disc
            // archives lack them); tools/shaders/build_cache.sh translates them too.
            static const char* dumpDirectory = getenv("SSA_DUMP_SHADERS");
            if (dumpDirectory != nullptr)
            {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%016llX.%s", dumpDirectory, (unsigned long long)shader.hash, vertex ? "vs" : "ps");
                if (FILE* file = fopen(path, "wbx")) // only once per shader
                {
                    fwrite(bytes.data(), 1, size, file);
                    fclose(file);
                }
            }

            // Vertex shader header at +shaderOffset: 6 dwords, then an index, the input count, one dword, and the list
            // (index + input) of 32-bit entries: address 12 bits (of the vertex fetch instruction), usage 4 bits, usage
            // index 4 bits. The microcode starts at the physical part (+virtual size) plus the header's first dword.
            if (vertex)
            {
                uint32_t header = container + g_memory.Read<uint32_t>(container + 0x18);
                uint32_t code = physical + g_memory.Read<uint32_t>(header);
                uint32_t first = g_memory.Read<uint32_t>(header + 0x18);
                uint32_t count = g_memory.Read<uint32_t>(header + 0x1C);
                for (uint32_t i = 0; i < count && i < 32; i++)
                {
                    uint32_t element = g_memory.Read<uint32_t>(header + 0x24 + (first + i) * 4);
                    uint32_t usage = (element >> 12) & 0xF;
                    uint32_t location = InputLocation(usage, (element >> 16) & 0xF);
                    if (location >= 32)
                        continue;
                    shader.inputs |= 1u << location;

                    // Its fetch indexes with source register bits 5-10, component bits 30-31: other than r0.x, the index
                    // is computed (XenosRecomp then reads the host-gathered vertices, except for normals and texcoords).
                    uint32_t fetch = g_memory.Read<uint32_t>(code + (element & 0xFFF) * 12);
                    bool computed = ((fetch >> 5) & 0x3F) != 0 || (fetch >> 30) != 0;
                    if (computed && usage != 3 && usage != 5 && usage != 6 && usage != 7)
                        shader.pulled |= 1u << location;
                }

                // The constant table (container +0x10, if the engine kept one) lists the samplers a vertex texture fetch
                // uses: D3DXSHADER_CONSTANTINFO entries (20 bytes: name, set (3: sampler) and register index as 16 bits).
                if (uint32_t tableOffset = g_memory.Read<uint32_t>(container + 0x10))
                {
                    uint32_t table = container + tableOffset + 4; // after the container's size field
                    uint32_t constants = g_memory.Read<uint32_t>(table + 0xC), info = table + g_memory.Read<uint32_t>(table + 0x10);
                    for (uint32_t i = 0; i < constants && i < 512; i++)
                    {
                        uint32_t entry = info + i * 20, registerIndex = g_memory.Read<uint16_t>(entry + 6);
                        if (g_memory.Read<uint16_t>(entry + 4) == 3 && registerIndex < 4)
                            shader.samplers |= 1u << registerIndex;
                    }
                }
            }
            // Pixel shader header at +shaderOffset: its third dword is the shader's part of SQ_PROGRAM_CNTL (bit 18 =
            // param_gen), its fourth its SQ_CONTEXT_MISC (param_gen_pos in bits 8-15, the register XenosRecomp
            // initialises with the pixel position). In all 516 dumped pixel shaders the two agree.
            else
            {
                uint32_t header = container + g_memory.Read<uint32_t>(container + 0x18);
                bool enabled = (g_memory.Read<uint32_t>(header + 0x8) >> 18) & 1;
                shader.paramGen = enabled ? 0x80000000u | ((g_memory.Read<uint32_t>(header + 0xC) >> 8) & 0xFF) : 0;
            }
            return shader;
        }

        void Record(uint32_t object, const GuestShader& shader)
        {
            if (object == 0)
                return;
            std::lock_guard lock(g_mutex);
            g_shaders[object] = shader;
        }
    }

    uint32_t InputLocation(uint32_t usage, uint32_t usageIndex)
    {
        switch (usage)
        {
        case 0: return usageIndex == 0 ? 0 : 15;  // POSITION
        case 3: return 1;                         // NORMAL
        case 6: return 2;                         // TANGENT
        case 7: return 3;                         // BINORMAL
        case 5: return usageIndex < 4 ? 4 + usageIndex : 12 + (usageIndex - 4); // TEXCOORD
        case 10: return usageIndex == 0 ? 8 : 11; // COLOR
        case 2: return 9;                         // BLENDINDICES
        case 1: return 10;                        // BLENDWEIGHT
        case 4: return 16;                        // PSIZE
        default: return ~0u;
        }
    }

    uint64_t HashBytes(const void* data, uint64_t size)
    {
        return XXH3_64bits(data, size);
    }

    const GuestShader* FindGuestShader(uint32_t object)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_shaders.find(object);
        return it != g_shaders.end() ? &it->second : nullptr;
    }
}

#ifdef GAME_HOOK_D3DDevice_CreateVertexShader
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_CreateVertexShader));
// D3DDevice_CreateVertexShader(container) -> shader object
PPC_FUNC(GAME_HOOK(D3DDevice_CreateVertexShader))
{
    d3d::GuestShader shader = d3d::Describe(ctx.r3.u32, ctx.r3.u32 + g_memory.Read<uint32_t>(ctx.r3.u32 + 4), true);
    GAME_IMP(D3DDevice_CreateVertexShader)(ctx, base);
    d3d::Record(ctx.r3.u32, shader);
}
#endif

#ifdef GAME_HOOK_D3DDevice_CreatePixelShader
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_CreatePixelShader));
// D3DDevice_CreatePixelShader(container) -> shader object
PPC_FUNC(GAME_HOOK(D3DDevice_CreatePixelShader))
{
    d3d::GuestShader shader = d3d::Describe(ctx.r3.u32, ctx.r3.u32 + g_memory.Read<uint32_t>(ctx.r3.u32 + 4), false);
    GAME_IMP(D3DDevice_CreatePixelShader)(ctx, base);
    d3d::Record(ctx.r3.u32, shader);
}
#endif

// Engines that build shader objects themselves (Alchemy: Giants) skip Create*Shader and call the Init functions with the
// object, which holds the container's virtual part (vertex shaders at +0x368, pixel shaders at +0x28, where
// Create*Shader copies it), and the microcode already in physical memory.
namespace
{
    template <auto Original>
    void InitShader(PPCContext& ctx, uint8_t* base, uint32_t containerOffset, bool vertex)
    {
        uint32_t object = ctx.r3.u32;
        uint32_t container = object + containerOffset;
        bool known = (g_memory.Read<uint32_t>(container) & 0xFFFFFF00) == 0x102A1100;
        d3d::GuestShader shader = known ? d3d::Describe(container, ctx.r4.u32, vertex) : d3d::GuestShader{};
        if (!known)
        {
            static bool reported;
            if (!reported)
                fprintf(stderr, "[shader] %s object 0x%08X: no container at +0x%X (0x%08X)\n", vertex ? "vertex" : "pixel",
                    object, containerOffset, g_memory.Read<uint32_t>(container));
            reported = true;
        }
        Original(ctx, base);
        if (known)
            d3d::Record(object, shader);
    }
}

#ifdef GAME_HOOK_D3DVertexShader_Init
PPC_FUNC_IMPL(GAME_IMP(D3DVertexShader_Init));
// D3DVertexShader_Init(object, microcode)
PPC_FUNC(GAME_HOOK(D3DVertexShader_Init))
{
    InitShader<GAME_IMP(D3DVertexShader_Init)>(ctx, base, 0x368, true);
}
#endif

#ifdef GAME_HOOK_D3DPixelShader_Init
PPC_FUNC_IMPL(GAME_IMP(D3DPixelShader_Init));
// D3DPixelShader_Init(object, microcode)
PPC_FUNC(GAME_HOOK(D3DPixelShader_Init))
{
    InitShader<GAME_IMP(D3DPixelShader_Init)>(ctx, base, 0x28, false);
}
#endif
