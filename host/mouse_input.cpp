#include "host/mouse_input.h"
#include "host/env.h"
#include "host/settings.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace mouseinput
{
    namespace
    {
        mouse::Button g_effective[mouse::ActionCount] = {};
        bool g_mapped = false;
        mouse::Capture g_capture;
        bool g_down[mouse::Count] = {};
        uint64_t g_wheelUntil[mouse::Count] = {}; // a wheel tick holds its action this long (ms, SDL_GetTicks)
        bool g_held[mouse::ActionCount] = {};
        bool g_applied = false; // the relative mode as last set

        constexpr uint64_t WheelPulse = 120;

        // SKY_PRESS's mouse items (test aid): mleft, mright, mmiddle, m4, m5 ("mleft:20" clicks at 20 s, "mleft:20:1" holds
        // 1 s) and wheelup, wheeldown (one tick). A script stands for a focused window.
        struct Step
        {
            uint64_t at, until;
            mouse::Button button;
            bool started = false, ended = false;
        };
        std::vector<Step> g_script;
        bool g_scripted = false;

        void LoadScript()
        {
            const char* text = skyenv::Get("SKY_PRESS");
            if (text == nullptr)
                return;
            static const std::pair<const char*, mouse::Button> Names[] = { { "mleft", mouse::Left },
                { "mright", mouse::Right }, { "mmiddle", mouse::Middle }, { "m4", mouse::X1 }, { "m5", mouse::X2 },
                { "wheelup", mouse::WheelUp }, { "wheeldown", mouse::WheelDown } };
            std::string script = text;
            for (size_t begin = 0; begin < script.size();)
            {
                size_t end = std::min(script.find(',', begin), script.size());
                std::string item = script.substr(begin, end - begin);
                begin = end + 1;
                size_t colon = item.find(':');
                if (colon == std::string::npos)
                    continue;
                std::string name = item.substr(0, colon);
                for (const auto& [word, button] : Names)
                    if (name == word)
                    {
                        double at = atof(item.c_str() + colon + 1), hold = 0.15;
                        if (size_t second = item.find(':', colon + 1); second != std::string::npos)
                            hold = atof(item.c_str() + second + 1);
                        g_script.push_back({ uint64_t(at * 1000), uint64_t((at + hold) * 1000), button });
                    }
            }
            g_scripted = !g_script.empty();
        }

        void Button(mouse::Button button, bool down)
        {
            if (button == mouse::None)
                return;
            if (button == mouse::WheelUp || button == mouse::WheelDown)
            {
                if (down && g_capture.Held())
                    g_wheelUntil[button] = SDL_GetTicks() + WheelPulse;
                return;
            }
            g_down[button] = down;
            if (down)
                g_capture.ButtonDown(button);
            else
                g_capture.ButtonUp(button);
        }

        void Click()
        {
            if (g_capture.On(mouse::Capture::Event::Click))
                fprintf(stderr, "[mouse] the click in the window takes the pointer back (not passed to the game)\n");
        }
    }

    void Configure(const RuntimeSettings& settings, std::span<const mouse::Default> defaults)
    {
        mouse::Effective(settings.mouse, defaults, g_effective);
        const bool was = g_mapped;
        g_mapped = mouse::AnyMapped(g_effective);
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            LoadScript();
        }
        if (g_mapped || was)
        {
            std::string text;
            for (size_t i = 0; i < mouse::ActionCount; i++)
                if (g_effective[i] != mouse::None)
                    text += std::string(text.empty() ? "" : ", ") + g_keyActions[i].name + "=" +
                            mouse::Name(g_effective[i]);
            fprintf(stderr, "[mouse] %s\n", g_mapped ? ("on: " + text).c_str() : "off");
        }
    }

    void OnEvent(const SDL_Event& event, SDL_Window* window)
    {
        if (!g_mapped)
            return;
        using Event = mouse::Capture::Event;
        switch (event.type)
        {
        case SDL_EVENT_WINDOW_FOCUS_LOST: g_capture.On(Event::FocusLost); break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: g_capture.On(Event::FocusGained); break;
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_HIDDEN: g_capture.On(Event::Minimized); break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_SHOWN: g_capture.On(Event::Restored); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            Click();
            Button(mouse::FromSdlButton(event.button.button), true);
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP: Button(mouse::FromSdlButton(event.button.button), false); break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (event.wheel.y != 0)
                Button(event.wheel.y > 0 ? mouse::WheelUp : mouse::WheelDown, true);
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: Release(window); break;
        default: break;
        }
    }

    void Frame(SDL_Window* window, bool overlay, bool gameMenu)
    {
        for (bool& held : g_held)
            held = false;
        if (!g_mapped)
        {
            if (g_applied && window != nullptr) // turned off in the F3 menu while held
                SDL_SetWindowRelativeMouseMode(window, false);
            g_applied = false;
            return;
        }
        const uint64_t now = SDL_GetTicks();
        for (Step& step : g_script)
        {
            if (!step.started && now >= step.at)
            {
                step.started = true;
                if (step.button != mouse::WheelUp && step.button != mouse::WheelDown)
                    Click();
                Button(step.button, true);
                fprintf(stderr, "[mouse] script: %s %s\n", mouse::Name(step.button),
                    g_capture.Held() ? "(held pointer: the game's)" : "(free pointer: not the game's)");
            }
            if (step.started && !step.ended && now >= step.until)
            {
                step.ended = true;
                Button(step.button, false);
            }
        }
        mouse::Capture::State state;
        state.mapped = true;
        state.focused = g_scripted || (window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0);
        state.minimized = window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0;
        state.overlay = overlay;
        state.gameMenu = gameMenu;
        const bool held = g_capture.Update(state);
        if (held != g_applied && window != nullptr)
        {
            SDL_SetWindowRelativeMouseMode(window, held);
            fprintf(stderr, "[mouse] pointer %s\n", held ? "held by the game" : "free");
        }
        g_applied = held;
        if (!held)
            return;
        for (size_t i = 0; i < mouse::ActionCount; i++)
        {
            const mouse::Button button = g_effective[i];
            if (button == mouse::None || g_capture.Swallowed(button))
                continue;
            g_held[i] =
                button == mouse::WheelUp || button == mouse::WheelDown ? now < g_wheelUntil[button] : g_down[button];
        }
    }

    const bool* HeldActions() { return g_mapped ? g_held : nullptr; }

    void Release(SDL_Window* window)
    {
        g_capture.On(mouse::Capture::Event::Quit);
        if (g_applied && window != nullptr)
            SDL_SetWindowRelativeMouseMode(window, false);
        g_applied = false;
    }
}
