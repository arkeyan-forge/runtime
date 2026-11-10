#pragma once

// What the GPU library takes from the game it is built for: its id and its built-in pipeline list (a zstd frame,
// games/<game>/pipelines/baseline.zst; empty when it has none). They are compiled into the executable's own objects
// (game_inputs.cpp), so the library itself is the same bytes for every game.
#include <cstddef>
#include <cstdint>

namespace gpu::detail
{
    struct PipelineBaselineData
    {
        const uint8_t* data;
        size_t size;
    };

    PipelineBaselineData PipelineBaseline();
    const char* GameId();
}
