// The GPU thread: the public gpu functions called by the game's render thread (and the main thread, for movies) queue
// their command list work here, so recording and submitting overlap the next draws' state translation. The callers
// ("producers") keep what must read guest memory at the call (texture and vertex data, which they copy into upload
// memory of their own) and copy the rest of a draw into a per-frame arena. Present ends a producer frame; producers
// run at most one frame ahead of the GPU thread. SKY_GPU_THREAD=0 runs everything on the caller, as before.
#include "gpu/context.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <utility>
#include <thread>
#include "../host/env.h"

using namespace plume;

namespace gpu::detail
{
    std::recursive_mutex g_producerMutex;
}

using namespace gpu::detail;

namespace
{
    constexpr size_t ArenaChunkSize = 4 << 20;
    constexpr uint64_t MinUploadBlockSize = 8 << 20;

    struct Worker
    {
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake;     // the GPU thread: commands queued, or stop
        std::condition_variable progress; // producers: a frame or a synchronous command done
        std::vector<std::function<void()>> commands;
        bool sleeping = false;
        bool stop = false;
        uint64_t presentsDone = 0;        // producer frames the GPU thread has ended
        // Busy time of the GPU thread and producers' waits for it, for the perf log.
        uint64_t busyNs = 0;
        uint64_t producerWaitNs = 0;
    };
    Worker* g_worker = nullptr;

    // Per-frame arena (producer side): chunks handed to the GPU thread with the frame's Present, then reused.
    struct Arena
    {
        std::vector<uint8_t*> chunks; // of this frame; the last one is being filled
        size_t used = ArenaChunkSize;
    };
    Arena g_arena;
    std::mutex g_freeChunksMutex;
    std::vector<uint8_t*> g_freeChunks;

    // Producer upload memory, tagged with the producer frame that last used it: reusable once the GPU finished it.
    struct UploadBlock
    {
        std::unique_ptr<RenderBuffer> buffer;
        uint64_t capacity = 0;
        uint64_t used = 0;
        uint64_t frame = 0;
    };
    std::vector<UploadBlock> g_uploadBlocks;
    size_t g_uploadCurrent = SIZE_MAX;

    // Texture workers (UploadTextureAsync): decode guest texture data into upload memory, in parallel.
    struct TextureWorkers
    {
        std::vector<std::thread> threads;
        std::mutex mutex;
        std::condition_variable wake, done;
        std::deque<std::pair<std::function<void()>, std::shared_ptr<std::atomic<bool>>>> jobs;
        bool stop = false;
        uint64_t busyNs = 0; // all threads together, for the perf log
        uint32_t jobsDone = 0;
    };
    TextureWorkers* g_textureWorkers = nullptr;

    void RunTextureWorker()
    {
        while (true)
        {
            std::pair<std::function<void()>, std::shared_ptr<std::atomic<bool>>> job;
            {
                std::unique_lock lock(g_textureWorkers->mutex);
                g_textureWorkers->wake.wait(
                    lock, [] { return !g_textureWorkers->jobs.empty() || g_textureWorkers->stop; });
                if (g_textureWorkers->jobs.empty())
                    return;
                job = std::move(g_textureWorkers->jobs.front());
                g_textureWorkers->jobs.pop_front();
            }
            uint64_t start = SDL_GetTicksNS();
            job.first();
            std::lock_guard lock(g_textureWorkers->mutex);
            g_textureWorkers->busyNs += SDL_GetTicksNS() - start;
            g_textureWorkers->jobsDone++;
            job.second->store(true);
            g_textureWorkers->done.notify_all();
        }
    }

    uint64_t g_producerFrame = 1;            // the frame producers record
    std::atomic<uint64_t> g_completedFrame;  // the last producer frame the GPU has finished
    uint64_t g_consumerFrame = 1;            // the frame the GPU thread records

    void Run()
    {
        std::vector<std::function<void()>> batch;
        while (true)
        {
            {
                std::unique_lock lock(g_worker->mutex);
                g_worker->sleeping = true;
                g_worker->wake.wait(lock, [] { return !g_worker->commands.empty() || g_worker->stop; });
                g_worker->sleeping = false;
                if (g_worker->commands.empty() && g_worker->stop)
                    return;
                batch.swap(g_worker->commands);
            }
            uint64_t start = SDL_GetTicksNS();
            for (std::function<void()>& command : batch)
                command();
            batch.clear();
            std::lock_guard lock(g_worker->mutex);
            g_worker->busyNs += SDL_GetTicksNS() - start;
        }
    }

    // Commands go to the GPU thread in batches (one lock and at most one wake-up per batch). Producers hold
    // g_producerMutex, which guards the batch being filled.
    constexpr size_t BatchSize = 64;
    std::vector<std::function<void()>> g_batch;

