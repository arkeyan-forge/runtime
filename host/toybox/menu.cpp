#include "host/toybox/model.h"
#include "game.h"
#include "d3d/upscale.h"
#include "gpu/edge_smoothing.h"
#include "gpu/gpu.h"
#include "host/gamemenu.h"
#include "host/settings.h"
#include "host/toybox.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string_view>

// The F3 menu's logic (docs/runtime/host/gamemenu.md): its side panel, the Settings, Achievements and Controls pages
// beside the rail. The look is draw.cpp (DrawMenu, DrawSettings, DrawAchievements, DrawCommands), the input toybox.cpp,
// what a setting changes and saves host/gamemenu.cpp.
namespace toybox
{
    namespace
    {
        // The settings, in the order the Settings page shows them, by section: Game, Video, Audio, Debug (SectionOf).
        // Report a problem (Help) is the panel's own row.
        enum Row
        {
            Commands,
            SkipIntro,
            Fullscreen,
            FrameRate,
            AutoFallback,
            Scale,
            Upscaler,
            FsrQuality,
            FrameGeneration,
            Anisotropy,
            EdgeSmoothing,
            GpuBackend,
            Vsync,
            Volume,
            PortalSound,
            Speakers,
            FpsCounter,
            DebugScreen,
            PerfLog,
            RowCount
        };

        std::string SectionOf(int row)
        {
            return row <= SkipIntro  ? L("Game", "Jeu")
                   : row <= Vsync    ? L("Video", "Vidéo")
                   : row <= Speakers ? L("Audio", "Audio")
                                     : L("Debug", "Débogage");
        }

        std::string LevelLabel(uint32_t level)
        {
            return level == 1 ? L("Compact", "Compact") : level == 2 ? L("Full", "Complet") : L("Off", "Non");
        }

        constexpr uint32_t Scales[] = { 0, 1, 2, 3, 4 };
        constexpr uint32_t Upscalers[] = { 0, 1 }; // off, FSR 3 (upscalemode::Upscaler)
        constexpr uint32_t Presets[] = { 4, 0, 1, 2, 3 }; // upscalemode::Quality: native first, the full size

        std::string PresetLabel(uint32_t preset)
        {
            return preset == 4   ? L("Native AA", "AA natif")
                   : preset == 1 ? L("Balanced", "Équilibré")
                   : preset == 2 ? L("Performance", "Performance")
                   : preset == 3 ? L("Ultra performance", "Ultra performance")
                                 : L("Quality", "Qualité");
        }
        constexpr uint32_t Levels[] = { 1, 2, 4, 8, 16 };
        constexpr uint32_t GpuBackends[] = { uint32_t(gpubackend::Backend::D3D12),
            uint32_t(gpubackend::Backend::Vulkan) };

        // The launcher's words for gpubackend's labels.
        const char* ApiLabel(const char* label) { return std::string_view(label) == "D3D12" ? "Direct3D 12" : label; }
        constexpr uint32_t SpeakerModes[] = { speakers::Auto, speakers::Stereo, speakers::Surround };

        std::string OnOff(bool on) { return on ? L("On", "Oui") : L("Off", "Non"); }

        // The rows shown, as indexes of Row: the edge smoothing row only for a game with a known FXAA pass, the portal's sound
        // only for a game whose portal has a speaker, the graphics API only on desktop Windows.
        std::vector<int> ShownRows()
        {
            std::vector<int> shown;
            for (int row = 0; row < RowCount; row++)
                if ((row != EdgeSmoothing || gamemenu::Current().edgeSmoothingShown) &&
                    (row != PortalSound || gamemenu::Current().portalSoundShown) &&
                    (row != AutoFallback || gamemenu::Current().fpsLimit == 60) &&
                    (row != FsrQuality || gamemenu::Current().upscaler != 0) &&
                    (row != GpuBackend || gamemenu::Current().gpuBackendShown))
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
            commands::Seed(g_commands, ARKEYAN_GAME_ID);
        }

