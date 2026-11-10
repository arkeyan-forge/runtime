#include "upscale_math.h"
#include "gpu/fsr/spirv_reflect.h"
#include "gpu/upscale_mode.h"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int g_checks = 0;
    int g_failures = 0;

    void Check(bool condition, int line)
    {
        g_checks++;
        if (!condition)
        {
            fprintf(stderr, "FAIL line %d\n", line);
            g_failures++;
        }
    }

#define CHECK(condition) Check((condition), __LINE__)

    using upscale::Matrix;

    Matrix Perspective(double aspect, double tanHalf, double nearZ, double farZ)
    {
        Matrix m{};
        m[0] = 1.0 / (aspect * tanHalf);
        m[5] = 1.0 / tanHalf;
        m[10] = farZ / (farZ - nearZ);
        m[11] = -nearZ * farZ / (farZ - nearZ);
        m[14] = 1.0;
        return m;
    }

    // A rotation about Y by `angle` then a translation.
    Matrix Transform(double angle, double x, double y, double z)
    {
        Matrix m = upscale::Identity;
        m[0] = std::cos(angle), m[2] = std::sin(angle);
        m[8] = -std::sin(angle), m[10] = std::cos(angle);
        m[3] = x, m[7] = y, m[11] = z;
        return m;
    }

    void TestJitter()
    {
        CHECK(upscale::Halton(1, 2) == 0.5);
        CHECK(upscale::Halton(2, 2) == 0.25);
        CHECK(upscale::Halton(3, 2) == 0.75);
        CHECK(std::fabs(upscale::Halton(1, 3) - 1.0 / 3.0) < 1e-12);
        CHECK(upscale::JitterPhases(1.0f) == 8);
        CHECK(upscale::JitterPhases(1.5f) == 18);
        CHECK(upscale::JitterPhases(2.0f) == 32);
        double sumX = 0.0, sumY = 0.0;
        for (uint64_t frame = 0; frame < 32; frame++)
        {
            const auto j = upscale::Jitter(frame, 32);
            CHECK(j[0] >= -0.5f && j[0] < 0.5f && j[1] >= -0.5f && j[1] < 0.5f);
            sumX += j[0], sumY += j[1];
        }
        CHECK(std::fabs(sumX / 32) < 0.05 && std::fabs(sumY / 32) < 0.05);
        CHECK(upscale::Jitter(5, 32) == upscale::Jitter(37, 32));
    }

    void TestMatrices()
    {
        const Matrix m = upscale::Multiply(Perspective(16.0 / 9.0, 0.6, 0.5, 4000.0), Transform(0.3, 1.0, -2.0, 30.0));
        Matrix inverse;
        CHECK(upscale::Invert(m, inverse));
        CHECK(upscale::Distance(upscale::Multiply(m, inverse), upscale::Identity) < 1e-9);
        CHECK(!upscale::Invert(Matrix{}, inverse));

        // Row-vector constants: clip = x c0 + y c1 + z c2 + c3.
        float rows[16];
        for (uint32_t i = 0; i < 16; i++)
            rows[i] = float(i + 1);
        const Matrix c = upscale::FromRowVectors(rows);
        CHECK(c[0 * 4 + 3] == 13.0 && c[3 * 4 + 0] == 4.0 && c[1 * 4 + 2] == 10.0);
    }

    // The camera recovered from projection x view x model, whatever the model's rotation, translation and scale.
    void TestCamera()
    {
        const double tanHalf = std::tan(0.5 * 0.9);
        Matrix model = Transform(0.7, 3.0, -2.0, 40.0);
        for (uint32_t i = 0; i < 12; i++)
            model[i] *= 2.5; // a uniform scale
        model[15] = 1.0;
        upscale::Camera camera{};
        CHECK(upscale::CameraOf(upscale::Multiply(Perspective(16.0 / 9.0, tanHalf, 0.5, 2000.0), model), camera));
        CHECK(std::fabs(camera.nearZ - 0.5f) < 1e-3f && std::fabs(camera.farZ - 2000.0f) < 1.0f);
        CHECK(std::fabs(camera.fovY - 0.9f) < 1e-4f && !camera.reversed);
        // Reversed depth (1 near, 0 far): the same planes, said reversed.
        CHECK(upscale::CameraOf(upscale::Multiply(Perspective(16.0 / 9.0, tanHalf, 2000.0, 0.5), model), camera));
        CHECK(camera.reversed && std::fabs(camera.nearZ - 0.5f) < 1e-3f && std::fabs(camera.farZ - 2000.0f) < 1.0f);
        CHECK(!upscale::CameraOf(upscale::Identity, camera)); // orthographic: no camera
    }

    void TestCameraVote()
    {
        const Matrix projection = Perspective(16.0 / 9.0, 0.6, 0.5, 4000.0);
        const Matrix viewBefore = Transform(0.10, 0.0, -3.0, 20.0), viewAfter = Transform(0.13, 0.4, -3.0, 19.5);
        const Matrix vpBefore = upscale::Multiply(projection, viewBefore);
        const Matrix vpAfter = upscale::Multiply(projection, viewAfter);
        Matrix expected, inverse;
        CHECK(upscale::Invert(vpAfter, inverse));
        expected = upscale::Multiply(vpBefore, inverse);

        upscale::CameraVote vote;
        auto frame = [&](const Matrix& vp, double moved) {
            // Twelve static objects at their own places; the same mesh twice (identity 7) pairs by order.
            for (uint64_t id = 1; id <= 12; id++)
                vote.Add(id == 12 ? 7 : id, upscale::Multiply(vp, Transform(0.2 * id, id * 3.0, 0.0, id * -2.0)), 1000);
            // Three characters, each moving its own way, drawn first in the frame.
            for (uint64_t id = 100; id < 103; id++)
                vote.Add(id, upscale::Multiply(vp, Transform(moved * id, moved * 5.0, 0.0, 1.0)), 3000);
        };
        Matrix reprojection;
        frame(vpBefore, 0.0);
        CHECK(!vote.Close(reprojection)); // no previous frame
        frame(vpAfter, 0.01);
        CHECK(vote.Close(reprojection));
        CHECK(upscale::Distance(reprojection, expected) < 1e-6);

        // A still camera: the identity.
        frame(vpAfter, 0.02);
        CHECK(vote.Close(reprojection));
        CHECK(upscale::Distance(reprojection, upscale::Identity) < 1e-6);

        // Only four objects, each moving its own way: no quorum.
        upscale::CameraVote movers;
        for (int f = 0; f < 2; f++)
        {
            for (uint64_t id = 1; id <= 4; id++)
                movers.Add(id, upscale::Multiply(vpBefore, Transform(0.1 * f * id, f * id * 1.0, 0.0, 0.0)), 100);
            CHECK(!movers.Close(reprojection));
        }

        // A ship the camera rides (Swap Force): eight draws keep their place on the screen while twelve of the world
        // move with the camera; the world wins and the ship's motion (the identity) is the other one. The three
        // characters, each its own way, make no cluster of a tenth of the weight.
        upscale::CameraVote ship;
        auto shipFrame = [&](const Matrix& vp, double moved) {
            for (uint64_t id = 1; id <= 12; id++)
                ship.Add(id, upscale::Multiply(vp, Transform(0.2 * id, id * 3.0, 0.0, id * -2.0)), 1000);
            for (uint64_t id = 20; id < 28; id++)
                ship.Add(id, upscale::Multiply(vpBefore, Transform(0.1 * id, id * 1.5, -1.0, 0.0)), 700);
            for (uint64_t id = 100; id < 103; id++)
                ship.Add(id, upscale::Multiply(vp, Transform(moved * id, moved * 5.0, 0.0, 1.0)), 300);
        };
        shipFrame(vpBefore, 0.0);
        CHECK(!ship.Close(reprojection));
        shipFrame(vpAfter, 0.01);
        CHECK(ship.Close(reprojection));
        CHECK(upscale::Distance(reprojection, expected) < 1e-6);
        Matrix others[2];
        CHECK(ship.Others(others) == 1);
        CHECK(upscale::Distance(others[0], upscale::Identity) < 1e-6);

        // A frame without a scene: nothing to pair with.
        frame(vpAfter, 0.0);
        vote.Forget();
        frame(vpAfter, 0.0);
        CHECK(!vote.Close(reprojection));
    }
}

