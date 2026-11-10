#include "write_watch.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include "host/env.h"
#include "platform/platform.h"

namespace
{
    enum : uint8_t
    {
        Protected = 1, // read-only now: the next write faults
        Watched = 2,   // was protected once: a fault on it is ours
    };

    struct Page
    {
        // The sequence value of the last write seen (a fault or WillWrite), 0 for none. Every stamp Watch returns and
        // every value stored here comes from g_sequence, so a write after a stamp is always larger than it.
        std::atomic<uint64_t> written;
        std::atomic<uint8_t> state;
        uint64_t protectedFrame; // under g_lock
        uint64_t coolUntil;      // under g_lock
    };
    static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<uint8_t>::is_always_lock_free);

    uint8_t* g_base;
    uint64_t g_low, g_size;
    uint32_t g_pageShift;
    Page* g_pages; // one per host page, zero-filled (PlatformReserveMemory)
    bool g_enabled, g_checking;
    std::atomic<uint64_t> g_sequence{ 1 };
    std::atomic<uint64_t> g_frame{ 1 };
    std::atomic<uint64_t> g_faults, g_protectedPages, g_unchanged;

    // Taken by the fault handler too: never held while touching guest memory, so a fault cannot meet it on its own
    // thread. Protecting a page and stamping are one step against a fault's unprotecting and stamping.
    std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
    struct SpinLock
    {
        SpinLock()
        {
            while (g_lock.test_and_set(std::memory_order_acquire))
            {}
        }
        ~SpinLock() { g_lock.clear(std::memory_order_release); }
    };

    uint8_t* PageAddress(uint64_t page) { return g_base + (page << g_pageShift); }

    // [first, last] for a range inside the watchable part of guest memory.
    bool Pages(const void* data, size_t bytes, uint64_t& first, uint64_t& last)
    {
        auto begin = static_cast<const uint8_t*>(data);
        if (!g_enabled || bytes == 0 || begin < g_base + g_low || begin + bytes > g_base + g_size)
            return false;
        first = uint64_t(begin - g_base) >> g_pageShift;
        last = uint64_t(begin + bytes - 1 - g_base) >> g_pageShift;
        return true;
    }

    bool OnWriteFault(void* address)
    {
        auto at = static_cast<uint8_t*>(address);
        if (at < g_base || at >= g_base + g_size)
            return false;
        uint64_t index = uint64_t(at - g_base) >> g_pageShift;
        Page& page = g_pages[index];
        if ((page.state.load(std::memory_order_acquire) & Watched) == 0)
            return false;
        SpinLock lock;
        // Another thread may have made it writable meanwhile: the retry then succeeds.
        if (page.state.load(std::memory_order_relaxed) & Protected)
        {
            page.written.store(g_sequence.fetch_add(1) + 1, std::memory_order_release);
            page.state.store(Watched, std::memory_order_release);
            PlatformProtectReadOnly(PageAddress(index), size_t(1) << g_pageShift, false);
            g_faults.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
}

void writewatch::Init(uint8_t* base, uint64_t size)
{
    const char* mode = skyenv::Get("SKY_WRITE_WATCH");
    if (mode != nullptr && strcmp(mode, "0") == 0)
        return;
    size_t pageSize = PlatformPageSize();
    if (pageSize == 0 || (pageSize & (pageSize - 1)) != 0)
        return;
    g_pageShift = uint32_t(std::countr_zero(uint64_t(pageSize)));
    g_base = base;
    g_size = size;
    // The bottom 64 KB stays inaccessible (null pointers, GuestMemory::Reserve): never watched.
    g_low = std::max<uint64_t>(0x10000, pageSize);
    g_pages = reinterpret_cast<Page*>(PlatformReserveMemory((size >> g_pageShift) * sizeof(Page)));
    if (g_pages == nullptr || !PlatformInstallWriteFaultFilter(OnWriteFault))
        return;
    g_checking = mode != nullptr && strcmp(mode, "check") == 0;
    g_enabled = true;
}

bool writewatch::Enabled() { return g_enabled; }

bool writewatch::Checking() { return g_checking; }

uint64_t writewatch::Watch(const void* data, size_t bytes)
{
    uint64_t first, last;
    if (!Pages(data, bytes, first, last))
        return 0;
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);
    SpinLock lock;
    for (uint64_t index = first; index <= last; index++)
    {
        Page& page = g_pages[index];
        if (page.coolUntil > frame)
            return 0;
        uint8_t state = page.state.load(std::memory_order_relaxed);
        if ((state & (Protected | Watched)) == Watched && frame < page.protectedFrame + writewatch::HotFrames)
        {
            page.coolUntil = frame + writewatch::CoolFrames;
            return 0;
        }
    }
    for (uint64_t index = first; index <= last;)
    {
        if (g_pages[index].state.load(std::memory_order_relaxed) & Protected)
        {
            index++;
            continue;
        }
        uint64_t end = index;
        while (end <= last && (g_pages[end].state.load(std::memory_order_relaxed) & Protected) == 0)
            end++;
        // Marked before the protection: a fault the moment it applies must find the page ours.
        for (uint64_t page = index; page < end; page++)
        {
            g_pages[page].state.store(Protected | Watched, std::memory_order_release);
            g_pages[page].protectedFrame = frame;
        }
        if (!PlatformProtectReadOnly(PageAddress(index), size_t(end - index) << g_pageShift, true))
        {
            for (uint64_t page = index; page < end; page++)
                g_pages[page].state.store(Watched, std::memory_order_release);
            return 0;
        }
        g_protectedPages.fetch_add(end - index, std::memory_order_relaxed);
        index = end;
    }
    return g_sequence.fetch_add(1) + 1;
}

bool writewatch::Unchanged(const void* data, size_t bytes, uint64_t stamp)
{
    uint64_t first, last;
    if (stamp == 0 || !Pages(data, bytes, first, last))
        return false;
    for (uint64_t index = first; index <= last; index++)
        if (g_pages[index].written.load(std::memory_order_acquire) > stamp)
            return false;
    g_unchanged.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void writewatch::WillWrite(void* data, size_t bytes)
{
    uint64_t first, last;
    if (!Pages(data, bytes, first, last))
        return;
    SpinLock lock;
    // A page that is not protected already counts as written after every stamp of a range holding it.
    for (uint64_t index = first; index <= last; index++)
    {
        Page& page = g_pages[index];
        if ((page.state.load(std::memory_order_relaxed) & Protected) == 0)
            continue;
        page.written.store(g_sequence.fetch_add(1) + 1, std::memory_order_release);
        page.state.store(Watched, std::memory_order_release);
        PlatformProtectReadOnly(PageAddress(index), size_t(1) << g_pageShift, false);
    }
}

void writewatch::FrameDone() { g_frame.fetch_add(1, std::memory_order_relaxed); }

writewatch::Stats writewatch::TakeStats()
{
    // g_faults keeps counting for FaultCount: TakeStats returns its growth since the last call.
    static std::atomic<uint64_t> taken;
    uint64_t faults = g_faults.load(std::memory_order_relaxed);
    return { faults - taken.exchange(faults, std::memory_order_relaxed),
        g_protectedPages.exchange(0, std::memory_order_relaxed), g_unchanged.exchange(0, std::memory_order_relaxed) };
}

uint64_t writewatch::FaultCount() { return g_faults.load(std::memory_order_relaxed); }
