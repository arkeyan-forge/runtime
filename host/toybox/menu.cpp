#include "host/toybox/model.h"
#include "game.h"
#include "gpu/edge_smoothing.h"
#include "host/gamemenu.h"
#include "host/settings.h"
#include "host/toybox.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <functional>

// The F3 menu's rows and logic (docs/runtime/host/gamemenu.md). The look is draw.cpp (DrawMenu), the input toybox.cpp, what a
// row changes and saves host/gamemenu.cpp.
namespace toybox
{
    namespace
    {
        enum Row
        {
            Volume,
            PortalSound,
            FpsCounter,
            DebugScreen,
            Fullscreen,
            FrameRate,
            AutoFallback,
            Scale,
            Anisotropy,
            EdgeSmoothing,
            Vsync,
            SkipIntro,
            Commands,
            Report,
            CloseMenu
        };
        static_assert(CloseMenu + 1 == Model::MenuRowCount);

        constexpr uint32_t Scales[] = { 0, 1, 2, 3, 4 };
        constexpr uint32_t Levels[] = { 1, 2, 4, 8, 16 };

        std::string OnOff(bool on) { return on ? L("On", "Oui") : L("Off", "Non"); }

        // The rows shown, as indexes of Row: the edge smoothing row only for a game with a known FXAA pass, the portal's sound
        // only for a game whose portal has a speaker.
        std::vector<int> ShownRows()
        {
            std::vector<int> shown;
            for (int row = 0; row < Model::MenuRowCount; row++)
                if ((row != EdgeSmoothing || gamemenu::Current().edgeSmoothingShown) &&
                    (row != PortalSound || gamemenu::Current().portalSoundShown) &&
                    (row != AutoFallback || gamemenu::Current().fpsLimit == 60))
                    shown.push_back(row);
            return shown;
        }

        // The launcher's words (a label of "Light" would be translated as the element in its French file).
        std::string EdgeSmoothingLabel(uint32_t mode)
        {
            return mode == edgesmoothing::Game    ? L("Game", "Jeu")
                   : mode == edgesmoothing::Light ? L("Gentle", "Doux")
                                                  : L("None", "Aucun");
        }

        // --- The Commands page ---------------------------------------------------------------------------------------
        // Its columns, in order (a header, a cell, what Accept waits for each).
        enum Column
        {
            KeyColumn,
            PadColumn,
            MouseColumn,
            ColumnCount
        };
        // Its lines after the game's rows: the mouse's switch, the Toy Box's three keys, Reset, Back.
        enum Extra
        {
            MouseSwitch,
            ToyboxOpen,
            ToyboxAdd,
            ToyboxClear,
            ResetLine,
            BackLine,
            ExtraCount
        };

        LauncherConfig g_commands; // launcher.ini as the page last read or wrote it (the game's copy seeded in memory)

        std::span<const commands::Row> InputRows() { return ::game::Current().inputRows; }

        void LoadCommands()
        {
            g_commands = LoadLauncherConfig(LauncherConfigPath());
            commands::Seed(g_commands, SSA_GAME_ID);
        }

        // Reads the file again, changes it, saves it, and hands the bindings to the running game (main.cpp takes them).
        bool ChangeCommands(const std::function<void(LauncherConfig&)>& change)
        {
            std::string path = LauncherConfigPath();
            g_commands = LoadLauncherConfig(path);
            change(g_commands);
            commands::Bindings bindings = commands::Read(g_commands, SSA_GAME_ID);
            commands::Publish(bindings);
            ToyboxSetKeys(bindings.toyboxKeys, bindings.toyboxTab);
            if (SaveLauncherConfig(path, g_commands))
                return true;
            fprintf(stderr, "[gamemenu] cannot write %s: %s\n", path.c_str(), SDL_GetError());
            return false;
        }