namespace
{
    // The settings' names, the presets' ratios and the scales they give (gpu/upscale_mode.h).
    void TestModes()
    {
        using namespace upscalemode;
        Upscaler upscaler = Upscaler::Off;
        CHECK(ParseUpscaler("fsr3", upscaler) && upscaler == Upscaler::Fsr3);
        CHECK(ParseUpscaler("off", upscaler) && upscaler == Upscaler::Off);
        CHECK(!ParseUpscaler("metalfx", upscaler));                              // never from the settings file
        CHECK(ParseUpscaler("metalfx", upscaler, true) && upscaler == Upscaler::MetalFx); // the environment's
        CHECK(!ParseUpscaler("FSR3", upscaler) && !ParseUpscaler("", upscaler));
        for (Upscaler u : { Upscaler::Off, Upscaler::Fsr3, Upscaler::MetalFx })
        {
            Upscaler back = Upscaler::Off;
            CHECK(ParseUpscaler(Name(u), back, true) && back == u);
        }
        Quality quality = Quality::Quality;
        const Quality all[] = { Quality::Quality, Quality::Balanced, Quality::Performance, Quality::UltraPerformance,
            Quality::Native };
        const float ratios[] = { 1.5f, 1.7f, 2.0f, 3.0f, 1.0f };
        for (uint32_t i = 0; i < QualityCount; i++)
        {
            CHECK(ParseQuality(Name(all[i]), quality) && quality == all[i]);
            CHECK(Ratio(all[i]) == ratios[i]); // FSR 3's presets (ffxFsr3UpscalerGetUpscaleRatioFromQualityMode)
        }
        CHECK(
            !ParseQuality("ultra", quality) && !ParseQuality("native_aa", quality) && !ParseQuality("Native", quality));
        CHECK(std::strcmp(Name(Quality::UltraPerformance), "ultra_performance") == 0);
        CHECK(std::strcmp(Name(Quality::Native), "native") == 0);
        // The four presets keep their values (the menus' indexes); native comes after them.
        CHECK(uint32_t(Quality::Quality) == 0 && uint32_t(Quality::UltraPerformance) == 3);
        CHECK(uint32_t(Quality::Native) == 4 && QualityCount == 5);

        CHECK(FrameScale(720) == 1 && FrameScale(1080) == 2 && FrameScale(1440) == 2 && FrameScale(2160) == 3);
        CHECK(FrameScale(0) == 1 && FrameScale(2880) == 4);
        // Window 720p: FSR at 1:1. 1440p: every preset at scale 1. 4K: 2, 2, 2, 1. 5K: 3, 2, 2, 1. Native: the frame's.
        const uint32_t expected[5][5] = { {}, { 1, 1, 1, 1, 1 }, { 1, 1, 1, 1, 2 }, { 2, 2, 2, 1, 3 },
            { 3, 2, 2, 1, 4 } };
        for (uint32_t frame = 1; frame <= 4; frame++)
            for (uint32_t i = 0; i < QualityCount; i++)
            {
                const uint32_t scene = SceneScale(frame, all[i]);
                CHECK(scene == expected[frame][i]);
                CHECK(scene >= 1 && scene <= frame);
            }
        // The jitter phases FSR wants for each ratio (ffxFsr3UpscalerGetJitterPhaseCount: 8 x ratio squared).
        CHECK(
            upscale::JitterPhases(1.0f) == 8 && upscale::JitterPhases(2.0f) == 32 && upscale::JitterPhases(1.5f) == 18);
        CHECK(upscale::JitterPhases(3.0f) == 72);
    }

