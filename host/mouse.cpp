#include "mouse.h"
#include <cctype>

namespace mouse
{
    namespace
    {
        constexpr const char* Names[Count] = { "", "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5",
            "Wheel Up", "Wheel Down" };

        bool Same(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
                if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }
    }

    const char* Name(Button button) { return button < Count ? Names[button] : ""; }

    bool FromName(std::string_view name, Button& button)
    {
        if (Same(name, "none"))
        {
            button = None;
            return true;
        }
        for (int i = 1; i < Count; i++)
            if (Same(name, Names[i]))
            {
                button = Button(i);
                return true;
            }
        return false;
    }

    bool IsMouseName(std::string_view name)
    {
        Button button;
        return !Same(name, "none") && FromName(name, button);
    }

    Button FromSdlButton(int sdlButton)
    {
        switch (sdlButton)
        {
        case 1: return Left;
        case 2: return Middle;
        case 3: return Right;
        case 4: return X1;
        case 5: return X2;
        default: return None;
        }
    }

    void Effective(const Bindings& bindings, std::span<const Default> defaults, Button (&out)[ActionCount])
    {
        for (Button& button : out)
            button = None;
        if (!bindings.enabled)
            return;
        // g_keyActions' order (host/settings.cpp); kept here so this file needs no SDL.
        static constexpr const char* Actions[ActionCount] = { "a", "b", "x", "y", "start", "back", "lb", "rb", "lt",
            "rt", "left_up", "left_down", "left_left", "left_right", "right_up", "right_down", "right_left",
            "right_right", "dpad_up", "dpad_down", "dpad_left", "dpad_right" };
        for (const Default& entry : defaults)
            for (size_t i = 0; i < ActionCount; i++)
                if (std::string_view(Actions[i]) == entry.action)
                    out[i] = entry.button;
        for (size_t i = 0; i < ActionCount; i++)
        {
            if (!bindings.set[i])
                continue;
            // A button stored for an action leaves the action that had it by default (one action per button).
            if (bindings.stored[i] != None)
                for (size_t j = 0; j < ActionCount; j++)
                    if (j != i && !bindings.set[j] && out[j] == bindings.stored[i])
                        out[j] = None;
            out[i] = bindings.stored[i];
        }
    }

    bool AnyMapped(const Button (&effective)[ActionCount])
    {
        for (Button button : effective)
            if (button != None)
                return true;
        return false;
    }

    bool Capture::Update(const State& state)
    {
        m_state = state;
        if (!state.focused || state.minimized) // a window that never had the focus waits for its click too
            m_needClick = true;
        Apply();
        return m_held;
    }

    bool Capture::On(Event event)
    {
        m_clickTook = false;
        switch (event)
        {
        case Event::FocusLost:
            m_state.focused = false;
            m_needClick = true;
            break;
        case Event::FocusGained: m_state.focused = true; break;
        case Event::Minimized:
            m_state.minimized = true;
            m_needClick = true;
            break;
        case Event::Restored: m_state.minimized = false; break;
        case Event::Quit: m_quit = true; break;
        case Event::Click:
            if (m_needClick && m_state.focused && !m_state.minimized)
            {
                m_needClick = false;
                m_clickTook = true;
            }
            break;
        }
        const bool wasHeld = m_held;
        Apply();
        return m_clickTook && m_held && !wasHeld;
    }

    void Capture::ButtonDown(Button button)
    {
        if (button >= Count)
            return;
        m_down[button] = true;
        if (m_clickTook || !m_held)
            m_swallow[button] = true; // the click that took the pointer, or one made while it was free
        m_clickTook = false;
    }

    void Capture::ButtonUp(Button button)
    {
        if (button >= Count)
            return;
        m_down[button] = false;
        m_swallow[button] = false;
    }

    void Capture::Apply()
    {
        const bool held = !m_quit && m_state.mapped && m_state.focused && !m_state.minimized && !m_state.overlay &&
                          !m_state.gameMenu && !m_needClick;
        if (held && !m_held)
            for (int i = 0; i < Count; i++)
                m_swallow[i] = m_swallow[i] || m_down[i]; // held while the pointer was free: not the game's
        m_held = held;
    }
}
