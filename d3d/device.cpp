#include "d3d/device.h"
#include "memory.h"

namespace d3d
{
    namespace
    {
        // Register ranges the device shadows, and where (device+0x2880..+0x2A6F).
        struct ShadowRange
        {
            uint32_t firstRegister;
            uint32_t count;
            uint32_t offset;
        };

        constexpr ShadowRange ShadowRanges[] = {
            { 0x2000, 0x10, 0x2880 }, // RB_SURFACE_INFO..PA_SC_SCREEN_SCISSOR_BR
            { 0x2080, 0x03, 0x28C0 }, // PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL/BR
            { 0x2100, 0x15, 0x28CC }, // VGT_MAX_VTX_INDX..PA_CL_VPORT_ZOFFSET
            { 0x2180, 0x05, 0x2920 }, // SQ_PROGRAM_CNTL..SQ_WRAPPING_1
            { 0x2200, 0x0C, 0x2934 }, // RB_DEPTHCONTROL..RB_BLENDCONTROL3
            { 0x2280, 0x15, 0x2964 }, // PA_SU_POINT_SIZE..VGT_ENHANCE
            { 0x2300, 0x26, 0x29B8 }, // PA_SC_LINE_CNTL..
            { 0x2380, 0x08, 0x2A50 },
        };
    }

    uint32_t CurrentDevice()
    {
        if constexpr (game::DeviceGlobal == 0)
            return 0; // the game's address file does not give it yet
        return g_memory.Read<uint32_t>(game::DeviceGlobal);
    }

    uint32_t RegisterOffset(uint32_t reg)
    {
        for (const ShadowRange& range : ShadowRanges)
        {
            if (reg >= range.firstRegister && reg < range.firstRegister + range.count)
                return range.offset + (reg - range.firstRegister) * 4;
        }
        return 0;
    }

    uint32_t ReadRegister(uint32_t device, uint32_t reg)
    {
        uint32_t offset = RegisterOffset(reg);
        return offset != 0 ? g_memory.Read<uint32_t>(device + offset) : 0;
    }
}
