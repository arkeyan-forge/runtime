#include "kernel/objects.h"
#include "thread.h"
#include <atomic>
#include <cstdio>
#include <thread>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t StatusInvalidParameter1 = 0xC00000EF;
    constexpr uint32_t MaximumWaitObjects = 64;
    constexpr uint32_t WaitAll = 0;
    constexpr uint32_t DuplicateCloseSource = 1;

    // X_RTL_CRITICAL_SECTION fields after its dispatcher header.
    constexpr uint32_t CsLockCount = 0x10;
    constexpr uint32_t CsRecursionCount = 0x14;
    constexpr uint32_t CsOwningThread = 0x18;

    std::atomic_ref<uint32_t> GuestAtomic(uint32_t address)
    {
        return std::atomic_ref<uint32_t>(*static_cast<uint32_t*>(g_memory.Translate(address)));
    }

    void InitializeCriticalSection(uint32_t cs)
    {
        g_memory.Write<uint8_t>(cs, 1); // synchronization event header
        g_memory.Write<int32_t>(cs + 4, 0);
        g_memory.Write<int32_t>(cs + CsLockCount, -1);
        g_memory.Write<int32_t>(cs + CsRecursionCount, 0);
        g_memory.Write<uint32_t>(cs + CsOwningThread, 0);
    }

    // The owner is the KTHREAD, stored big-endian like the console does.
    bool TryEnterCriticalSection(uint32_t cs, uint32_t thread)
    {
        uint32_t self = ByteSwap(thread);
        uint32_t expected = 0;
        if (!GuestAtomic(cs + CsOwningThread).compare_exchange_strong(expected, self) && expected != self)
            return false;

        g_memory.Write<int32_t>(cs + CsRecursionCount, g_memory.Read<int32_t>(cs + CsRecursionCount) + 1);
        return true;
    }

    void AcquireSpinLock(uint32_t lock, uint32_t pcr)
    {
        uint32_t expected = 0;
        while (!GuestAtomic(lock).compare_exchange_weak(expected, ByteSwap(pcr)))
        {
            expected = 0;
            std::this_thread::yield();
        }
    }

    void ReleaseSpinLock(uint32_t lock) { GuestAtomic(lock).store(0); }

    std::shared_ptr<KernelObject> ObjectFromHandle(uint32_t handle, uint32_t pcr)
    {
        if (handle == CurrentThreadHandle)
            return LookupDispatcher(CurrentKThread(pcr));
        return LookupHandle(handle);
    }

    uint32_t SetEvent(Event& event)
    {
        uint32_t previous;
        {
            std::lock_guard lock(g_kernelLock);
            previous = event.signaled;
            event.signaled = true;
            event.SyncHeader();
            event.WakeSleepers();
        }
        return previous;
    }

    uint32_t ResetEvent(Event& event)
    {
        std::lock_guard lock(g_kernelLock);
        uint32_t previous = event.signaled;
        event.signaled = false;
        event.SyncHeader();
        return previous;
    }

    std::shared_ptr<Event> EventFromDispatcher(uint32_t header)
    {
        // XAudio2's driver callback can signal its event before creating it; the console tolerates that.
        if (header == 0)
            return nullptr;

        auto event = std::dynamic_pointer_cast<Event>(LookupDispatcher(header));
        if (event == nullptr)
            fprintf(stderr, "[kernel] 0x%08X is not an event\n", header);
        return event;
    }
}

// Critical sections and spin locks

PPC_FUNC(__imp__RtlInitializeCriticalSection) { InitializeCriticalSection(ctx.r3.u32); }

PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    InitializeCriticalSection(ctx.r3.u32);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t thread = CurrentKThread(ctx.r13.u32);
    while (!TryEnterCriticalSection(cs, thread))
    {
        auto owner = GuestAtomic(cs + CsOwningThread);
        uint32_t current = owner.load();
        if (current != 0)
            owner.wait(current);
    }
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = TryEnterCriticalSection(ctx.r3.u32, CurrentKThread(ctx.r13.u32));
}

PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    int32_t recursion = g_memory.Read<int32_t>(cs + CsRecursionCount) - 1;
    g_memory.Write<int32_t>(cs + CsRecursionCount, recursion);
    if (recursion == 0)
    {
        auto owner = GuestAtomic(cs + CsOwningThread);
        owner.store(0);
        owner.notify_all();
    }
}

PPC_FUNC(__imp__KfAcquireSpinLock)
{
    AcquireSpinLock(ctx.r3.u32, ctx.r13.u32);
    ctx.r3.u64 = 0; // previous IRQL
}

PPC_FUNC(__imp__KfReleaseSpinLock) { ReleaseSpinLock(ctx.r3.u32); }

PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) { AcquireSpinLock(ctx.r3.u32, ctx.r13.u32); }

PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) { ReleaseSpinLock(ctx.r3.u32); }

// Kernel APCs and the L2 cache lock don't exist here.
PPC_FUNC(__imp__KeEnterCriticalRegion) {}
PPC_FUNC(__imp__KeLeaveCriticalRegion) {}
PPC_FUNC(__imp__KeLockL2) {}
PPC_FUNC(__imp__KeUnlockL2) {}

// Events

PPC_FUNC(__imp__KeSetEvent)
{
    auto event = EventFromDispatcher(ctx.r3.u32);
    ctx.r3.u64 = event != nullptr ? SetEvent(*event) : 0;
}

PPC_FUNC(__imp__KeResetEvent)
{
    auto event = EventFromDispatcher(ctx.r3.u32);
    ctx.r3.u64 = event != nullptr ? ResetEvent(*event) : 0;
}

