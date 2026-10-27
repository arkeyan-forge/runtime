#include "kernel/overlapped.h"
#include "kernel/objects.h"
#include "thread.h"
#include <atomic>

namespace
{
    // XOVERLAPPED fields.
    constexpr uint32_t InternalLow = 0x00;
    constexpr uint32_t InternalHigh = 0x04;
    constexpr uint32_t EventHandle = 0x0C;
    constexpr uint32_t CompletionRoutine = 0x10;
    constexpr uint32_t ExtendedError = 0x18;
}

void SignalEventHandle(uint32_t handle)
{
    auto event = handle != 0 ? LookupHandleAs<Event>(handle) : nullptr;
    if (event == nullptr)
        return;

    {
        std::lock_guard lock(g_kernelLock);
        event->signaled = true;
        event->SyncHeader();
        event->WakeSleepers();
    }
}

void CompleteXOverlapped(uint32_t overlapped, uint32_t result, uint32_t length, uint32_t pcr)
{
    g_memory.Write<uint32_t>(overlapped + InternalHigh, length);
    g_memory.Write<uint32_t>(overlapped + ExtendedError, result == 0 ? 0 : 0x80070000 | (result & 0xFFFF));

    // XGetOverlappedResult polls InternalLow from other threads: publish it last.
    std::atomic_thread_fence(std::memory_order_release);
    g_memory.Write<uint32_t>(overlapped + InternalLow, result);

    SignalEventHandle(g_memory.Read<uint32_t>(overlapped + EventHandle));

    if (uint32_t routine = g_memory.Read<uint32_t>(overlapped + CompletionRoutine))
    {
        if (auto thread = CurrentGuestThread(pcr))
            QueueUserApc(*thread, { routine, result, length, overlapped });
    }
}

uint32_t FinishXamCall(uint32_t overlapped, uint32_t result, uint32_t length, uint32_t pcr)
{
    if (overlapped == 0)
        return result;

    CompleteXOverlapped(overlapped, result, length, pcr);
    return ErrorIoPending;
}
