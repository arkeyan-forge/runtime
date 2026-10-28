#include "host/toybox/model.h"
#include "host/gamemenu.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>

// The F3 menu's rows and logic (docs/runtime/host/gamemenu.md). The look is draw.cpp (DrawMenu), the input toybox.cpp, what a
// row changes and saves host/gamemenu.cpp.
namespace toybox
{
    namespace
    {
        enum Row { Volume, FpsCounter, Fullscreen, FrameRate, Scale, Anisotropy, Vsync, SkipIntro, Report, CloseMenu };
        static_assert(CloseMenu + 1 == Model::MenuRowCount);

        constexpr uint32_t Scales[] = { 0, 1, 2, 3, 4 };
        constexpr uint32_t Levels[] = { 1, 2, 4, 8, 16 };

        std::string OnOff(bool on)
        {
            return on ? L("On", "Oui") : L("Off", "Non");
        }
    }

    std::vector<MenuRow> Model::MenuRows() const
    {
        using Kind = MenuRow::Kind;
        const gamemenu::Values& v = gamemenu::Current();
        std::vector<MenuRow> rows(MenuRowCount);
        rows[Volume] = { Kind::Slider, L("Volume", "Volume"), std::to_string(v.volume) + " %", int(v.volume), false };
        rows[FpsCounter] = { Kind::Toggle, L("FPS counter", "Compteur d'images"), OnOff(v.showFps), 0, false };
        rows[Fullscreen] = { Kind::Toggle, L("Fullscreen", "Plein écran"), OnOff(v.fullscreen), 0, false };
        rows[FrameRate] = { Kind::Choice, L("Frame rate", "Images par seconde"), std::to_string(v.fpsLimit), 0, false };
        rows[Scale] = { Kind::Choice, L("Resolution scale", "Échelle de résolution"),
            v.scale == 0 ? std::string(L("Auto", "Auto")) : "x" + std::to_string(v.scale), 0, true };
        rows[Anisotropy] = { Kind::Choice, L("Texture filtering", "Filtrage des textures"),
            v.anisotropy == 1 ? std::string(L("Off", "Non")) : std::to_string(v.anisotropy) + "x", 0, true };
        rows[Vsync] = { Kind::Toggle, L("VSync", "VSync"), OnOff(v.vsync), 0, true };
        rows[SkipIntro] = { Kind::Toggle, L("Skip the start-up logos", "Passer les logos de départ"), OnOff(v.skipIntroMovies), 0, true };
        rows[Report] = { Kind::Action, L("Report a problem", "Signaler un problème"), "", 0, false };
        rows[CloseMenu] = { Kind::Action, L("Close", "Fermer"), "", 0, false };
        return rows;
    }

    void Model::OpenMenu()
    {
        open = true;
        screen = Screen::Menu;
        dialogs.clear();
        menuRow = 0;
        m_dirty = true;
        fprintf(stderr, "[gamemenu] opened\n");
    }

    void Model::FocusMenuRow(int row, int step)
    {
        menuRow = std::clamp(row, 0, MenuRowCount - 1);
        MenuStep(menuRow, step);
        m_dirty = true;
    }

    // `step` -1 / +1 changes a value (a toggle flips, a choice moves along its list, the volume by 10), 0 does the row.
    void Model::MenuStep(int row, int step)
    {
        const gamemenu::Values& v = gamemenu::Current();
        bool saved = true;
        auto flip = [&](bool value, bool (*set)(bool)) { saved = set(!value); };
        auto along = [&](const uint32_t* list, size_t count, uint32_t value, bool (*set)(uint32_t)) {
            int at = 0;
            for (size_t i = 0; i < count; i++)
                if (list[i] == value)
                    at = int(i);
            // Accept (0) moves on and wraps; Left / Right stop at the ends.
            int to = step == 0 ? (at + 1) % int(count) : std::clamp(at + step, 0, int(count) - 1);
            saved = set(list[to]);
        };
        switch (row)
        {
        case Volume:
            if (step != 0)
                saved = gamemenu::SetVolume(uint32_t(std::clamp(int(v.volume) / 10 * 10 + step * 10, 0, 100)));
            return;
        case FpsCounter: flip(v.showFps, gamemenu::SetShowFps); break;
        case Fullscreen: flip(v.fullscreen, gamemenu::SetFullscreen); break;
        case FrameRate:
        {
            static const uint32_t Rates[] = { 30, 60 };
            along(Rates, 2, v.fpsLimit, gamemenu::SetFpsLimit);
            break;
        }
        case Scale: along(Scales, 5, v.scale, gamemenu::SetScale); break;
        case Anisotropy: along(Levels, 5, v.anisotropy, gamemenu::SetAnisotropy); break;
        case Vsync: flip(v.vsync, gamemenu::SetVsync); break;
        case SkipIntro: flip(v.skipIntroMovies, gamemenu::SetSkipIntroMovies); break;
        case Report:
            if (step == 0)
            {
                gamemenu::ReportResult result = gamemenu::WriteProblemReport();
                size_t slash = result.path.find_last_of("/\\");
                std::string name = slash == std::string::npos ? result.path : result.path.substr(slash + 1);
                std::string folder;
                if (slash != std::string::npos && slash > 0)
                {
                    size_t before = result.path.find_last_of("/\\", slash - 1);
                    folder = result.path.substr(before == std::string::npos ? 0 : before + 1, slash - (before == std::string::npos ? 0 : before + 1));
                }
                if (result.ok)
                    Toast(std::string(L("Report saved in ", "Rapport enregistré dans ")) + folder + ": " + name, 6);
                else
                    Toast(std::string(L("Could not save the report: ", "Rapport impossible : ")) + result.error, 6);
            }
            return;
        case CloseMenu:
            if (step == 0)
                Close();
            return;
        }
        if (!saved)
            Toast(L("Could not save the setting (it applies to this run)", "Réglage non enregistré (valable pour cette partie)"), 4);
    }

    void Model::RunMenu(Command command)
    {
        switch (command)
        {
        case Command::Up: menuRow = (menuRow + MenuRowCount - 1) % MenuRowCount; break;
        case Command::Down: menuRow = (menuRow + 1) % MenuRowCount; break;
        case Command::Left: MenuStep(menuRow, -1); break;
        case Command::Right: MenuStep(menuRow, 1); break;
        case Command::Accept: MenuStep(menuRow, 0); break;
        case Command::Back: Close(); break;
        default: break;
        }
    }
}
