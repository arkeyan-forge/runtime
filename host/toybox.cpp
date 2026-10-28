#include "host/toybox.h"
#include "gpu/gpu.h"
#include "host/art/game_art.h"
#include "host/gamemenu.h"
#include "host/glyphs.h"
#include "host/portal.h"
#include "game.h"
#include "host/portal_prompt.h"
#include "host/toybox/draw.h"
#include "host/toybox/model.h"
#include "vfs.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
#include "env.h"

// The Toy Box's input and frame (docs/runtime/host/toybox_ux.md): the logic is host/toybox/model.*, the look host/toybox/draw.*.
// Gamepad: hold View 0.25 s for the quick swap wheel (a tap still reaches the game, sent on release), View + Y for the
// Toy Box. Keyboard: hold Tab for the wheel, tap Tab or F6 for the Toy Box.
using toybox::Command;
using toybox::Screen;

namespace
{
    constexpr uint64_t HoldForWheel = 250'000'000;  // View or Tab held this long opens the wheel
    constexpr uint64_t TapLength = 120'000'000;     // a tap of View given back to the game
    constexpr uint64_t RepeatDelay = 400'000'000, RepeatPeriod = 80'000'000;
    constexpr int StickAim = 16000;                 // stick tilt that aims the wheel

    SDL_Window* g_window;
    bool g_enabled;
    // ToyboxNotice (any thread) waits here for ToyboxUpdate (main thread).
    std::mutex g_noticeMutex;
    std::pair<std::string, std::string> g_notice;
    bool g_noticePending;
    gpu::Texture* g_texture;
    std::vector<uint32_t> g_pixels;
    bool g_overlayShown;
    bool g_wasOpen;
    bool g_portraitsLoaded;

    // Gamepad.
    uint16_t g_lastButtons;
    uint16_t g_heldDirection;
    uint64_t g_nextRepeat;
    uint64_t g_backDown;       // View pressed at (0: not held, or already used)
    bool g_backUsed;           // the press opened something: no tap for the game
    uint64_t g_tapUntil;       // the game sees View until then
    bool g_wheelByPad;         // the wheel closes when View is released
    bool g_swallow;            // the game gets nothing until every button is released

    // Keyboard.
    uint64_t g_tabDown;
    bool g_wheelByTab;
    bool g_enterHeld;
    bool g_pinKey;
    bool g_mouseHeld;          // a hold button pressed with the mouse             // F2 held on the wheel: pin

    // SKY_TOYBOX_SCRIPT.
    struct ScriptStep
    {
        uint64_t at;
        std::string action;
    };
    std::vector<ScriptStep> g_script;
    size_t g_scriptNext;
    struct PadPress
    {
        uint64_t from, to;
        uint16_t buttons;
        int16_t lx, ly;
    };
    std::vector<PadPress> g_padPresses;

    toybox::Model& M() { return toybox::Get(); }

    // The game's "place a Skylander" prompt (host/portal_prompt.h; the adapter reads it: game.h).
    bool g_autoOpen = true;
    portalprompt::Tracker g_promptTracker;
    bool g_openedByPrompt;     // the Toy Box on screen is the one the prompt opened

    void LoadPortraits()
    {
        if (g_portraitsLoaded)
            return;
        g_portraitsLoaded = true;
        // This game's portraits from the player's disc, cached in its data folder (docs/runtime/host/game_art.md).
        art::Extraction found = art::Extract(SSA_GAME_ID, art::ReadFrom(g_disc), {}, art::Portraits);
        fprintf(stderr, "[toybox] %d portraits in %s\n", found.portraits, art::PortraitFolder(SSA_GAME_ID).string().c_str());
    }

    void OpenToybox(Screen screen = Screen::Home, int seat = 0)
    {
        LoadPortraits();
        M().Open(screen, seat);
    }

    // The wheel's place from a direction (x right, y up), -1 when centred.
    int PlaceOf(float x, float y)
    {
        if (std::sqrt(x * x + y * y) < 0.5f)
            return -1;
        float angle = std::atan2(x, y); // 0 up, clockwise
        int place = int(std::lround(angle / (6.2831853f / toybox::Model::WheelPlaces)));
        return (place % toybox::Model::WheelPlaces + toybox::Model::WheelPlaces) % toybox::Model::WheelPlaces;
    }

