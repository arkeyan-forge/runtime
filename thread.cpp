#include "thread.h"
#include "loader.h"
#include "memory.h"
#include "platform/platform.h"
#include <atomic>

namespace
{
    constexpr uint8_t ThreadObjectType = 6;
    constexpr size_t HostStackSize = 16 * 1024 * 1024; // recompiled calls nest on the host stack

    struct ThreadExit
    {
        uint32_t code;
    };

    struct StartInfo
    {
        std::shared_ptr<GuestThread> thread;
        uint32_t start;
        uint32_t context;
        uint32_t xapiStartup;
    };

    std::atomic<uint32_t> g_nextThreadId{ 1 };

    void HostThreadMain(const StartInfo* info)
    {
        GuestThread& thread = *info->thread;
        {
            std::unique_lock lock(g_kernelLock);
            g_kernelSignal.wait(lock, [&] { return thread.suspendCount == 0; });
        }

        PPCContext& ctx = thread.context->ctx;
        ctx.fpscr.loadFromHost();

        uint32_t function = info->start;
        if (info->xapiStartup != 0)
        {
            ctx.r3.u64 = info->start;
            ctx.r4.u64 = info->context;
            function = info->xapiStartup;
        }
        else
        {
            ctx.r3.u64 = info->context;
        }

        uint32_t exitCode;
        try
        {
            (PPC_LOOKUP_FUNC(g_memory.base, function))(ctx, g_memory.base);
            exitCode = ctx.r3.u32;
        }
        catch (const ThreadExit& exit)
        {
            exitCode = exit.code;
        }

        {
            std::lock_guard lock(g_kernelLock);
            thread.exited = true;
            thread.exitCode = exitCode;
            thread.SyncHeader();
            thread.WakeSleepers();
        }
    }
    // KPCR fields read by game code through r13.
    constexpr uint32_t PcrTlsPointer = 0x0;
    constexpr uint32_t PcrSelf = 0x30;
    constexpr uint32_t PcrStackBase = 0x70;
    constexpr uint32_t PcrStackLimit = 0x74;
    constexpr uint32_t PcrCurrentThread = 0x100;
    constexpr uint32_t PcrCurrentCpu = 0x10C;
    constexpr uint32_t PcrSize = 0x2D8;

    constexpr uint32_t KThreadThreadId = 0x14C;
    constexpr uint32_t KThreadSize = 0xAB0;
}

GuestThreadContext::GuestThreadContext(uint32_t stackSize, uint32_t threadId, uint8_t cpu)
{
    pcr = g_memory.SystemAlloc(PcrSize);
    kthread = g_memory.SystemAlloc(KThreadSize);

    // __declspec(thread) data (initialised from .tls) is addressed directly from the TLS pointer;
    // the dynamic slots follow it.
    uint32_t tls = g_memory.SystemAlloc(g_module.tlsDataSize + g_module.tlsSlotCount * 4);
    memcpy(g_memory.Translate(tls), g_memory.Translate(g_module.tlsRawData), g_module.tlsRawDataSize);
    tlsSlots = tls + g_module.tlsDataSize;

    uint32_t stack = g_memory.SystemAlloc(stackSize, 0x1000);
    uint32_t stackTop = stack + stackSize;

    g_memory.Write<uint32_t>(pcr + PcrTlsPointer, tls);
    g_memory.Write<uint32_t>(pcr + PcrSelf, pcr);
    g_memory.Write<uint32_t>(pcr + PcrStackBase, stackTop);
    g_memory.Write<uint32_t>(pcr + PcrStackLimit, stack);
    g_memory.Write<uint32_t>(pcr + PcrCurrentThread, kthread);
    g_memory.Write<uint8_t>(pcr + PcrCurrentCpu, cpu);
    g_memory.Write<uint32_t>(kthread + KThreadThreadId, threadId);

    ctx.r1.u64 = stackTop;
    ctx.r13.u64 = pcr;
}

std::shared_ptr<GuestThread> CreateGuestThread(uint32_t start, uint32_t context, uint32_t xapiStartup,
    uint32_t stackSize, bool suspended, uint8_t cpu)
{
    auto thread = std::make_shared<GuestThread>();
    thread->id = g_nextThreadId++;
    thread->context = new GuestThreadContext(stackSize, thread->id, cpu);
    thread->suspendCount = suspended ? 1 : 0;

    // Waits on the KTHREAD (KeWaitForSingleObject) resolve to this object.
    g_memory.Write<uint8_t>(thread->context->kthread, ThreadObjectType);
    RegisterDispatcher(thread->context->kthread, thread);

    PlatformStartThread(HostStackSize, [info = StartInfo{ thread, start, context, xapiStartup }] { HostThreadMain(&info); });

    return thread;
}

void ExitGuestThread(uint32_t exitCode)
{
    throw ThreadExit{ exitCode };
}

std::shared_ptr<GuestThread> CurrentGuestThread(uint32_t pcr)
{
    return std::dynamic_pointer_cast<GuestThread>(LookupDispatcher(CurrentKThread(pcr)));
}

void QueueUserApc(GuestThread& thread, const UserApc& apc)
{
    {
        std::lock_guard lock(g_kernelLock);
        thread.apcs.push_back(apc);
        if (thread.apcs.sleeper != nullptr)
            thread.apcs.sleeper->notify_one();
    }
}

void DeliverUserApcs(PPCContext& ctx, GuestThread& thread)
{
    while (true)
    {
        UserApc apc;
        {
            std::lock_guard lock(g_kernelLock);
            if (thread.apcs.empty())
                return;
            apc = thread.apcs.front();
            thread.apcs.pop_front();
        }

        PPCContext saved = ctx;
        ctx.r3.u64 = apc.context;
        ctx.r4.u64 = apc.argument1;
        ctx.r5.u64 = apc.argument2;
        (PPC_LOOKUP_FUNC(g_memory.base, apc.routine))(ctx, g_memory.base);
        ctx = saved;
    }
}

void StartHostDrivenThread(std::function<void(PPCContext&)> body, uint8_t cpu)
{
    auto* context = new GuestThreadContext(0x10000, g_nextThreadId++, cpu);
    PlatformStartThread(HostStackSize, [body = std::move(body), context]
    {
        context->ctx.fpscr.loadFromHost();
        body(context->ctx);
    });
}
