#pragma once

#include "gpu/edge_smoothing.h"
#include "gpu/gpu_backend.h"
#include "gpu/upscale_mode.h"
#include "host/launcher.h"
#include "host/mouse.h"
#include "host/pad_map.h"
#include "host/speakers.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Keyboard actions of the settings file's [input] key_<action> keys, in the contract's order (docs/getting-started/settings.md).
struct KeyAction
{
    const char* name;         // "a", "left_up"...
    const char* defaultKey;   // SDL key name
};
extern const KeyAction g_keyActions[22];

// How the game starts: the [video] and [input] sections of launcher.ini, the game section's languages, then the
// command line. Precedence: command line > SSA_* variables > launcher.ini > defaults.
struct RuntimeSettings
{
    uint32_t resolutionScale = 0; // 1-10: host render targets are this multiple of the guest's; 0: auto (gpu::Init
                                  // picks it from the GPU: 1 on integrated GPUs, 2 on discrete ones and Apple's)
    bool vsync = true;
    bool fullscreen = false;      // desktop fullscreen (no mode change)
    uint32_t anisotropy = 1;      // 1, 2, 4, 8, 16
    bool fpsAutoFallback = true; // [<game>.video] fps_auto_fallback: allow the automatic 60 -> 30 drop
    uint32_t fpsLimit = 30;       // 30 or 60 (an older limit above 60, or none, reads as 60)
    bool aspectAuto = true; // [video] aspect: auto follows a window wider than 16:9 (Hor+ camera, wide scene and
    // frame, HUD centred); 16:9 keeps the game's 16:9 picture, with bars on a wider window
    bool showFps = false; // [<game>.video] show_fps: the frame rate in a corner (the F3 menu's FPS counter)
    uint32_t debugScreen = 0; // [<game>.video] debug_screen: the diagnostic overlay, 0 off (default), 1 compact, 2 full
    bool reduceMotion = false; // [video] reduce_motion: the in-game messages fade without sliding (host/toybox/look.h)
    uint32_t volume = 100; // [<game>.audio] volume (or [audio]), 0-100: the master volume
    bool portalSound = true; // [<game>.audio] portal_sound (or [audio]): the trap's voice from the Traptanium speaker
    speakers::Mode speakers = speakers::Auto; // [<game>.audio] speakers (or [audio]), SKY_SPEAKERS: auto, stereo, 5.1
    bool skipIntroMovies = false; // skip the start-up logos (legal, atvilogo, xpeclogo, tfb); story movies still play
    bool textureDump = false; // dump the game's textures as DDS files (host/texture_pack.h)
    bool texturePacks = true; // replace textures with the files of the game's textures folder (the player's packs)
    // [<game>] hd_textures (the launcher's "Use HD textures"), SKY_HD_TEXTURES: the game's textures-hd folder, the pack the
    // launcher made from the disc; on when not set (the folder is only there once made). texture_packs has no say in it.
    bool hdTextures = true;
    bool originalResolution = false; // [<game>.video] or [video] original_resolution: the game's own smaller 3D scene,
    // stretched to the frame (Swap Force's 1040x620); default the scene at the frame's size
    edgesmoothing::Mode edgeSmoothing =
        edgesmoothing::Auto; // [<game>.video] or [video] edge_smoothing: the game's FXAA pass,
    // game (its own), light, off; auto (not chosen): light from resolution scale 2, game at 1. Only games with a known FXAA pass.
    // [<game>.video] or [video] upscaler (off, fsr3) and fsr_quality (native, quality, balanced, performance,
    // ultra_performance): the 3D scene upscaled by FSR 3 to the window's scale (gpu/upscale_mode.h); SKY_UPSCALER,
    // SKY_FSR_QUALITY.
    upscalemode::Upscaler upscaler = upscalemode::Upscaler::Off;
    upscalemode::Quality fsrQuality = upscalemode::Quality::Quality;
    // [<game>.video] or [video] fsr_frame_generation (off, on): FSR 3's frame generation, one frame made between two real
    // ones (gpu/fsr/framegen.h); only with upscaler = fsr3. SKY_FSR_FRAME_GENERATION.
    bool fsrFrameGeneration = false;
    // [<game>.video] or [video] gpu_backend (d3d12, vulkan): desktop Windows' graphics API, tried first, the other one
    // when it cannot start (gpu/gpu_backend.h); D3D12 when not set. SKY_GPU_BACKEND overrides it. Ignored elsewhere.
    gpubackend::Backend gpuBackend = gpubackend::RuntimeDefault;
    std::string inputDevice = "auto"; // auto, keyboard, gamepad
    std::string gamepad; // SDL GUID, empty: the first one connected
    std::string keys[22]; // SDL key name per g_keyActions entry
    PadMap padMap; // [input] / [<game>.input] pad_<button>=<button>: what each pad button does
    mouse::Bindings
        mouse; // [<game>.input] mouse=0|1 (off by default) and mouse_<action>=<mouse name>|none (host/mouse.h)
    // [input] key_toybox, key_toybox_add, key_toybox_clear: open the Toy Box, add the next figure, clear the portal (F6, F7,
    // F8); Tab also opens it while key_toybox is not set (toyboxTab).
    std::string toyboxKeys[3] = { "F6", "F7", "F8" };
    bool toyboxTab = true;
    std::string rumble[2] = { "normal", "normal" }; // rumble_p1 / rumble_p2: off, low, normal, strong
    std::string glyphs = "auto"; // button glyphs: auto (the last device used), xbox (auto among the next two), xbox360,
    // xboxone, playstation, nintendo, keyboard
    bool nightmareFromStart =
        false; // [<game>] nightmare_from_start: the hardest difficulty offered from the start (Swap Force, Imaginators)
    bool toyboxAutoOpen =
        true; // the Toy Box opens by itself when the game asks for a figure ([portal] toybox_auto_open)
    std::string language; // text language code, empty: the game's choice
    std::string voiceLanguage;

    bool help = false; // --help
    bool version = false; // --version
    bool listGamepads = false; // --list-gamepads
    bool portalProbe = false; // --portal-probe
    std::vector<std::string> arguments; // what the settings don't consume: the disc image, --select-iso, --launcher
};

// Fills `settings` from `config`, the environment (`getenv`) and the command line (argv[1..]). Bad values are reported
// in `warnings` and leave the previous source's value. Returns false on an unknown option or a missing value (`error`).
using GetEnv = std::function<const char*(const char*)>;
bool ParseSettings(const LauncherConfig& config, const std::string& gameSection, const GetEnv& getEnv, int argc,
    char** argv, RuntimeSettings& settings, std::vector<std::string>& warnings, std::string& error);

// The text of --help.
std::string SettingsHelp(const char* program);

// The effective configuration on one line, for the log.
std::string DescribeSettings(const RuntimeSettings& settings);

// Whether the last ParseSettings turned on [video] skip_intro_movies. The Bink hook (Movie_Update) uses it to end the
// start-up logos only; story movies always play.
bool SkipIntroMovies();

// Shared with the live F3 setting; safe to read from the render thread.
bool FpsAutoFallback();
void SetFpsAutoFallback(bool enabled);
