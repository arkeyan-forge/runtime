// Test the actual drawing hit registry and pointer/click handlers without initializing the renderer.
#include "draw.cpp"

void toybox::Model::Run(Command) {} // Click's unmatched right button goes Back; no game model is needed here.

int main()
{
    using namespace toybox;
    Model model;
    int failures = 0;
    auto check = [&](bool pass) {
        if (!pass)
            failures++;
    };
    AddHit({ 0, 0 }, { 20, 20 }, [&] { model.list.cursor = 3; });
    g_hits.back().target = "card=3";
    check(Pointer(5, 5, true));
    check(HoverTarget() == "card=3" && model.list.cursor == 0);
    Click(model, 5, 5, 1, false);
    check(model.list.cursor == 3);
    AddHit({ 30, 0 }, { 50, 20 }, [&] { model.menuRow = Model::MainSettings; });
    g_hits.back().target = "menu=4";
    Pointer(35, 5, true);
    check(HoverTarget() == "menu=4" && model.menuRow == Model::MainResume); // the panel opens on Resume
    Click(model, 35, 5, 1, false);
    check(model.menuRow == Model::MainSettings);
    // A Settings row and a rail row: hover names them, a click does them.
    AddHit({ 90, 0 }, { 110, 20 }, [&] { model.settingsRow = 6; });
    g_hits.back().target = "settings=6";
    AddHit({ 120, 0 }, { 140, 20 }, [&] { model.achievementTab = 2; });
    g_hits.back().target = "rail=1";
    Pointer(95, 5, true);
    check(HoverTarget() == "settings=6" && model.settingsRow == 0);
    Click(model, 95, 5, 1, false);
    Click(model, 125, 5, 1, false);
    check(model.settingsRow == 6 && model.achievementTab == 2);
    AddHit({ 60, 0 }, { 80, 20 }, [&] {
        model.commandRow = 7;
        model.commandColumn = 2;
    });
    g_hits.back().target = "commands=7/2";
    Pointer(65, 5, true);
    check(HoverTarget() == "commands=7/2" && model.commandRow == 0);
    Click(model, 65, 5, 1, false);
    check(model.commandRow == 7 && model.commandColumn == 2);
    // A non-hoverable dialog backdrop blocks both hover and the card's action.
    AddHit({ 0, 0 }, { 20, 20 }, {});
    g_hits.back().hover = false;
    model.list.cursor = 0;
    Pointer(5, 5, true);
    Click(model, 5, 5, 1, false);
    check(HoverTarget() == "none" && model.list.cursor == 0);
    Pointer(35, 5, false);
    check(HoverTarget() == "none");
    // The pointer moving onto a focusable thing moves the focus there, once: a key moving the focus away is not undone
    // while the pointer stays on the same thing; moving onto another one focuses it.
    g_hits.clear();
    g_hovered = -1;
    int focused = -1;
    AddHit({ 0, 0 }, { 20, 20 }, [] {});
    g_hits.back().focus = [&] { focused = 0; };
    AddHit({ 30, 0 }, { 50, 20 }, [] {});
    g_hits.back().focus = [&] { focused = 1; };
    AddHit({ 60, 0 }, { 80, 20 }, [] {}); // no focus: a seat (a choice, not a focus)
    Pointer(5, 5, true);
    check(focused == 0);
    g_hovered = HoveredHit(); // the frame drawn
    focused = 7;              // a key moved the focus
    Pointer(8, 6, true);
    check(focused == 7);
    Pointer(35, 5, true);
    check(focused == 1);
    g_hovered = HoveredHit();
    Pointer(65, 5, true);
    check(focused == 1);
    printf("%d checks, %d failures\n", 15, failures);
    puts(failures ? "FAIL: menu pointer and click"
                  : "PASS: menu/Settings/rail/card/Commands hover and click, blocked and outside hits, hover focus");
    return failures ? 1 : 0;
}
