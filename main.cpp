#include "host/audio.h"
#include "host/collection/datadir.h"
#include "host/collection/save_backup.h"
#include "host/display.h"
#include "host/glyphs.h"
#include "host/input.h"
#include "host/launcher.h"
#include "host/log.h"
#include "host/portal.h"
#include "host/settings.h"
#include "host/xex_title.h"
#include "host/texture_pack.h"
#include "host/toybox.h"
#include "gpu/gpu.h"
#include "d3d/ultrawide.h"
#include "kernel/video.h"
#include "game.h"
#include "language.h"
#include "loader.h"
#include "memory.h"
#include "platform/platform.h"
#include "thread.h"
#include "vfs.h"
#include <SDL3/SDL.h>
#include <stb_image.h>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#include "host/env.h"
#endif

// The window went away without a close request (SDL_EVENT_WINDOW_HIDDEN: on macOS an NSWindow closed or ordered out
// without windowShouldClose); SDL sends no SDL_EVENT_QUIT then, and the game ran on with no window. Cmd+H hides the
// window too, so a hidden application is not that case. Minimizing sends SDL_EVENT_WINDOW_MINIMIZED instead.
static bool IsWindowGone()
{
#ifdef __APPLE__
    id application = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(objc_getClass("NSApplication"), sel_registerName("sharedApplication"));
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
}

// Keyboard bindings (settings, [input] key_<action>; Escape is also back) and the gamepad, per the input device setting.
struct HostInput
{
    bool keyboard = true, gamepad = true;
    SDL_Scancode keys[std::size(g_keyActions)];
};

static HostInput MakeHostInput(const RuntimeSettings& settings)
{
    HostInput input;
    input.keyboard = settings.inputDevice != "gamepad";
    input.gamepad = settings.inputDevice != "keyboard";
    for (size_t i = 0; i < std::size(g_keyActions); i++)
        input.keys[i] = SDL_GetScancodeFromName(settings.keys[i].c_str());
    return input;
}

