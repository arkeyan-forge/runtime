#pragma once

#include "memory.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

// Host side of a kernel object the guest refers to by handle (Nt*) or by its dispatcher header in guest memory (Ke*).
// All signal state is guarded by g_kernelLock. A waiting thread sleeps on its own condition variable, listed in every
// object it waits for (`sleepers`): a signal wakes only those threads. One shared condition variable woke every waiting
// thread (~25 in Giants) at each SetEvent or ReleaseSemaphore: 27 % of Giants' render thread (docs/runtime/xbox360/threads_sync.md).
struct KernelObject
{
    uint32_t header = 0; // guest dispatcher header of a Ke* object, 0 for handle-only objects
    uint32_t binding = 0; // the id written in that header (LookupDispatcher)
    std::vector<std::condition_variable*> sleepers; // the threads in WaitForObjects on this object

    // Wakes the threads waiting for this object, after a change of its signal state. Call with g_kernelLock held.
    void WakeSleepers() const
    {
        for (std::condition_variable* sleeper : sleepers)
            sleeper->notify_one();
    }

    virtual ~KernelObject() = default;

    // Called with g_kernelLock held; `thread` is the waiter's KTHREAD address.
    virtual bool IsSignaled(uint32_t thread) const { return false; }
    // Consumes the signal after a successful wait (auto-reset events, semaphores, mutants).
    virtual void Acquire(uint32_t thread) {}
    // The value the console keeps in the dispatcher header's SignalState.
    virtual int32_t SignalState() const { return 0; }

    // Mirrors the signal state into the guest header for code that reads it directly. Call with g_kernelLock held.
    void SyncHeader() const
    {
        if (header != 0)
            g_memory.Write<int32_t>(header + 4, SignalState());
    }
};

struct Event : KernelObject
{
    bool manualReset;
    bool signaled;

    // PulseEvent only releases threads already waiting: all of them for a manual-reset event, one otherwise.
    uint32_t waiters = 0;
    uint64_t pulseEpoch = 0;
    uint32_t pulseReleases = 0;

    Event(bool manualReset, bool signaled) : manualReset(manualReset), signaled(signaled) {}
    bool IsSignaled(uint32_t) const override { return signaled; }
    void Acquire(uint32_t) override
    {
        if (!manualReset)
            signaled = false;
    }
    int32_t SignalState() const override { return signaled; }

    // `epoch` is the pulseEpoch a waiter saw when it started waiting.
    bool PulsedSince(uint64_t epoch) const { return pulseEpoch != epoch && (manualReset || pulseReleases > 0); }
    void ConsumePulse()
    {
        if (!manualReset)
            pulseReleases--;
    }
    void Pulse()
    {
        if (waiters != 0)
        {
            pulseEpoch++;
            pulseReleases = manualReset ? 0 : 1;
        }
        signaled = false;
    }
};

struct Semaphore : KernelObject
{
    int32_t count;
    int32_t maximum;

    Semaphore(int32_t count, int32_t maximum) : count(count), maximum(maximum) {}
    bool IsSignaled(uint32_t) const override { return count > 0; }
    void Acquire(uint32_t) override { count--; }
    int32_t SignalState() const override { return count; }
};

struct Mutant : KernelObject
{
    uint32_t owner = 0;
    uint32_t recursion = 0;

    bool IsSignaled(uint32_t thread) const override { return owner == 0 || owner == thread; }
    void Acquire(uint32_t thread) override
    {
        owner = thread;
        recursion++;
    }
    int32_t SignalState() const override { return owner == 0 ? 1 : 0; }
};

extern std::mutex g_kernelLock;
// Only a new thread's start waits on it (until it is resumed); objects wake their own sleepers.
extern std::condition_variable g_kernelSignal;

constexpr uint32_t CurrentThreadHandle = 0xFFFFFFFE;
constexpr uint32_t StatusUserApc = 0x000000C0;
constexpr uint32_t StatusTimeout = 0x00000102;
constexpr uint32_t StatusInvalidHandle = 0xC0000008;

inline uint32_t CurrentKThread(uint32_t pcr) { return g_memory.Read<uint32_t>(pcr + 0x100); }

// 100 ns intervals since 1601, like the console's KeQuerySystemTime.
uint64_t GuestSystemTime();

// Keeps the kernel's KeTimeStampBundle at `bundle` current, as the console's clock interrupt does every
// millisecond. XAPI's GetTickCount reads its TickCount directly.
void StartTimeStampBundle(uint32_t bundle);

uint32_t CreateHandle(std::shared_ptr<KernelObject> object);
std::shared_ptr<KernelObject> LookupHandle(uint32_t handle);
bool CloseHandle(uint32_t handle);

template <typename T>
std::shared_ptr<T> LookupHandleAs(uint32_t handle)
{
    return std::dynamic_pointer_cast<T>(LookupHandle(handle));
}

// Ke* functions pass the object's dispatcher header; the host object is created from its type on first use and
// bound through a marker in the header's wait list, which the (inlined) Ke*Initialize* calls overwrite. One object per
// header address: a header initialised again replaces the previous object.
std::shared_ptr<KernelObject> LookupDispatcher(uint32_t header);
void RegisterDispatcher(uint32_t header, std::shared_ptr<KernelObject> object);

// A user-mode APC: routine(context, argument1, argument2), run by its thread during an alertable wait.
struct UserApc
{
    uint32_t routine;
    uint32_t context;
    uint32_t argument1;
    uint32_t argument2;
};

// Guarded by g_kernelLock. `sleeper` is the thread's condition variable while it waits alertably, so that queueing an APC
// wakes it.
struct ApcQueue : std::deque<UserApc>
{
    std::condition_variable* sleeper = nullptr;
};

// KeWaitForMultipleObjects semantics. `timeoutPointer` is the guest's optional LARGE_INTEGER (100 ns units,
// negative = relative, positive = absolute system time). An alertable wait (non-null `apcs`) ends with
// StatusUserApc as soon as an APC is queued; the caller then delivers them. Returns STATUS_WAIT_0 + index,
// StatusTimeout or StatusUserApc.
uint32_t WaitForObjects(const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll,
    uint32_t timeoutPointer, uint32_t thread, ApcQueue* apcs = nullptr);