    // A hand-made SPIR-V module: what gpu/fsr/backend.cpp reads from FSR's shaders.
    void TestSpirvReflect()
    {
        std::vector<uint32_t> words = { 0x07230203, 0x00010300, 0, 100, 0 };
        auto op = [&](uint32_t opcode, std::vector<uint32_t> operands) {
            words.push_back(uint32_t(operands.size() + 1) << 16 | opcode);
            words.insert(words.end(), operands.begin(), operands.end());
        };
        auto name = [&](uint32_t id, const char* text) {
            std::vector<uint32_t> operands = { id };
            const size_t length = std::strlen(text) + 1;
            for (size_t i = 0; i < length; i += 4)
            {
                uint32_t word = 0;
                for (size_t b = 0; b < 4 && i + b < length; b++)
                    word |= uint32_t(uint8_t(text[i + b])) << (8 * b);
                operands.push_back(word);
            }
            op(5, operands);
        };
        enum : uint32_t
        {
            Main = 1,
            Float,
            Image,
            RwImage,
            Sampler,
            CbStruct,
            PtrImage,
            PtrRw,
            PtrSampler,
            PtrCb,
            Texture,
            Output,
            LinearClamp,
            Constants,
            Unbound
        };
        op(16, { Main, 17, 8, 4, 1 }); // OpExecutionMode LocalSize 8 4 1
        name(Texture, "r_input_color_jittered");
        name(Output, "rw_upscaled_output");
        name(LinearClamp, "s_LinearClamp");
        name(Constants, "cbFSR3Upscaler");
        for (auto [id, binding] :
            { std::pair(Texture, 16u), std::pair(Output, 49u), std::pair(LinearClamp, 9u), std::pair(Constants, 0u) })
        {
            op(71, { id, 34, 0 });       // DescriptorSet 0
            op(71, { id, 33, binding }); // Binding
        }
        op(71, { CbStruct, 2 }); // Block
        op(22, { Float, 32 });
        op(25, { Image, Float, 1, 0, 0, 0, 1, 0 });   // sampled
        op(25, { RwImage, Float, 1, 0, 0, 0, 2, 1 }); // storage
        op(26, { Sampler });
        op(30, { CbStruct, Float });
        op(32, { PtrImage, 0, Image });
        op(32, { PtrRw, 0, RwImage });
        op(32, { PtrSampler, 0, Sampler });
        op(32, { PtrCb, 2, CbStruct });
        op(59, { PtrCb, Constants, 2 });
        op(59, { PtrImage, Texture, 0 });
        op(59, { PtrRw, Output, 0 });
        op(59, { PtrSampler, LinearClamp, 0 });
        op(59, { PtrImage, Unbound, 0 }); // no binding: not a resource of the set

        gpu::spirv::Module module;
        CHECK(gpu::spirv::Reflect(words.data(), words.size(), module));
        CHECK(module.localSize[0] == 8 && module.localSize[1] == 4 && module.localSize[2] == 1);
        CHECK(module.bindings.size() == 4);
        if (module.bindings.size() == 4)
        {
            using gpu::spirv::Kind;
            CHECK(module.bindings[0].name == "cbFSR3Upscaler" && module.bindings[0].kind == Kind::ConstantBuffer &&
                  module.bindings[0].binding == 0);
            CHECK(module.bindings[1].name == "r_input_color_jittered" && module.bindings[1].kind == Kind::Texture &&
                  module.bindings[1].binding == 16);
            CHECK(module.bindings[2].name == "rw_upscaled_output" && module.bindings[2].kind == Kind::StorageTexture &&
                  module.bindings[2].binding == 49);
            CHECK(module.bindings[3].name == "s_LinearClamp" && module.bindings[3].kind == Kind::Sampler &&
                  module.bindings[3].binding == 9 && module.bindings[3].set == 0);
        }
        gpu::spirv::Module bad;
        const uint32_t notSpirv[] = { 0x12345678, 0, 0, 0, 0 };
        CHECK(!gpu::spirv::Reflect(notSpirv, 5, bad));
        words.push_back(0x00050000); // an instruction longer than the module
        CHECK(!gpu::spirv::Reflect(words.data(), words.size(), bad));
    }

