#pragma once

// The mouse in the games (docs/runtime/xbox360/input.md, "Mouse"): its buttons and wheel bound to the game's actions, a switch
// per game, and when the game holds the pointer (Capture). Off by default: with the switch off nothing here is used and the
// pointer is never hidden or held. Plain C++ (no SDL): ArkeyanMouseTest runs it.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mouse
{
    // The mouse's buttons and the wheel's two directions. Their names share the settings file's key list with the keyboard's
    // (SDL's key names): "Mouse Left", "Mouse Right", "Mouse Middle", "Mouse 4", "Mouse 5", "Wheel Up", "Wheel Down".
    enum Button : uint8_t
    {
        None,
        Left,
        Right,
        Middle,
        X1, // the side buttons: back
        X2, // forward
        WheelUp,
        WheelDown,
        Count
    };
    inline constexpr size_t ActionCount = 22; // g_keyActions (host/settings.h)

    const char* Name(Button button); // "" for None
    // A name of the list above (case-insensitive), or "none"; false for anything else.
    bool FromName(std::string_view name, Button& button);
    bool IsMouseName(std::string_view name);
    // SDL's button number (1 left, 2 middle, 3 right, 4 X1, 5 X2) as a Button; None for another.
    Button FromSdlButton(int sdlButton);

    // A game's defaults once the switch is on: its attack 1 on the left button, attack 2 on the right
    // (game::Adapter::mouseDefaults). Every other action has none.
    struct Default
    {
        const char* action; // g_keyActions' name ("x")
        Button button;
    };

    // [<game>.input] mouse=0|1 (the switch, off by default) and mouse_<action>=<mouse name>|none.
    struct Bindings
    {
        bool enabled = false;
        Button stored[ActionCount] = {}; // what mouse_<action> says
        bool set[ActionCount] = {};      // whether there is a mouse_<action> line
    };
    // The button each action is on (g_keyActions order): the stored lines over the game's defaults; None everywhere when off.
    void Effective(const Bindings& bindings, std::span<const Default> defaults, Button (&out)[ActionCount]);
    // Whether any action is on a mouse button or the wheel (the switch on and something mapped): the game may hold the pointer.
    bool AnyMapped(const Button (&effective)[ActionCount]);

    // When the game hides and holds the pointer in its window. Only while the game is played (no F3 menu, Toy Box or wheel
    // over it, not one of the game's own menus the runtime can tell: its front end), the window has focus and is not
    // minimized, and a mouse action is mapped. A loss of focus (Alt-Tab, Cmd-Tab, a dialog) or a minimized window lets it
    // go until a click in the window takes it back; that click, and a button still held when it is taken, never reach the
    // game. Closing the menu or the Toy Box takes it back at once. Quitting lets it go for good.
    class Capture
    {
    public:
        struct State
        {
            bool mapped = false;   // a mouse action is mapped (AnyMapped)
            bool focused = false;  // the window has the keyboard focus
            bool minimized = false;
            bool overlay = false;  // the F3 menu, the Toy Box or its wheel is open
            bool gameMenu =
                false; // the game shows one of its own menus the runtime can tell (game::Adapter::menuLevels)
        };
        enum class Event
        {
            FocusLost,
            FocusGained,
            Minimized,
            Restored,
            Click, // a mouse button went down in the window
            Quit,
        };

        // The conditions now (each frame); returns whether the pointer is held.
        bool Update(const State& state);
        // An event of the window; returns whether that click (Event::Click) is the one that takes the pointer back.
        bool On(Event event);
        bool Held() const { return m_held; }
        // A button is swallowed from the moment the pointer is taken until it is released (the game never sees it).
        bool Swallowed(Button button) const { return button < Count && m_swallow[button]; }
        void ButtonDown(Button button);
        void ButtonUp(Button button);

    private:
        State m_state;
        bool m_held = false, m_needClick = false, m_quit = false, m_clickTook = false;
        bool m_down[Count] = {}, m_swallow[Count] = {};
        void Apply();
    };
}
