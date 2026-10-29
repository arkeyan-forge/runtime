#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <type_traits>
#include <byteswap.h>

// The guest's 32-bit address space, reserved as one host block: guest address A lives at base + A.
struct GuestMemory
{
    uint8_t* base{};

    void Reserve();

    void* Translate(uint32_t guest) const { return base + guest; }
    uint32_t MapVirtual(const void* host) const { return uint32_t(static_cast<const uint8_t*>(host) - base); }

    template <typename T>
    T Read(uint32_t guest) const
    {
        T value;
        memcpy(&value, base + guest, sizeof(T));
        return Swap(value);
    }

    template <typename T>
    void Write(uint32_t guest, T value)
    {
        value = Swap(value);
        memcpy(base + guest, &value, sizeof(T));
    }

    // XenonUtils' ByteSwap converts a float to an integer and back: floating-point values swap their bits instead.
    template <typename T>
    static T Swap(T value)
    {
        if constexpr (std::is_floating_point_v<T>)
        {
            using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
            return std::bit_cast<T>(ByteSwap(std::bit_cast<Bits>(value)));
        }
        else
        {
            return ByteSwap(value);
        }
    }

    // Zeroed allocation for kernel-owned objects (thread blocks, kernel variables).
    // Bump allocator: memory is never reclaimed.
    uint32_t SystemAlloc(uint32_t size, uint32_t align = 0x10);

private:
    std::mutex m_mutex;
    uint32_t m_systemNext{};
};

extern GuestMemory g_memory;

// First-fit allocator over [begin, end) of guest memory, at page granularity. Allocations come back zeroed.
class GuestHeap
{
public:
    GuestHeap(uint32_t begin, uint32_t end, uint32_t pageSize);

    // Returns 0 when there is no room. A non-zero `address` requests exactly that range.
    uint32_t Alloc(uint32_t size, uint32_t align = 0, uint32_t address = 0);
    // Returns the size that was freed, 0 if `address` doesn't start an allocation.
    uint32_t Free(uint32_t address);
    // Base of the allocation containing `address`, 0 if none.
    uint32_t FindAllocation(uint32_t address, uint32_t* size = nullptr) const;
    // Calls `visit(base, size)` for every allocation, in address order (on a copy: `visit` may allocate or free).
    void ForEachAllocation(const std::function<void(uint32_t base, uint32_t size)>& visit) const;

    bool Contains(uint32_t address) const { return address >= m_begin && address < m_end; }
    uint32_t PageSize() const { return m_pageSize; }

private:
    mutable std::mutex m_mutex;
    uint32_t m_begin;
    uint32_t m_end;
    uint32_t m_pageSize;
    std::map<uint32_t, uint32_t> m_free; // base -> size
    std::map<uint32_t, uint32_t> m_used; // base -> size
};

// The console's virtual address ranges for 4 KB and 64 KB pages (0x70000000+ is kept for SystemAlloc).
extern GuestHeap g_virtualHeap4K;
extern GuestHeap g_virtualHeap64K;

// 512 MB of physical memory; allocations are addressed through the 0xA0000000 (64 KB pages),
// 0xC0000000 (16 MB pages) and 0xE0000000 (4 KB pages) windows. The windows don't alias each other yet.
extern GuestHeap g_physicalHeap;

// Allocates physical memory addressed through `window` (0xA0000000, 0xC0000000 or 0xE0000000) and returns the guest
// address, 0 when out of memory. The window is remembered so hardware given a physical address can find it again.
uint32_t PhysicalAlloc(uint32_t size, uint32_t alignment, uint32_t window);
// Guest address of a physical address inside an allocation from PhysicalAlloc, 0 if there is none.
uint32_t PhysicalToGuest(uint32_t physical);
