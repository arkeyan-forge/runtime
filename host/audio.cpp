#include "host/audio.h"
#include "host/audio_device.h"
#include "host/portal_speaker.h"
#include "host/speakers.h"
#include "host/windows_devices.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "env.h"

namespace
{
    std::atomic<SDL_AudioStream*> g_stream;
    std::atomic<bool> g_bound; // the stream feeds an output (else HostAudioSubmit drops the frames)
    std::atomic<uint32_t> g_volume{ 100 };
    speakers::Mode g_layout = speakers::Stereo; // set once, before the title's first frame
    speakers::LfeLowPass g_lfe;

    // The main thread's (HostAudioInit, HostAudioCheckOutput). The stream is bound to a device opened by its physical
    // id, never to SDL's default: SDL moves a default-opened stream to the new default by itself, and Windows makes a
    // portal with a speaker the default when it is plugged in (host/audio_device.h).
    SDL_AudioDeviceID g_device = 0;   // the logical device the stream is bound to
    SDL_AudioDeviceID g_physical = 0; // its physical device
    std::string g_defaultName;        // the system's default when the output was chosen
    uint64_t g_nextCheck = 0;

    std::string DefaultName()
    {
        const char* name = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        return name != nullptr ? name : "";
    }

    audio_device::Choice ChooseOutput()
    {
        std::vector<audio_device::Output> outputs;
        int count = 0;
        if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count))
        {
            for (int i = 0; i < count; i++)
            {
                const char* name = SDL_GetAudioDeviceName(ids[i]);
                outputs.push_back({ ids[i], name != nullptr ? name : "" });
            }
            SDL_free(ids);
        }
        g_defaultName = DefaultName();
        if (outputs.empty()) // a driver that lists none: its default, unless that is a portal's
            outputs.push_back({ SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, g_defaultName });
        return audio_device::Choose(g_defaultName, outputs, windows_devices::PortalAudioOutputNames());
    }

    // Binds the stream to `choice` (unbound when it is none), and says so.
    void Bind(const audio_device::Choice& choice, const SDL_AudioSpec& spec, const char* when)
    {
        SDL_AudioStream* stream = g_stream;
        for (const std::string& name : choice.skipped)
            fprintf(stderr, "[audio] skipped \"%s\": a portal's speaker, the portal's business\n", name.c_str());
        if (choice.id == 0)
            fprintf(stderr, "[audio] %s: no output but a portal's: no sound\n", when);
        else
            fprintf(stderr, "[audio] %s: \"%s\"%s\n", when, choice.name.c_str(),
                choice.isDefault ? " (the default)" : (" (the default is \"" + g_defaultName + "\")").c_str());

        g_bound = false;
        SDL_UnbindAudioStream(stream);
        if (g_device != 0)
            SDL_CloseAudioDevice(g_device);
        g_device = 0;
        g_physical = 0;
        if (choice.id == 0)
            return;
        g_device = SDL_OpenAudioDevice(choice.id, &spec);
        if (g_device == 0 || !SDL_BindAudioStream(g_device, stream))
        {
            fprintf(stderr, "[audio] cannot open \"%s\": %s\n", choice.name.c_str(), SDL_GetError());
            return;
        }
        g_physical = choice.id;
        SDL_ClearAudioStream(stream);
        g_bound = true;
    }

    SDL_AudioSpec StreamSpec() { return { SDL_AUDIO_F32, int(speakers::Channels(g_layout)), 48000 }; }
}

void HostAudioInit(speakers::Mode mode)
{
    // The chosen device's own channel count decides auto (SDL's disk driver reports 2: tests force 5.1).
    const audio_device::Choice choice = ChooseOutput();
    SDL_AudioSpec device{};
    int deviceChannels = choice.id != 0 && SDL_GetAudioDeviceFormat(choice.id, &device, nullptr) ? device.channels : 0;
    g_layout = speakers::Resolve(mode, deviceChannels);
    fprintf(stderr, "[audio] speakers %s: %s (device: %d channels)\n", speakers::Name(mode), speakers::Name(g_layout),
        deviceChannels);

    const SDL_AudioSpec spec = StreamSpec();
    SDL_AudioStream* stream = SDL_CreateAudioStream(&spec, nullptr);
    if (stream == nullptr)
        return;
    g_stream = stream;
    Bind(choice, spec, "output");
}

