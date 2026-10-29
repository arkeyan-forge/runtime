#include "memory.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t StatusNoMemory = 0xC0000017;

    constexpr uint32_t MemDecommit = 0x4000;
    constexpr uint32_t MemLargePages = 0x20000000;
    constexpr uint32_t Mem16MBPages = 0x80000000;
    constexpr uint32_t PageReadWrite = 0x04;

    constexpr uint32_t PhysicalMask = 0x1FFFFFFF;
    constexpr uint32_t PhysicalWindowsBase = 0xA0000000;
    constexpr uint32_t E0WindowOffset = 0x1000;

    // The window each physical allocation is addressed through, by physical base. The windows don't alias, so
    // hardware handed a physical address (XMA) needs it to find the guest address the title reads and writes.
    std::mutex g_windowMutex;
    std::map<uint32_t, uint32_t> g_physicalWindows;
    // Bumped when a physical allocation is freed: PhysicalToGuest's per-thread cache holds for one generation.
    std::atomic<uint32_t> g_windowGeneration;

    uint32_t WindowAddress(uint32_t window, uint32_t physical)
    {
        // As on the console, the 0xE0000000 window (4 KB pages) addresses physical memory 0x1000 bytes further on.
        return window == 0xE0000000 ? 0xE0000000 | (physical - E0WindowOffset) : window | physical;
    }

    GuestHeap& VirtualHeapFor(uint32_t address, uint32_t allocationType)
    {
        if (address != 0)
            return g_virtualHeap64K.Contains(address) ? g_virtualHeap64K : g_virtualHeap4K;
        return (allocationType & MemLargePages) ? g_virtualHeap64K : g_virtualHeap4K;
    }

    // Allocation base containing `address` in whichever heap owns it, 0 if none.
    uint32_t FindAllocation(uint32_t address, uint32_t* size)
    {
        if (address >= PhysicalWindowsBase)
        {
            uint32_t offset = g_physicalHeap.FindAllocation(address & PhysicalMask, size);
            return offset != 0 ? (address & ~PhysicalMask) | offset : 0;
        }
        return VirtualHeapFor(address, 0).FindAllocation(address, size);
    }
}

uint32_t PhysicalAlloc(uint32_t size, uint32_t alignment, uint32_t window)
{
    uint32_t physical = g_physicalHeap.Alloc(size, alignment);
    if (physical == 0)
        return 0;

    std::lock_guard lock(g_windowMutex);
    g_physicalWindows[physical] = window;
    return window | physical;
}

uint32_t PhysicalToGuest(uint32_t physical)
{
    physical &= PhysicalMask;

    // Every draw translates its vertex, index and texture addresses, most of them in a few allocations (the vertex
    // ring buffer): remember the allocations used last per thread, most recent first.
    struct Allocation
    {
        uint32_t base = 0, size = 0, window = 0;
    };
    constexpr uint32_t CacheSize = 8;
    thread_local Allocation cache[CacheSize];
    thread_local uint32_t cacheGeneration = ~0u;
    uint32_t generation = g_windowGeneration.load(std::memory_order_acquire);
    if (cacheGeneration != generation)
    {
        for (Allocation& allocation : cache)
            allocation = {};
        cacheGeneration = generation;
    }
    for (uint32_t i = 0; i < CacheSize; i++)
    {
        if (physical - cache[i].base < cache[i].size)
        {
            Allocation found = cache[i];
            std::copy_backward(cache, cache + i, cache + i + 1);
            cache[0] = found;
            return WindowAddress(found.window, physical);
        }
    }

    std::lock_guard lock(g_windowMutex);
    uint32_t size = 0;
    auto window = g_physicalWindows.find(g_physicalHeap.FindAllocation(physical, &size));
    if (window == g_physicalWindows.end())
        return 0;
    std::copy_backward(cache, cache + CacheSize - 1, cache + CacheSize);
    cache[0] = { window->first, size, window->second };
    return WindowAddress(window->second, physical);
}

PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    uint32_t baseAddressPointer = ctx.r3.u32;
    uint32_t regionSizePointer = ctx.r4.u32;
    uint32_t allocationType = ctx.r5.u32;

    uint32_t requested = g_memory.Read<uint32_t>(baseAddressPointer);
    GuestHeap& heap = VirtualHeapFor(requested, allocationType);
    uint32_t pageSize = heap.PageSize();
    uint32_t alignedBase = requested & ~(pageSize - 1);
    uint32_t size =
        (g_memory.Read<uint32_t>(regionSizePointer) + (requested - alignedBase) + pageSize - 1) & ~(pageSize - 1);

    uint32_t address;
    if (alignedBase == 0)
        address = heap.Alloc(size);
    else if (heap.FindAllocation(alignedBase) != 0)
        address = alignedBase; // committing pages of an earlier reservation
    else
        address = heap.Alloc(size, 0, alignedBase);

    if (address == 0)
    {
        fprintf(stderr, "[kernel] NtAllocateVirtualMemory: out of memory (base 0x%08X size 0x%08X)\n", requested, size);
        ctx.r3.u64 = StatusNoMemory;
        return;
    }

    g_memory.Write<uint32_t>(baseAddressPointer, address);
    g_memory.Write<uint32_t>(regionSizePointer, size);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    uint32_t baseAddressPointer = ctx.r3.u32;
    uint32_t regionSizePointer = ctx.r4.u32;
    uint32_t freeType = ctx.r5.u32;

    // Decommitted pages stay reserved; their contents simply become undefined.
    if (!(freeType & MemDecommit))
    {
        uint32_t address = g_memory.Read<uint32_t>(baseAddressPointer);
        GuestHeap& heap = VirtualHeapFor(address, 0);
        uint32_t size = heap.Free(heap.FindAllocation(address));
        g_memory.Write<uint32_t>(regionSizePointer, size);
    }

    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32;
    uint32_t protect = ctx.r5.u32;
    uint32_t maxAddress = ctx.r7.u32;
    uint32_t alignment = ctx.r8.u32;

    // The page size picks the window the allocation is addressed through.
    uint32_t window = 0xE0000000;
    uint32_t pageSize = 0x1000;
    if (protect & MemLargePages)
    {
        window = 0xA0000000;
        pageSize = 0x10000;
    }
    else if (protect & Mem16MBPages)
    {
        window = 0xC0000000;
        pageSize = 0x1000000;
    }

    // The 0xE0000000 window addresses physical memory 0x1000 bytes further on (see PhysicalToGuest): reserve that page
    // too, so the physical range the GPU sees never overlaps the next allocation.
    uint32_t reserved = ((size + pageSize - 1) & ~(pageSize - 1)) + (window == 0xE0000000 ? E0WindowOffset : 0);
    uint32_t address = PhysicalAlloc(reserved, alignment > pageSize ? alignment : pageSize, window);
    if (address == 0)
    {
        fprintf(stderr, "[kernel] MmAllocatePhysicalMemoryEx: out of memory (size 0x%08X)\n", size);
        ctx.r3.u64 = 0;
        return;
    }
    if ((address & PhysicalMask) + size - 1 > maxAddress)
        fprintf(stderr, "[kernel] MmAllocatePhysicalMemoryEx: 0x%08X exceeds max address 0x%08X\n",
            address & PhysicalMask, maxAddress);

    ctx.r3.u64 = address;
}

PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    uint32_t physical = ctx.r4.u32 & PhysicalMask;
    {
        std::lock_guard lock(g_windowMutex);
        g_physicalWindows.erase(physical);
        g_windowGeneration.fetch_add(1, std::memory_order_release);
    }
    g_physicalHeap.Free(physical);
}

// (type, physical address, size, protect): engine 1 of the title's XMA code maps its hardware contexts this way.
// The context array is ordinary guest memory here, so hand back the address the allocation is used through.
PPC_FUNC(__imp__MmMapIoSpace) { ctx.r3.u64 = PhysicalToGuest(ctx.r4.u32); }

PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    uint32_t address = ctx.r3.u32;
    ctx.r3.u64 = (address & PhysicalMask) + (address >= 0xE0000000 ? E0WindowOffset : 0);
}

PPC_FUNC(__imp__MmQueryAllocationSize)
{
    uint32_t size = 0;
    FindAllocation(ctx.r3.u32, &size);
    ctx.r3.u64 = size;
}

PPC_FUNC(__imp__MmQueryAddressProtect)
{
    uint32_t address = ctx.r3.u32;
    bool image = address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    ctx.r3.u64 = image || FindAllocation(address, nullptr) != 0 ? PageReadWrite : 0;
}

