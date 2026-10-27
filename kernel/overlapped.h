#pragma once

#include <cstdint>

constexpr uint32_t ErrorIoPending = 997;

// Signals the event behind an Nt* event handle, as a completed I/O or XAM call does. Ignores 0 and other handles.
void SignalEventHandle(uint32_t handle);

// Completes a XAM asynchronous call on its XOVERLAPPED (InternalLow = Win32 result, InternalHigh = length,
// dwExtendedError = HRESULT), then signals hEvent and queues pCompletionRoutine(result, length, overlapped) as an APC
// to the calling thread, whose PCR is `pcr`.
void CompleteXOverlapped(uint32_t overlapped, uint32_t result, uint32_t length, uint32_t pcr);

// What a XAM call that accepts an optional XOVERLAPPED returns once its work is done: with one, it completes it and
// returns ERROR_IO_PENDING, as the console does; without, the result itself.
uint32_t FinishXamCall(uint32_t overlapped, uint32_t result, uint32_t length, uint32_t pcr);
