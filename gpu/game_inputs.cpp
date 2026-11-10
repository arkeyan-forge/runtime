#include "gpu/game_inputs.h"
#if defined(ARKEYAN_PIPELINE_BASELINE)
#include "shaders/pipeline_baseline.h" // g_pipelineBaseline
#endif

namespace gpu::detail
{
    PipelineBaselineData PipelineBaseline()
    {
#if defined(ARKEYAN_PIPELINE_BASELINE)
        return { g_pipelineBaseline, sizeof(g_pipelineBaseline) };
#else
        return { nullptr, 0 };
#endif
    }

    const char* GameId() { return ARKEYAN_GAME_ID; }
}