        // A stored key as the player reads it (Skylauncher's KeyDisplayName): the letter of its place on the keyboard's
        // layout, the named keys in the player's language.
        std::string KeyName(const std::string& sdlName)
        {
            std::string shown = sdlName;
            if (SDL_Scancode place = SDL_GetScancodeFromName(sdlName.c_str()); place != SDL_SCANCODE_UNKNOWN)
                if (const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(place, SDL_KMOD_NONE, false));
                    name != nullptr && *name != 0)
                    shown = name;
            static const std::pair<const char*, const char*> Words[] = { { "Space", "Espace" }, { "Return", "Entrée" },
                { "Backspace", "Retour arrière" }, { "Escape", "Échap" }, { "Up", "Flèche haut" },
                { "Down", "Flèche bas" }, { "Left", "Flèche gauche" }, { "Right", "Flèche droite" },
                { "Left Shift", "Maj gauche" }, { "Right Shift", "Maj droite" }, { "Left Ctrl", "Ctrl gauche" },
                { "Right Ctrl", "Ctrl droite" }, { "Left Alt", "Alt gauche" }, { "Right Alt", "Alt droite" },
                { "Caps Lock", "Verr. maj" }, { "Delete", "Suppr" } };
            for (const auto& [english, french] : Words)
                if (shown == english)
                    return L(english, french);
            return shown;
        }

        // A pad button as an Xbox 360 pad prints it (Skylauncher's words), in the order of g_padButtons.
        std::string PadName(int button)
        {
            static const std::pair<const char*, const char*> Names[PadButtonCount] = { { "A", "A" }, { "B", "B" },
                { "X", "X" }, { "Y", "Y" }, { "LB", "LB" }, { "RB", "RB" }, { "LT", "LT" }, { "RT", "RT" },
                { "Start", "Start" }, { "Back", "Back" }, { "LS", "LS" }, { "RS", "RS" }, { "D-pad up", "Croix haut" },
                { "D-pad down", "Croix bas" }, { "D-pad left", "Croix gauche" }, { "D-pad right", "Croix droite" } };
            return button >= 0 && button < PadButtonCount ? L(Names[button].first, Names[button].second) : "-";
        }