    // --- Keyboard and mouse ---------------------------------------------------------------------------------------

    bool CanvasPoint(float windowX, float windowY, float& x, float& y)
    {
        int width = 0, height = 0;
        if (g_window == nullptr || !SDL_GetWindowSize(g_window, &width, &height) || width <= 0 || height <= 0)
            return false;
        float scale = std::min(float(width) / toybox::CanvasWidth, float(height) / toybox::CanvasHeight);
        x = (windowX - (width - toybox::CanvasWidth * scale) / 2) / scale;
        y = (windowY - (height - toybox::CanvasHeight * scale) / 2) / scale;
        return x >= 0 && y >= 0 && x < toybox::CanvasWidth && y < toybox::CanvasHeight;
    }

    void AimFromArrows()
    {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        float x = float(keys[SDL_SCANCODE_RIGHT]) - float(keys[SDL_SCANCODE_LEFT]);
        float y = float(keys[SDL_SCANCODE_UP]) - float(keys[SDL_SCANCODE_DOWN]);
        if (x != 0 || y != 0)
            M().AimWheel(PlaceOf(x, y));
    }

    void HandleKey(const SDL_KeyboardEvent& key, bool down)
    {
        toybox::Model& model = M();
        uint64_t now = SDL_GetTicksNS();
        if (!down)
        {
            if (key.key == SDLK_F2)
                g_pinKey = false;
            if (key.key == SDLK_TAB && g_tabDown != 0)
            {
                g_tabDown = 0;
                if (g_wheelByTab && model.screen == Screen::Wheel)
                {
                    g_wheelByTab = false;
                    model.ConfirmWheel();
                }
                else if (!g_wheelByTab && !model.open)
                    OpenToybox();
                g_wheelByTab = false;
            }
            if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER)
            {
                g_enterHeld = false;
                model.Hold(false, now);
            }
            return;
        }
        if (key.repeat && (key.key == SDLK_TAB || key.key == SDLK_F6 || key.key == SDLK_F3))
            return;
        if (!model.open)
        {
            if (key.key == SDLK_F3)
                model.OpenMenu(); // the F3 menu (docs/runtime/host/gamemenu.md)
            else if (key.key == SDLK_F6)
                OpenToybox();
            else if (key.key == SDLK_TAB)
                g_tabDown = now;
            return;
        }
        if (model.screen == Screen::Wheel)
        {
            if (key.key >= SDLK_1 && key.key <= SDLK_8)
                model.AimWheel(int(key.key - SDLK_1));
            else if (key.key == SDLK_UP || key.key == SDLK_DOWN || key.key == SDLK_LEFT || key.key == SDLK_RIGHT)
                AimFromArrows();
            else if (key.key == SDLK_RETURN || key.key == SDLK_KP_ENTER)
                model.ConfirmWheel();
            else if (key.key == SDLK_ESCAPE)
                model.Run(Command::Back);
            else if (key.key == SDLK_F3)
                model.WheelList();
            else if (key.key == SDLK_PAGEUP || key.key == SDLK_PAGEDOWN)
                model.WheelSeat(key.key == SDLK_PAGEDOWN ? 1 : -1);
            else if (key.key == SDLK_F2)
                g_pinKey = true;
            return;
        }
        if ((key.key == SDLK_F6 || key.key == SDLK_TAB) && model.screen != Screen::Name)
        {
            model.Close();
            return;
        }
        if (key.key == SDLK_F3 && model.screen == Screen::Menu) // F3 opened it: F3 closes it (elsewhere F3 is the Toy Box's View)
        {
            model.Close();
            return;
        }
        bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
        Command command = Command::None;
        switch (key.key)
        {
        case SDLK_UP: command = Command::Up; break;
        case SDLK_DOWN: command = Command::Down; break;
        case SDLK_LEFT: command = shift && model.screen == Screen::Stats ? Command::PreviousSection : Command::Left; break;
        case SDLK_RIGHT: command = shift && model.screen == Screen::Stats ? Command::NextSection : Command::Right; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            command = model.screen == Screen::Name && model.dialogs.empty() ? Command::Start : Command::Accept;
            g_enterHeld = true;
            break;
        case SDLK_SPACE:
            if (model.search.empty() && model.screen != Screen::Name)
                command = Command::Accept;
            break;
        case SDLK_ESCAPE: command = Command::Back; break;
        case SDLK_BACKSPACE:
            if (model.screen == Screen::Name || !model.search.empty())
                model.Backspace();
            else
                command = Command::Back;
            break;
        case SDLK_F2:
        case SDLK_APPLICATION: command = Command::Options; break;
        case SDLK_F3: command = Command::View; break;
        case SDLK_F4: command = model.screen == Screen::Home ? Command::Start : Command::None; break;
        case SDLK_PAGEUP: command = Command::PreviousSection; break;
        case SDLK_PAGEDOWN: command = Command::NextSection; break;
        case SDLK_DELETE: command = Command::Delete; break;
        default: break;
        }
        model.Run(command);
        if (command == Command::Accept)
            model.Hold(true, now);
    }

    // --- Script ----------------------------------------------------------------------------------------------------

    // Screenshot of what was last presented (offscreen output only): <SKY_TOYBOX_SHOTS>_<n>.png, or _<name>.png.
    void SaveShot(const std::string& name)
    {
        static int count;
        const char* prefix = skyenv::Get("SKY_TOYBOX_SHOTS");
        std::vector<uint32_t> rgba;
        uint32_t width, height;
        if (!gpu::ReadbackOutput(rgba, width, height))
        {
            fprintf(stderr, "[toybox] no screenshot: the output cannot be read back (not offscreen)\n");
            return;
        }
        std::string path = std::string(prefix != nullptr ? prefix : "toybox") + "_" + (name.empty() ? std::to_string(++count) : name) + ".png";
        if (SDL_Surface* image = SDL_CreateSurfaceFrom(int(width), int(height), SDL_PIXELFORMAT_RGBA32, rgba.data(), int(width * 4)))
        {
            fprintf(stderr, "[toybox] screenshot %s: %s\n", path.c_str(), SDL_SavePNG(image, path.c_str()) ? "saved" : SDL_GetError());
            SDL_DestroySurface(image);
        }
    }

    uint16_t PadButton(const std::string& name)
    {
        static const std::pair<const char*, uint16_t> Names[] = { { "a", Button::A }, { "b", Button::B }, { "x", Button::X },
            { "y", Button::Y }, { "back", Button::Back }, { "start", Button::Start }, { "lb", Button::LeftShoulder },
            { "rb", Button::RightShoulder }, { "up", Button::DpadUp }, { "down", Button::DpadDown }, { "left", Button::DpadLeft },
            { "right", Button::DpadRight } };
        for (const auto& [text, button] : Names)
        {
            if (name == text)
                return button;
        }
        return 0;
    }

    // SKY_TOYBOX_SCRIPT's "prompt=1" / "prompt=2d": the game asks that player for a Skylander for 3 s (d: and says its Skylander
    // died), as if ReadPortalPrompt said it, and "level": a level starts (game::LevelStarted): the out of health test.
    portalprompt::Prompt g_testPrompt;
    uint64_t g_testPromptUntil = 0;
    int g_testLevels = 0;

    // SKY_TOYBOX_SCRIPT="F6:100,Right:101,type=Ace:103,shot:104,pad=back*0.8:110": items at times in seconds since
    // launch. A key name (SDL_GetKeyFromName; "shift+Right") is pressed and released; "down=Tab" / "up=Tab" press or
    // release only; "type=<text>" types; "shot" or "shot=<name>" saves a screenshot; "pad=back+lright*0.8" holds pad
    // buttons (a b x y back start lb rb up down left right, lup ldown lleft lright for the stick) for 0.8 s (0.15 s
    // without "*"), as the Toy Box sees the pad; "click=x/y" and "rclick=x/y" click at canvas pixels.
    void LoadScript()
    {
        const char* script = skyenv::Get("SKY_TOYBOX_SCRIPT");
        if (script == nullptr)
            return;
        std::string text = script;
        for (size_t begin = 0; begin < text.size();)
        {
            size_t end = std::min(text.find(',', begin), text.size());
            std::string item = text.substr(begin, end - begin);
            size_t colon = item.rfind(':');
            if (colon != std::string::npos)
                g_script.push_back({ uint64_t(atof(item.c_str() + colon + 1) * 1e9), item.substr(0, colon) });
            begin = end + 1;
        }
        std::stable_sort(g_script.begin(), g_script.end(), [](const ScriptStep& a, const ScriptStep& b) { return a.at < b.at; });
    }

    void SendKey(const std::string& name, bool down, SDL_Keymod mod = SDL_KMOD_NONE)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.key = SDL_GetKeyFromName(name.c_str());
        event.key.mod = mod;
        event.key.down = down;
        ToyboxHandleEvent(event);
    }

    void RunScript()
    {
        uint64_t now = SDL_GetTicksNS();
        while (g_scriptNext < g_script.size() && g_script[g_scriptNext].at <= now)
        {
            std::string action = g_script[g_scriptNext++].action;
            fprintf(stderr, "[toybox] script: %s\n", action.c_str());
            if (action == "shot" || action.rfind("shot=", 0) == 0)
            {
                SaveShot(action.size() > 5 ? action.substr(5) : std::string());
                continue;
            }
            if (action.rfind("prompt=", 0) == 0)
            {
                g_testPrompt = { portalprompt::Kind::Player, atoi(action.c_str() + 7), action.back() == 'd' };
                g_testPromptUntil = now + 3000000000ull;
                continue;
            }
            if (action == "level")
            {
                game::LevelStarted("script level " + std::to_string(++g_testLevels));
                continue;
            }
            if (action.rfind("type=", 0) == 0)
            {
                SDL_Event event{};
                event.type = SDL_EVENT_TEXT_INPUT;
                event.text.text = action.c_str() + 5;
                ToyboxHandleEvent(event);
                continue;
            }
            if (action.rfind("pad=", 0) == 0)
            {
                std::string spec = action.substr(4);
                size_t star = spec.find('*');
                double hold = star != std::string::npos ? atof(spec.c_str() + star + 1) : 0.15;
                spec = spec.substr(0, star);
                PadPress press{ now, now + uint64_t(hold * 1e9), 0, 0, 0 };
                for (size_t b = 0; b <= spec.size();)
                {
                    size_t plus = std::min(spec.find('+', b), spec.size());
                    std::string name = spec.substr(b, plus - b);
                    press.buttons |= PadButton(name);
                    press.lx = name == "lleft" ? -32767 : name == "lright" ? 32767 : press.lx;
                    press.ly = name == "ldown" ? -32767 : name == "lup" ? 32767 : press.ly;
                    b = plus + 1;
                }
                g_padPresses.push_back(press);
                continue;
            }
            if (action.rfind("click=", 0) == 0 || action.rfind("rclick=", 0) == 0)
            {
                bool right = action[0] == 'r';
                std::string at = action.substr(right ? 7 : 6);
                float x = float(atof(at.c_str())), y = float(atof(at.c_str() + at.find('/') + 1));
                if (M().open)
                    toybox::Click(M(), x, y, right ? 3 : 1, false);
                continue;
            }
            if (action.rfind("down=", 0) == 0 || action.rfind("up=", 0) == 0)
            {
                bool down = action[0] == 'd';
                SendKey(action.substr(down ? 5 : 3), down);
                continue;
            }
            SDL_Keymod mod = SDL_KMOD_NONE;
            if (action.rfind("shift+", 0) == 0)
            {
                mod = SDL_KMOD_LSHIFT;
                action = action.substr(6);
            }
            SendKey(action, true, mod);
            SendKey(action, false, mod);
        }
    }

    void Present()
    {
        toybox::Model& model = M();
        bool visible = model.open || !model.toast.empty() || !model.fps.empty();
        if (!visible)
        {
            if (g_overlayShown)
            {
                gpu::SetOverlay(nullptr);
                g_overlayShown = false;
            }
            model.Clean();
            return;
        }
        if (!model.Dirty() && g_overlayShown)
            return;
        toybox::Draw(model, g_pixels);
        model.Clean();
        if (g_texture == nullptr)
            g_texture = gpu::CreateTexture({ .width = toybox::CanvasWidth, .height = toybox::CanvasHeight, .format = gpu::Format::R8G8B8A8_UNORM });
        gpu::UploadTexture(g_texture, g_pixels.data(), uint32_t(toybox::CanvasWidth * 4));
        gpu::SetOverlay(g_texture);
        g_overlayShown = true;
    }
}