    // Vulkan's storage images without DXC's formats (gpu::spirv::WithoutStorageFormats): Rg32f and R32f become Unknown,
    // and then one type (the R32f one's declaration and name go, its pointer and load take the Rg32f one); an image an
    // atomic reads keeps R32ui; a sampled image is untouched; the two capabilities come right after the header.
    void TestSpirvFormats()
    {
        std::vector<uint32_t> words = { 0x07230203, 0x00010300, 0, 100, 0 };
        auto op = [&](std::vector<uint32_t>& to, uint32_t opcode, std::vector<uint32_t> operands) {
            to.push_back(uint32_t(operands.size() + 1) << 16 | opcode);
            to.insert(to.end(), operands.begin(), operands.end());
        };
        enum : uint32_t
        {
            Float = 1,
            Uint,
            Image,
            RwImage,
            RwImage2,
            AtomicImage,
            PtrRw,
            PtrRw2,
            PtrAtomic,
            Motion,
            Depth,
            Counter,
            TexelPtr,
            Loaded
        };
        // The module, and what it must become (`expected`, after the header and the capabilities).
        std::vector<uint32_t> expected;
        auto both = [&](uint32_t opcode, std::vector<uint32_t> operands, std::vector<uint32_t> after) {
            op(words, opcode, operands);
            if (!after.empty())
                op(expected, opcode, after);
        };
        both(17, { 1 }, { 1 });                                   // OpCapability Shader
        both(5, { RwImage2, 0x00327772 }, {});                    // OpName "rw2": goes
        both(22, { Float, 32 }, { Float, 32 });
        both(21, { Uint, 32, 0 }, { Uint, 32, 0 });
        both(25, { Image, Float, 1, 0, 0, 0, 1, 0 }, { Image, Float, 1, 0, 0, 0, 1, 0 });       // sampled
        both(25, { RwImage, Float, 1, 0, 0, 0, 2, 6 }, { RwImage, Float, 1, 0, 0, 0, 2, 0 });   // Rg32f
        both(25, { RwImage2, Float, 1, 0, 0, 0, 2, 3 }, {});                                    // R32f: merged
        both(25, { AtomicImage, Uint, 1, 0, 0, 0, 2, 33 }, { AtomicImage, Uint, 1, 0, 0, 0, 2, 33 }); // R32ui
        both(32, { PtrRw, 0, RwImage }, { PtrRw, 0, RwImage });
        both(32, { PtrRw2, 0, RwImage2 }, { PtrRw2, 0, RwImage });
        both(32, { PtrAtomic, 0, AtomicImage }, { PtrAtomic, 0, AtomicImage });
        both(59, { PtrRw, Motion, 0 }, { PtrRw, Motion, 0 });
        both(59, { PtrRw2, Depth, 0 }, { PtrRw2, Depth, 0 });
        both(59, { PtrAtomic, Counter, 0 }, { PtrAtomic, Counter, 0 });
        both(61, { RwImage2, Loaded, Depth }, { RwImage, Loaded, Depth });                   // OpLoad
        both(60, { Uint, TexelPtr, Counter, Uint, Uint }, { Uint, TexelPtr, Counter, Uint, Uint }); // atomic

        const std::vector<uint32_t> out = gpu::spirv::WithoutStorageFormats(words.data(), words.size());
        const std::vector<uint32_t> capabilities = { 2u << 16 | 17, 55, 2u << 16 | 17, 56 };
        CHECK(out.size() == 9 + expected.size());
        if (out.size() == 9 + expected.size())
        {
            CHECK(std::equal(out.begin(), out.begin() + 5, words.begin()));
            CHECK(std::equal(capabilities.begin(), capabilities.end(), out.begin() + 5));
            CHECK(std::equal(expected.begin(), expected.end(), out.begin() + 9));
            gpu::spirv::Module module;
            CHECK(gpu::spirv::Reflect(out.data(), out.size(), module));
        }
        // Nothing to change: the same words, no capability added.
        CHECK(gpu::spirv::WithoutStorageFormats(out.data(), out.size()) == out);
        const uint32_t notSpirv[] = { 0x12345678, 0, 0, 0, 0 };
        CHECK(gpu::spirv::WithoutStorageFormats(notSpirv, 5).empty());
    }