        // A stick direction's pad cell (not editable), as Skylauncher words it.
        std::string StickName(std::string_view action)
        {
            if (action.starts_with("left_"))
                return L("L stick", "stick gauche");
            return action == "right_up"     ? L("R stick up", "stick droit haut")
                   : action == "right_down" ? L("R stick down", "stick droit bas")
                   : action == "right_left" ? L("R stick left", "stick droit gauche")
                                            : L("R stick right", "stick droit droite");
        }
    }

    int Model::MenuRowTotal() const { return int(ShownRows().size()); }

    std::vector<MenuRow> Model::MenuRows() const
    {
        using Kind = MenuRow::Kind;
        const gamemenu::Values& v = gamemenu::Current();
        std::vector<MenuRow> rows(MenuRowCount);
        rows[Volume] = { Kind::Slider, L("Volume", "Volume"), std::to_string(v.volume) + " %", int(v.volume), false };
        rows[PortalSound] = { Kind::Toggle, L("Portal sound", "Son du portail"), OnOff(v.portalSound), 0, false };
        rows[FpsCounter] = { Kind::Toggle, L("FPS counter", "Compteur d'images"), OnOff(v.showFps), 0, false };
        rows[DebugScreen] = { Kind::Toggle, L("Debug screen", "Écran de débogage"), OnOff(v.debugScreen), 0, false };
        rows[Fullscreen] = { Kind::Toggle, L("Fullscreen", "Plein écran"), OnOff(v.fullscreen), 0, false };
        rows[FrameRate] = { Kind::Choice, L("Frame rate", "Images par seconde"),
            v.fpsLimit == 0 ? std::string(L("Unlimited", "Illimité")) : std::to_string(v.fpsLimit), 0, false };
        rows[AutoFallback] = { Kind::Toggle, L("Drop to 30 FPS when needed", "Passer à 30 FPS si besoin"),
            OnOff(v.fpsAutoFallback), 0, false };
        rows[Scale] = { Kind::Choice, L("Resolution scale", "Échelle de résolution"),
            v.scale == 0 ? std::string(L("Auto", "Auto")) : "x" + std::to_string(v.scale), 0, true };
        rows[Anisotropy] = { Kind::Choice, L("Texture filtering", "Filtrage des textures"),
            v.anisotropy == 1 ? std::string(L("Off", "Non")) : std::to_string(v.anisotropy) + "x", 0, true };
        rows[EdgeSmoothing] = { Kind::Choice, L("Edge smoothing", "Lissage des bords"),
            EdgeSmoothingLabel(gamemenu::EdgeSmoothingNow()), 0, false };
        rows[Vsync] = { Kind::Toggle, L("VSync", "VSync"), OnOff(v.vsync), 0, true };
        rows[SkipIntro] = { Kind::Toggle, L("Skip the start-up logos", "Passer les logos de départ"),
            OnOff(v.skipIntroMovies), 0, true };
        rows[Commands] = { Kind::Action, L("Commands", "Commandes"), "", 0, false };
        rows[Report] = { Kind::Action, L("Report a problem", "Signaler un problème"), "", 0, false };
        rows[CloseMenu] = { Kind::Action, L("Close", "Fermer"), "", 0, false };
        std::vector<MenuRow> shown;
        for (int row : ShownRows())
            shown.push_back(rows[size_t(row)]);
        return shown;
    }

    void Model::OpenMenu()
    {
        open = true;
        screen = Screen::Menu;
        dialogs.clear();
        menuRow = 0;
        commandsOpen = false;
        waiting = Waiting::None;
        m_dirty = true;
        fprintf(stderr, "[gamemenu] opened\n");
    }

    void Model::FocusMenuRow(int row, int step)
    {
        menuRow = std::clamp(row, 0, MenuRowTotal() - 1);
        MenuStep(menuRow, step);
        m_dirty = true;
    }

    // `step` -1 / +1 changes a value (a toggle flips, a choice moves along its list, the volume by 10), 0 does the row.
    void Model::MenuStep(int row, int step)
    {
        row = ShownRows()[size_t(std::clamp(row, 0, MenuRowTotal() - 1))]; // the shown row's place in Row
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
        case PortalSound: flip(v.portalSound, gamemenu::SetPortalSound); break;
        case FpsCounter: flip(v.showFps, gamemenu::SetShowFps); break;
        case DebugScreen: flip(v.debugScreen, gamemenu::SetDebugScreen); break;
        case Fullscreen: flip(v.fullscreen, gamemenu::SetFullscreen); break;
        case FrameRate:
        {
            // The rates under the game's cap (game::Adapter::maxFpsLimit).
            uint32_t rates[std::size(gamemenu::FrameRates)];
            size_t count = 0;
            for (uint32_t rate : gamemenu::FrameRates)
                if (gamemenu::CapFrameRate(rate, ::game::Current().maxFpsLimit) == rate)
                    rates[count++] = rate;
            along(rates, count, v.fpsLimit, gamemenu::SetFpsLimit);
            break;
        }
        case AutoFallback: flip(v.fpsAutoFallback, gamemenu::SetFpsAutoFallback); break;
        case Scale: along(Scales, 5, v.scale, gamemenu::SetScale); break;
        case Anisotropy: along(Levels, 5, v.anisotropy, gamemenu::SetAnisotropy); break;
        case EdgeSmoothing:
        {
            static const uint32_t Modes[] = { edgesmoothing::Game, edgesmoothing::Light, edgesmoothing::Off };
            along(Modes, 3, gamemenu::EdgeSmoothingNow(), gamemenu::SetEdgeSmoothing);
            break;
        }
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
                    folder = result.path.substr(before == std::string::npos ? 0 : before + 1,
                        slash - (before == std::string::npos ? 0 : before + 1));
                }
                if (result.ok)
                    Toast(std::string(L("Report saved in ", "Rapport enregistré dans ")) + folder + ": " + name, 6);
                else
                    Toast(std::string(L("Could not save the report: ", "Rapport impossible : ")) + result.error, 6);
            }
            return;
        case Commands:
            if (step == 0)
            {
                LoadCommands();
                commandsOpen = true;
                commandRow = commandColumn = 0;
                fprintf(stderr, "[gamemenu] commands opened\n");
            }
            return;
        case CloseMenu:
            if (step == 0)
                Close();
            return;
        }
        if (!saved)
            Toast(L("Could not save the setting (it applies to this run)",
                      "Réglage non enregistré (valable pour cette partie)"),
                4);
    }

    void Model::RunMenu(Command command)
    {
        if (commandsOpen)
        {
            RunCommands(command);
            return;
        }
        switch (command)
        {
        case Command::Up: menuRow = (menuRow + MenuRowTotal() - 1) % MenuRowTotal(); break;
        case Command::Down: menuRow = (menuRow + 1) % MenuRowTotal(); break;
        case Command::Left: MenuStep(menuRow, -1); break;
        case Command::Right: MenuStep(menuRow, 1); break;
        case Command::Accept: MenuStep(menuRow, 0); break;
        case Command::Back: Close(); break;
        default: break;
        }
    }

    namespace
    {
        const char* ToyboxLabel(int which)
        {
            return which == 0   ? L("Open the Toy Box", "Ouvrir la Toy Box")
                   : which == 1 ? L("Add a figure", "Ajouter une figurine")
                                : L("Clear the figures", "Retirer les figurines");
        }

        std::string MouseName(mouse::Button button)
        {
            switch (button)
            {
            case mouse::Left: return L("Left click", "Clic gauche");
            case mouse::Right: return L("Right click", "Clic droit");
            case mouse::Middle: return L("Middle click", "Clic molette");
            case mouse::X1: return L("Mouse 4", "Souris 4");
            case mouse::X2: return L("Mouse 5", "Souris 5");
            case mouse::WheelUp: return L("Wheel up", "Molette haut");
            case mouse::WheelDown: return L("Wheel down", "Molette bas");
            default: return "-";
            }
        }

        size_t ActionOf(const commands::Row& row)
        {
            for (size_t i = 0; i < std::size(g_keyActions); i++)
                if (std::string_view(row.action) == g_keyActions[i].name)
                    return i;
            return 0;
        }

        void SaveFailed(const std::function<void(const std::string&)>& toast)
        {
            toast(L("Could not save the setting (it applies to this run)",
                "Réglage non enregistré (valable pour cette partie)"));
        }
    }

    std::vector<std::string> Model::CommandHeaders() const
    {
        std::vector<std::string> headers(ColumnCount);
        headers[KeyColumn] = L("Keyboard", "Clavier");
        headers[PadColumn] = L("Controller", "Manette");
        headers[MouseColumn] = L("Mouse", "Souris");
        return headers;
    }

    std::vector<CommandLine> Model::CommandLines() const
    {
        std::vector<CommandLine> lines;
        const bool mouseOn = commands::MouseOn(g_commands, game);
        mouse::Button buttons[mouse::ActionCount];
        mouse::Effective(commands::MouseLines(g_commands, game), ::game::Current().mouseDefaults, buttons);
        for (const commands::Row& row : InputRows())
        {
            CommandLine line{ L(row.label, row.french), std::vector<CommandLine::Cell>(ColumnCount), {} };
            line.cells[KeyColumn] = { KeyName(commands::Key(g_commands, game, row)), true };
            if (row.pad == nullptr)
                line.cells[PadColumn] = { StickName(row.action), false };
            else
            {
                std::string pads;
                for (int button = 0; button < PadButtonCount; button++)
                    if (commands::PadTarget(g_commands, game, button) == PadButtonIndex(row.pad))
                        pads += (pads.empty() ? "" : ", ") + PadName(button);
                line.cells[PadColumn] = { pads.empty() ? "-" : pads, true };
            }
            line.cells[MouseColumn] = { MouseName(buttons[ActionOf(row)]), mouseOn };
            lines.push_back(std::move(line));
        }
        lines.push_back({ L("Mouse", "Souris"), {}, mouseOn ? L("On", "Oui") : L("Off", "Non") });
        for (int which = 0; which < commands::ToyboxKeyCount; which++)
        {
            CommandLine line{ ToyboxLabel(which), std::vector<CommandLine::Cell>(ColumnCount), {} };
            line.cells[KeyColumn] = { KeyName(commands::ToyboxKey(g_commands, which)), true };
            if (which == 0 && commands::ToyboxTab(g_commands))
                line.cells[KeyColumn].text += ", " + KeyName("Tab");
            lines.push_back(std::move(line));
        }
        lines.push_back({ L("Reset to defaults", "Valeurs par défaut"), {}, {} });
        lines.push_back({ L("Back", "Retour"), {}, {} });
        if (waiting != Waiting::None && commandRow < int(lines.size()) && !lines[size_t(commandRow)].cells.empty())
            lines[size_t(commandRow)].cells[size_t(commandColumn)].text =
                waiting == Waiting::Key         ? L("Press a key", "Appuyez sur une touche")
                : waiting == Waiting::PadButton ? L("Press a button", "Appuyez sur un bouton")
                                                : L("Click or scroll", "Cliquez ou tournez la molette");
        return lines;
    }

    void Model::FocusCommand(int row, int cell)
    {
        if (waiting != Waiting::None)
            return;
        commandRow = row;
        if (cell >= 0)
            commandColumn = cell;
        RunCommands(Command::Accept);
    }

    void Model::RunCommands(Command command)
    {
        if (waiting != Waiting::None)
            return; // toybox.cpp hands the key, the button or the click to TakeKey / TakePadButton / TakeMouse
        const int rowCount = int(InputRows().size()), lineCount = rowCount + ExtraCount;
        auto notify = [this](const std::string& text) { Toast(text, 5); };
        m_dirty = true;
        switch (command)
        {
        case Command::Up: commandRow = (commandRow + lineCount - 1) % lineCount; break;
        case Command::Down: commandRow = (commandRow + 1) % lineCount; break;
        case Command::Left: commandColumn = std::max(commandColumn - 1, 0); break;
        case Command::Right: commandColumn = std::min(commandColumn + 1, ColumnCount - 1); break;
        case Command::Back: commandsOpen = false; break;
        case Command::Accept:
            if (commandRow < rowCount)
            {
                const commands::Row& row = InputRows()[size_t(commandRow)];
                if (commandColumn == PadColumn && row.pad == nullptr)
                {
                    Toast(L("A stick's directions keep their stick", "Les directions d'un stick restent sur le stick"));
                    break;
                }
                if (commandColumn == MouseColumn && !commands::MouseOn(g_commands, game))
                {
                    Toast(L("Turn the Mouse row on first", "Activez d'abord la ligne Souris"));
                    break;
                }
                waiting = commandColumn == KeyColumn   ? Waiting::Key
                          : commandColumn == PadColumn ? Waiting::PadButton
                                                       : Waiting::MouseButton;
                fprintf(stderr, "[gamemenu] commands: waiting for a %s for %s\n",
                    waiting == Waiting::Key         ? "key"
                    : waiting == Waiting::PadButton ? "pad button"
                                                    : "mouse button",
                    row.action);
                break;
            }
            switch (commandRow - rowCount)
            {
            case MouseSwitch:
            {
                bool on = !commands::MouseOn(g_commands, game);
                if (!ChangeCommands(
                        [&](LauncherConfig& config) { commands::SetMouseOn(config, game, InputRows(), on); }))
                    SaveFailed(notify);
                fprintf(stderr, "[gamemenu] mouse = %d\n", on ? 1 : 0);
                break;
            }
            case ToyboxOpen:
            case ToyboxAdd:
            case ToyboxClear:
                commandColumn = KeyColumn; // keyboard only
                waiting = Waiting::Key;
                fprintf(stderr, "[gamemenu] commands: waiting for a key for %s\n",
                    commands::ToyboxLines[commandRow - rowCount - ToyboxOpen]);
                break;
            case ResetLine:
            {
                bool saved =
                    ChangeCommands([&](LauncherConfig& config) { commands::Reset(config, game, InputRows()); });
                fprintf(stderr, "[gamemenu] commands reset to the defaults\n");
                if (saved)
                    Toast(L("Commands reset", "Commandes réinitialisées"), 5);
                else
                    SaveFailed(notify);
                break;
            }
            default: commandsOpen = false; break;
            }
            break;
        default: break;
        }
    }

    void Model::TakeKey(int scancode)
    {
        if (scancode == 0 && waiting != Waiting::None)
        {
            waiting = Waiting::None;
            m_dirty = true;
            fprintf(stderr, "[gamemenu] commands: cancelled\n");
            return;
        }
        const char* keyName = SDL_GetScancodeName(SDL_Scancode(scancode));
        if (waiting != Waiting::Key || keyName == nullptr || *keyName == 0)
            return;
        const size_t rowCount = InputRows().size();
        // Slots: the game's rows, then the Toy Box's keys (one line further down, after the mouse's switch).
        const size_t slot = size_t(commandRow) < rowCount ? size_t(commandRow) : size_t(commandRow) - 1;
        // F3 opens this menu; Tab opens the Toy Box while its own key is not set.
        if (scancode == SDL_SCANCODE_F3 ||
            (scancode == SDL_SCANCODE_TAB && commands::ToyboxTab(g_commands) && slot != rowCount))
        {
            Toast(L("This key opens the menus", "Cette touche ouvre les menus"));
            return;
        }
        int other = -1;
        bool saved = ChangeCommands(
            [&](LauncherConfig& config) { other = commands::SetKey(config, game, InputRows(), slot, keyName); });
        const std::string line =
            slot < rowCount ? std::string("key_") + InputRows()[slot].action : commands::ToyboxLines[slot - rowCount];
        if (other >= 0)
        {
            const size_t swapped = size_t(other);
            const std::string otherLine = swapped < rowCount ? std::string("key_") + InputRows()[swapped].action
                                                             : commands::ToyboxLines[swapped - rowCount];
            fprintf(stderr, "[gamemenu] %s = %s (swapped: %s = %s)\n", line.c_str(), keyName, otherLine.c_str(),
                commands::SlotKey(g_commands, game, InputRows(), swapped).c_str());
            if (std::string label = commands::SlotLabel(InputRows(), swapped, French()); !label.empty())
                Toast(std::string(L("Swapped with ", "Échangé avec ")) + label);
        }
        else
        {
            toast.clear();
            fprintf(stderr, "[gamemenu] %s = %s\n", line.c_str(), keyName);
        }
        if (!saved)
            SaveFailed([this](const std::string& text) { Toast(text, 5); });
        waiting = Waiting::None;
        settle = true;
        m_dirty = true;
    }

    void Model::TakePadButton(int button)
    {
        if (waiting != Waiting::PadButton || button < 0 || button >= PadButtonCount)
            return;
        const commands::Row& row = InputRows()[size_t(commandRow)];
        const int job = PadButtonIndex(row.pad);
        int other = -1;
        bool saved = ChangeCommands(
            [&](LauncherConfig& config) { other = commands::SetPad(config, game, InputRows(), job, button); });
        if (other >= 0)
        {
            int moved = commands::PadTarget(g_commands, game, other);
            fprintf(stderr, "[gamemenu] pad_%s = %s (swapped: pad_%s = %s)\n", g_padButtons[button], row.pad,
                g_padButtons[other], moved < 0 ? "none" : g_padButtons[moved]);
        }
        else
            fprintf(stderr, "[gamemenu] pad_%s = %s\n", g_padButtons[button], row.pad);
        if (!saved)
            SaveFailed([this](const std::string& text) { Toast(text, 5); });
        waiting = Waiting::None;
        settle = true;
        m_dirty = true;
    }

    void Model::TakeMouse(int button)
    {
        if (waiting != Waiting::MouseButton || button <= mouse::None || button >= mouse::Count)
            return;
        const commands::Row& row = InputRows()[size_t(commandRow)];
        const size_t action = ActionOf(row);
        int other = -1;
        bool saved = ChangeCommands([&](LauncherConfig& config) {
            other = commands::SetMouse(
                config, game, InputRows(), ::game::Current().mouseDefaults, action, mouse::Button(button));
        });
        if (other >= 0)
            fprintf(stderr, "[gamemenu] mouse_%s = %s (mouse_%s = none)\n", row.action,
                mouse::Name(mouse::Button(button)), g_keyActions[other].name);
        else
            fprintf(stderr, "[gamemenu] mouse_%s = %s\n", row.action, mouse::Name(mouse::Button(button)));
        if (!saved)
            SaveFailed([this](const std::string& text) { Toast(text, 5); });
        waiting = Waiting::None;
        settle = true;
        m_dirty = true;
    }
}