static GamepadState ReadHostGamepad(const HostInput& input, SDL_Gamepad* gamepad)
{
    GamepadState state;
    if (input.keyboard)
    {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        auto held = [&](size_t action) { return keys[input.keys[action]]; };
        // g_keyActions order: a b x y start back lb rb lt rt, left stick, right stick, d-pad.
        static const uint16_t buttons[] = { Button::A, Button::B, Button::X, Button::Y, Button::Start, Button::Back,
            Button::LeftShoulder, Button::RightShoulder };
        for (size_t i = 0; i < std::size(buttons); i++)
            if (held(i)) state.buttons |= buttons[i];
        static const uint16_t dpad[] = { Button::DpadUp, Button::DpadDown, Button::DpadLeft, Button::DpadRight };
        for (size_t i = 0; i < std::size(dpad); i++)
            if (held(18 + i)) state.buttons |= dpad[i];
        if (keys[SDL_SCANCODE_ESCAPE]) state.buttons |= Button::Back;
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
            { SDL_GAMEPAD_BUTTON_DPAD_UP, Button::DpadUp }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, Button::DpadDown },
            { SDL_GAMEPAD_BUTTON_DPAD_LEFT, Button::DpadLeft }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Button::DpadRight },
            { SDL_GAMEPAD_BUTTON_START, Button::Start }, { SDL_GAMEPAD_BUTTON_BACK, Button::Back },
            { SDL_GAMEPAD_BUTTON_LEFT_STICK, Button::LeftThumb }, { SDL_GAMEPAD_BUTTON_RIGHT_STICK, Button::RightThumb },
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, Button::LeftShoulder }, { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, Button::RightShoulder },
            { SDL_GAMEPAD_BUTTON_SOUTH, Button::A }, { SDL_GAMEPAD_BUTTON_EAST, Button::B },
            { SDL_GAMEPAD_BUTTON_WEST, Button::X }, { SDL_GAMEPAD_BUTTON_NORTH, Button::Y },
        };
        for (const auto& [sdlButton, button] : buttons)
        {
            if (SDL_GetGamepadButton(gamepad, sdlButton))
                state.buttons |= button;
        }
        auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(gamepad, a); };
        if (state.thumbLX == 0) state.thumbLX = axis(SDL_GAMEPAD_AXIS_LEFTX);
        if (state.thumbLY == 0) state.thumbLY = int16_t(-std::max(-32767, int(axis(SDL_GAMEPAD_AXIS_LEFTY))));
        if (state.thumbRX == 0) state.thumbRX = axis(SDL_GAMEPAD_AXIS_RIGHTX);
        if (state.thumbRY == 0) state.thumbRY = int16_t(-std::max(-32767, int(axis(SDL_GAMEPAD_AXIS_RIGHTY))));
        state.leftTrigger = std::max(state.leftTrigger, uint8_t(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7));
        state.rightTrigger = std::max(state.rightTrigger, uint8_t(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7));
    }
    return state;
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
    const GameInfo& game = *FindGame(SSA_GAME_ID); // the game this runtime was built for (-DSSA_GAME)
    RuntimeSettings settings;
    std::vector<std::string> warnings;
    std::string error;
    bool parsed = ParseSettings(LoadLauncherConfig(LauncherConfigPath()), game.id, getenv, argc, argv, settings, warnings, error);
    if (parsed && settings.portalProbe)
        return PortalProbeMain();
    if (parsed && !settings.help && !settings.version && !settings.listGamepads)
    {
        LogInit();
        LogHeader(settings);
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
        printf("SSARecomp %s\n", SSA_VERSION);
        return 0;
    }
    if (settings.listGamepads)
    {
        ListGamepads();
        return 0;
    }

    // The launcher's own arguments: the disc image, --select-iso, --launcher.
    std::vector<char*> arguments{ argv[0] };
    for (std::string& argument : settings.arguments)
        arguments.push_back(argument.data());
    std::string discPath = ResolveGameSource(int(arguments.size()), arguments.data(), game, skyenv::Get("SKY_HEADLESS") == nullptr);
    if (skyenv::Get("SKY_LAUNCHER_SHOTS") != nullptr)
        return 0; // screenshot test run: the launcher already quit
    if (discPath.empty())
        return 1;

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
            fprintf(stderr, "%s has no voice language setting yet, ignoring \"%s\"\n", game.name, settings.voiceLanguage.c_str());
        else if (!::game::Current().SetVoiceLanguage(settings.voiceLanguage))
            fprintf(stderr, "unknown voice language \"%s\", using English\n", settings.voiceLanguage.c_str());
    }

    PlatformInit();
    PlatformInstallFaultHandler(OnFault);

    if (!g_disc.Open(discPath.c_str()))
    {
        fprintf(stderr, "%s is not an Xbox 360 game disc image\n", discPath.c_str());
        return 1;
    }

    g_memory.Reserve();
    g_module = LoadXex(g_disc.ReadAll("default.xex"));

    if (const char* script = skyenv::Get("SKY_PRESS"))
        LoadInputScript(script);
    if (const char* script = skyenv::Get("SKY_PRESS2")) // the same for the second controller (Player 2)
        LoadInputScript(script, 1);

    // The host's main thread owns the window and input (macOS requires it); the title runs on its own threads.
    bool headless = skyenv::Get("SKY_HEADLESS") != nullptr;
    // The 3D at the frame's size where the game renders it smaller (Swap Force), unless original_resolution.
    if (::game::Current().fullScene.width != 0 && !settings.originalResolution)
        ultrawide::SetScene16x9(::game::Current().fullScene.width, ::game::Current().fullScene.height, false);
    SDL_Window* window = nullptr;
    if (!headless)
    {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO);
        HostAudioInit();
        gpu::g_settings = { .resolutionScale = settings.resolutionScale, .anisotropy = settings.anisotropy, .vsync = settings.vsync,
            .fpsLimit = settings.fpsLimit };
        window = CreateDisplayWindow("Skylanders Native", 1280, 720, settings.fullscreen); // the product name, for every game
