#pragma once

#include <cstdint>
#include <functional>

// Raises a graphics interrupt on the GPU interrupt thread: `prepare` runs there first (to write what the GPU
// would have written), then the callback registered with VdSetGraphicsInterruptCallback receives `source`.
void QueueGraphicsInterrupt(uint32_t source, std::function<void()> prepare);

// Blocks until the frame being swapped is shown: the console's GPU flips at a vertical blank, `presentInterval`
// (D3DRS_PRESENTINTERVAL) blanks after the previous flip at the earliest. This is what paces the title.
void WaitForFlip(uint32_t presentInterval);

// 30 (the game's own pacing) or 60: the frames the title asks to show every 2 blanks are shown at every blank.
void SetFrameRateLimit(uint32_t fps);

// The limit set by SetFrameRateLimit.
uint32_t FrameRateLimit();
