#include "kernel/objects.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <unordered_map>

std::mutex g_kernelLock;
std::condition_variable g_kernelSignal;

namespace
{
    // Dispatcher header types.
    constexpr uint8_t NotificationEventType = 0;
    constexpr uint8_t SynchronizationEventType = 1;
    constexpr uint8_t MutantType = 2;
    constexpr uint8_t SemaphoreType = 5;

    constexpr uint32_t SemaphoreLimit = 0x10;

    // The header's wait list (flink, blink) holds this marker and the binding id once the host object exists.
    constexpr uint32_t WaitListFlink = 0x8;
    constexpr uint32_t WaitListBlink = 0xC;
    constexpr uint32_t BindingMarker = 0x5353414B; // 'SSAK'

    std::mutex g_tableLock;
    std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
    std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_dispatchers; // binding id -> object
    uint32_t g_nextHandle = 0x10000;
    uint32_t g_nextBinding = 1;

    // Call with g_tableLock held.
    void Bind(uint32_t header, std::shared_ptr<KernelObject> object)
    {
        uint32_t binding = g_nextBinding++;
        object->header = header;
        g_dispatchers[binding] = std::move(object);
        g_memory.Write<uint32_t>(header + WaitListFlink, BindingMarker);
        g_memory.Write<uint32_t>(header + WaitListBlink, binding);
    }

    std::shared_ptr<KernelObject> CreateFromHeader(uint32_t header)
    {
        uint8_t type = g_memory.Read<uint8_t>(header);
        int32_t signalState = g_memory.Read<int32_t>(header + 4);

        switch (type)
        {
        case NotificationEventType:
            return std::make_shared<Event>(true, signalState != 0);
        case SynchronizationEventType:
            return std::make_shared<Event>(false, signalState != 0);
        case MutantType:
            return std::make_shared<Mutant>();
        case SemaphoreType:
            return std::make_shared<Semaphore>(signalState, g_memory.Read<int32_t>(header + SemaphoreLimit));
        default:
            fprintf(stderr, "[kernel] unsupported dispatcher object type %u at 0x%08X\n", type, header);
            return std::make_shared<Event>(true, false);
        }
    }
}

uint64_t GuestSystemTime()
{
    constexpr uint64_t UnixEpochAsFileTime = 116444736000000000ull;
    auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return UnixEpochAsFileTime + std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count() / 100;
}

uint32_t CreateHandle(std::shared_ptr<KernelObject> object)
{
    std::lock_guard lock(g_tableLock);
    uint32_t handle = g_nextHandle;
    g_nextHandle += 4;
    g_handles.emplace(handle, std::move(object));
    return handle;
}

std::shared_ptr<KernelObject> LookupHandle(uint32_t handle)
{
    std::lock_guard lock(g_tableLock);
    auto it = g_handles.find(handle);
    return it != g_handles.end() ? it->second : nullptr;
}

bool CloseHandle(uint32_t handle)
{
    std::lock_guard lock(g_tableLock);
    return g_handles.erase(handle) != 0;
}

std::shared_ptr<KernelObject> LookupDispatcher(uint32_t header)
{
    std::lock_guard lock(g_tableLock);
    if (g_memory.Read<uint32_t>(header + WaitListFlink) == BindingMarker)
    {
        auto it = g_dispatchers.find(g_memory.Read<uint32_t>(header + WaitListBlink));
        if (it != g_dispatchers.end() && it->second->header == header)
            return it->second;
    }

    auto object = CreateFromHeader(header);
    Bind(header, object);
    return object;
}

void RegisterDispatcher(uint32_t header, std::shared_ptr<KernelObject> object)
{
    std::lock_guard lock(g_tableLock);
    Bind(header, std::move(object));
}

uint32_t WaitForObjects(const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll, uint32_t timeoutPointer,
    uint32_t thread, ApcQueue* apcs)
{
    using Clock = std::chrono::steady_clock;

    std::optional<Clock::time_point> deadline;
    if (timeoutPointer != 0)
    {
        int64_t timeout = g_memory.Read<int64_t>(timeoutPointer);
        int64_t intervals = timeout < 0 ? -timeout : timeout - int64_t(GuestSystemTime());
        deadline = Clock::now() + std::chrono::nanoseconds(std::max<int64_t>(intervals, 0) * 100);
    }

    std::unique_lock lock(g_kernelLock);

    // Sleep on this thread's own condition variable, listed in every object waited for (and the APC queue).
    thread_local std::condition_variable t_sleeper;
    for (const auto& object : objects)
        object->sleepers.push_back(&t_sleeper);
    if (apcs != nullptr)
        apcs->sleeper = &t_sleeper;

    // Register as a waiter on every event so a PulseEvent issued from now on can release this thread.
    std::vector<Event*> events(objects.size());
    std::vector<uint64_t> epochs(objects.size());
    for (size_t i = 0; i < objects.size(); i++)
    {
        events[i] = dynamic_cast<Event*>(objects[i].get());
        if (events[i] != nullptr)
        {
            events[i]->waiters++;
            epochs[i] = events[i]->pulseEpoch;
        }
    }

    auto ready = [&](size_t i)
    {
        return objects[i]->IsSignaled(thread) || (events[i] != nullptr && events[i]->PulsedSince(epochs[i]));
    };
    auto take = [&](size_t i)
    {
        if (events[i] != nullptr && !events[i]->signaled)
            events[i]->ConsumePulse();
        else
            objects[i]->Acquire(thread);
        objects[i]->SyncHeader();
    };
    auto finish = [&](uint32_t result)
    {
        for (Event* event : events)
        {
            if (event != nullptr)
                event->waiters--;
        }
        for (const auto& object : objects)
        {
            auto& sleepers = object->sleepers;
            sleepers.erase(std::find(sleepers.begin(), sleepers.end(), &t_sleeper));
        }
        if (apcs != nullptr)
            apcs->sleeper = nullptr;
        return result;
    };

    while (true)
    {
        if (apcs != nullptr && !apcs->empty())
            return finish(StatusUserApc);

        if (waitAll && !objects.empty())
        {
            bool all = true;
            for (size_t i = 0; i < objects.size(); i++)
                all = all && ready(i);

            if (all)
            {
                for (size_t i = 0; i < objects.size(); i++)
                    take(i);
                return finish(0);
            }
        }
        else
        {
            for (size_t i = 0; i < objects.size(); i++)
            {
                if (ready(i))
                {
                    take(i);
                    return finish(uint32_t(i));
                }
            }
        }

        if (!deadline)
            t_sleeper.wait(lock);
        else if (t_sleeper.wait_until(lock, *deadline) == std::cv_status::timeout && Clock::now() >= *deadline)
            return finish(StatusTimeout);
    }
}