    void Send()
    {
        if (g_batch.empty())
            return;
        bool wake;
        {
            std::lock_guard lock(g_worker->mutex);
            if (g_worker->commands.empty())
                g_worker->commands.swap(g_batch);
            else
                for (std::function<void()>& command : g_batch)
                    g_worker->commands.push_back(std::move(command));
            wake = g_worker->sleeping;
        }
        g_batch.clear();
        if (wake)
            g_worker->wake.notify_one();
    }

    void Push(std::function<void()> command, bool send)
    {
        g_batch.push_back(std::move(command));
        if (send || g_batch.size() >= BatchSize)
            Send();
    }
}

bool gpu::detail::WorkerRunning() { return g_worker != nullptr; }

void gpu::detail::StartWorker()
{
    const char* value = skyenv::Get("SKY_GPU_THREAD");
    if (value != nullptr && atoi(value) == 0)
    {
        fprintf(stderr, "[gpu] no GPU thread (SKY_GPU_THREAD=0)\n");
        return;
    }
    if (!gpu::g_settings.gpuThread)
    {
        fprintf(stderr, "[gpu] no GPU thread ([gpu] thread=off)\n");
        return;
    }
    g_worker = new Worker;
    g_worker->thread = std::thread(Run);
    // Texture workers: the cores the game's busy threads (render, main) and the GPU thread leave, up to 4.
    g_textureWorkers = new TextureWorkers;
    uint32_t count = std::clamp(std::thread::hardware_concurrency(), 4u, 7u) - 3;
    for (uint32_t i = 0; i < count; i++)
        g_textureWorkers->threads.emplace_back(RunTextureWorker);
}

void gpu::detail::RunTextureJob(std::function<void()> job, std::shared_ptr<std::atomic<bool>> done)
{
    if (g_textureWorkers == nullptr)
    {
        job();
        done->store(true);
        return;
    }
    {
        std::lock_guard lock(g_textureWorkers->mutex);
        g_textureWorkers->jobs.emplace_back(std::move(job), std::move(done));
    }
    g_textureWorkers->wake.notify_one();
}

void gpu::detail::WaitTextureJob(const std::atomic<bool>& done)
{
    if (done.load() || g_textureWorkers == nullptr)
        return;
    std::unique_lock lock(g_textureWorkers->mutex);
    WaitServicingPresents(lock, g_textureWorkers->done, [&] { return done.load(); });
}

void gpu::detail::StopWorker()
{
    if (g_worker == nullptr)
        return;
    Send();
    {
        std::lock_guard lock(g_worker->mutex);
        g_worker->stop = true;
    }
    if (g_textureWorkers != nullptr)
    {
        {
            std::lock_guard lock(g_textureWorkers->mutex);
            g_textureWorkers->stop = true;
        }
        g_textureWorkers->wake.notify_all();
    }
    g_worker->wake.notify_one();
    g_worker->thread.join();
    delete g_worker;
    g_worker = nullptr;
    if (g_textureWorkers != nullptr)
    {
        for (std::thread& thread : g_textureWorkers->threads)
            thread.join();
        delete g_textureWorkers;
        g_textureWorkers = nullptr;
    }
    for (uint8_t* chunk : g_arena.chunks)
        delete[] chunk;
    g_arena = {};
    for (uint8_t* chunk : g_freeChunks)
        delete[] chunk;
    g_freeChunks.clear();
}

void gpu::detail::Enqueue(std::function<void()> command)
{
    if (g_worker == nullptr)
        command();
    else
        Push(std::move(command), false);
}

void gpu::detail::RunAndWait(const std::function<void()>& command)
{
    if (g_worker == nullptr)
    {
        command();
        return;
    }
    bool done = false;
    Push(
        [&] {
            command();
            std::lock_guard lock(g_worker->mutex);
            done = true;
            g_worker->progress.notify_all();
        },
        true);
    std::unique_lock lock(g_worker->mutex);
    WaitServicingPresents(lock, g_worker->progress, [&] { return done; });
}

void* gpu::detail::FrameAllocate(size_t bytes, size_t alignment)
{
    size_t offset = (g_arena.used + alignment - 1) / alignment * alignment;
    if (g_arena.chunks.empty() || offset + bytes > ArenaChunkSize)
    {
        uint8_t* chunk = nullptr;
        if (bytes <= ArenaChunkSize)
        {
            std::lock_guard lock(g_freeChunksMutex);
            if (!g_freeChunks.empty())
            {
                chunk = g_freeChunks.back();
                g_freeChunks.pop_back();
            }
        }
        if (chunk == nullptr)
            chunk = new uint8_t[std::max(bytes, ArenaChunkSize)];
        g_arena.chunks.push_back(chunk);
        offset = 0;
    }
    g_arena.used = offset + bytes;
    return g_arena.chunks.back() + offset;
}

