#pragma once

#include <cstdint>

constexpr uint32_t AudioFrameSamples = 256; // per channel, at 48 kHz
constexpr uint32_t AudioFrameChannels = 6;  // 5.1: FL, FR, C, LFE, BL, BR

namespace speakers
{
    enum Mode : uint32_t; // host/speakers.h
}

// Opens the default output device, or another when the default is a portal's (on the host's main thread), in stereo or 5.1 as `mode` resolves against it
// (host/speakers.h; the default, {}, is auto). Without it, frames are dropped and the layout stays stereo.
void HostAudioInit(speakers::Mode mode = {});

// Keeps the output off a portal's speaker as devices come and go (host/audio_device.h): the main loop calls it with each
// SDL event, and with null once a frame (it looks at the default once a second).
union SDL_Event;
void HostAudioCheckOutput(const SDL_Event* event);

// The layout of the run, stereo or 5.1 (never auto): what XAudioGetSpeakerConfig reports to the title.
speakers::Mode HostAudioLayout();

// Queues one mixed audio frame: 6 planar channels of 256 floats.
void HostAudioSubmit(const float* planarFrame);

// Whether an output device is open; without one the driver keeps its own 48 kHz clock.
bool HostAudioActive();

// Frames waiting in the output queue; the audio driver paces itself on this so the device is the clock.
uint32_t HostAudioQueuedFrames();

// The debug screen (kernel/audio.cpp): the times the device's queue was found empty since the start, and the game's
// audio clients (XAudio's render driver clients) playing now.
uint32_t AudioUnderruns();
uint32_t AudioVoicesActive();

// The master volume, 0 to 100 (the F3 menu, [<game>.audio] volume): applied to every frame HostAudioSubmit queues.
void HostAudioSetVolume(uint32_t percent);
uint32_t HostAudioVolume();
