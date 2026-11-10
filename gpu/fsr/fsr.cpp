#include "gpu/fsr/fsr.h"
#include "gpu/context.h"
#include "gpu/fsr/backend.h"
#include "gpu/fsr/framegen.h"
#include "gpu/fsr/framegen_rules.h"
#include "host/env.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include "shaders/fsr_shaders.h"

using namespace gpu::detail;
using namespace plume;

namespace
{
    // The inputs pass's push constants (fsr_inputs.hlsl / .metal: Constants).
    struct InputsConstants
    {
        float reprojection[16]; // rows
        float size[2];
        float jitter[2]; // in clip space
        float depthMin, depthScale;
        float invertDepth; // 1: FSR's depth is 1 - ndc z
        float padding;
    };
    static_assert(sizeof(InputsConstants) == 96);

    // InputsLayersCS's (UpscaleDesc::motionLayers): the same, then the scene's other motions. 224 bytes: within D3D12's
    // root constants (256), above Vulkan's guaranteed 128: a device that cannot make the pipeline keeps InputsCS.
    struct LayersConstants
    {
        InputsConstants inputs; // inputs.padding: how many of `others` are set
        float others[2][16];    // rows
    };
    static_assert(sizeof(LayersConstants) == 224);

    struct State
    {
        FfxInterface backend{};
        FfxFsr3UpscalerContext context{};
        bool created = false, failed = false;
        bool hdr = false; // the colour is a float format: FSR's HDR mode
        uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
        gpu::Texture* motion = nullptr; // RG16F, render size: the camera's motion in texture coordinates
        gpu::Texture* depth = nullptr;  // R32F, render size: normalised device depth (0 near, 1 far)
        // FSR 3.1's textures that the application keeps (gpu::fsr::CreateShared; freed by ReleaseAll).
        gpu::Texture* dilatedDepth = nullptr;
        gpu::Texture* dilatedMotion = nullptr;
        gpu::Texture* previousDepth = nullptr;
        uint64_t lastDispatchNs = 0;

        std::unique_ptr<RenderShader> inputsShader;
        RenderDescriptorSetBuilder inputsSetBuilder;
        std::unique_ptr<RenderPipelineLayout> inputsLayout;
        std::unique_ptr<RenderPipeline> inputsPipeline;
        std::unique_ptr<RenderDescriptorSet> inputsSets[3];
        // The float colour's copy without NaN (SanitizeCS): set 0 = { colour, copy }, the same push constants.
        std::unique_ptr<RenderShader> sanitizeShader;
        RenderDescriptorSetBuilder sanitizeSetBuilder;
        std::unique_ptr<RenderPipelineLayout> sanitizeLayout;
        std::unique_ptr<RenderPipeline> sanitizePipeline;
        std::unique_ptr<RenderDescriptorSet> sanitizeSets[3];
        gpu::Texture* cleanColor = nullptr; // RGBA16F, render size (HDR only)
        // InputsLayersCS: set 0 = { depth, motion, depth out, colour, previous colour }; the previous frame's colour.
        std::unique_ptr<RenderShader> layersShader;
        RenderDescriptorSetBuilder layersSetBuilder;
        std::unique_ptr<RenderPipelineLayout> layersLayout;
        std::unique_ptr<RenderPipeline> layersPipeline;
        std::unique_ptr<RenderDescriptorSet> layersSets[3];
        bool layersFailed = false;
        gpu::Texture* previousColor = nullptr; // the colour's size and format
        bool previousValid = false;
        uint64_t logged = 0;
    } g_state;

