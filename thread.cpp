#include "thread.h"
#include "loader.h"
#include "memory.h"
#include "platform/platform.h"
#include <atomic>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>

namespace
{
    // The guest context of this host thread, for the crash report (PrintGuestLocation).
    thread_local const PPCContext* t_guestContext;

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

    // A thread's guest blocks come from the system heap, which never frees: about 1000 stacks filled it. A thread that
    // ends gives them back here, and a new thread with the same stack size takes them. Its KTHREAD is reused only after
    // QuarantinedBlocks other threads of that size have ended: a title may still wait on the KTHREAD of a thread that
    // just ended (ObReferenceObjectByHandle), and must see that thread, not a new one.
    struct ThreadBlocks
    {
        uint32_t pcr, kthread, tls, stack;
    };
    constexpr size_t QuarantinedBlocks = 8;
    std::mutex g_blocksLock;
    std::map<uint32_t, std::deque<ThreadBlocks>> g_endedBlocks; // stack size -> blocks, oldest first

    uint32_t TlsSize() { return g_module.tlsDataSize + g_module.tlsSlotCount * 4; }

    ThreadBlocks TakeBlocks(uint32_t stackSize)
    {
        ThreadBlocks blocks{};
        bool reused = false;
        {
            std::lock_guard lock(g_blocksLock);
            auto ended = g_endedBlocks.find(stackSize);
            if (ended != g_endedBlocks.end() && ended->second.size() > QuarantinedBlocks)
            {
                blocks = ended->second.front();
                ended->second.pop_front();
                reused = true;
            }
        }
        if (reused)
        {
            // As SystemAlloc gives them: zeroed.
            memset(g_memory.Translate(blocks.pcr), 0, PcrSize);
            memset(g_memory.Translate(blocks.kthread), 0, KThreadSize);
            memset(g_memory.Translate(blocks.tls), 0, TlsSize());
            memset(g_memory.Translate(blocks.stack), 0, stackSize);
            return blocks;
        }
        blocks.pcr = g_memory.SystemAlloc(PcrSize);
        blocks.kthread = g_memory.SystemAlloc(KThreadSize);
        blocks.tls = g_memory.SystemAlloc(TlsSize());
        blocks.stack = g_memory.SystemAlloc(stackSize, 0x1000);
        return blocks;
    }

    void GiveBackBlocks(const GuestThreadContext& context)
    {
        std::lock_guard lock(g_blocksLock);
        g_endedBlocks[context.stackSize].push_back({ context.pcr, context.kthread, context.tls, context.stack });
    }

    void HostThreadMain(const StartInfo* info)
    {
        GuestThread& thread = *info->thread;
        {
            std::unique_lock lock(g_kernelLock);
            g_kernelSignal.wait(lock, [&] { return thread.suspendCount == 0; });
        }

        PPCContext& ctx = thread.context->ctx;
        t_guestContext = &ctx;
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
            CallGuest(ctx, function);
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
            // Still under the lock: once a waiter sees the exit, the blocks are free. A later wait on this thread's handle
            // no longer mirrors its state into the KTHREAD, which another thread may have by then.
            thread.header = 0;
            GiveBackBlocks(*thread.context);
        }
    }
}

GuestThreadContext::GuestThreadContext(uint32_t stackBytes, uint32_t threadId, uint8_t cpu) : stackSize(stackBytes)
{
    ThreadBlocks blocks = TakeBlocks(stackSize);
    pcr = blocks.pcr;
    kthread = blocks.kthread;
    tls = blocks.tls;
    stack = blocks.stack;

    // __declspec(thread) data (initialised from .tls) is addressed directly from the TLS pointer;
    // the dynamic slots follow it.
    memcpy(g_memory.Translate(tls), g_memory.Translate(g_module.tlsRawData), g_module.tlsRawDataSize);
    tlsSlots = tls + g_module.tlsDataSize;

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

std::shared_ptr<GuestThread> CreateGuestThread(
    uint32_t start, uint32_t context, uint32_t xapiStartup, uint32_t stackSize, bool suspended, uint8_t cpu)
{
    auto thread = std::make_shared<GuestThread>();
    thread->id = g_nextThreadId++;
    thread->context = std::make_unique<GuestThreadContext>(stackSize, thread->id, cpu);
    thread->suspendCount = suspended ? 1 : 0;

    // Waits on the KTHREAD (KeWaitForSingleObject) resolve to this object.
    g_memory.Write<uint8_t>(thread->context->kthread, ThreadObjectType);
    RegisterDispatcher(thread->context->kthread, thread);

    PlatformStartThread(
        HostStackSize, [info = StartInfo{ thread, start, context, xapiStartup }] { HostThreadMain(&info); });

    return thread;
}

void ExitGuestThread(uint32_t exitCode) { throw ThreadExit{ exitCode }; }

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
        CallGuest(ctx, apc.routine);
        ctx = saved;
    }
}

void StartHostDrivenThread(std::function<void(PPCContext&)> body, uint8_t cpu)
{
    auto* context = new GuestThreadContext(0x10000, g_nextThreadId++, cpu);
    PlatformStartThread(HostStackSize, [body = std::move(body), context] {
        t_guestContext = &context->ctx;
        context->ctx.fpscr.loadFromHost();
        body(context->ctx);
    });
}

void PrintGuestLocation()
{
    // Called from the fault handler, on the thread that faulted. The registers are those the recompiled code last stored:
    // a value it kept in a host register is not here, so lr and ctr name the last call that went through the context.
    const PPCContext* ctx = t_guestContext;
    if (ctx == nullptr)
    {
        fprintf(stderr, "[crash] where: not on a guest thread (a host thread: video, audio or the main loop)\n");
        return;
    }
    uint32_t pcr = ctx->r13.u32;
    uint32_t threadId = pcr != 0 ? g_memory.Read<uint32_t>(CurrentKThread(pcr) + 0x14C) : 0;
#if defined(PPC_CONFIG_CTR_AS_LOCAL) && defined(PPC_CONFIG_NON_VOLATILE_AS_LOCAL)
    // ctr and r31 are the translated functions' locals (docs/tools/toolchain/xenonrecomp.md): not in the context.
    fprintf(stderr, "[crash] where: guest thread %u, lr 0x%08X, r1 0x%08X, r3 0x%08X\n", threadId, uint32_t(ctx->lr),
        ctx->r1.u32, ctx->r3.u32);
#else
    fprintf(stderr, "[crash] where: guest thread %u, lr 0x%08X, ctr 0x%08X, r1 0x%08X, r3 0x%08X, r31 0x%08X\n",
        threadId, uint32_t(ctx->lr), ctx->ctr.u32, ctx->r1.u32, ctx->r3.u32, ctx->r31.u32);
#endif
}
