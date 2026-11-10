#include "gpu/frame_stats.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <SDL3/SDL_timer.h>

std::atomic<bool> gpu::framestats::g_enabled;

namespace
{
    using namespace gpu::framestats;

    // A frame's sample and what is needed to finish it. The ring holds about 17 s at 60 FPS; a frame is finished within
    // two of its present (the GPU thread runs at most one frame ahead of the GPU).
    struct Slot
    {
        Sample sample;
        uint64_t inputNs = 0;  // the last input read before the frame ended
        uint64_t gpuEnd = 0;   // the latest end among its command lists so far
        uint64_t gpuBusy = 0;
        bool gpuUnknown = false, gpuAny = false;
    };
    constexpr size_t Slots = 1024;
    constexpr uint64_t FinishedAfter = 4; // frames after which a sample without its GPU time is finished anyway

    std::mutex g_mutex;
    std::array<Slot, Slots> g_slots;
    uint64_t g_newest = 0;              // the newest presented frame
    uint64_t g_first = UINT64_MAX;      // samples up to it are partial (measured from the middle of a frame)
    bool g_haveTotals = false, g_haveGpu = false;
    Totals g_totals{};
    GpuTotals g_gpu{};

    std::atomic<uint32_t> g_draws, g_resolves, g_fileReads;
    std::atomic<uint64_t> g_inputNs, g_stallNs, g_eventNs;
    std::atomic<uint32_t> g_padToGameNs{ Unknown };
    std::atomic<uint64_t> g_clocks[MaxThreads];
    std::atomic<uint32_t> g_kinds[MaxThreads];
    std::atomic<uint32_t> g_threadCount;
    std::atomic<uint64_t> g_cost;

    // SDL's clock: the one of its events' timestamps and of the present times the GPU thread reports.
    uint64_t NowNs() { return SDL_GetTicksNS(); }

    Slot& At(uint64_t frame)
    {
        Slot& slot = g_slots[frame % Slots];
        if (slot.sample.index != frame)
        {
            slot = {};
            slot.sample.index = frame;
        }
        return slot;
    }

    // Adds the time from its creation to its end to the measurement's cost.
    struct Cost
    {
        uint64_t start = NowNs();
        ~Cost() { g_cost.fetch_add(NowNs() - start, std::memory_order_relaxed); }
    };

    uint32_t Narrow(uint64_t value) { return uint32_t(std::min<uint64_t>(value, Unknown - 1)); }

    bool Finished(const Slot& slot, uint64_t frame)
    {
        return slot.sample.index == frame && slot.sample.presented && frame > g_first &&
               (slot.sample.gpuDone || g_newest >= frame + FinishedAfter);
    }

    Sample Finish(const Slot& slot)
    {
        Sample sample = slot.sample;
        if (!sample.gpuDone)
            sample.gpuNs = Unknown;
        return sample;
    }
}

void gpu::framestats::SetEnabled(bool on)
{
    std::lock_guard lock(g_mutex);
    if (on && !g_enabled.load(std::memory_order_relaxed))
    {
        g_slots.fill({});
        g_newest = 0;
        g_first = UINT64_MAX;
        g_haveTotals = g_haveGpu = false;
        g_draws = g_resolves = g_fileReads = 0;
        g_inputNs = g_stallNs = g_eventNs = 0;
        g_padToGameNs = Unknown;
    }
    g_enabled.store(on, std::memory_order_relaxed);
}

void gpu::framestats::RegisterThread(Thread which, uint64_t clock)
{
    if (clock == 0)
        return;
    static std::mutex registering;
    std::lock_guard lock(registering);
    uint32_t count = g_threadCount.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < count; i++)
        if (g_clocks[i].load(std::memory_order_relaxed) == clock)
            return;
    if (count == MaxThreads)
        return;
    g_clocks[count].store(clock, std::memory_order_relaxed);
    g_kinds[count].store(uint32_t(which), std::memory_order_relaxed);
    g_threadCount.store(count + 1, std::memory_order_release);
}

size_t gpu::framestats::Threads(uint64_t* clocks, Thread* kinds)
{
    uint32_t count = g_threadCount.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; i++)
    {
        clocks[i] = g_clocks[i].load(std::memory_order_relaxed);
        kinds[i] = Thread(g_kinds[i].load(std::memory_order_relaxed));
    }
    return count;
}

uint64_t gpu::framestats::ThreadClock(Thread which)
{
    uint32_t count = g_threadCount.load(std::memory_order_acquire);
    uint64_t found = 0;
    for (uint32_t i = 0; i < count; i++) // the newest of its kind (the game's render thread may change)
        if (g_kinds[i].load(std::memory_order_relaxed) == uint32_t(which))
            found = g_clocks[i].load(std::memory_order_relaxed);
    return found;
}

void gpu::framestats::GuestFrame(uint32_t draws, uint32_t resolves)
{
    g_draws.store(draws, std::memory_order_relaxed);
    g_resolves.store(resolves, std::memory_order_relaxed);
}

void gpu::framestats::NoteInputRead()
{
    uint64_t now = NowNs();
    g_inputNs.store(now, std::memory_order_relaxed);
    uint64_t event = g_eventNs.exchange(0, std::memory_order_relaxed);
    if (event != 0 && now >= event)
        g_padToGameNs.store(uint32_t(std::min<uint64_t>(now - event, Unknown - 1)), std::memory_order_relaxed);
}

