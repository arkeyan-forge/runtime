#pragma once

// Per-frame measurements for the debug screen's Full level and the performance log (docs/runtime/host/gamemenu.md).
// Nothing is measured while Enabled() is false: each call site tests it first (one relaxed load), so the frame loop
// pays nothing when both are off. Every function is safe from any thread; none allocates.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gpu::framestats
{
    extern std::atomic<bool> g_enabled;
    inline bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
    // The debug screen at Full or the performance log recording. Turning it on starts from a clean history.
    void SetEnabled(bool on);

    constexpr uint32_t Unknown = UINT32_MAX;

    // One presented frame. Times are nanoseconds; Unknown where the platform or the frame cannot tell.
    struct Sample
    {
        uint64_t index = 0;     // the producer frame (gpu::detail::ProducerFrame) it ended
        uint64_t presentNs = 0; // SDL ticks when its present call returned
        uint32_t intervalNs = 0;        // present to present, on the thread that presents
        uint32_t rendererNs = 0;        // the renderer's CPU time on the game's render thread (the d3d hooks)
        uint32_t gameCpuNs = Unknown;   // the CPU time of the thread that ended the frame (the game's render thread)
        uint32_t vblankNs = 0;          // the game's render thread waiting for the emulated vertical blank
        uint32_t presentCallNs = 0;     // the present call (the swap chain's, with the display's wait), GPU thread
        uint32_t padToGameNs = Unknown; // the newest host input event before the game's input read, to that read
        uint32_t targetSwitches = 0;    // render target (framebuffer) changes on the GPU thread
        uint32_t gpuNs = Unknown;       // the GPU busy on its command lists (their union)
        uint32_t latencyNs = Unknown;   // the game's last input read before it ended, to its present call returning
        uint32_t stallNs = 0;           // the game's render thread waited for GPU work it needed back (read-backs)
        uint32_t draws = 0, resolves = 0; // the guest frame's own counters (d3d), 0 for a movie frame
        uint32_t uploads = 0;           // texture uploads queued
        uint64_t uploadBytes = 0;
        uint32_t faults = 0;            // write-watch faults
        uint32_t builds = 0;            // pipelines built at draw time
        uint32_t fileReads = 0;         // the game's file reads (NtReadFile)
        bool presented = false, gpuDone = false;
    };

    // The threads whose CPU use the debug screen shows; each registers itself (PlatformThreadCpuClock). A kind may
    // have several threads (the texture and pipeline workers): their times add up.
    enum class Thread : uint32_t
    {
        Game,     // the guest thread that swaps (the game's render thread)
        Renderer, // the GPU thread (gpu/worker.cpp)
        Main,     // the host's main thread (window, input, movies, this overlay)
        Textures, // the texture decode workers
        Shaders,  // the pipeline build threads (start-up)
        Audio,    // the audio driver thread (kernel/audio.cpp)
        Count
    };
    constexpr size_t MaxThreads = 16;
    void RegisterThread(Thread which, uint64_t clock);
    // The registered threads' clocks and kinds, at most MaxThreads; returns how many.
    size_t Threads(uint64_t* clocks, Thread* kinds);
    uint64_t ThreadClock(Thread which); // the first of that kind, 0: none

    // The game's render thread, at the guest's swap: the guest frame's draw and resolve counts.
    void GuestFrame(uint32_t draws, uint32_t resolves);
    void NoteInputRead();      // XamInputGetState
    void NoteInputEvent(uint64_t sdlNs); // a key, button or stick event, with SDL's timestamp (main thread)
    void NoteFileRead();       // NtReadFile
    void NoteStall(uint64_t ns); // a producer waited for the GPU thread to run work it needed back

    // Producer side of gpu::Present (`frame`: the producer frame it ends); the counters are totals since the start.
    struct Totals
    {
        uint64_t rendererNs, uploads, uploadBytes, faults, vblankNs;
        uint64_t gameCpuNs; // UINT64_MAX: unknown
    };
    void ProducerFrame(uint64_t frame, const Totals& totals);
    // GPU thread, when the frame's present returned (`builds`: pipelines built at draw time since the start).
    struct GpuTotals
    {
        uint64_t presentCallNs; // the frame's own
        uint64_t builds, targetSwitches; // since the start
    };
    void Presented(uint64_t frame, uint64_t presentNs, uint64_t intervalNs, const GpuTotals& totals);
    // GPU thread, a command list of `frame` done on the GPU between `start` and `end` (one clock per backend); `last`:
    // the frame's last one (its present pass). `known` false: the backend could not time it.
    void GpuSpan(uint64_t frame, uint64_t start, uint64_t end, bool known, bool last);

    // The measurement's own time (these calls and the GPU timing they need), for the debug screen's cost line.
    void AddCost(uint64_t ns);
    uint64_t CostNs();

    // The finished samples newer than `after` (oldest first; a sample is finished once its GPU time is known or it is
    // a few frames old), at most out.size(); returns how many. `after` = the last index read, for the next call.
    size_t ReadNew(uint64_t& after, std::span<Sample> out);
    // The most recent finished samples (oldest first), at most out.size().
    size_t ReadRecent(std::span<Sample> out);
}
