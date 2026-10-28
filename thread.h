#pragma once

#include "kernel/objects.h"
#include <functional>
#include <ppc_config.h>
#include <ppc_context.h>

// What a console thread sees: r13 points at its PCR (TLS pointer, current KTHREAD, CPU number),
// plus its KTHREAD, TLS block and stack, all in guest memory.
struct GuestThreadContext
{
    PPCContext ctx{};
    uint32_t pcr{};
    uint32_t kthread{};
    uint32_t tlsSlots{}; // dynamic TLS slots (KeTlsGetValue / KeTlsSetValue)

    GuestThreadContext(uint32_t stackSize, uint32_t threadId, uint8_t cpu);
};

// A guest thread as a waitable kernel object, signaled once it exits. Fields are guarded by g_kernelLock.
struct GuestThread : KernelObject
{
    GuestThreadContext* context{};
    uint32_t id{};
    uint32_t suspendCount{};
    bool exited{};
    uint32_t exitCode{};
    ApcQueue apcs;

    bool IsSignaled(uint32_t) const override { return exited; }
    int32_t SignalState() const override { return exited; }
};

// Runs `start(context)`, or `xapiStartup(start, context)` when given, on a new host thread.
std::shared_ptr<GuestThread> CreateGuestThread(uint32_t start, uint32_t context, uint32_t xapiStartup,
    uint32_t stackSize, bool suspended, uint8_t cpu);

// Unwinds the calling guest thread back to its host entry (ExTerminateThread).
[[noreturn]] void ExitGuestThread(uint32_t exitCode);

// The GuestThread whose PCR is `pcr`, or null for host-driven threads.
std::shared_ptr<GuestThread> CurrentGuestThread(uint32_t pcr);

// Queues a user-mode APC to `thread` and wakes it if it waits alertably.
void QueueUserApc(GuestThread& thread, const UserApc& apc);

// Runs the APCs queued to the calling thread, preserving its guest registers.
void DeliverUserApcs(PPCContext& ctx, GuestThread& thread);

// Runs host code that calls into the guest (interrupts, timers) on its own thread with a guest context.
void StartHostDrivenThread(std::function<void(PPCContext&)> body, uint8_t cpu);

// The crash report's "[crash] where:" line for the calling thread: its guest thread id and last stored lr, ctr, r1, r3, r31
// (the guest function itself is named by the host stack that follows, `sub_XXXXXXXX`). Safe to call from a fault handler.
void PrintGuestLocation();
