#include "host/audio.h"
#include "host/collection/collection.h"
#include "host/collection/datadir.h"
#include "host/collection/progress.h"
#include "host/collection/save_backup.h"
#include "host/display.h"
#include "host/env.h"
#include "host/gamemenu.h"
#include "host/glyphs.h"
#include "host/input.h"
#include "host/launcher.h"
#include "host/log.h"
#include "host/mouse_input.h"
#include "host/pad_map.h"
#include "host/portable.h"
#include "host/portal.h"
#include "host/settings.h"
#include "host/xex_title.h"
#include "host/texture_pack.h"
#include "host/toybox.h"
#include "gpu/gpu.h"
#include "gpu/first_frame.h"
#include "d3d/ultrawide.h"
#include "d3d/upscale.h"
#include "kernel/video.h"
#include "game.h"
#include "language.h"
#include "loader.h"
#include "memory.h"
#include "platform/platform.h"
#include "gpu/frame_stats.h"
#include "gpu/fsr/framegen.h"
#include "gpu/fsr/framegen_rules.h"
#include "thread.h"
#include "vfs.h"
#include "write_watch.h"
#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#endif

// The window went away without a close request (SDL_EVENT_WINDOW_HIDDEN: on macOS an NSWindow closed or ordered out
// without windowShouldClose); SDL sends no SDL_EVENT_QUIT then, and the game ran on with no window. Cmd+H hides the
// window too, so a hidden application is not that case. Minimizing sends SDL_EVENT_WINDOW_MINIMIZED instead.
static bool IsWindowGone()
{
#ifdef __APPLE__
    id application = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(
        objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
    return !reinterpret_cast<BOOL (*)(id, SEL)>(objc_msgSend)(application, sel_registerName("isHidden"));
#else
    return false;
#endif
}

static void OnFault(const char* description, const void* hostAddress)
{
    auto* address = static_cast<const uint8_t*>(hostAddress);
    if (address >= g_memory.base && address < g_memory.base + 0x100000000ull)
        fprintf(stderr, "\nFault (%s) at guest address 0x%08X\n", description, uint32_t(address - g_memory.base));
    else
        fprintf(stderr, "\nFault (%s) at host address %p\n", description, hostAddress);
    PrintGuestLocation();
}

// Keyboard bindings (settings, [input] key_<action>; Escape is also back) and the gamepad, per the input device setting.
struct HostInput
{
    bool keyboard = true, gamepad = true;
    SDL_Scancode keys[std::size(g_keyActions)];
    PadMap padMap;                  // what each pad button does (the keyboard has its own keys)
    std::string rumble[2];          // off, low, normal, strong, per player
    bool raw = false; // the pad as it is, no keyboard and no remapping (the F3 menu's Commands page waits for a button)
};

// Desktop Windows: D3D12 showed nothing (gpu/first_frame.h). Vulkan for this game from now on, then a new start: the
// launcher starts the game again when it said it would (SKY_LAUNCHER_RESTARTS=1), else a new copy starts here. Both get
// SKY_GPU_BACKEND=vulkan, so a settings file that could not be written never brings D3D12 back: no loop. Called from
// the GPU's watch thread while the game's threads may be stuck on the GPU: the exit does not wait for them.
static void SwitchToVulkan(const char* why)
{
    const std::string path = LauncherConfigPath();
    LauncherConfig config = LoadLauncherConfig(path);
    gpu::firstframe::WriteSwitch(config, std::string(ARKEYAN_GAME_ID) + ".video", why);
    if (SaveLauncherConfig(path, config))
        fprintf(stderr, "[gpu] this game starts with Vulkan from now on: gpu_backend = vulkan in [%s.video]\n",
            ARKEYAN_GAME_ID);
    else
        fprintf(stderr, "[gpu] cannot write %s: Vulkan for this start only\n", path.c_str());
    savebackup::SetGameRunning(false); // the new copy must not find this one's marker
    const bool launcher = skyenv::Get("SKY_LAUNCHER_RESTARTS") != nullptr;
    if (launcher)
        fprintf(stderr, "[gpu] restarting with Vulkan: the launcher starts the game again\n");
    else if (PlatformStartAgain("SKY_GPU_BACKEND", "vulkan"))
        fprintf(stderr, "[gpu] restarting with Vulkan: a new copy of the game started\n");
    else
        fprintf(stderr, "[gpu] could not start the game again: start it again, it will use Vulkan\n");
    fflush(stderr);
    std::_Exit(gpu::firstframe::RestartExitCode);
}

static HostInput MakeHostInput(const RuntimeSettings& settings)
{
    HostInput input;
    input.keyboard = settings.inputDevice != "gamepad";
    input.gamepad = settings.inputDevice != "keyboard";
    for (size_t i = 0; i < std::size(g_keyActions); i++)
        input.keys[i] = SDL_GetScancodeFromName(settings.keys[i].c_str());
    input.padMap = settings.padMap;
    input.rumble[0] = settings.rumble[0];
    input.rumble[1] = settings.rumble[1];
    return input;
}

// `mouse`: the actions the mouse holds (Player 1's, host/mouse_input.h), else nullptr.
static GamepadState ReadHostGamepad(const HostInput& input, SDL_Gamepad* gamepad, const bool* mouse = nullptr)
{
    GamepadState state;
    if ((input.keyboard || mouse != nullptr) && !input.raw)
    {
        const bool* keys = ToyboxKeyboardState();
        auto held = [&](size_t action) {
            return (input.keyboard && keys[input.keys[action]]) || (mouse != nullptr && mouse[action]);
        };
        // g_keyActions order: a b x y start back lb rb lt rt, left stick, right stick, d-pad.
        static const uint16_t buttons[] = { Button::A, Button::B, Button::X, Button::Y, Button::Start, Button::Back,
            Button::LeftShoulder, Button::RightShoulder };
        for (size_t i = 0; i < std::size(buttons); i++)
            if (held(i))
                state.buttons |= buttons[i];
        static const uint16_t dpad[] = { Button::DpadUp, Button::DpadDown, Button::DpadLeft, Button::DpadRight };
        for (size_t i = 0; i < std::size(dpad); i++)
            if (held(18 + i))
                state.buttons |= dpad[i];
        if (input.keyboard && keys[SDL_SCANCODE_ESCAPE])
            state.buttons |= Button::Back;
        state.leftTrigger = held(8) ? 255 : 0;
        state.rightTrigger = held(9) ? 255 : 0;
        state.thumbLY = int16_t((held(10) - held(11)) * 32767);
        state.thumbLX = int16_t((held(13) - held(12)) * 32767);
        state.thumbRY = int16_t((held(14) - held(15)) * 32767);
        state.thumbRX = int16_t((held(17) - held(16)) * 32767);
    }

    if (gamepad != nullptr)
    {
        static const std::pair<SDL_GamepadButton, uint16_t> buttons[] = {
            { SDL_GAMEPAD_BUTTON_DPAD_UP, Button::DpadUp },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN, Button::DpadDown },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, Button::DpadLeft },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Button::DpadRight },
            { SDL_GAMEPAD_BUTTON_START, Button::Start },
            { SDL_GAMEPAD_BUTTON_BACK, Button::Back },
            { SDL_GAMEPAD_BUTTON_TOUCHPAD, Button::Back }, // PS4 / PS5: the touchpad's click is BACK too (its glyph)
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, Button::LeftThumb },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK, Button::RightThumb },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Button::LeftShoulder },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Button::RightShoulder },
            { SDL_GAMEPAD_BUTTON_SOUTH, Button::A },
            { SDL_GAMEPAD_BUTTON_EAST, Button::B },
            { SDL_GAMEPAD_BUTTON_WEST, Button::X },
            { SDL_GAMEPAD_BUTTON_NORTH, Button::Y },
        };
        GamepadState pad;
        for (const auto& [sdlButton, button] : buttons)
        {
            if (SDL_GetGamepadButton(gamepad, sdlButton))
                pad.buttons |= button;
        }
        auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(gamepad, a); };
        pad.thumbLX = axis(SDL_GAMEPAD_AXIS_LEFTX);
        pad.thumbLY = int16_t(-std::max(-32767, int(axis(SDL_GAMEPAD_AXIS_LEFTY))));
        pad.thumbRX = axis(SDL_GAMEPAD_AXIS_RIGHTX);
        pad.thumbRY = int16_t(-std::max(-32767, int(axis(SDL_GAMEPAD_AXIS_RIGHTY))));
        pad.leftTrigger = uint8_t(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
        pad.rightTrigger = uint8_t(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
        if (!input.raw)
            pad = RemapPad(pad, input.padMap);
        state.buttons |= pad.buttons;
        if (state.thumbLX == 0)
            state.thumbLX = pad.thumbLX;
        if (state.thumbLY == 0)
            state.thumbLY = pad.thumbLY;
        if (state.thumbRX == 0)
            state.thumbRX = pad.thumbRX;
        if (state.thumbRY == 0)
            state.thumbRY = pad.thumbRY;
        state.leftTrigger = std::max(state.leftTrigger, pad.leftTrigger);
        state.rightTrigger = std::max(state.rightTrigger, pad.rightTrigger);
    }
    return state;
}