PPC_FUNC(__imp__NtCreateEvent)
{
    // Event type 0 is a notification (manual-reset) event, 1 a synchronization (auto-reset) one.
    auto event = std::make_shared<Event>(ctx.r5.u32 == 0, ctx.r6.u32 != 0);
    g_memory.Write<uint32_t>(ctx.r3.u32, CreateHandle(event));
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtSetEvent)
{
    auto event = LookupHandleAs<Event>(ctx.r3.u32);
    if (event == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    uint32_t previous = SetEvent(*event);
    if (ctx.r4.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r4.u32, previous);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtClearEvent)
{
    auto event = LookupHandleAs<Event>(ctx.r3.u32);
    if (event == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    ResetEvent(*event);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtPulseEvent)
{
    auto event = LookupHandleAs<Event>(ctx.r3.u32);
    if (event == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    uint32_t previous;
    {
        std::lock_guard lock(g_kernelLock);
        previous = event->signaled;
        event->Pulse();
        event->SyncHeader();
        event->WakeSleepers();
    }

    if (ctx.r4.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r4.u32, previous);
    ctx.r3.u64 = StatusSuccess;
}

// Semaphores and mutants

PPC_FUNC(__imp__NtCreateSemaphore)
{
    auto semaphore = std::make_shared<Semaphore>(ctx.r5.s32, ctx.r6.s32);
    g_memory.Write<uint32_t>(ctx.r3.u32, CreateHandle(semaphore));
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtReleaseSemaphore)
{
    auto semaphore = LookupHandleAs<Semaphore>(ctx.r3.u32);
    if (semaphore == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    int32_t previous;
    {
        std::lock_guard lock(g_kernelLock);
        previous = semaphore->count;
        semaphore->count = std::min(semaphore->count + ctx.r4.s32, semaphore->maximum);
        semaphore->SyncHeader();
        semaphore->WakeSleepers();
    }

    if (ctx.r5.u32 != 0)
        g_memory.Write<int32_t>(ctx.r5.u32, previous);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtCreateMutant)
{
    auto mutant = std::make_shared<Mutant>();
    if (ctx.r5.u32 != 0)
    {
        mutant->owner = CurrentKThread(ctx.r13.u32);
        mutant->recursion = 1;
    }
    g_memory.Write<uint32_t>(ctx.r3.u32, CreateHandle(mutant));
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtReleaseMutant)
{
    auto mutant = LookupHandleAs<Mutant>(ctx.r3.u32);
    if (mutant == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    {
        std::lock_guard lock(g_kernelLock);
        if (mutant->owner == CurrentKThread(ctx.r13.u32) && --mutant->recursion == 0)
            mutant->owner = 0;
        mutant->SyncHeader();
        mutant->WakeSleepers();
    }
    ctx.r3.u64 = StatusSuccess;
}

// Handles

PPC_FUNC(__imp__NtClose) { ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? StatusSuccess : StatusInvalidHandle; }

PPC_FUNC(__imp__NtDuplicateObject)
{
    auto object = ObjectFromHandle(ctx.r3.u32, ctx.r13.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    g_memory.Write<uint32_t>(ctx.r4.u32, CreateHandle(object));
    if (ctx.r5.u32 & DuplicateCloseSource)
        CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    uint32_t handle = ctx.r3.u32;
    uint32_t objectPointer = ctx.r5.u32;

    // Only threads have a guest-side object (their KTHREAD) so far.
    uint32_t object = 0;
    if (handle == CurrentThreadHandle)
        object = CurrentKThread(ctx.r13.u32);
    else if (auto thread = LookupHandleAs<GuestThread>(handle))
        object = thread->context->kthread;
    else
        fprintf(stderr, "[kernel] ObReferenceObjectByHandle: no guest object for handle 0x%08X\n", handle);

    g_memory.Write<uint32_t>(objectPointer, object);
    ctx.r3.u64 = object != 0 ? StatusSuccess : StatusInvalidHandle;
}

PPC_FUNC(__imp__ObDereferenceObject) {}

// Waits

namespace
{
    // Shared by every wait: an alertable wait on a guest thread runs the APCs that interrupt it.
    uint32_t Wait(PPCContext& ctx, const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll,
        uint32_t timeoutPointer, bool alertable)
    {
        auto thread = alertable ? CurrentGuestThread(ctx.r13.u32) : nullptr;
        uint32_t status = WaitForObjects(
            objects, waitAll, timeoutPointer, CurrentKThread(ctx.r13.u32), thread != nullptr ? &thread->apcs : nullptr);
        if (status == StatusUserApc)
            DeliverUserApcs(ctx, *thread);
        return status;
    }
}

PPC_FUNC(__imp__KeWaitForSingleObject)
{
    std::vector objects{ LookupDispatcher(ctx.r3.u32) };
    ctx.r3.u64 = Wait(ctx, objects, false, ctx.r7.u32, ctx.r6.u32 != 0);
}

PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    std::vector<std::shared_ptr<KernelObject>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
        objects.push_back(LookupDispatcher(g_memory.Read<uint32_t>(ctx.r4.u32 + i * 4)));

    ctx.r3.u64 = Wait(ctx, objects, ctx.r5.u32 == WaitAll, ctx.r9.u32, ctx.r8.u32 != 0);
}

PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    auto object = ObjectFromHandle(ctx.r3.u32, ctx.r13.u32);
    if (object == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    std::vector objects{ object };
    ctx.r3.u64 = Wait(ctx, objects, false, ctx.r6.u32, ctx.r5.u32 != 0);
}

PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    // Like NT, an empty or oversized handle list fails at once. The title relies on it: after a level load that queued
    // no WideLoad jobs it waits for all of zero completion events (sub_82846398), and must not block.
    if (ctx.r3.u32 == 0 || ctx.r3.u32 > MaximumWaitObjects)
    {
        ctx.r3.u64 = StatusInvalidParameter1;
        return;
    }

    std::vector<std::shared_ptr<KernelObject>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
    {
        auto object = ObjectFromHandle(g_memory.Read<uint32_t>(ctx.r4.u32 + i * 4), ctx.r13.u32);
        if (object == nullptr)
        {
            ctx.r3.u64 = StatusInvalidHandle;
            return;
        }
        objects.push_back(std::move(object));
    }

    ctx.r3.u64 = Wait(ctx, objects, ctx.r5.u32 == WaitAll, ctx.r8.u32, ctx.r7.u32 != 0);
}
