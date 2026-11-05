#pragma once

// Which guest pages were written since the renderer last read them, so that its checks of guest data (the buffer cache's
// vertex and index ranges, the textures) skip what nothing wrote instead of hashing it every frame. A watched page is
// read-only; the first write to it faults, the fault handler makes it writable again and stamps it, and the write
// proceeds. docs/runtime/gpu/renderer_threads.md, "The write watch".
//
// A page written again within HotFrames of being watched is left writable for CoolFrames frames: such a page (the
// game's own data next to a vertex buffer, a buffer it rewrites every frame) would fault every frame, and a fault
// costs more than hashing the page. Ranges that touch it are hashed meanwhile, as without the watch.
//
// Writes the host's kernel makes (a file read into guest memory) do not fault: they fail (EFAULT, Windows'
// ERROR_NOACCESS). Whatever reads into guest memory through a system call calls WillWrite first.
//
// Not available (Watch returns 0, the callers hash) without Init (the UWP build does not call it), with
// SKY_WRITE_WATCH=0, or where the fault hook cannot be installed. SKY_WRITE_WATCH=check: the callers also hash what the watch says is unchanged and count the
// differences (none expected).
#include <cstddef>
#include <cstdint>

namespace writewatch
{
    constexpr uint64_t HotFrames = 8;
    constexpr uint64_t CoolFrames = 240;

    // `base`, `size`: the guest memory block (page-aligned). Once, before any other call.
    void Init(uint8_t* base, uint64_t size);
    bool Enabled();
    bool Checking(); // SKY_WRITE_WATCH=check

    // Protects the pages holding [data, data + bytes) and returns a stamp for Unchanged, taken once they are all
    // protected: read the data after the call. 0 when the range cannot be watched (outside guest memory, a page
    // cooling down, the watch off).
    uint64_t Watch(const void* data, size_t bytes);

    // No write reached the pages of [data, data + bytes) since the Watch that returned `stamp` (never for 0).
    bool Unchanged(const void* data, size_t bytes, uint64_t stamp);

    // The host is about to write [data, data + bytes) in a way that cannot fault (a system call): its pages are made
    // writable and count as written. Does nothing outside guest memory.
    void WillWrite(void* data, size_t bytes);

    // Once a producer frame (the frame count HotFrames and CoolFrames are in).
    void FrameDone();

    // Since the last call: faults taken, pages protected, ranges found unchanged.
    struct Stats
    {
        uint64_t faults, protectedPages, unchanged;
    };
    Stats TakeStats();
}
