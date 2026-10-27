// Temporary scaffold until the Metal renderer replaces command submission: the game's D3D still writes GPU
// commands nobody executes, so submitted work counts as executed the moment it is kicked off: its fences retire
// and its callbacks fire. Unsubmitted work must not look done, or the engine recycles frames still being built.
#include "d3d/ring.h"
#include "kernel/video.h"
#include "game_hooks.h"
#include "memory.h"
#include <mutex>
#include <vector>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    // D3DDevice fields.
    constexpr uint32_t DeviceRetiredFencePointer = 0x2B10; // where the GPU writes the last retired fence
    constexpr uint32_t DeviceInterruptInfo = 0x2B14;       // block the GPU fills before a CP interrupt
    constexpr uint32_t DeviceCurrentFence = 0x2B1C;

    // Interrupt info block fields read by D3D's interrupt handler.
    constexpr uint32_t InterruptInfoCallback = 16;
    constexpr uint32_t InterruptInfoContext = 20;

    constexpr uint32_t CommandProcessorInterrupt = 1;
    constexpr uint32_t CallbackRaisesInterrupt = 0x2;
    constexpr uint32_t DefaultCallbackFlags = 0x6;

    struct PendingCallback
    {
        uint32_t device;
        uint32_t callback;
        uint32_t context;
    };

    std::mutex g_callbackLock;
    std::vector<PendingCallback> g_pendingCallbacks;

    // KickOff submits first, then throttles with BlockOnFence: inside it, everything handed out is submitted.
    thread_local bool t_insideKickOff;

    // The current fence is the next one to be handed out, so everything submitted is below it.
    // While the device records a command buffer it has no GPU fence to retire.
    void RetireSubmittedFences(uint32_t device)
    {
        uint32_t retired = g_memory.Read<uint32_t>(device + DeviceRetiredFencePointer);
        if (retired != 0)
            g_memory.Write<uint32_t>(retired, g_memory.Read<uint32_t>(device + DeviceCurrentFence) - 1);
    }

    // D3D's own test: a fence is retired once the retired fence has caught up with it.
    bool IsFenceRetired(uint32_t device, uint32_t fence)
    {
        uint32_t retiredPointer = g_memory.Read<uint32_t>(device + DeviceRetiredFencePointer);
        if (retiredPointer == 0)
            return true;

        uint32_t current = g_memory.Read<uint32_t>(device + DeviceCurrentFence);
        uint32_t retired = g_memory.Read<uint32_t>(retiredPointer);
        return current - fence >= current - retired;
    }

    // The commands are "executed" once submitted: deliver the callbacks queued before this point.
    void RaiseSubmittedCallbacks()
    {
        std::vector<PendingCallback> callbacks;
        {
            std::lock_guard lock(g_callbackLock);
            callbacks.swap(g_pendingCallbacks);
        }

        for (const auto& pending : callbacks)
        {
            QueueGraphicsInterrupt(CommandProcessorInterrupt, [pending]
            {
                uint32_t info = g_memory.Read<uint32_t>(pending.device + DeviceInterruptInfo);
                g_memory.Write<uint32_t>(info + InterruptInfoCallback, pending.callback);
                g_memory.Write<uint32_t>(info + InterruptInfoContext, pending.context);
            });
        }
    }
}

#ifdef GAME_HOOK_D3DDevice_KickOff
PPC_FUNC_IMPL(GAME_IMP(D3DDevice_KickOff));
// D3DDevice_KickOff
PPC_FUNC(GAME_HOOK(D3DDevice_KickOff))
{
    uint32_t device = ctx.r3.u32;
    bool outermost = !t_insideKickOff;
    t_insideKickOff = true;
    d3d::ScanRing(device); // the kick-off may wrap the ring
    GAME_IMP(D3DDevice_KickOff)(ctx, base);
    d3d::ResyncRing(device);
    t_insideKickOff = !outermost;
    RetireSubmittedFences(device);
    RaiseSubmittedCallbacks();
}
#endif

// It runs the original kick-off hook's function after its own checks.
#if defined(GAME_HOOK_D3DDevice_BlockOnFence) && defined(GAME_HOOK_D3DDevice_KickOff)
// D3DDevice_BlockOnFence(device, fence): a fence still in unsubmitted commands needs a kick-off, after which
// it has executed. Nothing else is ever pending.
PPC_FUNC(GAME_HOOK(D3DDevice_BlockOnFence))
{
    uint32_t device = ctx.r3.u32;
    if (IsFenceRetired(device, ctx.r4.u32))
        return;

    if (t_insideKickOff)
    {
        RetireSubmittedFences(device);
        return;
    }

    PPCContext saved = ctx;
    GAME_HOOK(D3DDevice_KickOff)(ctx, base);
    ctx = saved;
}
#endif

#ifdef GAME_HOOK_D3DDevice_InsertCallback
// D3DDevice_InsertCallback(device, commandBuffer, flags, callback, context): would write packets making the GPU
// fill the interrupt info block and raise a CP interrupt. Queue the callback instead; it writes no commands.
PPC_FUNC(GAME_HOOK(D3DDevice_InsertCallback))
{
    uint32_t flags = (ctx.r5.u32 & DefaultCallbackFlags) != 0 ? ctx.r5.u32 : ctx.r5.u32 | DefaultCallbackFlags;
    if (flags & CallbackRaisesInterrupt)
    {
        std::lock_guard lock(g_callbackLock);
        g_pendingCallbacks.push_back({ ctx.r3.u32, ctx.r6.u32, ctx.r7.u32 });
    }

    ctx.r3.u64 = ctx.r4.u64; // command buffer position, unchanged
}
#endif
