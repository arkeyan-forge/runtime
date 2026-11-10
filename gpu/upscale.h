#pragma once

// Temporal upscaling of a jittered picture with its depth and motion (docs/runtime/gpu/upscaling.md): AMD FSR 3.1 on
// every backend (gpu/fsr/); MetalFX's temporal scaler on Metal as an internal comparison (SKY_UPSCALER=metalfx, never
// offered to players). The motion is the camera's, from a reprojection matrix and the depth.
#include "gpu/gpu.h"

namespace gpu
{
    enum class Upscaler : uint8_t
    {
        Fsr3,
        MetalFx,
    };

    // Whether this device has `upscaler` (after Init).
    bool UpscalerAvailable(Upscaler upscaler);

    struct UpscaleDesc
    {
        Upscaler upscaler;
        Texture* color;  // the jittered picture
        Texture* depth;  // its depth target, the same size
        Texture* output; // the result, any size the upscaler supports; made with TextureUsage::Storage
        float jitter[2]; // the picture's sub-pixel offset this frame, in its pixels (x right, y down)
        // This frame's clip space to the previous frame's (row-major, column vectors): the camera's motion.
        double reprojection[16];
        // The draws agreed on the camera's motion (upscale::CameraVote). Not: reprojection is the identity, and frame
        // generation takes the frame as a camera cut (a cut changes the draws: nothing pairs).
        bool cameraFound;
        float depthMin, depthMax; // the viewport's depth range: window depth = min + ndc z x (max - min)
        bool depthReversed;       // ndc z is 1 at the near plane (FSR gets 1 - z: 0 near, 1 far)
        bool reset;               // no usable history (the first frame, or after frames without a scene)
        float sharpness;          // FSR's sharpening, 0 (off) to 1
        // The camera, for FSR's depth tests: near and far planes, vertical field of view in radians.
        float cameraNear, cameraFar, fovY;
        // game::Adapter::sceneMotionLayers: the scene's other motions (up to two more reprojections, of draws moving apart
        // from the camera's); the inputs pass gives each pixel the one that finds its colour in the previous frame, and
        // a depth outside the scene's range (the HUD's portrait and its mask) the nearest depth and no motion.
        bool motionLayers;
        // game::Adapter::frameGenerationCutShift (0: 0.5).
        float cutShift;
        uint32_t otherMotionCount;
        double otherMotions[2][16];
    };

    // Queued like the draws: runs after what was recorded before and before what is recorded after.
    void Upscale(const UpscaleDesc& desc);
}
