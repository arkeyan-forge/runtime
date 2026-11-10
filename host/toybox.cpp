#include "host/toybox.h"
#include "gpu/gpu.h"
#include "d3d/ultrawide.h"
#include "host/art/game_art.h"
#include "host/gamemenu.h"
#include "host/glyphs.h"
#include "host/pad_map.h"
#include "host/portal.h"
#include "game.h"
#include "host/portal_prompt.h"
#include "host/portal_script.h"
#include "host/toybox/draw.h"
#include "host/toybox/in_play.h"
#include "host/toybox/model.h"
#include "host/toybox/keys.h"
#include "host/toybox_remote.h"
#include "vfs.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <utility>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    struct PendingAchievement
    {
        std::string name;
        uint32_t points = 0;
        std::vector<uint8_t> png;
    };
    PendingAchievement g_achievement; // under g_noticeMutex
    bool g_achievementPending;
    void (*g_achievementPreview)();
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

    // Keyboard. The Toy Box's keys (ToyboxSetKeys): the saved key opens it; while the Tab alias is on, Tab does too (tap: the Toy Box,
    // hold: the wheel), else the open key also holds for the wheel.
    toybox::Keys g_keys;
    uint64_t g_tabDown;
    bool g_wheelByTab;
    bool g_enterHeld;
    bool g_pinKey;
    bool g_mouseHeld; // a hold button pressed with the mouse             // F2 held on the wheel: pin

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
    struct KeyHold
    {
        uint64_t from, to;
        SDL_Scancode scancode;
        std::string name;
        bool logged;
    };
    std::vector<KeyHold> g_keyHolds; // "hold=K": a key the game's input reads as held (ToyboxKeyboardState)
    bool g_padArmed; // the Commands page waits for a pad button and every button was released since

    toybox::Model& M() { return toybox::Get(); }

    // The key held for the wheel (a tap opens the Toy Box), and the key that opens the Toy Box at once.
    bool HoldKey(const SDL_KeyboardEvent& key) { return g_keys.Hold(key.scancode); }
    bool OpenKey(const SDL_KeyboardEvent& key) { return g_keys.Open(key.scancode); }

    // The game's "place a Skylander" prompt (host/portal_prompt.h; the adapter reads it: game.h).
    bool g_autoOpen = true;
    portalprompt::Tracker g_promptTracker;
    bool g_openedByPrompt; // the Toy Box on screen is the one the prompt opened

    void LoadPortraits()
    {
        if (g_portraitsLoaded)
            return;
        g_portraitsLoaded = true;
        // This game's portraits (and Imaginators' part names) from the player's disc, cached in its data folder
        // (docs/runtime/host/game_art.md).
        art::Extraction found = art::Extract(ARKEYAN_GAME_ID, art::ReadFrom(g_disc), {}, art::Portraits | art::PartNames);
        fprintf(stderr, "[toybox] %d portraits in %s%s\n", found.portraits,
            art::PortraitFolder(ARKEYAN_GAME_ID).string().c_str(), found.partNames ? ", the parts' names read" : "");
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

    // F3 held in play: with G, T, C or R it is the debug screen's key (as in Minecraft); released alone, it opens the
    // menu. The game does not see those four keys while F3 is down (ToyboxKeyboardState).
    std::atomic<bool> g_f3Held;
    bool g_f3Combo = false;

    bool DebugCombo(toybox::Model& model, SDL_Keycode key)
    {
        using toybox::L;
        switch (key)
        {
        case SDLK_G: return gamemenu::ToggleDebugGraphs();
        case SDLK_T: return gamemenu::ToggleDebugThreads();
        case SDLK_C:
        {
            std::string text = gamemenu::DebugScreenText();
            if (text.empty())
                return false;
            bool copied = SDL_SetClipboardText(text.c_str());
            fprintf(stderr, "[gamemenu] debug screen copied: %s (%zu bytes)\n", copied ? "yes" : "no", text.size());
            model.Notice(copied ? L("Debug screen copied to the clipboard", "Écran de débogage copié")
                                : L("Could not copy the debug screen", "Copie impossible"),
                3);
            return true;
        }
        case SDLK_R:
            if (gamemenu::RecordPerfLogFor(10))
                model.Notice(L("Recording 10 s to the logs folder", "Enregistrement de 10 s dans les journaux"), 3);
            return true;
        default: return false;
        }
    }

    // The F3 menu's panel and the pages it opens beside the rail (not the Toy Box's own).
    bool MenuScreen(const toybox::Model& model)
    {
        return model.screen == Screen::Menu || model.screen == Screen::Settings ||
               model.screen == Screen::Achievements || model.screen == Screen::Commands;
    }

    void HandleKey(const SDL_KeyboardEvent& key, bool down)
    {
        toybox::Model& model = M();
        uint64_t now = SDL_GetTicksNS();
        if (!down && key.key == SDLK_F3 && g_f3Held.exchange(false))
        {
            if (!g_f3Combo && !model.open)
                model.OpenMenu(); // the F3 menu (docs/runtime/host/gamemenu.md)
            return;
        }
        if (!down)
        {
            if (key.key == SDLK_F2)
                g_pinKey = false;
            if (HoldKey(key) && g_tabDown != 0)
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
        if (model.open && model.waiting != toybox::Model::Waiting::None)
        {
            // The Commands page waits for a key or a pad button; Escape cancels.
            if (!key.repeat)
                model.TakeKey(key.key == SDLK_ESCAPE ? 0 : int(key.scancode));
            return;
        }
        if (key.repeat && (HoldKey(key) || OpenKey(key) || key.key == SDLK_F3))
            return;
        if (!model.open)
        {
            if (g_f3Held && key.key != SDLK_F3)
            {
                if (!key.repeat && DebugCombo(model, key.key))
                    g_f3Combo = true;
                return;
            }
            if (key.key == SDLK_F3 && (key.mod & SDL_KMOD_SHIFT) != 0)
                gamemenu::CycleDebugScreen(); // Shift+F3: the debug screen off, compact, full
            else if (key.key == SDLK_F3)
            {
                g_f3Held = true; // the menu opens when it is released alone
                g_f3Combo = false;
            }
            else if (OpenKey(key))
                OpenToybox();
            else if (HoldKey(key))
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
        if (key.key == SDLK_TAB && model.screen == Screen::Home && model.dialogs.empty())
        {
            // Tab and Shift+Tab: the next or previous row (seats, search and filters, the grid); the open key closes.
            if (!key.repeat)
                model.Run((key.mod & SDL_KMOD_SHIFT) != 0 ? Command::PreviousRow : Command::NextRow);
            return;
        }
        if ((OpenKey(key) || HoldKey(key)) && model.screen != Screen::Name)
        {
            model.Close();
            return;
        }
        if (key.key == SDLK_F3 && MenuScreen(model)) // F3 opened it: F3 closes it (in the Toy Box F3 is its View)
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
        case SDLK_LEFT:
            command = shift && (model.screen == Screen::Stats || model.screen == Screen::Creation)
                          ? Command::PreviousSection
                          : Command::Left;
            break;
        case SDLK_RIGHT:
            command = shift && (model.screen == Screen::Stats || model.screen == Screen::Creation)
                          ? Command::NextSection
                          : Command::Right;
            break;
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

    // A mouse button down at a canvas point (SDL's number: 1 left, 2 middle, 3 right, 4 and 5 the side ones). The
    // Commands page waiting for a mouse button takes it, and it does nothing else.
    void MouseDown(float x, float y, int sdlButton, bool twice)
    {
        toybox::Model& model = M();
        if (model.waiting == toybox::Model::Waiting::MouseButton)
        {
            model.TakeMouse(mouse::FromSdlButton(sdlButton));
            return;
        }
        if (sdlButton != SDL_BUTTON_LEFT && sdlButton != SDL_BUTTON_RIGHT)
            return;
        toybox::Click(model, x, y, sdlButton == SDL_BUTTON_RIGHT ? 3 : 1, twice);
        g_mouseHeld = true;
        model.Hold(true, SDL_GetTicksNS());
    }

    // A wheel tick (up > 0): the page waiting for a mouse button takes it; else it scrolls what is on screen.
    void MouseWheel(float up)
    {
        toybox::Model& model = M();
        if (model.waiting == toybox::Model::Waiting::MouseButton)
            model.TakeMouse(up > 0 ? mouse::WheelUp : mouse::WheelDown);
        else if (model.screen == Screen::Stats || model.screen == Screen::Creation)
            model.Run(up > 0 ? Command::Right : Command::Left);
        else if (model.screen == Screen::Home || model.screen == Screen::Add || model.screen == Screen::WheelList)
            model.ScrollList(up > 0 ? -1 : 1);
        else if (MenuScreen(model))
            model.Run(up > 0 ? Command::Up : Command::Down);
    }

    // The pointer moved to a canvas point: the wheel aims, the rest shows what is under it.
    void MouseMove(float x, float y, bool inside)
    {
        toybox::Model& model = M();
        if (model.screen == Screen::Wheel)
        {
            int place = inside ? toybox::WheelPlaceAt(x, y) : -2;
            if (place >= -1)
                model.AimWheel(place);
        }
        else if (toybox::Pointer(x, y, inside))
            model.Touch();
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
        std::string path = std::string(prefix != nullptr ? prefix : "toybox") + "_" +
                           (name.empty() ? std::to_string(++count) : name) + ".png";
        if (SDL_Surface* image =
                SDL_CreateSurfaceFrom(int(width), int(height), SDL_PIXELFORMAT_RGBA32, rgba.data(), int(width * 4)))
        {
            fprintf(stderr, "[toybox] screenshot %s: %s\n", path.c_str(),
                SDL_SavePNG(image, path.c_str()) ? "saved" : SDL_GetError());
            SDL_DestroySurface(image);
        }
    }

    uint16_t PadButton(const std::string& name)
    {
        static const std::pair<const char*, uint16_t> Names[] = { { "a", Button::A }, { "b", Button::B },
            { "x", Button::X }, { "y", Button::Y }, { "back", Button::Back }, { "start", Button::Start },
            { "lb", Button::LeftShoulder }, { "rb", Button::RightShoulder }, { "up", Button::DpadUp },
            { "down", Button::DpadDown }, { "left", Button::DpadLeft }, { "right", Button::DpadRight } };
        for (const auto& [text, button] : Names)
        {
            if (name == text)
                return button;
        }
        return 0;
    }

    // SKY_TOYBOX_SCRIPT's "prompt=1" / "prompt=2d" / "prompt=1v": the game asks that player for a Skylander for 3 s (d: and
    // says its Skylander died; v: for a vehicle instead), as if ReadPortalPrompt said it, and "level": a level starts
    // (game::LevelStarted): the out of health test.
    portalprompt::Prompt g_testPrompt;
    uint64_t g_testPromptUntil = 0;
    int g_testLevels = 0;

    // SKY_TOYBOX_SCRIPT="F6:100,Right:101,type=Ace:103,shot:104,pad=back*0.8:110": items at times in seconds since
    // launch. A key name (SDL_GetKeyFromName; "shift+Right") is pressed and released; "down=Tab" / "up=Tab" press or
    // release only; "type=<text>" types; "shot" or "shot=<name>" saves a screenshot; "pad=back+lright*0.8" holds pad
    // buttons (a b x y back start lb rb up down left right, lup ldown lleft lright for the stick) for 0.8 s (0.15 s
    // without "*"), as the Toy Box sees the pad; "click=x/y" and "rclick=x/y" click at canvas pixels, "move=x/y" moves the
    // pointer there (hover), "wheel=up" / "wheel=down" turn the wheel one tick (each logs the focus); "hold=K*0.5" holds a
    // key (an SDL scancode name) for the game's input 0.5 s and logs the pad state the game gets from it; "place=<dump>",
    // "remove=<slot>" and "remove=all" change the emulated portal (host/portal_script.h).
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
        std::stable_sort(
            g_script.begin(), g_script.end(), [](const ScriptStep& a, const ScriptStep& b) { return a.at < b.at; });
    }

    void SendKey(const std::string& name, bool down, SDL_Keymod mod = SDL_KMOD_NONE)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.key = SDL_GetKeyFromName(name.c_str());
        event.key.scancode = SDL_GetScancodeFromKey(event.key.key, nullptr);
        event.key.mod = mod;
        event.key.down = down;
        ToyboxHandleEvent(event);
    }

    // After a scripted key or mouse action: where the focus is, so a check can read it from the log. "home" is the Home
    // row and its place: seats (the top row's place), bar (0 search, 1 Filters) or grid (the card).
    void LogFocus()
    {
        const toybox::Model& model = M();
        const char* row = model.homeRow == toybox::Model::Row::Seats ? "seats"
                          : model.homeRow == toybox::Model::Row::Bar ? "bar"
                                                                     : "grid";
        int place = model.homeRow == toybox::Model::Row::Seats ? model.column
                    : model.homeRow == toybox::Model::Row::Bar ? model.barColumn
                                                               : model.list.cursor;
        fprintf(stderr,
            "[toybox] script: focus: screen %d, menu row %d, commands row %d column %d, settings row %d, "
            "achievement %d, card %d, home %s %d seat %d, hover %s\n",
            int(model.screen), model.menuRow, model.commandRow, model.commandColumn, model.settingsRow,
            model.achievementCursor, model.list.cursor, row, place, model.seat, toybox::HoverTarget().c_str());
    }

    // SKY_TOYBOX_CLOCK=frames: the script's times and its pad and key holds count the frames the game presented, 30 a
    // second, instead of the wall clock: a recorded route keeps its pace on a machine too busy for 30 fps, where the
    // game itself slows down frame by frame.
    uint64_t ScriptClock()
    {
        static const bool frames = [] {
            const char* clock = skyenv::Get("SKY_TOYBOX_CLOCK");
            return clock != nullptr && strcmp(clock, "frames") == 0;
        }();
        return frames ? gpu::FramesPresented() * 1'000'000'000ull / 30 : SDL_GetTicksNS();
    }

    // SKY_TOYBOX_SCRIPT_FILE=<file>: lines appended to the file while the game runs are script items run at once, or
    // "+<seconds> <item>" that many seconds of the script's clock after the line is read: a live run steered from
    // outside, as SKY_PRESS_FILE does for the pad, or a route started at a moment the caller sees in the log.
    void ReadScriptFile(uint64_t clock)
    {
        static const char* path = skyenv::Get("SKY_TOYBOX_SCRIPT_FILE");
        static size_t offset = 0;
        static uint64_t lastCheck = 0;
        uint64_t now = SDL_GetTicksNS();
        if (path == nullptr || now - lastCheck < 100000000ull)
            return;
        lastCheck = now;
        size_t size = 0;
        char* data = static_cast<char*>(SDL_LoadFile(path, &size));
        if (data == nullptr)
            return;
        std::string text = size > offset ? std::string(data + offset, size - offset) : std::string();
        SDL_free(data);
        size_t end = text.rfind('\n');
        if (end == std::string::npos)
            return;
        offset += end + 1;
        for (size_t begin = 0; begin < end;)
        {
            size_t stop = text.find('\n', begin);
            std::string item = text.substr(begin, stop - begin);
            begin = stop + 1;
            uint64_t at = clock;
            if (item.size() > 1 && item[0] == '+')
            {
                size_t space = item.find(' ');
                at += uint64_t(atof(item.c_str() + 1) * 1e9);
                item = space == std::string::npos ? std::string() : item.substr(space + 1);
            }
            if (item.empty())
                continue;
            auto later = std::upper_bound(g_script.begin() + ptrdiff_t(g_scriptNext), g_script.end(), at,
                [](uint64_t t, const ScriptStep& step) { return t < step.at; });
            g_script.insert(later, { at, item });
        }
    }

    void RunScript()
    {
        uint64_t now = SDL_GetTicksNS(), clock = ScriptClock();
        ReadScriptFile(clock);
        while (g_scriptNext < g_script.size() && g_script[g_scriptNext].at <= clock)
        {
            std::string action = g_script[g_scriptNext++].action;
            fprintf(stderr, "[toybox] script: %s\n", action.c_str());
            if (action == "shot" || action.rfind("shot=", 0) == 0)
            {
                SaveShot(action.size() > 5 ? action.substr(5) : std::string());
                continue;
            }
            if (portalscript::Item item = portalscript::Parse(action); item.kind != portalscript::Item::Kind::None)
            {
                if (item.kind == portalscript::Item::Kind::Place)
                    PortalPlaceFigure(item.path);
                else if (item.kind == portalscript::Item::Kind::Remove)
                    PortalRemoveFigure(item.slot);
                else
                    PortalRemoveAllFigures();
                continue;
            }
            if (action == "achievement")
            {
                if (g_achievementPreview != nullptr)
                    g_achievementPreview();
                continue;
            }
            if (action.rfind("prompt=", 0) == 0)
            {
                g_testPrompt = { portalprompt::Kind::Player, atoi(action.c_str() + 7), action.back() == 'd',
                    action.back() == 'v' };
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
                PadPress press{ clock, clock + uint64_t(hold * 1e9), 0, 0, 0 };
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
            if (action.rfind("hold=", 0) == 0)
            {
                std::string name = action.substr(5);
                size_t star = name.find('*');
                double hold = star != std::string::npos ? atof(name.c_str() + star + 1) : 0.5;
                name = name.substr(0, star);
                if (SDL_Scancode scancode = SDL_GetScancodeFromName(name.c_str()); scancode != SDL_SCANCODE_UNKNOWN)
                    g_keyHolds.push_back({ clock, clock + uint64_t(hold * 1e9), scancode, name, false });
                continue;
            }
            if (action.rfind("click=", 0) == 0 || action.rfind("rclick=", 0) == 0 || action.rfind("move=", 0) == 0)
            {
                std::string at = action.substr(action.find('=') + 1);
                float x = float(atof(at.c_str())), y = float(atof(at.c_str() + at.find('/') + 1));
                if (M().open && action[0] == 'm')
                    MouseMove(x, y, true);
                else if (M().open)
                {
                    MouseDown(x, y, action[0] == 'r' ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT, false);
                    g_mouseHeld = false;
                    M().Hold(false, now);
                }
                LogFocus();
                continue;
            }
            if (action.rfind("wheel=", 0) == 0)
            {
                if (M().open)
                    MouseWheel(action == "wheel=up" ? 1.0f : -1.0f);
                LogFocus();
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
            if (M().open)
                LogFocus();
        }
    }

    bool g_debugRedraw = false; // the debug screen refreshed: the next overlay drawing is its cost

    void Present()
    {
        toybox::Model& model = M();
        bool visible = model.open || !model.toast.empty() || !model.fps.empty() || gamemenu::Current().debugScreen;
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
        // The debug screen's cost line counts the overlay its refresh makes drawn again (2 Hz) over the game; with a
        // menu open the drawing is the menu's.
        uint64_t start = std::exchange(g_debugRedraw, false) && !model.open ? SDL_GetTicksNS() : 0;
        toybox::Draw(model, g_pixels);
        model.Clean();
        if (g_texture == nullptr)
            g_texture = gpu::CreateTexture({ .width = toybox::CanvasWidth,
                .height = toybox::CanvasHeight,
                .format = gpu::Format::R8G8B8A8_UNORM });
        gpu::UploadTexture(g_texture, g_pixels.data(), uint32_t(toybox::CanvasWidth * 4));
        gpu::SetOverlay(g_texture);
        g_overlayShown = true;
        if (start != 0)
            gamemenu::AddDebugScreenCost(SDL_GetTicksNS() - start);
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
    // The launcher's Toy Box window talks to this game (host/toybox_remote.h).
    toyboxremote::Start(PortalSessionFolder(), ARKEYAN_GAME_ID);
    fprintf(stderr,
        "[toybox] hold %s or View: quick swap wheel; %s or View + Y: the Toy Box; F3 or View + Start: the game menu\n",
        SDL_GetScancodeName(g_keys.tab ? SDL_SCANCODE_TAB : g_keys.open), SDL_GetScancodeName(g_keys.open));
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
        // F3 held: its combination keys are the debug screen's, not the game's.
        bool toggle = HoldKey(event.key) || OpenKey(event.key) || event.key.key == SDLK_F3 || g_f3Held;
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
        float x = 0, y = 0;
        bool inside = CanvasPoint(event.button.x, event.button.y, x, y);
        if (model.open && (inside || model.waiting == toybox::Model::Waiting::MouseButton))
            MouseDown(x, y, event.button.button, event.button.clicks >= 2);
        return model.open;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        g_mouseHeld = false;
        model.Hold(false, SDL_GetTicksNS());
        return model.open;
    case SDL_EVENT_MOUSE_MOTION:
    {
        float x = 0, y = 0;
        bool inside = CanvasPoint(event.motion.x, event.motion.y, x, y);
        if (model.open)
            MouseMove(x, y, inside);
        return model.open;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (model.open && event.wheel.y != 0)
            MouseWheel(event.wheel.y);
        return model.open;
    default: return false;
    }
}

void ToyboxFilterGamepad(GamepadState& state)
{
    if (!g_enabled)
        return;
    toybox::Model& model = M();
    uint64_t now = SDL_GetTicksNS(), clock = ScriptClock();
    for (KeyHold& hold : g_keyHolds)
    {
        if (!hold.logged && clock >= hold.from + 100'000'000 && clock < hold.to)
        {
            hold.logged = true;
            fprintf(stderr,
                "[toybox] script: %s held: the game gets buttons 0x%04X, left stick %d/%d, right stick %d/%d, triggers "
                "%d/%d\n",
                hold.name.c_str(), state.buttons, state.thumbLX, state.thumbLY, state.thumbRX, state.thumbRY,
                state.leftTrigger, state.rightTrigger);
        }
    }
    std::erase_if(g_keyHolds, [&](const KeyHold& hold) { return clock >= hold.to + 1'000'000'000; });
    for (const PadPress& press : g_padPresses)
    {
        if (clock >= press.from && clock < press.to)
        {
            state.buttons |= press.buttons;
            if (press.lx != 0)
                state.thumbLX = press.lx;
            if (press.ly != 0)
                state.thumbLY = press.ly;
        }
    }
    std::erase_if(g_padPresses, [&](const PadPress& press) { return clock >= press.to + 1'000'000'000; });

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
        bool held = buttons != 0 || std::abs(state.thumbRX) > StickDeadZone ||
                    std::abs(state.thumbRY) > StickDeadZone || state.leftTrigger > TriggerDeadZone ||
                    state.rightTrigger > TriggerDeadZone;
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

    // --- The F3 menu's Commands page: a pad button it waits for (the first pressed after all were released), then none
    // until every button is released again --------------------------------------------------------------------------
    if (model.waiting != toybox::Model::Waiting::PadButton)
        g_padArmed = false;
    if (model.screen == Screen::Commands && (model.waiting == toybox::Model::Waiting::PadButton || model.settle))
    {
        constexpr uint8_t TriggerPressed = 30;
        bool any = buttons != 0 || state.leftTrigger > TriggerPressed || state.rightTrigger > TriggerPressed;
        if (model.settle)
            model.settle = any;
        else if (!any)
            g_padArmed = true;
        else if (g_padArmed)
        {
            for (int button = 0; button < PadButtonCount; button++)
            {
                uint16_t mask = PadButtonMask(button);
                bool down = mask != 0 ? (buttons & mask) != 0
                                      : (button == 6 ? state.leftTrigger : state.rightTrigger) > TriggerPressed;
                if (down)
                {
                    model.TakePadButton(button);
                    break;
                }
            }
        }
        state = {};
        return;
    }

    // --- The Toy Box ---------------------------------------------------------------------------------------------------
    if ((buttons & Button::Back) && ((pressed & Button::Y) || (MenuScreen(model) && (pressed & Button::Start))))
    {
        model.Close();
        g_swallow = true;
        state = {};
        return;
    }
    constexpr int Threshold = 20000;
    uint16_t moves = buttons;
    if (state.thumbLY > Threshold)
        moves |= Button::DpadUp;
    if (state.thumbLY < -Threshold)
        moves |= Button::DpadDown;
    if (state.thumbLX < -Threshold)
        moves |= Button::DpadLeft;
    if (state.thumbLX > Threshold)
        moves |= Button::DpadRight;
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
    if (fire & Button::DpadUp)
        model.Run(Command::Up);
    if (fire & Button::DpadDown)
        model.Run(Command::Down);
    if (fire & Button::DpadLeft)
        model.Run(Command::Left);
    if (fire & Button::DpadRight)
        model.Run(Command::Right);
    if (pressed & Button::LeftShoulder)
        model.Run(Command::PreviousSection);
    if (pressed & Button::RightShoulder)
        model.Run(Command::NextSection);
    if (pressed & Button::A)
        model.Run(Command::Accept);
    if (model.open && (pressed & Button::X))
        model.Run(Command::Options);
    if (model.open && (pressed & Button::Y) && !(buttons & Button::Back))
        model.Run(Command::View);
    if (model.open && (pressed & Button::Start))
        model.Run(Command::Start);
    if (model.open && (pressed & Button::B))
        model.Run(Command::Back);
    if (!g_enterHeld && !g_mouseHeld)
        model.Hold((buttons & Button::A) != 0, now);
    if (!model.open)
        g_swallow = true;
    state = {};
}

void ToyboxSetAutoOpen(bool enabled) { g_autoOpen = enabled; }

static toybox::InPlay g_inPlay;
static uint64_t g_portalResets = 0;

// Opens the Toy Box on the seat the game asks for, and closes it once the prompt is gone. A figure the game gave up on
// (it asks for the seat again while that figure, which it had taken, is still on it, or says the Skylander died) is out of
// health until the next level: the Toy Box marks it, whether or not it opens by itself, once the figure has played in
// this level (toybox::InPlay: the asks of a level's start are not deaths).
static void FollowPrompt(toybox::Model& model)
{
    if (model.realPortal)
        return;
    portalprompt::Prompt prompt =
        game::Current().ReadPortalPrompt != nullptr ? game::Current().ReadPortalPrompt() : portalprompt::Prompt{};
    const uint64_t now = SDL_GetTicksNS();
    if (g_testPromptUntil > now)
        prompt = g_testPrompt;
    portalprompt::Prompt appeared;
    const portalprompt::Tracker::Event event = g_promptTracker.Update(prompt, appeared);
    if (const uint64_t resets = PortalResets(); resets != g_portalResets)
    {
        g_portalResets = resets;
        g_inPlay.StartOver();
    }
    // The seat asked for now, as the catalogue names it ("p1", "vehicle1"); no allocation, this runs every frame.
    const portalprompt::Prompt& asked = g_promptTracker.Current();
    char askedId[24] = "";
    if (asked.kind == portalprompt::Kind::Player)
        snprintf(askedId, sizeof(askedId), "%s%d", asked.vehicle ? "vehicle" : "p", asked.player);
    const bool playing = !game::InMenuLevel() && !ultrawide::MoviePlaying();
    for (size_t i = 0; i < model.seats.size(); i++)
    {
        const toybox::SeatState& s = model.seats[i];
        g_inPlay.Update(i, { s.path, s.placedAt, !s.waiting && !s.physical, s.seat.id == askedId, playing }, now);
    }
    switch (event)
    {
    case portalprompt::Tracker::Event::Appeared:
    {
        // The seat asked for: "p1", or "vehicle1" for a vehicle (the catalogue's seat ids).
        std::string id = (appeared.vehicle ? "vehicle" : "p") + std::to_string(appeared.player);
        // Said at the top over the game while it lasts (the Toy Box may be closed, or not open by itself).
        model.prompt =
            std::string(appeared.vehicle
                            ? toybox::L("Put a vehicle for Player ", "Posez un véhicule pour le joueur ")
                            : toybox::L("Put a Skylander for Player ", "Posez un Skylander pour le joueur ")) +
            std::to_string(appeared.player);
        model.Touch();
        for (size_t i = 0; i < model.seats.size(); i++)
        {
            if (model.seats[i].seat.id != id)
                continue;
            if (!model.seats[i].path.empty() && !g_inPlay.Played(i))
                fprintf(stderr, "[toybox] player %d's figure has not played in this level yet: not out of health\n",
                    appeared.player);
            else if (model.MarkOutOfHealth(int(i), appeared.defeated))
                fprintf(stderr, "[toybox] player %d's figure is out of health (%s)\n", appeared.player,
                    appeared.defeated ? "the game says its Skylander died"
                                      : "the game asks for the seat again while it is on it");
            if (!g_autoOpen)
                break;
            fprintf(stderr, "[toybox] prompt for player %d%s\n", appeared.player, appeared.vehicle ? "'s vehicle" : "");
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
        model.prompt.clear();
        model.Touch();
        if (!g_autoOpen)
            break;
        fprintf(stderr, "[toybox] the prompt is gone\n");
        if (g_openedByPrompt && model.open && model.screen == Screen::Home && model.dialogs.empty())
            model.Close();
        g_openedByPrompt = false;
        break;
    case portalprompt::Tracker::Event::None: break;
    }
    if (!model.open)
        g_openedByPrompt = false;
}

static std::atomic<bool> g_levelStarted{ false };

void ToyboxLevelStarted() { g_levelStarted = true; }

void ToyboxNotice(const char* english, const char* french)
{
    std::lock_guard lock(g_noticeMutex);
    g_notice = { english, french };
    g_noticePending = true;
}

void ToyboxAchievement(const std::string& name, uint32_t points, std::vector<uint8_t> png)
{
    std::lock_guard lock(g_noticeMutex);
    g_achievement = { name, points, std::move(png) };
    g_achievementPending = true;
}

void ToyboxSetAchievementPreview(void (*preview)()) { g_achievementPreview = preview; }

static std::vector<ToyboxAchievementRow> (*g_achievementList)();

void ToyboxSetAchievementList(std::vector<ToyboxAchievementRow> (*list)()) { g_achievementList = list; }

std::vector<ToyboxAchievementRow> ToyboxAchievements()
{
    return g_achievementList != nullptr ? g_achievementList() : std::vector<ToyboxAchievementRow>();
}

namespace
{
    collection::Image DecodePng(const std::vector<uint8_t>& png)
    {
        collection::Image image;
        if (png.empty())
            return image;
        SDL_Surface* loaded = SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true);
        if (loaded == nullptr)
            return image;
        if (SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32))
        {
            image.width = rgba->w;
            image.height = rgba->h;
            image.rgba.resize(size_t(rgba->w) * size_t(rgba->h));
            for (int y = 0; y < rgba->h; y++)
                memcpy(&image.rgba[size_t(y) * size_t(rgba->w)],
                    static_cast<const uint8_t*>(rgba->pixels) + size_t(y) * size_t(rgba->pitch), size_t(rgba->w) * 4);
            SDL_DestroySurface(rgba);
        }
        SDL_DestroySurface(loaded);
        return image;
    }
}

namespace
{
    // The launcher's Toy Box window (host/toybox_remote.h): its commands, run as the seats here run them, then the seats
    // whenever they change (looked at 5 times a second, only while a window is connected).
    void ServeWindow(toybox::Model& model, uint64_t now)
    {
        static uint64_t nextLook = 0;
        static std::vector<toyboxlink::Seat> sent;
        if (!toyboxremote::Connected())
            return;
        bool changed = toyboxremote::TakeHello();
        std::vector<toyboxlink::Message> replies;
        toyboxlink::Message command;
        while (toyboxremote::Next(command))
        {
            const std::vector<std::string>& f = command.fields;
            int seat = f.empty() ? -1 : atoi(f[0].c_str());
            std::string result;
            if (command.verb == "put")
                result = model.RemotePut(seat, f.size() > 1 ? f[1] : std::string());
            else if (command.verb == "off")
                result = model.RemoteOff(seat);
            else if (command.verb == "swap")
                result = model.RemoteSwap();
            else
                result = model.RemoteClear();
            const bool known = seat >= 0 && seat < int(model.seats.size()); // a seat number from outside: checked
            fprintf(stderr, "[toybox-link] %s %s: %s\n", command.verb.c_str(), known ? model.SeatTag(seat).c_str() : "",
                result.empty() ? "done" : result.c_str());
            replies.push_back({ "done", { command.verb, std::to_string(seat), result.empty() ? "ok" : result } });
            model.Touch();
            changed = true;
        }
        if (!changed && now < nextLook)
            return;
        nextLook = now + 200'000'000;
        std::vector<toyboxlink::Seat> seats = model.RemoteSeats();
        std::vector<toyboxlink::Message> messages;
        if (changed || seats != sent)
        {
            for (size_t i = 0; i < seats.size(); i++)
                messages.push_back(toyboxlink::SeatMessage(int(i), seats[i]));
            messages.push_back({ "sync", { std::to_string(seats.size()) } });
            sent = std::move(seats);
        }
        messages.insert(messages.end(), replies.begin(), replies.end());
        if (!messages.empty())
            toyboxremote::Send(messages);
    }
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
            model.Notice(toybox::French() ? g_notice.second : g_notice.first, 5);
        }
        if (g_achievementPending)
        {
            g_achievementPending = false;
            model.Achievement(g_achievement.name, g_achievement.points, DecodePng(g_achievement.png));
        }
    }
    if (g_levelStarted.exchange(false))
    {
        model.ClearOutOfHealth();
        g_inPlay.StartOver();
    }
    FollowPrompt(model);
    if (gamemenu::UpdateDebugScreen())
    {
        model.Touch();
        g_debugRedraw = true;
    }
    if (std::string name = gamemenu::FinishedPerfLog(); !name.empty())
        model.Notice(
            std::string(toybox::L("Performance log saved: ", "Journal de performances enregistré : ")) + name, 5);
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
    ServeWindow(model, now);
    model.Tick(now);
    Present();
}

bool ToyboxWaitsForPadButton()
{
    return g_enabled && M().open && M().screen == Screen::Commands && M().waiting == toybox::Model::Waiting::PadButton;
}

const bool* ToyboxKeyboardState()
{
    const bool* keys = SDL_GetKeyboardState(nullptr);
    bool f3 = g_f3Held.load(std::memory_order_relaxed);
    if (g_keyHolds.empty() && !f3)
        return keys;
    static bool merged[SDL_SCANCODE_COUNT];
    std::copy(keys, keys + SDL_SCANCODE_COUNT, merged);
    uint64_t now = ScriptClock();
    for (const KeyHold& hold : g_keyHolds)
        if (now >= hold.from && now < hold.to)
            merged[hold.scancode] = true;
    if (f3) // the debug screen's keys (F3+G, T, C, R)
        for (SDL_Scancode taken : { SDL_SCANCODE_G, SDL_SCANCODE_T, SDL_SCANCODE_C, SDL_SCANCODE_R })
            merged[taken] = false;
    return merged;
}

bool ToyboxOverlayOpen() { return g_enabled && M().open; }

bool ToyboxWaitsForMouseButton() { return g_enabled && M().open && M().waiting == toybox::Model::Waiting::MouseButton; }

void ToyboxSetKeys(const std::string (&keys)[3], bool tab)
{
    g_keys.Set(keys, tab);
    g_tabDown = 0; // a rebind must not leave a press waiting for the old key's release
    g_wheelByTab = false;
    PortalSetHotkeys(g_keys.add, g_keys.clear);
}

std::string ToyboxWheelKeyName()
{
    SDL_Scancode code = g_keys.tab ? SDL_SCANCODE_TAB : g_keys.open;
    const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false));
    return name != nullptr && *name != 0 ? name : SDL_GetScancodeName(code);
}
