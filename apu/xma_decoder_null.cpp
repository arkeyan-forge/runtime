// Built without FFmpeg (SSA_FFMPEG=OFF): XMA contexts produce no audio.
#include "apu/xma_decoder.h"

std::unique_ptr<XmaFrameDecoder> CreateXmaFrameDecoder() { return nullptr; }
