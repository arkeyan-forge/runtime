#include "kernel/objects.h"
#include "loader.h"
#include "platform/platform.h"
#include "thread.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t CreateSuspended = 0x1;
    constexpr uint32_t TlsOutOfIndexes = 0xFFFFFFFF;

    std::atomic<uint64_t> g_tlsSlotsInUse;

    // Dynamic TLS slots follow the static TLS block the PCR points at.
    uint32_t TlsSlotAddress(const PPCContext& ctx, uint32_t index)
    {
        return g_memory.Read<uint32_t>(ctx.r13.u32) + g_module.tlsDataSize + index * 4;
    }

    // A zero-length sleep or a yield hands the hardware thread to another ready thread, or returns at once: the title
    // polls job queues and spin locks that way (its main thread spins while the render job waits for the vertical
    // blank). On the console that only costs a hardware thread it owns; here it would keep host cores busy, so a thread
    // that keeps yielding with no work in between (32 times, less than 100 us apart) backs off with short sleeps.
    void Yield()
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto SpinGap = std::chrono::microseconds(100);
        constexpr uint32_t SpinsBeforeSleeping = 32;
        constexpr auto BackOff = std::chrono::microseconds(200);

        thread_local Clock::time_point t_lastYield;
        thread_local uint32_t t_spins;

        t_spins = Clock::now() - t_lastYield < SpinGap ? t_spins + 1 : 0;
        if (t_spins < SpinsBeforeSleeping)
            std::this_thread::yield();
        else
            std::this_thread::sleep_for(BackOff);
        t_lastYield = Clock::now();
    }

    std::shared_ptr<GuestThread> ThreadFromHandle(uint32_t handle, uint32_t pcr)
    {
        if (handle == CurrentThreadHandle)
            return std::dynamic_pointer_cast<GuestThread>(LookupDispatcher(CurrentKThread(pcr)));
        return LookupHandleAs<GuestThread>(handle);
    }
}

PPC_FUNC(__imp__ExCreateThread)
{
    uint32_t handlePointer = ctx.r3.u32;
    uint32_t stackSize = ctx.r4.u32 != 0 ? ctx.r4.u32 : g_module.stackSize;
    uint32_t threadIdPointer = ctx.r5.u32;
    uint32_t xapiStartup = ctx.r6.u32;
    uint32_t start = ctx.r7.u32;
    uint32_t context = ctx.r8.u32;
    uint32_t flags = ctx.r9.u32;

    stackSize = std::max(0x4000u, (stackSize + 0xFFF) & ~0xFFFu);
    uint32_t affinity = flags >> 24;
    uint8_t cpu = affinity != 0 ? uint8_t(__builtin_ctz(affinity)) : 0;

    auto thread = CreateGuestThread(start, context, xapiStartup, stackSize, flags & CreateSuspended, cpu);
    fprintf(stderr, "[thread] %u created: start 0x%08X context 0x%08X%s\n", thread->id, start, context,
        (flags & CreateSuspended) ? " (suspended)" : "");
    if (handlePointer != 0)
        g_memory.Write<uint32_t>(handlePointer, CreateHandle(thread));
    if (threadIdPointer != 0)
        g_memory.Write<uint32_t>(threadIdPointer, thread->id);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__ExTerminateThread)
{
    ExitGuestThread(ctx.r3.u32);
}

PPC_FUNC(__imp__NtResumeThread)
{
    auto thread = ThreadFromHandle(ctx.r3.u32, ctx.r13.u32);
    if (thread == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    uint32_t previous;
    {
        std::lock_guard lock(g_kernelLock);
        previous = thread->suspendCount;
        if (thread->suspendCount != 0)
            thread->suspendCount--;
    }
    g_kernelSignal.notify_all();
    fprintf(stderr, "[thread] %u resumed\n", thread->id);

    if (ctx.r4.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r4.u32, previous);
    ctx.r3.u64 = StatusSuccess;
}

// Scheduling hints: the host scheduler decides.
PPC_FUNC(__imp__KeSetAffinityThread)
{
    if (ctx.r5.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r5.u32, ctx.r4.u32);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeQueryBasePriorityThread)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeSetDisableBoostThread)
{
    ctx.r3.u64 = 0;
}

// KeDelayExecutionThread(mode, alertable, interval): an alertable sleep ends early to run queued APCs.
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    if (ctx.r4.u32 != 0)
    {
        if (auto thread = CurrentGuestThread(ctx.r13.u32))
        {
            uint32_t status = WaitForObjects({}, false, ctx.r5.u32, CurrentKThread(ctx.r13.u32), &thread->apcs);
            if (status == StatusUserApc)
                DeliverUserApcs(ctx, *thread);
            ctx.r3.u64 = status == StatusUserApc ? StatusUserApc : StatusSuccess;
            return;
        }
    }

    int64_t interval = g_memory.Read<int64_t>(ctx.r5.u32);
    if (interval == 0)
        Yield();
    else
        std::this_thread::sleep_for(std::chrono::nanoseconds((interval < 0 ? -interval : interval - int64_t(GuestSystemTime())) * 100));
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtYieldExecution)
{
    Yield();
    ctx.r3.u64 = StatusSuccess;
}

// Dynamic TLS

PPC_FUNC(__imp__KeTlsAlloc)
{
    uint64_t inUse = g_tlsSlotsInUse.load();
    while (true)
    {
        uint32_t index = __builtin_ctzll(~inUse);
        if (~inUse == 0 || index >= g_module.tlsSlotCount)
        {
            ctx.r3.u64 = TlsOutOfIndexes;
            return;
        }
        if (g_tlsSlotsInUse.compare_exchange_weak(inUse, inUse | (1ull << index)))
        {
            ctx.r3.u64 = index;
            return;
        }
    }
}

PPC_FUNC(__imp__KeTlsFree)
{
    g_tlsSlotsInUse &= ~(1ull << ctx.r3.u32);
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__KeTlsGetValue)
{
    ctx.r3.u64 = g_memory.Read<uint32_t>(TlsSlotAddress(ctx, ctx.r3.u32));
}

PPC_FUNC(__imp__KeTlsSetValue)
{
    g_memory.Write<uint32_t>(TlsSlotAddress(ctx, ctx.r3.u32), ctx.r4.u32);
    ctx.r3.u64 = 1;
}

// Only the debugger thread-naming exception (MSVC's SetThreadName) is raised by this title; log the name.
PPC_FUNC(__imp__RtlRaiseException)
{
    constexpr uint32_t SetThreadNameException = 0x406D1388;
    uint32_t record = ctx.r3.u32;
    uint32_t code = g_memory.Read<uint32_t>(record);
    if (code != SetThreadNameException)
    {
        fprintf(stderr, "[kernel] RtlRaiseException 0x%08X ignored (lr=0x%08X)\n", code, uint32_t(ctx.lr));
        return;
    }

    // EXCEPTION_RECORD.ExceptionInformation: THREADNAME_INFO { 0x1000, name, thread id, flags }.
    uint32_t name = g_memory.Read<uint32_t>(record + 0x14 + 4);
    uint32_t threadId = g_memory.Read<uint32_t>(record + 0x14 + 8);
    uint32_t self = g_memory.Read<uint32_t>(CurrentKThread(ctx.r13.u32) + 0x14C);
    auto* text = static_cast<const char*>(g_memory.Translate(name));
    fprintf(stderr, "[thread] %u named \"%s\"\n", threadId == 0xFFFFFFFF ? self : threadId, text);
    if (threadId == 0xFFFFFFFF || threadId == self)
        PlatformSetThreadName(text);
}
