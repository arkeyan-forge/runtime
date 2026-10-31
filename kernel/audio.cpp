// The kernel's audio render driver. The title's XAudio2 is driven by it: the driver calls the registered client
// once per audio frame (256 samples at 48 kHz) and the client answers with XAudioSubmitRenderDriverFrame.
// Submitted frames go to the host output device (host/audio), which also paces the driver.
#include "host/audio.h"
#include "memory.h"
#include "thread.h"
#include <algorithm>
#include <bit>
#include "byteswap.h"
#include <chrono>
#include <mutex>
#include <thread>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t DriverHandleTag = 0x41550000;
    constexpr uint32_t MaxClients = 8;
    constexpr auto FramePeriod = std::chrono::nanoseconds(1'000'000'000ull * AudioFrameSamples / 48000);
    constexpr uint32_t TargetQueuedFrames = 6; // ~32 ms of latency

    struct Client
    {
        // Held while the callback runs: unregistering takes it, so no callback outlives XAudioUnregisterRenderDriverClient
        // (the console guarantees that). Recursive in case the client unregisters from its own callback.
        std::recursive_mutex mutex;
        bool active = false;
        std::chrono::steady_clock::time_point firstFrame;
        uint32_t callback = 0;
        uint32_t contextCell = 0; // the callback receives a pointer to a cell holding the client's context
    };

    Client g_clients[MaxClients];

    // Each client submits its frame from inside its callback, so the frames of one period are summed here, on the
    // driver thread, before the next client's callback runs.
    float g_mix[AudioFrameChannels * AudioFrameSamples];

    // One thread plays the hardware: every frame period it asks each client for a frame, in turn, then outputs the mix.
    void DriverThread(PPCContext& ctx)
    {
        auto next = std::chrono::steady_clock::now();
        while (true)
        {
            std::fill(std::begin(g_mix), std::end(g_mix), 0.0f);
            for (uint32_t index = 0; index < MaxClients; index++)
            {
                Client& client = g_clients[index];
                std::lock_guard lock(client.mutex);
                if (!client.active || std::chrono::steady_clock::now() < client.firstFrame)
                    continue;

                ctx.r3.u64 = client.contextCell;
                (PPC_LOOKUP_FUNC(g_memory.base, client.callback))(ctx, g_memory.base);
            }
            HostAudioSubmit(g_mix);

            // One frame per period, never a burst, like the hardware: a title may read XAudio2's SamplesPlayed as the
            // play cursor of a looping buffer (Giants mixes into a 1024-sample loop and reads SamplesPlayed mod 1024),
            // so the cursor must move one frame at a time. Devices take their data in larger chunks; refilling the
            // queue as they do advanced it 4 frames at once, the title saw no progress and the loop replayed.
            // The device still sets the pace: its queue level trims the period, 1/64 per frame off the target.
            auto period = FramePeriod;
            if (HostAudioActive())
                period += FramePeriod * (int32_t(HostAudioQueuedFrames()) - int32_t(TargetQueuedFrames)) / 64;
            next += period;
            auto now = std::chrono::steady_clock::now();
            if (now - next > FramePeriod * TargetQueuedFrames)
                next = now; // after a stall, resume at once instead of catching up in a burst
            std::this_thread::sleep_until(next);
        }
    }
}

PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    uint32_t callbackPointer = ctx.r3.u32; // { callback, context }
    uint32_t driverPointer = ctx.r4.u32;

    for (uint32_t index = 0; index < MaxClients; index++)
    {
        Client& client = g_clients[index];
        {
            std::lock_guard lock(client.mutex);
            if (client.active)
                continue;

            // Like the hardware, the first frame is requested one frame period after registration, which leaves
            // the client time to finish initialising.
            client.active = true;
            client.firstFrame = std::chrono::steady_clock::now() + FramePeriod;
            client.callback = g_memory.Read<uint32_t>(callbackPointer);
            if (client.contextCell == 0)
                client.contextCell = g_memory.SystemAlloc(4);
            g_memory.Write<uint32_t>(client.contextCell, g_memory.Read<uint32_t>(callbackPointer + 4));
        }

        g_memory.Write<uint32_t>(driverPointer, DriverHandleTag | index);
        static std::once_flag started;
        std::call_once(started, [] { StartHostDrivenThread(DriverThread, 0); });
        ctx.r3.u64 = StatusSuccess;
        return;
    }

    ctx.r3.u64 = 0x8007000E; // E_OUTOFMEMORY
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    Client& client = g_clients[(ctx.r3.u32 & 0xFFFF) % MaxClients];
    std::lock_guard lock(client.mutex);
    client.active = false;
    ctx.r3.u64 = StatusSuccess;
}

// (driver, samples): 6 planar channels of 256 big-endian floats.
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    const uint32_t* samples = static_cast<const uint32_t*>(g_memory.Translate(ctx.r4.u32));
    for (uint32_t i = 0; i < AudioFrameChannels * AudioFrameSamples; i++)
        g_mix[i] += std::bit_cast<float>(ByteSwap(samples[i]));
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__XAudioGetSpeakerConfig)
{
    g_memory.Write<uint32_t>(ctx.r3.u32, 0x00010001);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    g_memory.Write<uint32_t>(ctx.r4.u32, std::bit_cast<uint32_t>(1.0f));
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__XAudioGetDuckerLevel)
{
    g_memory.Write<uint32_t>(ctx.r3.u32, 0);
    ctx.r3.u64 = StatusSuccess;
}
