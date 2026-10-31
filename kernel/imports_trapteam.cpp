// Imports that Skylanders: Trap Team links against and the earlier games do not (five). The console is offline with one
// local profile: the network quality-of-service calls fail as with no cable, the thread lookups find nothing. Every output is
// written; the first call of each is logged with its argument registers, because none of them is documented here.
#include "kernel/alloc_log.h"
#include "kernel/objects.h"
#include "memory.h"
#include <atomic>
#include <cstdio>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t StatusNotFound = 0xC0000225;
    constexpr uint32_t WsaNetDown = 10050;

    // Logs the first call of each import, with its first argument registers.
#define LOG_FIRST_CALL(name)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        static std::atomic_flag reported;                                                                              \
        if (!reported.test_and_set())                                                                                  \
        {                                                                                                              \
            fprintf(stderr, "[kernel] " name " (lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X)\n",                 \
                uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);                                     \
        }                                                                                                              \
    } while (0)
}

// (caller, flags, event, XNQOS** out): no service to look up without a network; the result is the lookup's error.
PPC_FUNC(__imp__NetDll_XNetQosServiceLookup)
{
    LOG_FIRST_CALL("NetDll_XNetQosServiceLookup");
    uint32_t flags = ctx.r4.u32, event = ctx.r5.u32;
    if (ctx.r6.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r6.u32, 0);
    ctx.r3.u64 = WsaNetDown;
    AllocLog(ctx, "NetDll_XNetQosServiceLookup (flags, event, out)", flags, event, ctx.r6.u32, WsaNetDown);
}

// (caller, XNQOS*): nothing was ever returned by the lookup, so there is nothing to release.
PPC_FUNC(__imp__NetDll_XNetQosRelease)
{
    LOG_FIRST_CALL("NetDll_XNetQosRelease");
    AllocLog(ctx, "NetDll_XNetQosRelease (qos)", ctx.r4.u32, 0, 0, StatusSuccess);
    ctx.r3.u64 = StatusSuccess;
}

// (thread id, KTHREAD** out): not looked up by id yet; the title gets "not found" with a null pointer.
PPC_FUNC(__imp__ObLookupThreadByThreadId)
{
    LOG_FIRST_CALL("ObLookupThreadByThreadId");
    AllocLog(ctx, "ObLookupThreadByThreadId (id, out)", ctx.r3.u32, ctx.r4.u32, 0, StatusNotFound);
    if (ctx.r4.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r4.u32, 0);
    ctx.r3.u64 = StatusNotFound;
}

// (object, type, attributes, access mode, access, HANDLE* out): no handle for a kernel object yet.
PPC_FUNC(__imp__ObOpenObjectByPointer)
{
    LOG_FIRST_CALL("ObOpenObjectByPointer");
    AllocLog(
        ctx, "ObOpenObjectByPointer (object, type, attributes)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, StatusNotFound);
    ctx.r3.u64 = StatusNotFound;
}

// Arguments unknown: logged on the first call and answered with success.
PPC_FUNC(__imp__XamGetLocaleEx)
{
    LOG_FIRST_CALL("XamGetLocaleEx");
    AllocLog(ctx, "XamGetLocaleEx (r3, r4, r5)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, StatusSuccess);
    ctx.r3.u64 = StatusSuccess;
}