const void* gpu::detail::FrameCopy(const void* data, size_t bytes)
{
    if (data == nullptr || bytes == 0)
        return data;
    void* copy = FrameAllocate(bytes, 16);
    memcpy(copy, data, bytes);
    return copy;
}

uint64_t gpu::detail::ProducerFrame() { return g_producerFrame; }

uint64_t gpu::detail::CompletedFrame() { return g_completedFrame.load(); }

void gpu::detail::ConsumerFrameDone(bool finished)
{
    // Called on the GPU thread (or the caller without one) after a wait: the GPU finished the frame before the one it
    // records, and this one too if `finished`.
    uint64_t completed = finished ? g_consumerFrame : g_consumerFrame - 1;
    if (completed > g_completedFrame.load())
        g_completedFrame = completed;
}

uint64_t gpu::detail::AllocateUpload(uint64_t size, RenderBuffer*& buffer, uint64_t alignment)
{
    uint64_t frame = g_producerFrame;
    if (g_uploadCurrent < g_uploadBlocks.size())
    {
        UploadBlock& block = g_uploadBlocks[g_uploadCurrent];
        uint64_t offset = AlignUp(block.used, alignment);
        if (block.frame == frame && offset + size <= block.capacity)
        {
            block.used = offset + size;
            buffer = block.buffer.get();
            return offset;
        }
    }
    uint64_t completed = g_completedFrame.load();
    g_uploadCurrent = SIZE_MAX;
    for (size_t i = 0; i < g_uploadBlocks.size(); i++)
    {
        UploadBlock& block = g_uploadBlocks[i];
        if (block.frame <= completed && block.capacity >= size)
        {
            g_uploadCurrent = i;
            break;
        }
    }
    if (g_uploadCurrent == SIZE_MAX)
    {
        UploadBlock block;
        block.capacity = std::max(size, MinUploadBlockSize);
        block.buffer = g_context->device->createBuffer(RenderBufferDesc::UploadBuffer(
            block.capacity, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX | RenderBufferFlag::CONSTANT |
                                RenderBufferFlag::DEVICE_ADDRESSABLE));
        g_uploadBlocks.push_back(std::move(block));
        g_uploadCurrent = g_uploadBlocks.size() - 1;
    }
    UploadBlock& block = g_uploadBlocks[g_uploadCurrent];
    block.frame = frame;
    block.used = size;
    buffer = block.buffer.get();
    return 0;
}

void gpu::detail::EndProducerFrame(std::function<void()> present)
{
    // The frame's arena goes with its Present; the next frame starts a new one.
    std::vector<uint8_t*> chunks = std::move(g_arena.chunks);
    g_arena = {};
    uint64_t frame = g_producerFrame++;
    if (g_worker == nullptr)
    {
        present();
        g_consumerFrame = frame + 1;
        for (uint8_t* chunk : chunks)
            delete[] chunk;
        return;
    }
    Push(
        [present = std::move(present), chunks = std::move(chunks), frame] {
            present();
            g_consumerFrame = frame + 1;
            {
                std::lock_guard lock(g_freeChunksMutex);
                for (uint8_t* chunk : chunks)
                    g_freeChunks.push_back(chunk);
            }
            std::lock_guard lock(g_worker->mutex);
            g_worker->presentsDone = frame;
            g_worker->progress.notify_all();
        },
        true);
    // At most one frame ahead: the next frame may be recorded while the GPU thread records this one.
    uint64_t start = SDL_GetTicksNS();
    std::unique_lock lock(g_worker->mutex);
    WaitServicingPresents(lock, g_worker->progress, [&] { return g_worker->presentsDone + 1 >= frame; });
    g_worker->producerWaitNs += SDL_GetTicksNS() - start;
}

void gpu::detail::TakeWorkerTimes(WorkerTimes& times)
{
    times = {};
    if (g_worker == nullptr)
        return;
    {
        std::lock_guard lock(g_worker->mutex);
        times.busyNs = std::exchange(g_worker->busyNs, 0);
        times.producerWaitNs = std::exchange(g_worker->producerWaitNs, 0);
    }
    if (g_textureWorkers != nullptr)
    {
        std::lock_guard lock(g_textureWorkers->mutex);
        times.textureThreads = uint32_t(g_textureWorkers->threads.size());
        times.textureBusyNs = std::exchange(g_textureWorkers->busyNs, 0);
        times.textureJobs = std::exchange(g_textureWorkers->jobsDone, 0);
    }
}

void gpu::detail::ShutdownUploads()
{
    g_uploadBlocks.clear();
    g_uploadCurrent = SIZE_MAX;
}
