#include "host/audio.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace
{
    std::atomic<SDL_AudioStream*> g_stream;
}

void HostAudioInit()
{
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, 48000 };
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr)
        return;

    SDL_ResumeAudioStreamDevice(stream);
    g_stream = stream;
}

void HostAudioSubmit(const float* planarFrame)
{
    // SSA_AUDIO_DUMP=<path> also writes the stereo output there as raw float32, 48 kHz, to check it without speakers.
    static FILE* dump = getenv("SSA_AUDIO_DUMP") ? fopen(getenv("SSA_AUDIO_DUMP"), "wb") : nullptr;
    SDL_AudioStream* stream = g_stream;
    if (stream == nullptr && dump == nullptr)
        return;

    // Downmix 5.1 to stereo.
    constexpr float Side = 0.7071f;
    float stereo[AudioFrameSamples * 2];
    for (uint32_t i = 0; i < AudioFrameSamples; i++)
    {
        auto channel = [&](uint32_t c) { return planarFrame[c * AudioFrameSamples + i]; };
        float center = channel(2) * Side;
        stereo[i * 2 + 0] = std::clamp(channel(0) + center + channel(4) * Side, -1.0f, 1.0f);
        stereo[i * 2 + 1] = std::clamp(channel(1) + center + channel(5) * Side, -1.0f, 1.0f);
    }
    if (stream != nullptr)
        SDL_PutAudioStreamData(stream, stereo, sizeof(stereo));
    if (dump != nullptr)
        fwrite(stereo, sizeof(stereo), 1, dump);
}

bool HostAudioActive()
{
    return g_stream != nullptr;
}

uint32_t HostAudioQueuedFrames()
{
    SDL_AudioStream* stream = g_stream;
    if (stream == nullptr)
        return 0;
    return uint32_t(SDL_GetAudioStreamQueued(stream)) / (AudioFrameSamples * 2 * sizeof(float));
}