void gpu::framestats::NoteInputEvent(uint64_t sdlNs)
{
    // The first event since the game's last read: the wait for the read starts there.
    uint64_t none = 0;
    g_eventNs.compare_exchange_strong(none, sdlNs != 0 ? sdlNs : NowNs(), std::memory_order_relaxed);
}
void gpu::framestats::NoteFileRead() { g_fileReads.fetch_add(1, std::memory_order_relaxed); }
void gpu::framestats::NoteStall(uint64_t ns) { g_stallNs.fetch_add(ns, std::memory_order_relaxed); }

void gpu::framestats::AddCost(uint64_t ns) { g_cost.fetch_add(ns, std::memory_order_relaxed); }
uint64_t gpu::framestats::CostNs() { return g_cost.load(std::memory_order_relaxed); }

void gpu::framestats::ProducerFrame(uint64_t frame, const Totals& totals)
{
    Cost cost;
    std::lock_guard lock(g_mutex);
    if (g_first == UINT64_MAX)
        g_first = frame; // this one started before the measurement did
    Slot& slot = At(frame);
    Sample& s = slot.sample;
    if (g_haveTotals)
    {
        s.rendererNs = Narrow(totals.rendererNs - g_totals.rendererNs);
        s.uploads = Narrow(totals.uploads - g_totals.uploads);
        s.uploadBytes = totals.uploadBytes - g_totals.uploadBytes;
        s.faults = Narrow(totals.faults - g_totals.faults);
        s.vblankNs = Narrow(totals.vblankNs - g_totals.vblankNs);
        if (totals.gameCpuNs != UINT64_MAX && g_totals.gameCpuNs != UINT64_MAX &&
            totals.gameCpuNs >= g_totals.gameCpuNs)
            s.gameCpuNs = Narrow(totals.gameCpuNs - g_totals.gameCpuNs);
    }
    g_totals = totals;
    g_haveTotals = true;
    s.draws = g_draws.exchange(0, std::memory_order_relaxed);
    s.resolves = g_resolves.exchange(0, std::memory_order_relaxed);
    s.fileReads = g_fileReads.exchange(0, std::memory_order_relaxed);
    s.stallNs = Narrow(g_stallNs.exchange(0, std::memory_order_relaxed));
    slot.inputNs = g_inputNs.load(std::memory_order_relaxed);
    s.padToGameNs = g_padToGameNs.exchange(Unknown, std::memory_order_relaxed);
}

void gpu::framestats::Presented(uint64_t frame, uint64_t presentNs, uint64_t intervalNs, const GpuTotals& totals)
{
    Cost cost;
    std::lock_guard lock(g_mutex);
    Slot& slot = At(frame);
    Sample& s = slot.sample;
    // SDL ticks and the steady clock are the same monotonic clock on every platform the runtime builds for; the
    // latency is still clamped, never negative.
    uint64_t now = NowNs();
    s.presentNs = presentNs;
    s.intervalNs = Narrow(intervalNs);
    s.latencyNs = slot.inputNs != 0 && now >= slot.inputNs ? Narrow(now - slot.inputNs) : Unknown;
    s.presentCallNs = Narrow(totals.presentCallNs);
    if (g_haveGpu)
    {
        s.builds = Narrow(totals.builds - g_gpu.builds);
        s.targetSwitches = Narrow(totals.targetSwitches - g_gpu.targetSwitches);
    }
    g_gpu = totals;
    g_haveGpu = true;
    s.presented = true;
    g_newest = std::max(g_newest, frame);
}

void gpu::framestats::GpuSpan(uint64_t frame, uint64_t start, uint64_t end, bool known, bool last)
{
    Cost cost;
    std::lock_guard lock(g_mutex);
    Slot& slot = At(frame);
    if (!known || end < start)
        slot.gpuUnknown = true;
    else
    {
        // Command lists of one queue run in order, but may overlap a little: count the time any of them ran.
        uint64_t from = std::max(start, slot.gpuEnd);
        if (end > from)
            slot.gpuBusy += end - from;
        slot.gpuEnd = std::max(slot.gpuEnd, end);
        slot.gpuAny = true;
    }
    if (last)
    {
        slot.sample.gpuDone = true;
        slot.sample.gpuNs = slot.gpuUnknown || !slot.gpuAny ? Unknown : Narrow(slot.gpuBusy);
    }
}

size_t gpu::framestats::ReadNew(uint64_t& after, std::span<Sample> out)
{
    std::lock_guard lock(g_mutex);
    size_t count = 0;
    uint64_t frame = std::max(after + 1, g_newest >= Slots ? g_newest - Slots + 1 : 1);
    for (; frame <= g_newest && count < out.size(); frame++)
    {
        const Slot& slot = g_slots[frame % Slots];
        if (slot.sample.index == frame && slot.sample.presented && frame > g_first)
        {
            if (!Finished(slot, frame))
                break;
            out[count++] = Finish(slot);
        }
        else if (g_newest < frame + FinishedAfter)
            break; // not presented yet
        after = frame;
    }
    return count;
}

size_t gpu::framestats::ReadRecent(std::span<Sample> out)
{
    std::lock_guard lock(g_mutex);
    uint64_t newest = g_newest;
    while (newest > 0 && newest + FinishedAfter > g_newest && !Finished(g_slots[newest % Slots], newest))
        newest--;
    size_t count = 0;
    for (uint64_t frame = newest; frame > 0 && newest - frame < Slots && count < out.size(); frame--)
    {
        const Slot& slot = g_slots[frame % Slots];
        if (slot.sample.index != frame || frame <= g_first)
            break;
        if (Finished(slot, frame))
            out[count++] = Finish(slot);
    }
    std::reverse(out.begin(), out.begin() + std::ptrdiff_t(count));
    return count;
}