// Sends what the title asked for (XamInputSetState) to the user's pad, scaled by the player's rumble setting. SDL stops a
// rumble after its duration, so a running one is sent again every half second.
static void SendRumble(int user, SDL_Gamepad* gamepad, const std::string& strength)
{
    static uint16_t lastLeft[MaxUsers] = {}, lastRight[MaxUsers] = {};
    static uint64_t sentAt[MaxUsers] = {};
    static SDL_Gamepad* lastPad[MaxUsers] = {};
    uint16_t asked[2] = {};
    uint32_t serial = 0;
    GetHostVibration(user, asked[0], asked[1], serial);
    const uint16_t left = gamepad != nullptr ? ScaleRumble(asked[0], strength) : 0;
    const uint16_t right = gamepad != nullptr ? ScaleRumble(asked[1], strength) : 0;
    const uint64_t now = SDL_GetTicks();
    const bool changed = left != lastLeft[user] || right != lastRight[user] || gamepad != lastPad[user];
    if (!changed && ((left | right) == 0 || now - sentAt[user] < 500))
        return;
    if (gamepad != nullptr)
        SDL_RumbleGamepad(gamepad, left, right, (left | right) != 0 ? 1000 : 0);
    if (changed && gamepad != nullptr)
        fprintf(stderr, "[input] rumble player %d: the game asked %u/%u, %s: %u/%u\n", user + 1, unsigned(asked[0]),
            unsigned(asked[1]), strength.c_str(), unsigned(left), unsigned(right));
    lastLeft[user] = left;
    lastRight[user] = right;
    lastPad[user] = gamepad;
    sentAt[user] = now;
}