// NtQueryVirtualMemory(address, MEMORY_BASIC_INFORMATION* out, debug memory flag): base address, allocation base,
// allocation protect, region size, state, protect, type (7 words). Every allocation here is committed and backed, so a
// region in a heap (or in the image) is MEM_COMMIT from the queried page to the allocation's end; elsewhere MEM_FREE,
// one page. Swap Force's XAPI heap creation (sub_82F45998) asks it whether its initial region is committed or only
// reserved: without an answer it built the heap at address 0. Layout and statuses as in Xenia (BSD).
PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    constexpr uint32_t MemCommit = 0x1000, MemFree = 0x10000, MemPrivate = 0x20000, MemImage = 0x1000000;
    constexpr uint32_t StatusInvalidParameter = 0xC000000D;
    uint32_t address = ctx.r3.u32;
    uint32_t info = ctx.r4.u32;
    if (info == 0)
    {
        ctx.r3.u64 = StatusInvalidParameter;
        return;
    }
    uint32_t page = address & ~0xFFFu, allocation = 0, size = 0, type = MemPrivate;
    if (address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
    {
        allocation = PPC_IMAGE_BASE;
        size = PPC_IMAGE_SIZE;
        type = MemImage;
    }
    else
    {
        allocation = FindAllocation(address, &size);
    }
    bool used = allocation != 0;
    uint32_t words[7] = { page, used ? allocation : 0, used ? PageReadWrite : 0,
        used ? allocation + size - page : 0x1000, used ? MemCommit : MemFree, used ? PageReadWrite : 0,
        used ? type : 0 };
    for (int i = 0; i < 7; i++)
        g_memory.Write<uint32_t>(info + 4 * i, words[i]);
    ctx.r3.u64 = StatusSuccess;
}

// MmQueryStatistics(MM_STATISTICS*): the caller sets the size field (first) to 104. Layout as in Xenia (BSD): size,
// total physical pages, kernel pages, a title section and a system section (available pages, total and reserved
// virtual bytes, physical, pool, stack, image, heap, virtual, page-table and cache pages), highest physical page.
// 512 MB of 4 KB pages; the title's used pages are everything allocated in the guest heaps, the rest is available.
PPC_FUNC(__imp__MmQueryStatistics)
{
    constexpr uint32_t StatisticsSize = 104;
    constexpr uint32_t StatusInvalidParameter = 0xC000000D;
    constexpr uint32_t StatusBufferTooSmall = 0xC0000023;
    constexpr uint32_t TotalPages = 0x20000;
    constexpr uint32_t KernelPages = 0x100;
    constexpr uint32_t TitleSection = 12;

    uint32_t stats = ctx.r3.u32;
    if (stats == 0)
    {
        ctx.r3.u64 = StatusInvalidParameter;
        return;
    }
    if (g_memory.Read<uint32_t>(stats) != StatisticsSize)
    {
        ctx.r3.u64 = StatusBufferTooSmall;
        return;
    }

    uint64_t usedBytes = 0;
    for (const GuestHeap* heap : { &g_physicalHeap, &g_virtualHeap4K, &g_virtualHeap64K })
        heap->ForEachAllocation([&](uint32_t, uint32_t size) { usedBytes += size; });
    uint32_t usedPages = uint32_t(std::min<uint64_t>(usedBytes / 0x1000, TotalPages - KernelPages));

    memset(g_memory.Translate(stats), 0, StatisticsSize);
    auto title = [&](uint32_t field, uint32_t value) {
        g_memory.Write<uint32_t>(stats + TitleSection + field * 4, value);
    };
    g_memory.Write<uint32_t>(stats + 0, StatisticsSize);
    g_memory.Write<uint32_t>(stats + 4, TotalPages);
    g_memory.Write<uint32_t>(stats + 8, KernelPages);
    title(0, TotalPages - KernelPages - usedPages); // available pages
    title(1, 0x2FFE0000); // total virtual bytes (Xenia's figure)
    title(2, uint32_t(std::min<uint64_t>(usedBytes, 0xFFFFFFFF))); // reserved virtual bytes
    title(3, usedPages); // physical pages
    title(4, 0x10); // pool
    title(5, 0x100); // stack
    title(6, uint32_t(PPC_IMAGE_SIZE / 0x1000)); // image
    title(7, 0x100); // heap
    title(8, 0x100); // virtual
    title(9, 0x100); // page table
    title(10, 0x100); // cache
    g_memory.Write<uint32_t>(stats + StatisticsSize - 4, TotalPages - 1); // highest physical page
    ctx.r3.u64 = StatusSuccess;
}
