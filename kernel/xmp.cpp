// XMsg: the title's statically linked XMP (Xbox Music Player) library talks to the system's music player app through
// these calls. Nothing plays a user soundtrack here, so the title always keeps playback control.
//
// This matters for music: Wwise registers the game's background-music bus and mutes it unless
// XMPTitleHasPlaybackControl() (0x824FC430: XMPGetPlaybackController, then both outputs must be 0) says the title
// has control. Message numbers and layouts as in Xenia's XMP app (src/xenia/kernel/xam/apps/xmp_app.cc, BSD-3-Clause).
#include "kernel/overlapped.h"
#include "memory.h"
#include <atomic>
#include <cstdio>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t XmpApp = 0xFA;
    constexpr uint32_t XmpGetStatus = 0x00070009;            // { client, XMP_STATE* }
    constexpr uint32_t XmpSetPlaybackController = 0x0007001A; // { client, controller, playback client }
    constexpr uint32_t XmpGetPlaybackController = 0x0007001B; // { client, controller*, locked* }
    constexpr uint32_t XmpStateIdle = 0;

    // Stores a result through an output pointer of the message buffer, when the caller passed one.
    void WriteOutput(uint32_t buffer, uint32_t field, uint32_t value)
    {
        uint32_t pointer = buffer != 0 ? g_memory.Read<uint32_t>(buffer + field) : 0;
        if (pointer != 0)
            g_memory.Write<uint32_t>(pointer, value);
    }

    uint32_t DispatchMessage(uint32_t app, uint32_t message, uint32_t buffer)
    {
        if (app == XmpApp && message == XmpGetStatus)
        {
            WriteOutput(buffer, 4, XmpStateIdle);
            return ErrorSuccess;
        }
        if (app == XmpApp && message == XmpGetPlaybackController)
        {
            WriteOutput(buffer, 4, 0); // the title
            WriteOutput(buffer, 8, 0); // not locked by the user
            return ErrorSuccess;
        }
        if (app == XmpApp && message == XmpSetPlaybackController)
            return ErrorSuccess;

        static std::atomic_flag reported;
        if (!reported.test_and_set())
            fprintf(stderr, "[xmsg] unhandled message 0x%08X to app 0x%X\n", message, app);
        return ErrorSuccess;
    }
}

// XMsgInProcessCall(app, message, buffer, buffer length)
PPC_FUNC(__imp__XMsgInProcessCall) { ctx.r3.u64 = DispatchMessage(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32); }

// XMsgStartIORequestEx(app, message, overlapped, buffer, buffer length, unknown)
PPC_FUNC(__imp__XMsgStartIORequestEx)
{
    uint32_t result = DispatchMessage(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
    ctx.r3.u64 = FinishXamCall(ctx.r5.u32, result, 0, ctx.r13.u32);
}