void ToyboxInit(SDL_Window* window)
{
    if (window == nullptr || !PortalIsConnected() && !PortalIsEmulated())
    {
        // A real portal may still be plugged in later: the Toy Box follows it when it is.
        if (window == nullptr)
            return;
    }
    g_window = window;
    if (!toybox::DrawInit())
    {
        fprintf(stderr, "[toybox] unavailable: the fonts did not load\n");
        return;
    }
    // SKY_TOYBOX_REAL_VIEW=1 (test aid): the real portal's read-only seats, from the emulated portal's traffic.
    const char* realView = skyenv::Get("SKY_TOYBOX_REAL_VIEW");
    M().Init(!PortalIsEmulated() || (realView != nullptr && *realView != '\0'));
    g_enabled = true;
    LoadScript();
    fprintf(stderr, "[toybox] hold Tab or View: quick swap wheel; F6, tap Tab or View + Y: the Toy Box; F3 or View + Start: the game menu\n");
}

bool ToyboxHandleEvent(const SDL_Event& event)
{
    if (!g_enabled)
        return false;
    toybox::Model& model = M();
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    {
        bool toggle = event.key.key == SDLK_TAB || event.key.key == SDLK_F6 || event.key.key == SDLK_F3;
        bool wasOpen = model.open;
        HandleKey(event.key, event.type == SDL_EVENT_KEY_DOWN);
        return wasOpen || model.open || toggle;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (model.open && model.screen != Screen::Wheel)
            model.Type(event.text.text);
        return model.open;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        float x, y;
        if (model.open && CanvasPoint(event.button.x, event.button.y, x, y))
        {
            toybox::Click(model, x, y, event.button.button == SDL_BUTTON_RIGHT ? 3 : 1, event.button.clicks >= 2);
            g_mouseHeld = true;
            model.Hold(true, SDL_GetTicksNS());
        }
        return model.open;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        g_mouseHeld = false;
        model.Hold(false, SDL_GetTicksNS());
        return model.open;
    case SDL_EVENT_MOUSE_MOTION:
    {
        float x, y;
        if (model.open && model.screen == Screen::Wheel && CanvasPoint(event.motion.x, event.motion.y, x, y))
        {
            int place = toybox::WheelPlaceAt(x, y);
            if (place >= -1)
                model.AimWheel(place);
        }
        return model.open;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (model.open && model.screen == Screen::Stats)
            model.Run(event.wheel.y > 0 ? Command::Right : Command::Left);
        else if (model.open && (model.screen == Screen::Home || model.screen == Screen::Add || model.screen == Screen::WheelList))
            model.ScrollList(event.wheel.y > 0 ? -1 : 1);
        return model.open;
    default:
        return false;
    }
}

void ToyboxFilterGamepad(GamepadState& state)
{
    if (!g_enabled)
        return;
    toybox::Model& model = M();
    uint64_t now = SDL_GetTicksNS();
    for (const PadPress& press : g_padPresses)
    {
        if (now >= press.from && now < press.to)
        {
            state.buttons |= press.buttons;
            if (press.lx != 0)
                state.thumbLX = press.lx;
            if (press.ly != 0)
                state.thumbLY = press.ly;
        }
    }
    std::erase_if(g_padPresses, [&](const PadPress& press) { return now >= press.to + 1'000'000'000; });

    uint16_t buttons = state.buttons;
    uint16_t pressed = buttons & ~g_lastButtons, released = g_lastButtons & ~buttons;
    g_lastButtons = buttons;

    // --- The game is playing: View held or tapped -----------------------------------------------------------------
    if (!model.open)
    {
        if (pressed & Button::Back)
        {
            g_backDown = now;
            g_backUsed = false;
        }
        if ((buttons & Button::Back) && g_backDown != 0)
        {
            if (pressed & Button::Y)
            {
                g_backDown = 0;
                g_backUsed = true;
                OpenToybox();
            }
            else if (pressed & Button::Start) // View + Start: the F3 menu
            {
                g_backDown = 0;
                g_backUsed = true;
                model.OpenMenu();
            }
            else if (now - g_backDown >= HoldForWheel)
            {
                g_backDown = 0;
                g_backUsed = true;
                g_wheelByPad = true;
                model.OpenWheel(0);
            }
        }
        if ((released & Button::Back) && !g_backUsed)
            g_tapUntil = now + TapLength;
        if (released & Button::Back)
            g_backDown = 0;
        if (model.open)
        {
            state = {};
            return;
        }
        // View is the wheel's until released or given back as a tap; Y too while View is down.
        state.buttons &= uint16_t(~Button::Back);
        if (buttons & Button::Back)
            state.buttons &= uint16_t(~(Button::Y | Button::Start));
        if (now < g_tapUntil)
            state.buttons |= Button::Back;
        // Buttons that closed a menu stay away from the game until released (sticks and triggers past a dead zone:
        // a stick at rest is rarely exactly centred).
        constexpr int StickDeadZone = 8000, TriggerDeadZone = 30;
        bool held = buttons != 0 || std::abs(state.thumbRX) > StickDeadZone || std::abs(state.thumbRY) > StickDeadZone
            || state.leftTrigger > TriggerDeadZone || state.rightTrigger > TriggerDeadZone;
        g_swallow = g_swallow && held;
        if (g_swallow)
            state = {};
        return;
    }

    // --- The wheel -------------------------------------------------------------------------------------------------
    if (model.screen == Screen::Wheel)
    {
        float x = state.thumbLX / 32767.0f, y = state.thumbLY / 32767.0f;
        if (std::abs(state.thumbLX) > StickAim || std::abs(state.thumbLY) > StickAim)
            model.AimWheel(PlaceOf(x, y));
        uint16_t dpad = buttons & (Button::DpadUp | Button::DpadDown | Button::DpadLeft | Button::DpadRight);
        if (dpad != 0)
            model.AimWheel(PlaceOf(float((dpad & Button::DpadRight) != 0) - float((dpad & Button::DpadLeft) != 0),
                float((dpad & Button::DpadUp) != 0) - float((dpad & Button::DpadDown) != 0)));
        model.PinFromWheel(now, (buttons & Button::X) != 0 || g_pinKey);
        if ((released & Button::Back) && g_wheelByPad)
        {
            g_wheelByPad = false;
            model.ConfirmWheel();
        }
        else if (pressed & Button::A)
            model.ConfirmWheel();
        else if (pressed & Button::B)
            model.Run(Command::Back);
        else if (pressed & Button::Y)
            model.WheelList();
        else if (pressed & Button::LeftShoulder)
            model.WheelSeat(-1);
        else if (pressed & Button::RightShoulder)
            model.WheelSeat(1);
        if (!model.open)
            g_swallow = true;
        state = {};
        return;
    }
    if (released & Button::Back)
        g_wheelByPad = false;

    // --- The Toy Box ---------------------------------------------------------------------------------------------------
    if ((buttons & Button::Back) && ((pressed & Button::Y) || (model.screen == Screen::Menu && (pressed & Button::Start))))
    {
        model.Close();
        g_swallow = true;
        state = {};
        return;
    }
    constexpr int Threshold = 20000;
    uint16_t moves = buttons;
    if (state.thumbLY > Threshold) moves |= Button::DpadUp;
    if (state.thumbLY < -Threshold) moves |= Button::DpadDown;
    if (state.thumbLX < -Threshold) moves |= Button::DpadLeft;
    if (state.thumbLX > Threshold) moves |= Button::DpadRight;
    uint16_t directions = moves & (Button::DpadUp | Button::DpadDown | Button::DpadLeft | Button::DpadRight);
    uint16_t fire = 0;
    if (directions != g_heldDirection)
    {
        fire = directions & ~g_heldDirection;
        g_heldDirection = directions;
        g_nextRepeat = now + RepeatDelay;
    }
    else if (directions != 0 && now >= g_nextRepeat)
    {
        fire = directions;
        g_nextRepeat = now + RepeatPeriod;
    }
    if (fire & Button::DpadUp) model.Run(Command::Up);
    if (fire & Button::DpadDown) model.Run(Command::Down);
    if (fire & Button::DpadLeft) model.Run(Command::Left);
    if (fire & Button::DpadRight) model.Run(Command::Right);
    if (pressed & Button::LeftShoulder) model.Run(Command::PreviousSection);
    if (pressed & Button::RightShoulder) model.Run(Command::NextSection);
    if (pressed & Button::A) model.Run(Command::Accept);
    if (model.open && (pressed & Button::X)) model.Run(Command::Options);
    if (model.open && (pressed & Button::Y) && !(buttons & Button::Back)) model.Run(Command::View);
    if (model.open && (pressed & Button::Start)) model.Run(Command::Start);
    if (model.open && (pressed & Button::B)) model.Run(Command::Back);
    if (!g_enterHeld && !g_mouseHeld)
        model.Hold((buttons & Button::A) != 0, now);
    if (!model.open)
        g_swallow = true;
    state = {};
}

void ToyboxSetAutoOpen(bool enabled)
{
    g_autoOpen = enabled;
}

// Opens the Toy Box on the seat the game asks for, and closes it once the prompt is gone. A figure the game gave up on
// (it asks for the seat again while that figure, which it had taken, is still on it, or says the Skylander died) is out of
// health until the next level: the Toy Box marks it, whether or not it opens by itself.
static void FollowPrompt(toybox::Model& model)
{
    if (model.realPortal)
        return;
    portalprompt::Prompt prompt = game::Current().ReadPortalPrompt != nullptr ? game::Current().ReadPortalPrompt() : portalprompt::Prompt{};
    if (g_testPromptUntil > SDL_GetTicksNS())
        prompt = g_testPrompt;
    portalprompt::Prompt appeared;
    switch (g_promptTracker.Update(prompt, appeared))
    {
    case portalprompt::Tracker::Event::Appeared:
    {
        std::string id = "p" + std::to_string(appeared.player);
        for (size_t i = 0; i < model.seats.size(); i++)
        {
            if (model.seats[i].seat.id != id)
                continue;
            if (model.MarkOutOfHealth(int(i), appeared.defeated))
                fprintf(stderr, "[toybox] player %d's figure is out of health (%s)\n", appeared.player,
                    appeared.defeated ? "the game says its Skylander died" : "the game asks for the seat again while it is on it");
            if (!g_autoOpen)
                break;
            fprintf(stderr, "[toybox] prompt for player %d\n", appeared.player);
            // Left alone when the player has it open already.
            if (!model.open)
            {
                OpenToybox(Screen::Home, int(i));
                g_openedByPrompt = true;
            }
            break;
        }
        break;
    }
    case portalprompt::Tracker::Event::Gone:
        if (!g_autoOpen)
            break;
        fprintf(stderr, "[toybox] the prompt is gone\n");
        if (g_openedByPrompt && model.open && model.screen == Screen::Home && model.dialogs.empty())
            model.Close();
        g_openedByPrompt = false;
        break;
    case portalprompt::Tracker::Event::None:
        break;
    }
    if (!model.open)
        g_openedByPrompt = false;
}

static std::atomic<bool> g_levelStarted{ false };

void ToyboxLevelStarted()
{
    g_levelStarted = true;
}

void ToyboxNotice(const char* english, const char* french)
{
    std::lock_guard lock(g_noticeMutex);
    g_notice = { english, french };
    g_noticePending = true;
}

void ToyboxUpdate()
{
    if (!g_enabled)
        return;
    RunScript();
    toybox::Model& model = M();
    {
        std::lock_guard lock(g_noticeMutex);
        if (g_noticePending)
        {
            g_noticePending = false;
            model.Notice(toybox::French() ? g_notice.second : g_notice.first, 4);
        }
    }
    if (g_levelStarted.exchange(false))
        model.ClearOutOfHealth();
    FollowPrompt(model);
    if (std::string fps = gamemenu::FpsText(); fps != model.fps)
    {
        model.fps = fps;
        model.Touch();
    }
    uint64_t now = SDL_GetTicksNS();
    // Tab held: the wheel.
    if (g_tabDown != 0 && !model.open && now - g_tabDown >= HoldForWheel)
    {
        g_wheelByTab = true;
        model.OpenWheel(0);
        AimFromArrows();
    }
    if (model.open != g_wasOpen)
    {
        g_wasOpen = model.open;
        if (model.open && model.screen != Screen::Wheel)
            SDL_StartTextInput(g_window);
        else
            SDL_StopTextInput(g_window);
    }
    // The key caps follow the glyph style (the last device used).
    static glyphs::Style shownStyle = glyphs::Current();
    if (glyphs::Current() != shownStyle)
    {
        shownStyle = glyphs::Current();
        model.Touch();
    }
    model.Tick(now);
    Present();
}