        // Reads the file again, changes it, saves it, and hands the bindings to the running game (main.cpp takes them).
        bool ChangeCommands(const std::function<void(LauncherConfig&)>& change)
        {
            std::string path = LauncherConfigPath();
            g_commands = LoadLauncherConfig(path);
            change(g_commands);
            commands::Bindings bindings = commands::Read(g_commands, ARKEYAN_GAME_ID);
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

    // --- The panel --------------------------------------------------------------------------------------------------------

    std::vector<MenuRow> Model::MainRows() const
    {
        using Kind = MenuRow::Kind;
        const gamemenu::Values& v = gamemenu::Current();
        int onPortal = 0;
        for (const SeatState& s : seats)
            onPortal += !s.path.empty() || s.physical ? 1 : 0;
        auto row = [](Kind kind, std::string label, std::string value, int percent = 0) {
            MenuRow r;
            r.kind = kind;
            r.label = std::move(label);
            r.value = std::move(value);
            r.percent = percent;
            return r;
        };
        std::vector<MenuRow> rows(look::MainRows);
        rows[MainVolume] = row(Kind::Slider, L("Volume", "Volume"), std::to_string(v.volume), int(v.volume));
        rows[MainResume] = row(Kind::Action, L("Resume", "Reprendre"), "");
        rows[MainToyBox] = row(Kind::Action, "Toy Box",
            onPortal == 0 ? std::string() : std::to_string(onPortal) + L(" on the portal", " sur le portail"));
        rows[MainAchievements] = row(Kind::Action, L("Achievements", "Succès"),
            achievementRows.empty()
                ? std::string()
                : std::to_string(AchievementsUnlocked()) + " / " + std::to_string(achievementRows.size()));
        rows[MainSettings] = row(Kind::Action, L("Settings", "Réglages"), "");
        rows[MainPerformance] = row(Kind::Choice, L("Performance", "Performances"), LevelLabel(v.debugScreen));
        rows[MainReport] = row(Kind::Action, L("Report a problem", "Signaler un problème"), "");
        rows[MainQuit] = row(Kind::Action, L("Quit to the launcher", "Quitter vers le launcher"), "");
        return rows;
    }

    void Model::OpenMenu()
    {
        open = true;
        screen = Screen::Menu;
        dialogs.clear();
        fromMenu = false;
        menuRow = MainResume;
        waiting = Waiting::None;
        RefreshSeats(SDL_GetTicksNS());
        achievementRows = ToyboxAchievements();
        m_dirty = true;
        fprintf(stderr, "[gamemenu] opened\n");
    }

    void Model::FocusMenuRow(int row, int step)
    {
        menuRow = std::clamp(row, 0, look::MainRows - 1);
        MainStep(menuRow, step);
        m_dirty = true;
    }

    void Model::SetVolumeAt(int percent)
    {
        menuRow = MainVolume;
        uint32_t volume = uint32_t(std::clamp((percent + 5) / 10 * 10, 0, 100));
        if (volume != gamemenu::Current().volume && !gamemenu::SetVolume(volume))
            Toast(L("Could not save the setting (it applies to this run)",
                      "Réglage non enregistré (valable pour cette partie)"),
                4);
        m_dirty = true;
    }

    // `step` -1 / +1 changes the volume by 10 or flips the Performance switch; 0 does the row.
    void Model::MainStep(int row, int step)
    {
        const gamemenu::Values& v = gamemenu::Current();
        switch (row)
        {
        case MainVolume:
            if (step != 0 && !gamemenu::SetVolume(uint32_t(std::clamp(int(v.volume) / 10 * 10 + step * 10, 0, 100))))
                Toast(L("Could not save the setting (it applies to this run)",
                          "Réglage non enregistré (valable pour cette partie)"),
                    4);
            return;
        case MainResume:
            if (step == 0)
                Close();
            return;
        case MainToyBox:
            if (step == 0)
                OpenPage(look::Page::ToyBox);
            return;
        case MainAchievements:
            if (step == 0)
                OpenPage(look::Page::Achievements);
            return;
        case MainSettings:
            if (step == 0)
                OpenPage(look::Page::Settings);
            return;
        case MainPerformance:
            // The debug screen's three levels (off, compact, full): A and Right move on, Left back, round the ends.
            if (!gamemenu::SetDebugScreen((v.debugScreen + uint32_t(step < 0 ? 2 : 1)) % 3))
                Toast(L("Could not save the setting (it applies to this run)",
                          "Réglage non enregistré (valable pour cette partie)"),
                    4);
            return;
        case MainReport:
            if (step == 0)
                MenuStep(-1, 0);
            return;
        case MainQuit:
            if (step == 0)
                Ask({ L("Quit to the launcher?", "Quitter vers le launcher ?"),
                    L("What the game has not saved is lost.", "Ce que le jeu n'a pas enregistré est perdu."),
                    { L("Quit", "Quitter"), L("Keep playing", "Continuer") }, 1, false, [](int choice) {
                        if (choice != 0)
                            return;
                        fprintf(stderr, "[gamemenu] quit to the launcher\n");
                        SDL_Event quit{};
                        quit.type = SDL_EVENT_QUIT;
                        SDL_PushEvent(&quit);
                    } });
            return;
        default: return;
        }
    }

    void Model::RunMenu(Command command)
    {
        switch (command)
        {
        case Command::Up: menuRow = (menuRow + look::MainRows - 1) % look::MainRows; break;
        case Command::Down: menuRow = (menuRow + 1) % look::MainRows; break;
        case Command::Left:
        case Command::Right:
            if (menuRow == MainVolume || menuRow == MainPerformance)
                MainStep(menuRow, command == Command::Left ? -1 : 1);
            break;
        case Command::Accept:
            if (menuRow == MainVolume)
                break;
            MainStep(menuRow, 0);
            break;
        case Command::Back: Close(); break;
        default: break;
        }
    }

    // --- The pages beside the rail ---------------------------------------------------------------------------------------

    look::Page Model::CurrentPage() const
    {
        switch (screen)
        {
        case Screen::Settings: return look::Page::Settings;
        case Screen::Achievements: return look::Page::Achievements;
        case Screen::Commands: return look::Page::Controls;
        default: return look::Page::ToyBox;
        }
    }

    void Model::OpenPage(look::Page page)
    {
        // A page reached from the panel (or from another page that was) comes back to the panel with B.
        bool back = screen == Screen::Menu || fromMenu;
        dialogs.clear();
        waiting = Waiting::None;
        switch (page)
        {
        case look::Page::ToyBox:
            if (screen != Screen::Home)
                Open(Screen::Home, seat);
            break;
        case look::Page::Achievements:
            achievementRows = ToyboxAchievements();
            achievementCursor = achievementFirstRow = 0;
            open = true;
            screen = Screen::Achievements;
            fprintf(stderr, "[gamemenu] achievements opened (%d of %d unlocked)\n", AchievementsUnlocked(),
                int(achievementRows.size()));
            break;
        case look::Page::Settings:
            open = true;
            screen = Screen::Settings;
            settingsRow = 0;
            settingsScroll = 0;
            fprintf(stderr, "[gamemenu] settings opened\n");
            break;
        case look::Page::Controls: OpenCommands(); break;
        case look::Page::Performance:
        {
            // The debug screen is over the game: turning it on (compact) goes back there to show it.
            bool on = gamemenu::Current().debugScreen == 0;
            gamemenu::SetDebugScreen(on ? 1 : 0);
            if (on)
                Close();
            m_dirty = true;
            return;
        }
        case look::Page::Count: break;
        }
        fromMenu = back;
        m_dirty = true;
    }

    void Model::Leave()
    {
        if (!fromMenu)
        {
            Close();
            return;
        }
        int row = screen == Screen::Settings       ? MainSettings
                  : screen == Screen::Achievements ? MainAchievements
                  : screen == Screen::Commands     ? MainSettings
                                                   : MainToyBox;
        if ((screen == Screen::Stats || screen == Screen::Upgrades) && stats.dirty)
            SaveStats();
        OpenMenu();
        menuRow = row;
    }

    // --- Settings --------------------------------------------------------------------------------------------------------

    namespace
    {
        // The Settings page's rows: every row shown, in Row's order (the first of a section carries its heading).
        std::vector<int> ShownSettings() { return ShownRows(); }

        template <class T>
        int IndexOf(const T* list, size_t count, T value)
        {
            for (size_t i = 0; i < count; i++)
                if (list[i] == value)
                    return int(i);
            return 0;
        }

        // The frame rates under the game's cap (game::Adapter::maxFpsLimit).
        size_t CappedRates(uint32_t (&rates)[std::size(gamemenu::FrameRates)])
        {
            size_t count = 0;
            for (uint32_t rate : gamemenu::FrameRates)
                if (gamemenu::CapFrameRate(rate, ::game::Current().maxFpsLimit) == rate)
                    rates[count++] = rate;
            return count;
        }
    }

    int Model::SettingsRowTotal() const { return int(ShownSettings().size()); }

    std::vector<MenuRow> Model::SettingsRows() const
    {
        std::vector<MenuRow> all = MenuRows();
        std::vector<MenuRow> rows;
        for (int row : ShownSettings())
        {
            MenuRow shown = all[size_t(row)];
            shown.section = SectionOf(row);
            if (rows.empty() || rows.back().section != shown.section)
                shown.heading = shown.section;
            rows.push_back(std::move(shown));
        }
        return rows;
    }

    std::vector<MenuRow> Model::MenuRows() const
    {
        using Kind = MenuRow::Kind;
        const gamemenu::Values& v = gamemenu::Current();
        std::vector<MenuRow> rows(RowCount);
        uint32_t rates[std::size(gamemenu::FrameRates)];
        const size_t rateCount = CappedRates(rates);
        const uint32_t edge = gamemenu::EdgeSmoothingNow();
        static const uint32_t Modes[] = { edgesmoothing::Game, edgesmoothing::Light, edgesmoothing::Off };
        auto row = [](Kind kind, const char* label, std::string value, const char* help, int cost = -1,
                       bool nextStart = false) {
            MenuRow r;
            r.kind = kind;
            r.label = label;
            r.value = std::move(value);
            r.help = help;
            r.cost = cost;
            r.nextStart = nextStart;
            return r;
        };
        rows[Volume] = row(Kind::Slider, L("Volume", "Volume"), std::to_string(v.volume) + " %",
            L("The game's music, voices and effects together.", "La musique, les voix et les effets du jeu ensemble."));
        rows[Volume].percent = int(v.volume);
        rows[PortalSound] = row(Kind::Toggle, L("Portal sound", "Son du portail"), OnOff(v.portalSound),
            L("The traps' voices from the portal's speaker.", "La voix des pièges dans le haut-parleur du portail."));
        auto layoutLabel = [](uint32_t mode) {
            return std::string(mode == speakers::Surround ? "5.1" : L("Stereo", "Stéréo"));
        };
        rows[Speakers] = row(Kind::Choice, L("Speakers", "Enceintes"),
            v.speakers == speakers::Auto ? std::string(L("Auto", "Auto")) + " (" + layoutLabel(v.speakersNow) + ")"
                                         : layoutLabel(v.speakers),
            L("Stereo or 5.1 surround. Auto: 5.1 on a 6-channel output.",
                "Stéréo ou 5.1. Auto : 5.1 sur une sortie à 6 canaux."),
            -1, true);
        rows[Speakers].option = IndexOf(SpeakerModes, std::size(SpeakerModes), v.speakers);
        rows[Speakers].options = int(std::size(SpeakerModes));
        rows[FpsCounter] = row(Kind::Toggle, L("FPS counter", "Compteur d'images"), OnOff(v.showFps),
            L("A small counter in the corner.", "Un petit compteur dans le coin."));
        rows[DebugScreen] = row(Kind::Choice, L("Debug screen", "Écran de débogage"), LevelLabel(v.debugScreen),
            L("Frame times, CPU and GPU over the game. Shift+F3 in play.",
                "Temps d'image, CPU et GPU par-dessus le jeu. Maj+F3 en jeu."));
        rows[DebugScreen].option = int(std::min<uint32_t>(v.debugScreen, 2));
        rows[DebugScreen].options = 3;
        rows[PerfLog] =
            row(Kind::Toggle, L("Record a performance log", "Enregistrer les performances"), OnOff(v.perfLog),
                L("A CSV line per frame in the logs folder, for a report.",
                    "Une ligne CSV par image dans le dossier des journaux, pour un rapport."));
        rows[Commands] = row(Kind::Action, L("Commands", "Commandes"), "",
            L("The keyboard, pad and mouse keys of the game.",
                "Les touches du clavier, de la manette et de la souris."));
        rows[Fullscreen] = row(Kind::Toggle, L("Fullscreen", "Plein écran"), OnOff(v.fullscreen),
            L("The game fills the screen.", "Le jeu occupe tout l'écran."));
        rows[FrameRate] = row(Kind::Choice, L("Frame rate", "Images par seconde"), std::to_string(v.fpsLimit),
            L("Smoother above 30: more work for the whole machine.",
                "Plus fluide au-delà de 30 : plus de travail pour toute la machine."),
            v.fpsLimit == 30 ? 1 : 3);
        rows[FrameRate].option = IndexOf(rates, rateCount, v.fpsLimit);
        rows[FrameRate].options = int(rateCount);
        rows[AutoFallback] =
            row(Kind::Toggle, L("Drop to 30 FPS when needed", "Passer à 30 FPS si besoin"), OnOff(v.fpsAutoFallback),
                L("30 while 60 cannot be held, back to 60 when it can.",
                    "30 tant que 60 ne tient pas, puis 60 dès que possible."));
        // With FSR on, what it renders at and what it upscales to.
        std::string scaleText = v.scale == 0 ? std::string(L("Auto", "Auto")) : "x" + std::to_string(v.scale);
        if (v.upscaler == 1 && (upscale::Active() || upscale::SwitchesLive()))
        {
            const uint32_t frame = upscale::WindowFrameScale();
            const uint32_t scene = upscalemode::SceneScale(frame, upscalemode::Quality(v.fsrQuality));
            scaleText = "x" + std::to_string(scene) + L(", FSR 3 to x", ", FSR 3 vers x") + std::to_string(frame);
        }
        rows[Scale] = row(Kind::Choice, L("Resolution scale", "Échelle de résolution"), scaleText,
            L("Sharper picture, more work for the graphics card.",
                "Image plus nette, plus de travail pour la carte graphique."),
            v.scale == 0   ? 3
            : v.scale == 1 ? 1
            : v.scale == 2 ? 3
            : v.scale == 3 ? 4
                           : 5,
            true);
        rows[Scale].option = IndexOf(Scales, std::size(Scales), v.scale);
        rows[Scale].options = int(std::size(Scales));
        rows[Upscaler] = row(Kind::Choice, L("Upscaling", "Mise à l'échelle"),
            v.upscaler == 1 ? std::string("FSR 3") : std::string(L("Off", "Non")),
            L("AMD FSR 3: fewer pixels drawn, a sharp picture rebuilt.",
                "AMD FSR 3 : moins de pixels dessinés, une image nette reconstruite."),
            v.upscaler == 1 ? 1 : -1, !upscale::SwitchesLive());
        rows[Upscaler].option = IndexOf(Upscalers, std::size(Upscalers), v.upscaler);
        rows[Upscaler].options = int(std::size(Upscalers));
        rows[FsrQuality] = row(Kind::Choice, L("FSR quality", "Qualité FSR"), PresetLabel(v.fsrQuality),
            L("Lower: fewer pixels, faster, softer.", "Plus bas : moins de pixels, plus rapide, plus doux."));
        rows[FsrQuality].option = IndexOf(Presets, std::size(Presets), v.fsrQuality);
        rows[FsrQuality].options = int(std::size(Presets));
        rows[FrameGeneration] =
            row(Kind::Toggle, L("Frame generation", "Génération d'images"), OnOff(v.frameGeneration),
                L("FSR 3 makes a frame between two: twice the frames shown, a little more delay.",
                    "FSR 3 crée une image entre deux : deux fois plus d'images, un peu plus de délai."),
                v.frameGeneration ? 1 : -1);
        switch (gamemenu::FrameGenerationBlocked())
        {
        case gamemenu::FrameGenerationBlock::Later:
            rows[FrameGeneration].unavailable = L("Coming later.", "Bientôt disponible.");
            rows[FrameGeneration].value = OnOff(false); // the setting is ignored meanwhile
            rows[FrameGeneration].cost = -1;
            break;
        case gamemenu::FrameGenerationBlock::Device:
            rows[FrameGeneration].unavailable =
                L("Not available on this graphics card.", "Indisponible sur cette carte graphique.");
            break;
        case gamemenu::FrameGenerationBlock::Game:
            rows[FrameGeneration].unavailable =
                L("Not available in this game yet.", "Pas encore disponible dans ce jeu.");
            break;
        case gamemenu::FrameGenerationBlock::Upscaler:
            rows[FrameGeneration].unavailable = L("Needs Upscaling on FSR 3.", "Demande la mise à l'échelle FSR 3.");
            break;
        case gamemenu::FrameGenerationBlock::None: break;
        }
        rows[Anisotropy] = row(Kind::Choice, L("Texture filtering", "Filtrage des textures"),
            v.anisotropy == 1 ? std::string(L("Off", "Non")) : std::to_string(v.anisotropy) + "x",
            L("Keeps the ground sharp in the distance.", "Garde le sol net au loin."),
            v.anisotropy <= 1   ? 0
            : v.anisotropy <= 4 ? 1
                                : 2,
            true);
        rows[Anisotropy].option = IndexOf(Levels, std::size(Levels), v.anisotropy);
        rows[Anisotropy].options = int(std::size(Levels));
        rows[EdgeSmoothing] = row(Kind::Choice, L("Edge smoothing", "Lissage des bords"), EdgeSmoothingLabel(edge),
            L("Softens jagged edges. Gentle keeps more detail.",
                "Adoucit les bords en escalier. Doux garde plus de détails."),
            edge == edgesmoothing::Off ? 0 : 1);
        rows[EdgeSmoothing].option = IndexOf(Modes, std::size(Modes), edge);
        rows[EdgeSmoothing].options = int(std::size(Modes));
        // The chosen one, and the one that runs when they differ (a change waits for the next start, or a fallback).
        {
            const char* chosen = gpubackend::Label(gpubackend::Backend(v.gpuBackend));
            std::string value = ApiLabel(chosen);
            if (std::string_view(chosen) != gpu::BackendName())
                value += std::string(L(" (now ", " (actuel : ")) + ApiLabel(gpu::BackendName()) + ")";
            rows[GpuBackend] = row(Kind::Choice, L("Graphics API", "API graphique"), value,
                L("Direct3D 12 by default; the game uses the other if it cannot start.",
                    "Direct3D 12 par défaut ; le jeu prend l'autre s'il ne démarre pas."),
                -1, true);
            rows[GpuBackend].option = IndexOf(GpuBackends, std::size(GpuBackends), v.gpuBackend);
            rows[GpuBackend].options = int(std::size(GpuBackends));
        }
        rows[Vsync] = row(Kind::Toggle, L("VSync", "VSync"), OnOff(v.vsync),
            L("Waits for the display: no tearing.", "Attend l'écran : pas de déchirure."), -1, true);
        rows[SkipIntro] = row(Kind::Toggle, L("Skip the start-up logos", "Passer les logos de départ"),
            OnOff(v.skipIntroMovies),
            L("Straight to the title when the game starts.", "Directement à l'écran titre au démarrage."), -1, true);
        return rows;
    }

    void Model::FocusSettingsRow(int row, int step)
    {
        settingsRow = std::clamp(row, 0, SettingsRowTotal() - 1);
        MenuStep(ShownSettings()[size_t(settingsRow)], step);
        m_dirty = true;
    }

    void Model::RunSettings(Command command)
    {
        const int total = SettingsRowTotal();
        switch (command)
        {
        case Command::Up: settingsRow = (settingsRow + total - 1) % total; break;
        case Command::Down: settingsRow = (settingsRow + 1) % total; break;
        case Command::Left:
        case Command::Right:
        case Command::Accept:
            MenuStep(ShownSettings()[size_t(settingsRow)], command == Command::Left    ? -1
                                                           : command == Command::Right ? 1
                                                                                       : 0);
            break;
        case Command::PreviousSection:
        case Command::NextSection:
            OpenPage(look::PageStep(look::Page::Settings, command == Command::NextSection ? 1 : -1));
            break;
        case Command::Back: Leave(); break;
        default: break;
        }
    }

    // `row` is a Row (-1: Report a problem); `step` -1 / +1 changes a value (a toggle flips, a choice moves along its list, the
    // volume by 10), 0 does the row.
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
            break;
        case PortalSound: flip(v.portalSound, gamemenu::SetPortalSound); break;
        case Speakers: along(SpeakerModes, std::size(SpeakerModes), v.speakers, gamemenu::SetSpeakers); break;
        case FpsCounter: flip(v.showFps, gamemenu::SetShowFps); break;
        case DebugScreen:
        {
            static const uint32_t DebugLevels[] = { 0, 1, 2 };
            along(DebugLevels, 3, v.debugScreen, gamemenu::SetDebugScreen);
            break;
        }
        case PerfLog:
            if (gamemenu::SetPerfLog(!v.perfLog))
            {
                if (gamemenu::Current().perfLog)
                    Toast(L("Recording: a CSV file in the logs folder",
                              "Enregistrement : un fichier CSV dans le dossier des journaux"),
                        4);
            }
            else
                Toast(L("Could not create the performance log", "Impossible de créer le journal de performances"), 4);
            return;
        case Commands:
            if (step == 0)
                OpenPage(look::Page::Controls);
            return;
        case Fullscreen: flip(v.fullscreen, gamemenu::SetFullscreen); break;
        case FrameRate:
        {
            uint32_t rates[std::size(gamemenu::FrameRates)];
            along(rates, CappedRates(rates), v.fpsLimit, gamemenu::SetFpsLimit);
            break;
        }
        case AutoFallback: flip(v.fpsAutoFallback, gamemenu::SetFpsAutoFallback); break;
        case Scale: along(Scales, std::size(Scales), v.scale, gamemenu::SetScale); break;
        case Upscaler: along(Upscalers, std::size(Upscalers), v.upscaler, gamemenu::SetUpscaler); break;
        case FsrQuality: along(Presets, std::size(Presets), v.fsrQuality, gamemenu::SetFsrQuality); break;
        case FrameGeneration:
            if (gamemenu::FrameGenerationBlocked() != gamemenu::FrameGenerationBlock::None)
                return;
            flip(v.frameGeneration, gamemenu::SetFrameGeneration);
            break;
        case Anisotropy: along(Levels, std::size(Levels), v.anisotropy, gamemenu::SetAnisotropy); break;
        case EdgeSmoothing:
        {
            static const uint32_t Modes[] = { edgesmoothing::Game, edgesmoothing::Light, edgesmoothing::Off };
            along(Modes, 3, gamemenu::EdgeSmoothingNow(), gamemenu::SetEdgeSmoothing);
            break;
        }
        case GpuBackend: along(GpuBackends, std::size(GpuBackends), v.gpuBackend, gamemenu::SetGpuBackend); break;
        case Vsync: flip(v.vsync, gamemenu::SetVsync); break;
        case SkipIntro: flip(v.skipIntroMovies, gamemenu::SetSkipIntroMovies); break;
        case -1:
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
        default: return;
        }
        if (!saved)
            Toast(L("Could not save the setting (it applies to this run)",
                      "Réglage non enregistré (valable pour cette partie)"),
                4);
    }

