// Audio output through XAudio2 2.9, for the UWP build (Xbox), which has no SDL. Same contract as host/audio.cpp: stereo
// or 5.1 (host/speakers.h), auto from the mastering voice's channels, and SKY_AUDIO_DUMP works the same.
#include "host/audio.h"
#include "host/portal_speaker.h"
#include "host/speakers.h"
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
    std::atomic<uint32_t> g_volume{ 100 };
    speakers::Mode g_layout = speakers::Stereo;
    speakers::LfeLowPass g_lfe;
    float g_buffers[BufferCount][AudioFrameSamples * AudioFrameChannels];
    uint32_t g_nextBuffer;

    uint32_t QueuedBuffers(IXAudio2SourceVoice* voice)
    {
        XAUDIO2_VOICE_STATE state;
        voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return state.BuffersQueued;
    }
}

void HostAudioInit(speakers::Mode mode)
{
    IXAudio2MasteringVoice* master = nullptr;
    if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(g_xaudio->CreateMasteringVoice(&master)))
    {
        fprintf(stderr, "[audio] XAudio2 unavailable: no sound\n");
        return;
    }
    // The mastering voice takes the device's channel count: auto is 5.1 from 6.
    XAUDIO2_VOICE_DETAILS details{};
    master->GetVoiceDetails(&details);
    g_layout = speakers::Resolve(mode, int(details.InputChannels));
    fprintf(stderr, "[audio] speakers %s: %s (device: %u channels)\n", speakers::Name(mode), speakers::Name(g_layout),
        details.InputChannels);

    // Stereo as before (a plain float format: front left, front right); 5.1 with the 5.1 mask, the guest's own order.
    const bool surround = g_layout == speakers::Surround;
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = surround ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_IEEE_FLOAT;
    format.Format.nChannels = WORD(speakers::Channels(g_layout));
    format.Format.nSamplesPerSec = 48000;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = format.Format.nChannels * format.Format.wBitsPerSample / 8;
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    if (surround)
    {
        format.Format.cbSize = sizeof(format) - sizeof(WAVEFORMATEX);
        format.Samples.wValidBitsPerSample = 32;
        format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, without ksmedia.h's GUID definitions.
        format.SubFormat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
    }
    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(g_xaudio->CreateSourceVoice(&voice, &format.Format)) || FAILED(voice->Start()))
    {
        fprintf(stderr, "[audio] XAudio2 source voice failed: no sound\n");
        return;
    }
    g_voice = voice;
}

void HostAudioSubmit(const float* planarFrame)
{
    // SKY_AUDIO_DUMP=<path> also writes the output there as raw float32, 48 kHz, interleaved (2 channels, or 6 in 5.1),
    // to check it without speakers.
    static FILE* dump = skyenv::Get("SKY_AUDIO_DUMP") ? fopen(skyenv::Get("SKY_AUDIO_DUMP"), "wb") : nullptr;
    IXAudio2SourceVoice* voice = g_voice;
    if (voice == nullptr && dump == nullptr)
        return;

    const float gain = float(g_volume.load()) / 100.0f;
    float* out = g_buffers[g_nextBuffer];
    uint32_t count = AudioFrameSamples * 2;
    if (g_layout == speakers::Surround)
    {
        speakers::Interleave(planarFrame, out);
        g_lfe.Process(out);
        // The portal speaker's voice when the computer plays it (host/portal_speaker.h), on the front pair.
        float voiceFrame[AudioFrameSamples * 2] = {};
        HostSpeakerStream().MixInto(voiceFrame, AudioFrameSamples);
        speakers::AddToFront(voiceFrame, out);
        count = AudioFrameSamples * AudioFrameChannels;
    }
    else
    {
        speakers::FoldToStereo(planarFrame, out);
        // The portal speaker's voice when the computer plays it (host/portal_speaker.h), under the same volume.
        HostSpeakerStream().MixInto(out, AudioFrameSamples);
    }
    speakers::ApplyGain(out, count, gain);
    if (dump != nullptr)
        fwrite(out, count * sizeof(float), 1, dump);

    // A full ring means the voice stalled: drop the frame rather than overwrite one it has not played.
    if (voice != nullptr && QueuedBuffers(voice) < BufferCount)
    {
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = UINT32(count * sizeof(float));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(out);
        if (SUCCEEDED(voice->SubmitSourceBuffer(&buffer)))
            g_nextBuffer = (g_nextBuffer + 1) % BufferCount;
    }
}

bool HostAudioActive() { return g_voice != nullptr; }

speakers::Mode HostAudioLayout() { return g_layout; }

uint32_t HostAudioQueuedFrames()
{
    IXAudio2SourceVoice* voice = g_voice;
    return voice != nullptr ? QueuedBuffers(voice) : 0;
}

void HostAudioSetVolume(uint32_t percent) { g_volume = percent < 100 ? percent : 100; }

uint32_t HostAudioVolume() { return g_volume; }