    void Message(FfxMsgType type, const wchar_t* message)
    {
        char text[256];
        size_t i = 0;
        for (; i + 1 < sizeof(text) && message[i] != 0; i++)
            text[i] = message[i] < 128 ? char(message[i]) : '?';
        text[i] = '\0';
        fprintf(stderr, "[fsr] %s: %s\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
    }

    bool Fail(const char* what, FfxErrorCode code = FFX_OK)
    {
        fprintf(stderr, "[fsr] %s (%d); the scene is no longer upscaled\n", what, int(code));
        g_state.failed = true;
        return false;
    }

    std::unique_ptr<RenderShader> CreateShader(const char* entry)
    {
        const RenderShaderFormat format = g_context->renderInterface->getCapabilities().shaderFormat;
#if defined(ARKEYAN_METAL)
        return g_context->device->createShader(g_fsr_inputs_metallib, sizeof(g_fsr_inputs_metallib), entry, format);
#else
        std::span<const uint8_t> blob;
#if defined(_WIN32)
        if (format == RenderShaderFormat::DXIL)
            blob = strcmp(entry, "InputsCS") == 0         ? std::span<const uint8_t>(g_fsr_InputsCS_dxil)
                   : strcmp(entry, "InputsLayersCS") == 0 ? std::span<const uint8_t>(g_fsr_InputsLayersCS_dxil)
                                                          : std::span<const uint8_t>(g_fsr_SanitizeCS_dxil);
#endif
#if !defined(PLUME_D3D12_COREWINDOW)
        if (format == RenderShaderFormat::SPIRV)
            blob = strcmp(entry, "InputsCS") == 0         ? std::span<const uint8_t>(g_fsr_InputsCS_spirv)
                   : strcmp(entry, "InputsLayersCS") == 0 ? std::span<const uint8_t>(g_fsr_InputsLayersCS_spirv)
                                                          : std::span<const uint8_t>(g_fsr_SanitizeCS_spirv);
#endif
        return blob.empty() ? nullptr : g_context->device->createShader(blob.data(), blob.size(), entry, format);
#endif
    }

    // Made on the first frame that asks for it; a device that cannot keeps the camera's motion alone (logged once).
    bool CreateLayersPipeline()
    {
        State& s = g_state;
        if (s.layersPipeline != nullptr || s.layersFailed)
            return s.layersPipeline != nullptr;
        s.layersFailed = true;
        s.layersShader = CreateShader("InputsLayersCS");
        if (s.layersShader == nullptr)
            return false;
        s.layersSetBuilder.begin();
        s.layersSetBuilder.addTexture(0);
        s.layersSetBuilder.addReadWriteTexture(1);
        s.layersSetBuilder.addReadWriteTexture(2);
        s.layersSetBuilder.addTexture(3);
        s.layersSetBuilder.addTexture(4);
        s.layersSetBuilder.end();
        for (auto& set : s.layersSets)
            set = s.layersSetBuilder.create(g_context->device.get());
        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(s.layersSetBuilder);
        layoutBuilder.addPushConstant(0, 1, sizeof(LayersConstants), RenderShaderStageFlag::COMPUTE);
        layoutBuilder.end();
        s.layersLayout = layoutBuilder.create(g_context->device.get());
        s.layersPipeline = s.layersLayout != nullptr ? g_context->device->createComputePipeline(RenderComputePipelineDesc(
                                                           s.layersLayout.get(), s.layersShader.get(), 8, 8, 1))
                                                     : nullptr;
        s.layersFailed = s.layersPipeline == nullptr;
        if (s.layersFailed)
            fprintf(stderr, "[fsr] the per-pixel motion pass could not be made: the camera's motion alone\n");
        return !s.layersFailed;
    }

    bool CreateSanitizePipeline()
    {
        State& s = g_state;
        if (s.sanitizePipeline != nullptr)
            return true;
        s.sanitizeShader = CreateShader("SanitizeCS");
        if (s.sanitizeShader == nullptr)
            return false;
        s.sanitizeSetBuilder.begin();
        s.sanitizeSetBuilder.addTexture(0);
        s.sanitizeSetBuilder.addReadWriteTexture(1);
        s.sanitizeSetBuilder.end();
        for (auto& set : s.sanitizeSets)
            set = s.sanitizeSetBuilder.create(g_context->device.get());
        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(s.sanitizeSetBuilder);
        layoutBuilder.addPushConstant(0, 1, sizeof(InputsConstants), RenderShaderStageFlag::COMPUTE);
        layoutBuilder.end();
        s.sanitizeLayout = layoutBuilder.create(g_context->device.get());
        s.sanitizePipeline = g_context->device->createComputePipeline(
            RenderComputePipelineDesc(s.sanitizeLayout.get(), s.sanitizeShader.get(), 8, 8, 1));
        return s.sanitizePipeline != nullptr;
    }

    bool CreateInputsPipeline()
    {
        State& s = g_state;
        if (s.inputsPipeline != nullptr)
            return true;
        s.inputsShader = CreateShader("InputsCS");
        if (s.inputsShader == nullptr)
            return false;
        // Set 0 = { scene depth, motion, depth }, push constants (binding 0, set 1).
        s.inputsSetBuilder.begin();
        s.inputsSetBuilder.addTexture(0);
        s.inputsSetBuilder.addReadWriteTexture(1);
        s.inputsSetBuilder.addReadWriteTexture(2);
        s.inputsSetBuilder.end();
        for (auto& set : s.inputsSets)
            set = s.inputsSetBuilder.create(g_context->device.get());
        RenderPipelineLayoutBuilder layoutBuilder;
        layoutBuilder.begin();
        layoutBuilder.addDescriptorSet(s.inputsSetBuilder);
        layoutBuilder.addPushConstant(0, 1, sizeof(InputsConstants), RenderShaderStageFlag::COMPUTE);
        layoutBuilder.end();
        s.inputsLayout = layoutBuilder.create(g_context->device.get());
        s.inputsPipeline = g_context->device->createComputePipeline(
            RenderComputePipelineDesc(s.inputsLayout.get(), s.inputsShader.get(), 8, 8, 1));
        return s.inputsPipeline != nullptr;
    }

    void Destroy()
    {
        State& s = g_state;
        if (s.created)
        {
            Submit(); // waits for the GPU: FFX's resources may be in use
            gpu::framegen::detail::Release(); // it reads the shared textures below
            ffxFsr3UpscalerContextDestroy(&s.context);
            gpu::fsr::ReleaseAll();
            s.created = false;
            s.dilatedDepth = s.dilatedMotion = s.previousDepth = nullptr;
        }
        if (s.motion != nullptr)
            DeferDestroy(s.motion); // the GPU thread: gpu::DestroyTexture is a producer's call
        if (s.depth != nullptr)
            DeferDestroy(s.depth); // the GPU thread: gpu::DestroyTexture is a producer's call
        if (s.cleanColor != nullptr)
            DeferDestroy(s.cleanColor);
        if (s.previousColor != nullptr)
            DeferDestroy(s.previousColor);
        s.motion = s.depth = s.cleanColor = s.previousColor = nullptr;
        s.previousValid = false;
    }

    // The context and our input textures for these sizes.
    bool Prepare(uint32_t renderWidth, uint32_t renderHeight, uint32_t outputWidth, uint32_t outputHeight, bool hdr,
        RenderFormat colourFormat, RenderFormat outputFormat)
    {
        State& s = g_state;
        if (s.created && s.renderWidth == renderWidth && s.renderHeight == renderHeight &&
            s.outputWidth == outputWidth && s.outputHeight == outputHeight && s.hdr == hdr)
            return true;
        Destroy();
        if (!CreateInputsPipeline())
            return Fail("the inputs pass could not be made");
        s.motion = gpu::CreateTexture({ .width = renderWidth,
            .height = renderHeight,
            .format = RenderFormat::R16G16_FLOAT,
            .usage = gpu::TextureUsage::Storage });
        s.depth = gpu::CreateTexture({ .width = renderWidth,
            .height = renderHeight,
            .format = RenderFormat::R32_FLOAT,
            .usage = gpu::TextureUsage::Storage });
        if (s.motion == nullptr || s.depth == nullptr)
            return Fail("no motion or depth texture");
        if (hdr)
        {
            s.cleanColor = gpu::CreateTexture({ .width = renderWidth,
                .height = renderHeight,
                .format = RenderFormat::R16G16B16A16_FLOAT,
                .usage = gpu::TextureUsage::Storage });
            if (s.cleanColor == nullptr || !CreateSanitizePipeline())
                return Fail("no colour copy for the HDR scene");
        }

        gpu::fsr::MakeInterface(s.backend);
        FfxFsr3UpscalerContextDescription description{};
        // Motion at the render size, depth 0 near 1 far (the inputs pass), no auto exposure; HDR for a float colour.
        description.flags = hdr ? FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE : 0;
        description.maxRenderSize = { renderWidth, renderHeight };
        description.maxUpscaleSize = { outputWidth, outputHeight };
        description.fpMessage = Message;
        description.backendInterface = s.backend;
        const FfxErrorCode error = ffxFsr3UpscalerContextCreate(&s.context, &description);
        if (error != FFX_OK)
        {
            gpu::fsr::ReleaseAll();
            return Fail("the FSR 3 context could not be made", error);
        }
        FfxFsr3UpscalerSharedResourceDescriptions shared{};
        ffxFsr3UpscalerGetSharedResourceDescriptions(&s.context, &shared);
        s.dilatedDepth = gpu::fsr::CreateShared(shared.dilatedDepth);
        s.dilatedMotion = gpu::fsr::CreateShared(shared.dilatedMotionVectors);
        s.previousDepth = gpu::fsr::CreateShared(shared.reconstructedPrevNearestDepth);
        if (s.dilatedDepth == nullptr || s.dilatedMotion == nullptr || s.previousDepth == nullptr)
        {
            ffxFsr3UpscalerContextDestroy(&s.context);
            gpu::fsr::ReleaseAll();
            return Fail("FSR 3's shared textures could not be made");
        }
        s.created = true;
        s.logged = 0;
        s.hdr = hdr;
        s.renderWidth = renderWidth, s.renderHeight = renderHeight;
        s.outputWidth = outputWidth, s.outputHeight = outputHeight;
        fprintf(stderr, "[fsr] FSR 3.1 upscaler %ux%u -> %ux%u, %s colour (formats %d -> %d)\n", renderWidth,
            renderHeight, outputWidth, outputHeight, hdr ? "HDR" : "SDR", int(colourFormat), int(outputFormat));
        return true;
    }

    void RecordSanitize(RenderCommandList& list, const gpu::UpscaleDesc& desc, const InputsConstants& constants);

    // InputsLayersCS (UpscaleDesc::motionLayers), then this frame's colour kept for the next. False: InputsCS instead.
    bool RecordLayers(RenderCommandList& list, const gpu::UpscaleDesc& desc, const InputsConstants& inputs)
    {
        State& s = g_state;
        if (!CreateLayersPipeline())
            return false;
        const gpu::TextureDesc& colour = desc.color->desc;
        if (s.previousColor == nullptr || s.previousColor->desc.width != colour.width ||
            s.previousColor->desc.height != colour.height || s.previousColor->desc.format != colour.format)
        {
            if (s.previousColor != nullptr)
                DeferDestroy(s.previousColor);
            s.previousColor = gpu::CreateTexture({ .width = colour.width,
                .height = colour.height,
                .format = colour.format,
                .usage = gpu::TextureUsage::Sampled });
            s.previousValid = false;
            if (s.previousColor == nullptr)
                return false;
        }
        LayersConstants constants{};
        constants.inputs = inputs;
        // Without the previous frame's colour (the first frame, a reset) there is nothing to compare with.
        const uint32_t others = s.previousValid && !desc.reset ? std::min<uint32_t>(desc.otherMotionCount, 2) : 0;
        constants.inputs.padding = float(others);
        for (uint32_t m = 0; m < others; m++)
            for (uint32_t i = 0; i < 16; i++)
                constants.others[m][i] = float(desc.otherMotions[m][i]);

        Transition(list, *desc.color, RenderTextureLayout::SHADER_READ, RenderBarrierStage::COMPUTE);
        Transition(list, *s.previousColor, RenderTextureLayout::SHADER_READ, RenderBarrierStage::COMPUTE);
        RenderDescriptorSet* set = s.layersSets[g_context->frames % 3].get();
        set->setTexture(0, desc.depth->texture.get(), RenderTextureLayout::SHADER_READ);
        set->setTexture(1, s.motion->texture.get(), RenderTextureLayout::GENERAL);
        set->setTexture(2, s.depth->texture.get(), RenderTextureLayout::GENERAL);
        set->setTexture(3, desc.color->texture.get(), RenderTextureLayout::SHADER_READ);
        set->setTexture(4, s.previousColor->texture.get(), RenderTextureLayout::SHADER_READ);
        g_context->bound = {};
        list.setComputePipelineLayout(s.layersLayout.get());
        list.setPipeline(s.layersPipeline.get());
        list.setComputeDescriptorSet(set, 0);
        list.setComputePushConstants(0, &constants);
        list.dispatch((s.renderWidth + 7) / 8, (s.renderHeight + 7) / 8, 1);

        Transition(list, *desc.color, RenderTextureLayout::COPY_SOURCE, RenderBarrierStage::COPY);
        Transition(list, *s.previousColor, RenderTextureLayout::COPY_DEST, RenderBarrierStage::COPY);
        list.copyTexture(s.previousColor->texture.get(), desc.color->texture.get());
        s.previousValid = true;
        return true;
    }

    void RecordInputs(RenderCommandList& list, const gpu::UpscaleDesc& desc)
    {
        State& s = g_state;
        const float width = float(s.renderWidth), height = float(s.renderHeight);
        InputsConstants constants{};
        for (uint32_t i = 0; i < 16; i++)
            constants.reprojection[i] = float(desc.reprojection[i]);
        constants.size[0] = width, constants.size[1] = height;
        constants.jitter[0] = 2.0f * desc.jitter[0] / width;
        constants.jitter[1] = -2.0f * desc.jitter[1] / height;
        constants.depthMin = desc.depthMin;
        constants.depthScale = desc.depthMax != desc.depthMin ? 1.0f / (desc.depthMax - desc.depthMin) : 1.0f;
        constants.invertDepth = desc.depthReversed ? 1.0f : 0.0f;

        Transition(list, *desc.depth, RenderTextureLayout::SHADER_READ, RenderBarrierStage::COMPUTE);
        Transition(list, *s.motion, RenderTextureLayout::GENERAL, RenderBarrierStage::COMPUTE);
        Transition(list, *s.depth, RenderTextureLayout::GENERAL, RenderBarrierStage::COMPUTE);
        if (desc.motionLayers && RecordLayers(list, desc, constants))
            return RecordSanitize(list, desc, constants);
        RenderDescriptorSet* set = s.inputsSets[g_context->frames % 3].get();
        set->setTexture(0, desc.depth->texture.get(), RenderTextureLayout::SHADER_READ);
        set->setTexture(1, s.motion->texture.get(), RenderTextureLayout::GENERAL);
        set->setTexture(2, s.depth->texture.get(), RenderTextureLayout::GENERAL);
        g_context->bound = {};
        list.setComputePipelineLayout(s.inputsLayout.get());
        list.setPipeline(s.inputsPipeline.get());
        list.setComputeDescriptorSet(set, 0);
        list.setComputePushConstants(0, &constants);
        list.dispatch((s.renderWidth + 7) / 8, (s.renderHeight + 7) / 8, 1);
        RecordSanitize(list, desc, constants);
    }

    void RecordSanitize(RenderCommandList& list, const gpu::UpscaleDesc& desc, const InputsConstants& constants)
    {
        State& s = g_state;
        if (s.cleanColor == nullptr)
            return;
        Transition(list, *desc.color, RenderTextureLayout::SHADER_READ, RenderBarrierStage::COMPUTE);
        Transition(list, *s.cleanColor, RenderTextureLayout::GENERAL, RenderBarrierStage::COMPUTE);
        RenderDescriptorSet* clean = s.sanitizeSets[g_context->frames % 3].get();
        clean->setTexture(0, desc.color->texture.get(), RenderTextureLayout::SHADER_READ);
        clean->setTexture(1, s.cleanColor->texture.get(), RenderTextureLayout::GENERAL);
        list.setComputePipelineLayout(s.sanitizeLayout.get());
        list.setPipeline(s.sanitizePipeline.get());
        list.setComputeDescriptorSet(clean, 0);
        list.setComputePushConstants(0, &constants);
        list.dispatch((s.renderWidth + 7) / 8, (s.renderHeight + 7) / 8, 1);
    }
}

bool gpu::fsr::Available() { return g_context != nullptr && g_context->device != nullptr && ShadersAvailable(); }

bool gpu::fsr::Upscale(const UpscaleDesc& desc)
{
    static const bool debugInputs = skyenv::Get("SKY_FSR_DEBUG") != nullptr && atoi(skyenv::Get("SKY_FSR_DEBUG")) == 2;
    State& s = g_state;
    if (s.failed)
        return false;
    const uint32_t renderWidth = desc.color->desc.width, renderHeight = desc.color->desc.height;
    const RenderFormat format = desc.color->desc.format;
    const bool hdr = format == RenderFormat::R16G16B16A16_FLOAT || format == RenderFormat::R32G32B32A32_FLOAT ||
                     format == RenderFormat::R16G16_FLOAT || format == RenderFormat::R32_FLOAT;
    if (!Prepare(renderWidth, renderHeight, desc.output->desc.width, desc.output->desc.height, hdr, format,
            desc.output->desc.format))
        return false;
    // FSR records into a command list of its own, which starts on the GPU after the frame's work so far (and the next
    // one after FSR's: below). After Prepare, which may have waited for the GPU (Submit).
    Flush();
    RenderCommandList& recording = Record();

    RecordInputs(recording, desc);
    if (debugInputs && s.logged >= 2 && s.logged < 4)
    {
        std::vector<uint32_t> raw;
        uint32_t w = 0, h = 0;
        if (ReadbackNow(s.motion, raw, w, h, true) && !raw.empty())
            fprintf(stderr, "[fsr] inputs: motion bits %08X %08X", raw[0], raw[raw.size() / 2]);
        if (ReadbackNow(s.depth, raw, w, h, true) && !raw.empty())
            fprintf(stderr, ", depth bits %08X %08X\n", raw[0], raw[raw.size() / 2]);
    }

    const uint64_t now = SDL_GetTicksNS();
    const float frameMs =
        s.lastDispatchNs != 0 ? std::clamp(float(now - s.lastDispatchNs) / 1e6f, 1.0f, 100.0f) : 16.7f;
    s.lastDispatchNs = now;

    FfxFsr3UpscalerDispatchDescription dispatch{};
    dispatch.commandList = &recording;
    dispatch.color = External(s.cleanColor != nullptr ? s.cleanColor : desc.color, false);
    dispatch.depth = External(s.depth, false);
    dispatch.motionVectors = External(s.motion, false);
    dispatch.output = External(desc.output, true);
    dispatch.dilatedDepth = External(s.dilatedDepth, true);
    dispatch.dilatedMotionVectors = External(s.dilatedMotion, true);
    dispatch.reconstructedPrevNearestDepth = External(s.previousDepth, true);
    dispatch.jitterOffset = { desc.jitter[0], desc.jitter[1] };
    dispatch.motionVectorScale = { float(renderWidth), float(renderHeight) };
    dispatch.renderSize = { renderWidth, renderHeight };
    dispatch.upscaleSize = { desc.output->desc.width, desc.output->desc.height };
    dispatch.enableSharpening = desc.sharpness > 0.0f;
    dispatch.sharpness = std::clamp(desc.sharpness, 0.0f, 1.0f);
    dispatch.frameTimeDelta = frameMs;
    dispatch.preExposure = 1.0f;
    dispatch.reset = desc.reset;
    dispatch.cameraNear = desc.cameraNear;
    dispatch.cameraFar = desc.cameraFar;
    dispatch.cameraFovAngleVertical = desc.fovY;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    // SKY_FSR_DEBUG=1: FSR's debug view in the output (its inputs and internal textures in a grid).
    static const bool debugView = skyenv::Get("SKY_FSR_DEBUG") != nullptr && atoi(skyenv::Get("SKY_FSR_DEBUG")) != 0;
    dispatch.flags = debugView ? FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW : 0;
    if (debugView && s.logged++ < 8)
        fprintf(stderr, "[fsr] dispatch: jitter %.3f %.3f, reset %d, motion row0 %.4f %.4f %.4f %.4f\n", desc.jitter[0],
            desc.jitter[1], int(desc.reset), desc.reprojection[0], desc.reprojection[1], desc.reprojection[2],
            desc.reprojection[3]);
    const FfxErrorCode error = ffxFsr3UpscalerContextDispatch(&s.context, &dispatch);
    if (error != FFX_OK)
        return Fail("an FSR 3 dispatch failed", error);
    // The output is read by the game's composite draw next.
    Transition(recording, *desc.output, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
    // Frame generation interpolates with this frame's dilated depth and motion (gpu/fsr/framegen.h).
    gpu::framegen::detail::Upscaled({ renderWidth, renderHeight, { desc.jitter[0], desc.jitter[1] }, desc.cameraNear,
        desc.cameraFar, desc.fovY, desc.reset,
        desc.cameraFound ? float(std::min(::framegen::CameraShift(desc.reprojection), 1e6)) : 1e6f,
        desc.cutShift > 0.0f ? desc.cutShift : 0.5f,
        s.dilatedDepth, s.dilatedMotion, s.previousDepth });
    // Recorded in the same list as the frame's other work, FSR raced it in Trap Team on Metal although the barriers
    // order it (8x8 tiles of garbage kept by the history, whole frames in false colours; docs/runtime/gpu/upscaling.md).
    Flush();
    return true;
}

void gpu::fsr::Shutdown()
{
    State& s = g_state;
    if (g_context == nullptr)
        return;
    gpu::framegen::detail::Release();
    Destroy();
    gpu::fsr::ReleaseDevice();
    s.failed = false;
    s.lastDispatchNs = 0;
    for (auto& set : s.inputsSets)
        set.reset();
    s.inputsPipeline.reset();
    for (auto& set : s.sanitizeSets)
        set.reset();
    s.sanitizePipeline.reset();
    s.sanitizeLayout.reset();
    s.sanitizeShader.reset();
    s.inputsLayout.reset();
    s.inputsShader.reset();
}