#ifndef __APPLE__ // macOS: the bundle's icon (SSABuild's BundleIcon)
        if (window != nullptr) // the game's own icon from its default.xex; without one, the system's
        {
            std::vector<uint8_t> png = ReadTitleMetadata(g_disc.ReadAll("default.xex")).iconPng;
            int iconWidth = 0, iconHeight = 0, channels = 0;
            if (unsigned char* rgba = stbi_load_from_memory(png.data(), int(png.size()), &iconWidth, &iconHeight, &channels, 4))
            {
                if (SDL_Surface* icon = SDL_CreateSurfaceFrom(iconWidth, iconHeight, SDL_PIXELFORMAT_RGBA32, rgba, iconWidth * 4))
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
        }
    }

    SetFrameRateLimit(settings.fpsLimit);
    glyphs::Init(settings);
    texpack::ConfigureForGame(game.id, settings.textureDump, settings.texturePacks);
    PortalInit();
    ToyboxInit(window);
    ToyboxSetAutoOpen(settings.toyboxAutoOpen);
    // While the game runs, Skylauncher refuses to restore a backup over its saves (Settings > Game data,
    // save_backup.h). The marker holds this process id; a crash leaves it behind and the launcher sees it is gone.
    savebackup::SetGameRunning(true);
    auto mainThread = CreateGuestThread(g_module.entryPoint, 0, 0, g_module.stackSize, false, 0);

    HostInput input = MakeHostInput(settings);
    auto wanted = [&](SDL_JoystickID id) { return settings.gamepad.empty() || GamepadGuid(id) == settings.gamepad; };
    SDL_Gamepad* gamepad = nullptr;
    SDL_Gamepad* secondGamepad = nullptr; // Player 2's: another pad than the first one, no keyboard
    HostInput secondInput = input;
    secondInput.keyboard = false;
    while (true)
    {
        {
            std::lock_guard lock(g_kernelLock);
            if (mainThread->exited)
                break;
        }

        if (headless)
        {
            SDL_Delay(16);
            continue;
        }

        glyphs::RunTestScript();
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            // Closing the window, Cmd+Q, Ctrl+C or SIGTERM, or the window disappearing. The title's threads are still
            // running and use SDL (the audio driver thread feeds the audio stream, the renderer presents to the
            // window), so tearing SDL down first (SDL_Quit) would free objects under them; the system reclaims
            // everything at exit.
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (event.type == SDL_EVENT_WINDOW_HIDDEN && IsWindowGone()))
            {
                savebackup::SetGameRunning(false);
                std::_Exit(0);
            }
            // The chosen gamepad (--gamepad) replaces another one in use when it shows up.
            if (event.type == SDL_EVENT_GAMEPAD_ADDED && input.gamepad
                && (gamepad == nullptr || (!wanted(SDL_GetGamepadID(gamepad)) && wanted(event.gdevice.which))))
            {
                if (gamepad != nullptr)
                    SDL_CloseGamepad(gamepad);
                gamepad = SDL_OpenGamepad(event.gdevice.which);
                fprintf(stderr, "[input] gamepad %s (%s)\n", SDL_GetGamepadName(gamepad), GamepadGuid(event.gdevice.which).c_str());
            }
            else if (event.type == SDL_EVENT_GAMEPAD_ADDED && input.gamepad && gamepad != nullptr && secondGamepad == nullptr
                && SDL_GetGamepadID(gamepad) != event.gdevice.which)
            {
                secondGamepad = SDL_OpenGamepad(event.gdevice.which);
                fprintf(stderr, "[input] second gamepad (Player 2) %s (%s)\n", SDL_GetGamepadName(secondGamepad),
                    GamepadGuid(event.gdevice.which).c_str());
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED && secondGamepad != nullptr && SDL_GetGamepadID(secondGamepad) == event.gdevice.which)
            {
                SDL_CloseGamepad(secondGamepad);
                secondGamepad = nullptr;
            }
            if (event.type == SDL_EVENT_GAMEPAD_REMOVED && gamepad != nullptr && SDL_GetGamepadID(gamepad) == event.gdevice.which)
            {
                SDL_CloseGamepad(gamepad);
                gamepad = std::exchange(secondGamepad, nullptr); // Player 2's pad becomes Player 1's
            }
            ToyboxHandleEvent(event);
            glyphs::OnEvent(event, gamepad);
        }
        // The title gets no input while the Toy Box or the wheel is open; View may become the wheel.
        GamepadState state = ReadHostGamepad(input, gamepad);
        ToyboxFilterGamepad(state);
        SetHostGamepad(state);
        SetHostGamepad(ReadHostGamepad(secondInput, secondGamepad), 1, secondGamepad != nullptr);
        ToyboxUpdate();
        PresentDisplayFrame(); // paced by vsync
    }

    // Not exit(): the title's other threads are still running, and atexit handlers and static destructors would run
    // under them (and could wait on them forever).
    fprintf(stderr, "Main thread exited with 0x%08X\n", mainThread->exitCode);
    savebackup::SetGameRunning(false);
    std::_Exit(0);
}