    void Model::OpenCommands()
    {
        LoadCommands();
        open = true;
        screen = Screen::Commands;
        commandRow = commandColumn = 0;
        waiting = Waiting::None;
        fprintf(stderr, "[gamemenu] commands opened\n");
    }

    // --- Achievements ----------------------------------------------------------------------------------------------------

    int Model::AchievementsUnlocked() const
    {
        int count = 0;
        for (const ToyboxAchievementRow& row : achievementRows)
            count += row.unlocked != 0 ? 1 : 0;
        return count;
    }

    std::vector<int> Model::AchievementsShown() const
    {
        std::vector<int> shown;
        for (size_t i = 0; i < achievementRows.size(); i++)
        {
            bool unlocked = achievementRows[i].unlocked != 0;
            if (achievementTab == 0 || (achievementTab == 1) == unlocked)
                shown.push_back(int(i));
        }
        return shown;
    }

    void Model::SetAchievementTab(int tab)
    {
        achievementTab = std::clamp(tab, 0, 2);
        achievementCursor = achievementFirstRow = 0;
        m_dirty = true;
    }

    void Model::FocusAchievement(int index)
    {
        achievementCursor = index;
        m_dirty = true;
    }

    void Model::RunAchievements(Command command)
    {
        const int count = int(AchievementsShown().size()), columns = look::AchievementColumns;
        auto move = [&](int to) {
            if (count > 0)
                achievementCursor = std::clamp(to, 0, count - 1);
        };
        switch (command)
        {
        case Command::Left:
            if (achievementCursor % columns > 0)
                move(achievementCursor - 1);
            break;
        case Command::Right:
            if (achievementCursor % columns < columns - 1 && achievementCursor + 1 < count)
                move(achievementCursor + 1);
            break;
        case Command::Up:
            if (achievementCursor >= columns)
                move(achievementCursor - columns);
            break;
        case Command::Down:
            if (achievementCursor / columns < (count - 1) / columns)
                move(achievementCursor + columns);
            break;
        case Command::PreviousSection: SetAchievementTab((achievementTab + 2) % 3); break;
        case Command::NextSection: SetAchievementTab((achievementTab + 1) % 3); break;
        case Command::Back: Leave(); break;
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
        case Command::Back: Leave(); break;
        case Command::PreviousSection:
        case Command::NextSection:
            OpenPage(look::PageStep(look::Page::Controls, command == Command::NextSection ? 1 : -1));
            break;
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
            default: Leave(); break;
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
