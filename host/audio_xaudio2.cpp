// Audio output through XAudio2 2.9, for the UWP build (Xbox), which has no SDL. Same contract as host/audio.cpp: the
// 5.1 mix is downmixed to stereo the same way, and SKY_AUDIO_DUMP works the same.
#include "host/audio.h"
#include <windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "env.h"

namespace
{
    // XAudio2 reads submitted buffers in place: a ring well above the ~6 frames the driver keeps queued.
    constexpr uint32_t BufferCount = 32;

    IXAudio2* g_xaudio;
    std::atomic<IXAudio2SourceVoice*> g_voice;
    float g_buffers[BufferCount][AudioFrameSamples * 2];
    uint32_t g_nextBuffer;

    uint32_t QueuedBuffers(IXAudio2SourceVoice* voice)
    {
        XAUDIO2_VOICE_STATE state;
        voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return state.BuffersQueued;
    }
}

void HostAudioInit()
{
    IXAudio2MasteringVoice* master = nullptr;
    if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(g_xaudio->CreateMasteringVoice(&master)))
    {
        fprintf(stderr, "[audio] XAudio2 unavailable: no sound\n");
        return;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(g_xaudio->CreateSourceVoice(&voice, &format)) || FAILED(voice->Start()))
    {
        fprintf(stderr, "[audio] XAudio2 source voice failed: no sound\n");
        return;
    }
    g_voice = voice;
}

void HostAudioSubmit(const float* planarFrame)
{
    // SKY_AUDIO_DUMP=<path> also writes the stereo output there as raw float32, 48 kHz, to check it without speakers.
    static FILE* dump = skyenv::Get("SKY_AUDIO_DUMP") ? fopen(skyenv::Get("SKY_AUDIO_DUMP"), "wb") : nullptr;
    IXAudio2SourceVoice* voice = g_voice;
    if (voice == nullptr && dump == nullptr)
        return;

    // Downmix 5.1 to stereo.
    constexpr float Side = 0.7071f;
    float* stereo = g_buffers[g_nextBuffer];
    for (uint32_t i = 0; i < AudioFrameSamples; i++)
    {
        auto channel = [&](uint32_t c) { return planarFrame[c * AudioFrameSamples + i]; };
        float center = channel(2) * Side;
        stereo[i * 2 + 0] = std::clamp(channel(0) + center + channel(4) * Side, -1.0f, 1.0f);
        stereo[i * 2 + 1] = std::clamp(channel(1) + center + channel(5) * Side, -1.0f, 1.0f);
    }
    if (dump != nullptr)
        fwrite(stereo, sizeof(g_buffers[0]), 1, dump);

    // A full ring means the voice stalled: drop the frame rather than overwrite one it has not played.
    if (voice != nullptr && QueuedBuffers(voice) < BufferCount)
    {
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = sizeof(g_buffers[0]);
        buffer.pAudioData = reinterpret_cast<const BYTE*>(stereo);
        if (SUCCEEDED(voice->SubmitSourceBuffer(&buffer)))
            g_nextBuffer = (g_nextBuffer + 1) % BufferCount;
    }
}

bool HostAudioActive()
{
    return g_voice != nullptr;
}

uint32_t HostAudioQueuedFrames()
{
    IXAudio2SourceVoice* voice = g_voice;
    return voice != nullptr ? QueuedBuffers(voice) : 0;
}