static std::string GamepadGuid(SDL_JoystickID id)
{
    char text[64];
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), text, sizeof(text));
    return text;
}

// --list-gamepads: gamepads can take a moment to show up after SDL starts.
static void ListGamepads()
{
    SDL_Init(SDL_INIT_GAMEPAD);
    for (uint64_t end = SDL_GetTicks() + 500; SDL_GetTicks() < end;)
    {
        SDL_PumpEvents();
        SDL_Delay(20);
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (count == 0)
        printf("No gamepad connected.\n");
    for (int i = 0; i < count; i++)
        printf("%s  %s\n", GamepadGuid(ids[i]).c_str(), SDL_GetGamepadNameForID(ids[i]));
    SDL_free(ids);
}

int main(int argc, char** argv)
{
    datadir::MigrateOnce(); // the older folders into <data root> (datadir.h), before any path is used
    const GameInfo& game = *FindGame(ARKEYAN_GAME_ID); // the game this runtime was built for (-DARKEYAN_GAME)
    // A portable's figures and keys, before the settings are read.
    const std::string firstLaunch = portable::FirstLaunch(game, collection::Folder());
    RuntimeSettings settings;
    std::vector<std::string> warnings;
    std::string error;
    bool parsed =
        ParseSettings(LoadLauncherConfig(LauncherConfigPath()), game.id, getenv, argc, argv, settings, warnings, error);
    if (parsed && settings.portalProbe)
        return PortalProbeMain();
    if (parsed && !settings.help && !settings.version && !settings.listGamepads)
    {
        LogInit();
        LogHeader(settings);
        if (!firstLaunch.empty())
            fprintf(stderr, "%s\n", firstLaunch.c_str());
    }
    for (const std::string& warning : warnings)
        fprintf(stderr, "[settings] %s\n", warning.c_str());
    if (!parsed)
    {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    if (settings.help)
    {
        printf("%s", SettingsHelp(game.runtime).c_str());
        return 0;
    }
    if (settings.version)
    {
        printf("ArkeyanRuntime %s\n", ARKEYAN_VERSION);
        return 0;
    }
    if (settings.listGamepads)
    {
        ListGamepads();
        return 0;
    }

    // One copy of the game at a time: the installed game and a portable one share the saves, and so the running game's
    // marker (save_backup.h).
    if (savebackup::GameRunning())
    {
        const std::string message = std::string(game.name) + " is already running. Close it before starting it again.";
        fprintf(stderr, "%s\n", message.c_str());
        if (skyenv::Get("SKY_HEADLESS") == nullptr)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, game.name, message.c_str(), nullptr);
        return 1;
    }

    // The launcher's own arguments: the disc image, --select-iso, --launcher.
    std::vector<char*> arguments{ argv[0] };
    for (std::string& argument : settings.arguments)
        arguments.push_back(argument.data());
    std::string discPath =
        ResolveGameSource(int(arguments.size()), arguments.data(), game, skyenv::Get("SKY_HEADLESS") == nullptr);
    if (skyenv::Get("SKY_LAUNCHER_SHOTS") != nullptr)
        return 0; // screenshot test run: the launcher already quit
    if (discPath.empty())
        return 1;

    // SKY_NO_FPS_CAP=1 (test aid): the game's cap is not applied, to measure above it (docs/porting/debugging.md).
    if (skyenv::Get("SKY_NO_FPS_CAP") == nullptr)
        if (uint32_t capped = gamemenu::CapFrameRate(settings.fpsLimit, ::game::Current().maxFpsLimit);
            capped != settings.fpsLimit)
        {
            fprintf(stderr, "[settings] fps_limit=%u: this game is capped at %u FPS, using %u\n", settings.fpsLimit,
                ::game::Current().maxFpsLimit, capped);
            settings.fpsLimit = capped;
        }
    if (!settings.language.empty())
    {
        bool known = SetTextLanguage(settings.language);
        if (::game::Current().SetTextLanguage != nullptr)
            known = ::game::Current().SetTextLanguage(settings.language) || known;
        if (!known)
            fprintf(stderr, "unknown text language \"%s\", using French\n", settings.language.c_str());
    }
    if (!settings.voiceLanguage.empty())
    {
        if (::game::Current().SetVoiceLanguage == nullptr)
            fprintf(stderr, "%s has no voice language setting yet, ignoring \"%s\"\n", game.name,
                settings.voiceLanguage.c_str());
        else if (!::game::Current().SetVoiceLanguage(settings.voiceLanguage))
            fprintf(stderr, "unknown voice language \"%s\", using English\n", settings.voiceLanguage.c_str());
    }

    if (settings.nightmareFromStart)
    {
        if (::game::Current().SetNightmareFromStart == nullptr)
            fprintf(stderr, "%s has no locked difficulty, ignoring nightmare_from_start\n", game.name);
        else
            ::game::Current().SetNightmareFromStart(true);
    }

    PlatformInit();
    PlatformInstallFaultHandler(OnFault);

    if (!g_disc.Open(discPath.c_str()))
    {
        fprintf(stderr, "%s is not an Xbox 360 game disc image\n", discPath.c_str());
        return 1;
    }

    g_memory.Reserve();
    writewatch::Init(g_memory.base, 0x100000000ull);
    g_module = LoadXex(g_disc.ReadAll("default.xex"));

    if (const char* script = skyenv::Get("SKY_PRESS"))
        LoadInputScript(script);
    if (const char* script = skyenv::Get("SKY_PRESS2")) // the same for the second controller (Player 2)
        LoadInputScript(script, 1);

    // The host's main thread owns the window and input (macOS requires it); the title runs on its own threads.
    bool headless = skyenv::Get("SKY_HEADLESS") != nullptr;
    // The 3D at the frame's size where the game renders it smaller (Swap Force), unless original_resolution or FSR: FSR
    // needs the scene apart from the frame (a smaller target), and upscales the game's own scene to the frame itself.
    const bool fullScene = ::game::Current().fullScene.width != 0 && !settings.originalResolution &&
                           settings.upscaler == upscalemode::Upscaler::Off;
    if (fullScene)
        ultrawide::SetScene16x9(::game::Current().fullScene.width, ::game::Current().fullScene.height, false,
            ::game::Current().maxSceneWidth);
    SDL_Window* window = nullptr;
    if (!headless)
    {
        // Video has its own diagnostics and Linux fallback in CreateDisplayWindow. A failed audio or gamepad
        // subsystem must not prevent the window from being created.
        if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
            fprintf(stderr, "[display] SDL audio/gamepad init failed: %s\n", SDL_GetError());
        HostAudioInit(settings.speakers);
        gpu::g_settings = { .resolutionScale = settings.resolutionScale,
            .anisotropy = settings.anisotropy,
            .vsync = settings.vsync,
            .fpsLimit = settings.fpsLimit,
            .edgeSmoothing = settings.edgeSmoothing,
            .backend = settings.gpuBackend };
        gpu::SetFirstFrameFallback(SwitchToVulkan);
        // The game's name and its version (games/<id>/game.txt): "Skylanders: Spyro's Adventure v0.9.1".
        const std::string title = std::string(g_gameInfo.name) + " v" + ARKEYAN_GAME_VERSION;
        window = CreateDisplayWindow(title.c_str(), 1280, 720, settings.fullscreen);
#ifndef __APPLE__ // macOS: the bundle's icon (Arkeyan-Builder's BundleIcon)
        if (window !=
            nullptr) // game_icon.png beside the executable (Arkeyan-Builder's GameIcon: the disc's logo); else the icon of default.xex; else the system's
        {
            std::vector<uint8_t> png;
            if (const char* base = SDL_GetBasePath())
                if (std::ifstream file(std::filesystem::path(base) / "game_icon.png", std::ios::binary); file)
                    png.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (png.empty())
                png = portable::ReadOwn(portable::IconName);
            if (png.empty())
                png = ReadTitleMetadata(g_disc.ReadAll("default.xex")).iconPng;
            int iconWidth = 0, iconHeight = 0, channels = 0;
            if (unsigned char* rgba =
                    stbi_load_from_memory(png.data(), int(png.size()), &iconWidth, &iconHeight, &channels, 4))
            {
                if (SDL_Surface* icon =
                        SDL_CreateSurfaceFrom(iconWidth, iconHeight, SDL_PIXELFORMAT_RGBA32, rgba, iconWidth * 4))
                {
                    SDL_SetWindowIcon(window, icon);
                    SDL_DestroySurface(icon);
                }
                stbi_image_free(rgba);
            }
        }
#endif
        if (window != nullptr)
        {
            int windowWidth = 0, windowHeight = 0;
            SDL_SyncWindow(window);
            SDL_GetWindowSizeInPixels(window, &windowWidth, &windowHeight);
            if (::game::Current().ultrawide) // else the game keeps its 16:9 (game.h)
                ultrawide::Configure(settings.aspectAuto, uint32_t(windowWidth), uint32_t(windowHeight));
            upscale::Configure(uint32_t(windowHeight), settings.upscaler, settings.fsrQuality, fullScene);
            const ::framegen::Hold hold = ::framegen::HeldBack(gpu::framegen::Preview(),
                ::game::Current().frameGeneration, skyenv::Get("SKY_FSR_FRAME_GENERATION_ANY_GAME") != nullptr);
            gpu::framegen::SetBlocked(hold != ::framegen::Hold::None);
            const bool frameGeneration = settings.fsrFrameGeneration && gpu::framegen::Available();
            if (settings.fsrFrameGeneration && !frameGeneration)
                fprintf(stderr, "[framegen] asked, but this GPU backend cannot run it: off\n");
            else if (frameGeneration && hold == ::framegen::Hold::Later)
                fprintf(stderr,
                    "[framegen] asked, but it is coming later (SKY_FSR_FRAME_GENERATION_PREVIEW=1 runs it): off\n");
            else if (frameGeneration && hold == ::framegen::Hold::Game)
                fprintf(stderr, "[framegen] asked, but not available in this game yet: off\n");
            gpu::framegen::SetEnabled(frameGeneration);
            // SKY_FSR_FG_SNAPSHOT=<prefix>: generated frames and the real ones after them as <prefix>_<gen|real>_<n>.png
            // (offscreen only; SKY_FSR_FG_SNAPSHOT_EVERY, SKY_FSR_FG_SNAPSHOT_FROM: gpu/fsr/framegen.cpp).
            if (const char* prefix = skyenv::Get("SKY_FSR_FG_SNAPSHOT"))
                gpu::framegen::SetSnapshotHook([prefix = std::string(prefix)](const char* kind, uint64_t index,
                                                   const std::vector<uint32_t>& rgba, uint32_t w, uint32_t h) {
                    const std::string path = prefix + "_" + kind + "_" + std::to_string(index) + ".png";
                    stbi_write_png(path.c_str(), int(w), int(h), 4, rgba.data(), int(w * 4));
                });
        }
    }

    if (headless)
        fprintf(stderr, "[gpu] not initialized: SKY_HEADLESS requested\n");

    SetFrameRateLimit(settings.fpsLimit);
    glyphs::Init(settings);
    texpack::ConfigureForGame(game.id, settings.textureDump, settings.texturePacks, settings.hdTextures);
    PortalInit();
    gamemenu::Init(settings, window, ARKEYAN_GAME_VERSION);
    // fps_auto_fallback never set (0.9.5 and earlier: on for everyone): the GPU's default, written to the game's video
    // section once, so the launcher's page and the F3 menu show what runs.
    if (!settings.fpsAutoFallbackSet && window != nullptr)
    {
        const char* why = "";
        const bool on = gpu::DropTo30ByDefault(&why);
        fprintf(stderr, "[video] drop to 30 FPS when needed: %s by default (%s)\n", on ? "on" : "off", why);
        gamemenu::SetFpsAutoFallback(on);
    }
    ToyboxInit(window);
    ToyboxSetAutoOpen(settings.toyboxAutoOpen);
    // While the game runs, Skylauncher refuses to restore a backup over its saves (Settings > Game data,
    // save_backup.h). The marker holds this process id; a crash leaves it behind and the launcher sees it is gone.
    savebackup::SetGameRunning(true);
    progress::BeginSession(game.id); // play time: written every minute and at exit (collection/progress.h)
    auto mainThread = CreateGuestThread(g_module.entryPoint, 0, 0, g_module.stackSize, false, 0);

    HostInput input = MakeHostInput(settings);
    mouseinput::Configure(settings, ::game::Current().mouseDefaults);
    auto wanted = [&](SDL_JoystickID id) { return settings.gamepad.empty() || GamepadGuid(id) == settings.gamepad; };
    SDL_Gamepad* gamepad = nullptr;
    SDL_Gamepad* secondGamepad = nullptr; // Player 2's: another pad than the first one, no keyboard
    HostInput secondInput = input;
    secondInput.keyboard = false;
    gpu::framestats::RegisterThread(gpu::framestats::Thread::Main, PlatformThreadCpuClock());
    while (true)
    {
        {
            std::lock_guard lock(g_kernelLock);
            if (mainThread->exited)
                break;
        }
        progress::TickSession();

        if (headless)
        {
            SDL_Delay(16);
            continue;
        }

        glyphs::RunTestScript();
        HostAudioCheckOutput(nullptr);
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            mouseinput::OnEvent(event, window); // first: a quit frees the pointer before anything else
            HostAudioCheckOutput(&event);
            if (gpu::framestats::Enabled() &&
                (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP ||
                    event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                    event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION))
                gpu::framestats::NoteInputEvent(event.common.timestamp); // the debug screen's pad-to-game latency
            // Closing the window, Cmd+Q, Ctrl+C or SIGTERM, or the window disappearing. The title's threads are still
            // running and use SDL (the audio driver thread feeds the audio stream, the renderer presents to the
            // window), so tearing SDL down first (SDL_Quit) would free objects under them; the system reclaims
            // everything at exit.
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (event.type == SDL_EVENT_WINDOW_HIDDEN && IsWindowGone()))
            {
                savebackup::SetGameRunning(false);
                progress::EndSession();
                std::_Exit(0);
            }
            // The chosen gamepad (--gamepad) replaces another one in use when it shows up.
            if (event.type == SDL_EVENT_GAMEPAD_ADDED && input.gamepad &&
                (gamepad == nullptr || (!wanted(SDL_GetGamepadID(gamepad)) && wanted(event.gdevice.which))))
            {
                if (gamepad != nullptr)
                    SDL_CloseGamepad(gamepad);
                gamepad = SDL_OpenGamepad(event.gdevice.which);
                fprintf(stderr, "[input] gamepad %s (%s)\n", SDL_GetGamepadName(gamepad),
                    GamepadGuid(event.gdevice.which).c_str());
            }
            else if (event.type == SDL_EVENT_GAMEPAD_ADDED && input.gamepad && gamepad != nullptr &&
                     secondGamepad == nullptr && SDL_GetGamepadID(gamepad) != event.gdevice.which)
            {
                secondGamepad = SDL_OpenGamepad(event.gdevice.which);
                fprintf(stderr, "[input] second gamepad (Player 2) %s (%s)\n", SDL_GetGamepadName(secondGamepad),
                    GamepadGuid(event.gdevice.which).c_str());
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED && secondGamepad != nullptr &&
                SDL_GetGamepadID(secondGamepad) == event.gdevice.which)
            {
                SDL_CloseGamepad(secondGamepad);
                secondGamepad = nullptr;
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad != nullptr &&
                SDL_GetGamepadID(gamepad) == event.gdevice.which)
            {
                SDL_CloseGamepad(gamepad);
                gamepad = std::exchange(secondGamepad, nullptr); // Player 2's pad becomes Player 1's
            }
            ToyboxHandleEvent(event);
            glyphs::OnEvent(event, gamepad);
        }
        // The F3 menu's Commands page changed a key or a button: in use from this frame.
        if (commands::Take(settings))
        {
            input = MakeHostInput(settings);
            secondInput = input;
            secondInput.keyboard = false;
            mouseinput::Configure(settings, ::game::Current().mouseDefaults);
        }
        input.raw = ToyboxWaitsForPadButton();
        // The title gets no input while the Toy Box or the wheel is open; View may become the wheel.
        mouseinput::Frame(window, ToyboxOverlayOpen(), ::game::InMenuLevel());
        GamepadState state = ReadHostGamepad(input, gamepad, mouseinput::HeldActions());
        ToyboxFilterGamepad(state);
        SetHostGamepad(state);
        SetHostGamepad(ReadHostGamepad(secondInput, secondGamepad), 1, secondGamepad != nullptr);
        SendRumble(0, gamepad, input.rumble[0]);
        SendRumble(1, secondGamepad, input.rumble[1]);
        ToyboxUpdate();
        PresentDisplayFrame(); // paced by vsync
    }

    // Not exit(): the title's other threads are still running, and atexit handlers and static destructors would run
    // under them (and could wait on them forever).
    fprintf(stderr, "Main thread exited with 0x%08X\n", mainThread->exitCode);
    savebackup::SetGameRunning(false);
    progress::EndSession();
    std::_Exit(0);
}
