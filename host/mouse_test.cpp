// The mouse (host/mouse.h): the names, the bindings over a game's defaults, and every transition of the capture: off by
// default, focus lost and won back by a click (that click never reaches the game), the F3 menu or the Toy Box, a minimized
// window, the game's own menu, nothing mapped, quitting. No SDL, no game data: ./SSAMouseTest
#include "host/mouse.h"
#include <cstdio>

using namespace mouse;

namespace
{
    int g_checks = 0, g_failures = 0;
#define EXPECT(condition)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                               \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

    constexpr Default Giants[] = { { "a", Left }, { "x", Right } };
    constexpr size_t A = 0, X = 2, Y = 3; // g_keyActions' order

    Capture::State Playing()
    {
        Capture::State state;
        state.mapped = state.focused = true;
        return state;
    }
}

int main()
{
    // Names: one list with the keyboard's, any case; "none".
    Button button = None;
    EXPECT(FromName("Mouse Left", button) && button == Left);
    EXPECT(FromName("wheel down", button) && button == WheelDown);
    EXPECT(FromName("Mouse 4", button) && button == X1);
    EXPECT(FromName("none", button) && button == None);
    EXPECT(!FromName("Space", button) && !FromName("Mouse 9", button));
    EXPECT(IsMouseName("Mouse Right") && !IsMouseName("none") && !IsMouseName("Q"));
    for (int b = Left; b < Count; b++)
        EXPECT(FromName(Name(Button(b)), button) && button == Button(b));
    EXPECT(FromSdlButton(1) == Left && FromSdlButton(3) == Right && FromSdlButton(2) == Middle &&
           FromSdlButton(5) == X2 && FromSdlButton(9) == None);

    // Bindings: off by default, nothing mapped whatever is stored.
    Bindings bindings;
    Button out[ActionCount];
    Effective(bindings, Giants, out);
    EXPECT(!AnyMapped(out));
    bindings.stored[Y] = Middle;
    bindings.set[Y] = true;
    Effective(bindings, Giants, out);
    EXPECT(!AnyMapped(out));
    // On: the game's defaults, and every other action where it is.
    bindings = {};
    bindings.enabled = true;
    Effective(bindings, Giants, out);
    EXPECT(out[A] == Left && out[X] == Right && out[Y] == None && AnyMapped(out));
    // A stored line wins; the button it takes leaves the default action that had it.
    bindings.stored[Y] = Left;
    bindings.set[Y] = true;
    Effective(bindings, Giants, out);
    EXPECT(out[Y] == Left && out[A] == None && out[X] == Right);
    // "none" removes a default; all removed: nothing mapped (the pointer is never held).
    bindings = {};
    bindings.enabled = true;
    bindings.set[A] = bindings.set[X] = true;
    Effective(bindings, Giants, out);
    EXPECT(out[A] == None && out[X] == None && !AnyMapped(out));
    // The wheel can be an action too.
    bindings.stored[Y] = WheelUp;
    bindings.set[Y] = true;
    Effective(bindings, Giants, out);
    EXPECT(out[Y] == WheelUp && AnyMapped(out));

    // Capture: off (nothing mapped) never holds, even focused and clicked.
    {
        Capture capture;
        Capture::State state = Playing();
        state.mapped = false;
        EXPECT(!capture.Update(state));
        EXPECT(!capture.On(Capture::Event::Click) && !capture.Held());
    }
    // Playing, focused, mapped: held at once.
    {
        Capture capture;
        EXPECT(capture.Update(Playing()));
        // F3 menu or Toy Box: let go; closed: held again at once (no click needed).
        Capture::State state = Playing();
        state.overlay = true;
        EXPECT(!capture.Update(state));
        EXPECT(capture.Update(Playing()));
        // The game's own menu (its front end): not held.
        state = Playing();
        state.gameMenu = true;
        EXPECT(!capture.Update(state));
        EXPECT(capture.Update(Playing()));
        // A button held when the menu closed is not the game's until it is released.
        state.overlay = true;
        state.gameMenu = false;
        capture.Update(state);
        capture.ButtonDown(Left);
        EXPECT(capture.Update(Playing()) && capture.Swallowed(Left));
        capture.ButtonUp(Left);
        EXPECT(!capture.Swallowed(Left));
        capture.ButtonDown(Left); // a click while held: the game's
        EXPECT(!capture.Swallowed(Left));
        capture.ButtonUp(Left);
    }
    // Alt-Tab / Cmd-Tab (focus lost): let go; focus back: still free until a click in the window, which is swallowed.
    {
        Capture capture;
        capture.Update(Playing());
        EXPECT(!capture.On(Capture::Event::FocusLost) && !capture.Held());
        EXPECT(!capture.On(Capture::Event::FocusGained) && !capture.Held());
        EXPECT(!capture.Update(Playing())); // the frame's state says focused: still waits for the click
        EXPECT(capture.On(Capture::Event::Click) && capture.Held());
        capture.ButtonDown(Left);
        EXPECT(capture.Swallowed(Left)); // the click that took it back is not an attack
        capture.ButtonUp(Left);
        capture.ButtonDown(Right);
        EXPECT(!capture.Swallowed(Right)); // the next one is
        capture.ButtonUp(Right);
        // A click while unfocused does nothing.
        capture.On(Capture::Event::FocusLost);
        EXPECT(!capture.On(Capture::Event::Click) && !capture.Held());
    }
    // A dialog over the window takes the focus: the same. Minimized: let go, restored: a click takes it back.
    {
        Capture capture;
        capture.Update(Playing());
        EXPECT(!capture.On(Capture::Event::Minimized) && !capture.Held());
        EXPECT(!capture.On(Capture::Event::Restored) && !capture.Held());
        EXPECT(capture.On(Capture::Event::Click) && capture.Held());
    }
    // A click in the window while a menu is open does not hold it (the menu takes the click); the menu closing does.
    {
        Capture capture;
        capture.Update(Playing());
        capture.On(Capture::Event::FocusLost);
        capture.On(Capture::Event::FocusGained);
        Capture::State state = Playing();
        state.overlay = true;
        capture.Update(state);
        EXPECT(!capture.On(Capture::Event::Click) && !capture.Held());
        EXPECT(capture.Update(Playing())); // the click was spent on the window: back to the game when it closes
    }
    // A window that never had the focus (the launcher in front): the click that focuses it is not an attack.
    {
        Capture capture;
        Capture::State state = Playing();
        state.focused = false;
        EXPECT(!capture.Update(state));
        EXPECT(!capture.On(Capture::Event::FocusGained) && !capture.Held());
        EXPECT(capture.On(Capture::Event::Click) && capture.Held());
        capture.ButtonDown(Left);
        EXPECT(capture.Swallowed(Left));
    }
    // Quitting: let go for good.
    {
        Capture capture;
        capture.Update(Playing());
        EXPECT(!capture.On(Capture::Event::Quit) && !capture.Held());
        EXPECT(!capture.Update(Playing()) && !capture.On(Capture::Event::Click));
    }

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
