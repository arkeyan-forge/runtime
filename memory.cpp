#include "memory.h"
#include "platform/platform.h"
#include <cstdio>
#include <cstdlib>

GuestMemory g_memory;
GuestHeap g_virtualHeap4K(0x00010000, 0x40000000, 0x1000);
GuestHeap g_virtualHeap64K(0x40000000, 0x70000000, 0x10000);
GuestHeap g_physicalHeap(0x00010000, 0x20000000, 0x1000); // offset 0 would read as a failed allocation

static constexpr uint64_t GuestMemorySize = 0x100000000ull;
static constexpr uint32_t SystemHeapBegin = 0x70000000;
static constexpr uint32_t SystemHeapEnd = 0x80000000;

void GuestMemory::Reserve()
{
    base = PlatformReserveMemory(GuestMemorySize);
    if (base == nullptr)
    {
        fprintf(stderr, "Cannot reserve 4 GB of address space for guest memory\n");
        exit(1);
    }

    // Nothing is mapped at the bottom of the console's address space: trap null pointer accesses.
    PlatformProtectNoAccess(base, 0x10000);

    m_systemNext = SystemHeapBegin;
}

uint32_t GuestMemory::SystemAlloc(uint32_t size, uint32_t align)
{
    std::lock_guard lock(m_mutex);

    uint32_t address = (m_systemNext + align - 1) & ~(align - 1);
    if (uint64_t(address) + size > SystemHeapEnd)
    {
        fprintf(stderr, "System heap exhausted allocating 0x%X bytes\n", size);
        abort();
    }

    m_systemNext = address + size;
    return address;
}

GuestHeap::GuestHeap(uint32_t begin, uint32_t end, uint32_t pageSize) : m_begin(begin), m_end(end), m_pageSize(pageSize)
{
    m_free.emplace(begin, end - begin);
}

uint32_t GuestHeap::Alloc(uint32_t size, uint32_t align, uint32_t address)
{
    std::lock_guard lock(m_mutex);

    size = (size + m_pageSize - 1) & ~(m_pageSize - 1);
    align = align > m_pageSize ? align : m_pageSize;
    if (size == 0)
        return 0;

    for (auto it = m_free.begin(); it != m_free.end(); ++it)
    {
        auto [blockBase, blockSize] = *it;
        uint64_t blockEnd = uint64_t(blockBase) + blockSize;
        uint64_t base = address != 0 ? address : (uint64_t(blockBase) + align - 1) & ~uint64_t(align - 1);
        if (base < blockBase || base + size > blockEnd)
            continue;

        m_free.erase(it);
        if (base > blockBase)
            m_free.emplace(blockBase, uint32_t(base - blockBase));
        if (base + size < blockEnd)
            m_free.emplace(uint32_t(base + size), uint32_t(blockEnd - base - size));

        m_used.emplace(uint32_t(base), size);
        return uint32_t(base);
    }

    return 0;
}

uint32_t GuestHeap::Free(uint32_t address)
{
    std::lock_guard lock(m_mutex);

    auto used = m_used.find(address);
    if (used == m_used.end())
        return 0;

    uint32_t base = used->first;
    uint32_t size = used->second;
    const uint32_t freedSize = size;
    m_used.erase(used);
    memset(g_memory.Translate(base), 0, size);

    // Coalesce with the neighbouring free blocks.
    auto next = m_free.lower_bound(base);
    if (next != m_free.end() && next->first == base + size)
    {
        size += next->second;
        next = m_free.erase(next);
    }
    if (next != m_free.begin())
    {
        auto previous = std::prev(next);
        if (previous->first + previous->second == base)
        {
            base = previous->first;
            size += previous->second;
            m_free.erase(previous);
        }
    }
    m_free.emplace(base, size);

    return freedSize;
}

uint32_t GuestHeap::FindAllocation(uint32_t address, uint32_t* size) const
{
    std::lock_guard lock(m_mutex);

    auto it = m_used.upper_bound(address);
    if (it == m_used.begin())
        return 0;

    --it;
    if (address >= it->first + it->second)
        return 0;

    if (size != nullptr)
        *size = it->second;
    return it->first;
}

void GuestHeap::ForEachAllocation(const std::function<void(uint32_t base, uint32_t size)>& visit) const
{
    std::map<uint32_t, uint32_t> used;
    {
        std::lock_guard lock(m_mutex);
        used = m_used;
    }
    for (const auto& [base, size] : used)
        visit(base, size);
}
