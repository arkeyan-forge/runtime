#pragma once

#include <cstdint>

constexpr uint32_t AudioFrameSamples = 256; // per channel, at 48 kHz
constexpr uint32_t AudioFrameChannels = 6;  // 5.1: FL, FR, C, LFE, BL, BR

// Opens the default output device (on the host's main thread). Without it, frames are dropped.
void HostAudioInit();

// Queues one mixed audio frame: 6 planar channels of 256 floats.
void HostAudioSubmit(const float* planarFrame);

// Whether an output device is open; without one the driver keeps its own 48 kHz clock.
bool HostAudioActive();

// Frames waiting in the output queue; the audio driver paces itself on this so the device is the clock.
uint32_t HostAudioQueuedFrames();