void HostAudioCheckOutput(const SDL_Event* event)
{
    if (g_stream == nullptr)
        return;
    bool check = false;
    if (event != nullptr)
    {
        // A device came or went (our own one gone: its logical id), or a format changed.
        check = event->type == SDL_EVENT_AUDIO_DEVICE_ADDED || event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED ||
                event->type == SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED;
        if (check && event->adevice.recording)
            check = false;
    }
    else
    {
        // The default can change with no event (the system's settings): its name, once a second (no allocation).
        const uint64_t now = SDL_GetTicks();
        if (now < g_nextCheck)
            return;
        g_nextCheck = now + 1000;
        const char* name = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        check = name != nullptr && g_defaultName != name;
    }
    if (!check)
        return;
    const std::string before = g_defaultName;
    const audio_device::Choice choice = ChooseOutput();
    if (choice.id != 0 && choice.id == g_physical && g_bound)
    {
        if (g_defaultName != before && !choice.isDefault)
            fprintf(stderr, "[audio] skipped \"%s\", the new default: a portal's speaker; still \"%s\"\n",
                g_defaultName.c_str(), choice.name.c_str());
        return; // still the right one
    }
    if (choice.id == 0 && g_device == 0)
        return; // still none
    Bind(choice, StreamSpec(), "output now");
}

void HostAudioSubmit(const float* planarFrame)
{
    // SKY_AUDIO_DUMP=<path> also writes the output there as raw float32, 48 kHz, interleaved (2 channels, or 6 in 5.1),
    // to check it without speakers.
    static FILE* dump = skyenv::Get("SKY_AUDIO_DUMP") ? fopen(skyenv::Get("SKY_AUDIO_DUMP"), "wb") : nullptr;
    SDL_AudioStream* stream = g_stream;
    if (stream == nullptr && dump == nullptr)
        return;

    const float gain = float(g_volume.load()) / 100.0f; // linear: 100 leaves the samples as the game mixed them
    float out[AudioFrameSamples * AudioFrameChannels];
    uint32_t count = AudioFrameSamples * 2;
    if (g_layout == speakers::Surround)
    {
        speakers::Interleave(planarFrame, out);
        g_lfe.Process(out);
        // The portal speaker's voice when the computer plays it (host/portal_speaker.h), on the front pair.
        float voice[AudioFrameSamples * 2] = {};
        HostSpeakerStream().MixInto(voice, AudioFrameSamples);
        speakers::AddToFront(voice, out);
        count = AudioFrameSamples * AudioFrameChannels;
    }
    else
    {
        speakers::FoldToStereo(planarFrame, out);
        // The portal speaker's voice when the computer plays it (host/portal_speaker.h), under the same volume.
        HostSpeakerStream().MixInto(out, AudioFrameSamples);
    }
    speakers::ApplyGain(out, count, gain);
    if (stream != nullptr && g_bound)
        SDL_PutAudioStreamData(stream, out, int(count * sizeof(float)));
    if (dump != nullptr)
        fwrite(out, count * sizeof(float), 1, dump);
}

bool HostAudioActive() { return g_bound; }

speakers::Mode HostAudioLayout() { return g_layout; }

uint32_t HostAudioQueuedFrames()
{
    SDL_AudioStream* stream = g_stream;
    if (stream == nullptr || !g_bound)
        return 0;
    return uint32_t(SDL_GetAudioStreamQueued(stream)) /
           (AudioFrameSamples * speakers::Channels(g_layout) * sizeof(float));
}

void HostAudioSetVolume(uint32_t percent) { g_volume = std::min<uint32_t>(percent, 100); }

uint32_t HostAudioVolume() { return g_volume; }