    // The scene's depth range: the widest holding a quarter of the weight, not a light pass over the whole range.
    void TestSceneDepthRange()
    {
        using upscale::DepthRange;
        // Swap Force's frame: depth-only quads over 1 -> 0 (14 x 6 vertices), the world in 1 -> 0.2.
        const DepthRange slices[] = { { 1.0f, 0.0f, 84 }, { 1.0f, 0.2f, 60000 }, { 0.1f, 0.0f, 900 } };
        DepthRange r = upscale::SceneDepthRange(slices, 3);
        CHECK(r.minDepth == 1.0f && r.maxDepth == 0.2f);
        // A sky at the far plane (heavy) and the world over the whole range: the world's, as before.
        const DepthRange sky[] = { { 1.0f, 1.0f, 50000 }, { 0.0f, 1.0f, 40000 } };
        r = upscale::SceneDepthRange(sky, 2);
        CHECK(r.minDepth == 0.0f && r.maxDepth == 1.0f);
        // One range: itself; none: 0 -> 1.
        r = upscale::SceneDepthRange(slices + 2, 1);
        CHECK(r.minDepth == 0.1f && r.maxDepth == 0.0f);
        r = upscale::SceneDepthRange(nullptr, 0);
        CHECK(r.minDepth == 0.0f && r.maxDepth == 1.0f);
        // No range holds a quarter: the widest of all.
        const DepthRange spread[] = { { 0.0f, 0.5f, 20 }, { 0.5f, 0.6f, 20 }, { 0.6f, 0.7f, 20 }, { 0.7f, 0.8f, 20 },
            { 0.8f, 0.9f, 21 } };
        r = upscale::SceneDepthRange(spread, 5);
        CHECK(r.minDepth == 0.0f && r.maxDepth == 0.5f);
    }
}

int main()
{
    TestJitter();
    TestMatrices();
    TestCameraVote();
    TestModes();
    TestSpirvReflect();
    TestSpirvFormats();
    TestCamera();
    TestSceneDepthRange();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
