#include "launcher_ui.h"
#include "launcher_assets.h"
#include "launcher_ui_draw.h"
#include "launcher_theme.h"
#include "widgets/launcher_widgets.h"
#include "vfs.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <span>
#include <stb_image_write.h>
#include <thread>
#include <vector>
#include "env.h"

using namespace LauncherTheme;

namespace
{
    double NowSeconds() { return static_cast<double>(SDL_GetTicksNS()) / 1'000'000'000.0; }

    bool Contains(const SDL_FRect& r, float x, float y)
    {
        return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
    }

    std::string Lower(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        return out;
    }

    std::string Upper(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        return out;
    }

    std::string FileNameOnly(const std::string& path)
    {
        size_t s = path.find_last_of("/\\");
        return s == std::string::npos ? path : path.substr(s + 1);
    }

    bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.good())
            return false;
        std::streamoff size = file.tellg();
        if (size <= 0)
            return false;
        out.resize(size_t(size));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(out.data()), size);
        return file.good();
    }

    SDL_Texture* LoadCachedAsset(
        LauncherAssets::Provider& provider, LauncherAssets::Role role, LauncherPainter& painter)
    {
        LauncherAssets::Asset asset = provider.Get(role);
        if (!asset.valid)
            return nullptr;
        std::vector<uint8_t> png;
        if (!ReadWholeFile(asset.path, png))
            return nullptr;
        return painter.LoadPng({ png.data(), png.size() });
    }

    std::string SettingGet(const LauncherConfig& cfg, std::string_view section, std::string_view key)
    {
        auto sit = cfg.find(std::string(section));
        if (sit == cfg.end())
            return {};
        auto kit = sit->second.find(std::string(key));
        if (kit == sit->second.end())
            return {};
        return kit->second;
    }

    void SettingSet(LauncherConfig& cfg, std::string_view section, std::string_view key, const std::string& value)
    {
        cfg[std::string(section)][std::string(key)] = value;
    }

    bool ParseBool(std::string_view v)
    {
        std::string l = Lower(v);
        return l == "1" || l == "true" || l == "yes" || l == "on";
    }

    std::string ChooseIsoDialog(SDL_Window* window, const GameInfo& game, const std::string& previous)
    {
        static const SDL_DialogFileFilter filters[] = { { "Xbox 360 disc image (.iso)", "iso" } };
        struct R
        {
            std::atomic<bool> done{ false };
            std::string path;
            bool failed = false;
        };
        R r;
        SDL_PropertiesID p = SDL_CreateProperties();
        SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(filters));
        SDL_SetNumberProperty(p, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
        std::string title = std::string("Choose your ") + game.name + " disc image";
        SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
        if (window)
            SDL_SetPointerProperty(p, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
        if (!previous.empty())
            SDL_SetStringProperty(p, SDL_PROP_FILE_DIALOG_LOCATION_STRING, previous.c_str());
        SDL_ShowFileDialogWithProperties(
            SDL_FILEDIALOG_OPENFILE,
            [](void* ud, const char* const* files, int) {
                R* s = static_cast<R*>(ud);
                if (!files || !files[0])
                {
                    s->failed = true;
                    s->done = true;
                    return;
                }
                s->path = files[0];
                s->done = true;
            },
            &r, p);
        SDL_DestroyProperties(p);
        while (!r.done)
        {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        if (r.failed)
            fprintf(stderr, "launcher: file dialog failed: %s\n", SDL_GetError());
        return r.path;
    }

    struct VerifyJob
    {
        std::thread th;
        std::atomic<bool> done{ false };
        std::atomic<float> progress{ 0.0f };
        std::mutex m;
        std::string stage = "Starting";
        GameSourceCheck check{};
        TitleMetadata meta{};
        std::string path;
        int gen = 0;

        ~VerifyJob()
        {
            if (th.joinable())
                th.join();
        }
        float P() { return progress.load(); }
        std::string S()
        {
            std::lock_guard lk(m);
            return stage;
        }
        void Start(const std::string& image, const GameInfo& game)
        {
            if (th.joinable())
                th.join();
            ++gen;
            int cgen = gen;
            done = false;
            progress = 0.0f;
            {
                std::lock_guard lk(m);
                stage = "Starting";
                check = {};
                meta = {};
            }
            path = image;
            th = std::thread([this, cgen, image, &game]() {
                (void)cgen;
                auto report = [&](float f, std::string_view t) {
                    progress.store(f);
                    std::lock_guard lk(m);
                    stage.assign(t);
                };
                GameSourceCheck res = CheckGameSource(image, game, report);
                if (res.status == GameSourceStatus::Ok)
                {
                    report(0.98f, "Reading title name and icon from disc");
                    DiscFileSystem d;
                    if (d.Open(image.c_str()))
                    {
                        std::vector<uint8_t> xex = d.ReadAll("default.xex");
                        if (!xex.empty())
                            meta = ReadTitleMetadata(xex);
                    }
                }
                {
                    std::lock_guard lk(m);
                    check = std::move(res);
                    stage = "Done";
                }
                progress.store(1.0f);
                done.store(true);
            });
        }
        void Cancel()
        {
            ++gen;
            done = true;
            progress = 0.0f;
            {
                std::lock_guard lk(m);
                stage = "Cancelled";
            }
        }
    };

    enum class Phase
    {
        Idle,
        Verifying,
        Ready,
        Failed
    };
    enum class Act
    {
        None,
        Browse,
        Play,
        Change,
        Settings,
        CloseSettings,
        ToggleFS,
        Export,
        Cancel,
        GameSel,
        Quit
    };

    struct Hotspot
    {
        SDL_FRect rect;
        Act act = Act::None;
        int idx = -1;
        bool enabled = true;
    };

    struct App
    {
        const GameInfo& game;
        std::string configPath;
        LauncherConfig& config;
        SDL_Window* win = nullptr;
        SDL_Renderer* ren = nullptr;
        SDL_Gamepad* pad = nullptr;
        SDL_Cursor* hand = nullptr;
        SDL_Cursor* arrow = nullptr;
        LauncherPainter painter;
        LauncherWidgets* widgets = nullptr;

        Phase phase = Phase::Idle;
        double now = 0, last = 0, delta = 0, phaseStart = 0;
        bool running = true;
        std::string chosen;
        std::string srcPath;
        uint64_t srcSize = 0;
        GameSourceCheck check{};
        TitleMetadata meta{};
        SDL_Texture* iconTex = nullptr;
        SDL_Texture* logoTex = nullptr;
        SDL_Texture* heroTex = nullptr;
        LauncherAssets::Provider assets;
        long long verifyMs = 0;

        VerifyJob verify;
        float dispProg = 0;
        bool compSched = false;
        double compAt = 0;

        bool settingsOpen = false;
        float settingsT = 0;
        bool dragOver = false;
        bool fullscreen = false;
        std::string toast;
        double toastUntil = 0;

        std::vector<Hotspot> hs;
        SDL_FPoint mouse{ -9999, -9999 };
        bool mouseInside = false;

        // test
        bool testMode = false;
        std::string shotsDir, testDrop;
        double testStart = 0, testDropAt = 0;
        bool tEmpty = false, tHover = false, tDrag = false, tVer = false, tReady = false, tSet = false, tDone = false;

        App(const GameInfo& g, std::string cp, LauncherConfig& c) : game(g), configPath(std::move(cp)), config(c) {}
        bool Init();
        void Shutdown();
        std::string Run();
        void Event(const SDL_Event& e);
        void Click(float x, float y);
        void Update(double t, float dt);
        void Test();
        void StartVerify(const std::string& p);
        void FinishVerify();
        void Browse();
        void BeginPlay();
        void ToggleFS();
        void Save();
        void Toast(std::string m, double s = 2.6);
        void Cap(const std::string& n);
        void PushDrop(const std::string& p);
        void Add(const SDL_FRect& r, Act a, int idx = -1, bool en = true) { hs.push_back({ r, a, idx, en }); }
        bool Hover(const Hotspot& h) { return mouseInside && h.enabled && Contains(h.rect, mouse.x, mouse.y); }
        void Draw();
        void DrawHero();
        void DrawBottomBar();
        void DrawFooter();
        void DrawSettings();
        void DrawToast();
        void DrawPortalDecor(float cx, float cy, float r, float t);
    };

    bool App::Init()
    {
        SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        const char* vd = SDL_GetCurrentVideoDriver();
        bool off = vd && (strcmp(vd, "offscreen") == 0 || strcmp(vd, "dummy") == 0);
        SDL_WindowFlags fl = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Skylanders: Spyro's Adventure — Launcher");
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1280);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 800);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<Sint64>(fl));
        if (off)
            SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
        win = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        if (!win)
        {
            fprintf(stderr, "launcher: window failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetWindowMinimumSize(win, 1024, 640);
        ren = off ? SDL_CreateRenderer(win, "software") : SDL_CreateRenderer(win, nullptr);
        if (!ren)
            ren = SDL_CreateRenderer(win, "software");
        if (!ren)
        {
            fprintf(stderr, "launcher: renderer failed\n");
            return false;
        }
        SDL_SetRenderLogicalPresentation(ren, 1280, 800, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        SDL_SetRenderVSync(ren, 1);
        if (!painter.Init(ren))
        {
            fprintf(stderr, "launcher: painter failed\n");
            return false;
        }
        widgets = new LauncherWidgets(&painter);
        hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
        arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        SDL_ShowCursor();
        fullscreen = LauncherFullscreen(config);
        if (fullscreen)
            SDL_SetWindowFullscreen(win, true);
        if (const char* s = skyenv::Get("SKY_LAUNCHER_SHOTS"))
        {
            testMode = true;
            shotsDir = s;
            std::error_code ec;
            std::filesystem::create_directories(shotsDir, ec);
        }
        if (const char* d = skyenv::Get("SKY_LAUNCHER_TEST_DROP"))
            testDrop = d;
        now = last = NowSeconds();
        testStart = now;
        testDropAt = now + 1.25;
        phaseStart = now;
        int ng = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&ng);
        if (ids)
        {
            if (ng > 0)
                pad = SDL_OpenGamepad(ids[0]);
            SDL_free(ids);
        }
        return true;
    }

    void App::Shutdown()
    {
        verify.th.join();
        // Textures returned by painter.LoadPng are owned by the painter and released in its Shutdown.
        iconTex = nullptr;
        logoTex = nullptr;
        heroTex = nullptr;
        delete widgets;
        widgets = nullptr;
        painter.Shutdown();
        if (pad)
            SDL_CloseGamepad(pad);
        if (hand)
            SDL_DestroyCursor(hand);
        if (arrow)
            SDL_DestroyCursor(arrow);
        if (ren)
            SDL_DestroyRenderer(ren);
        if (win)
            SDL_DestroyWindow(win);
    }

    std::string App::Run()
    {
        if (!Init())
        {
            Shutdown();
            return {};
        }
        std::string prev = SettingGet(config, game.id, "iso");
        if (!testMode && !prev.empty())
            StartVerify(prev);
        while (running)
        {
            now = NowSeconds();
            delta = static_cast<float>(std::clamp(now - last, 0.0, 0.1));
            last = now;
            SDL_Event e;
            while (SDL_PollEvent(&e))
                Event(e);
            Update(now, delta);
            Draw();
            Test();
            if (testMode)
                SDL_SetRenderVSync(ren, 0);
            SDL_RenderPresent(ren);
            if (!testMode)
                SDL_Delay(1);
            else
                SDL_Delay(4);
        }
        Shutdown();
        return chosen;
    }

    void App::Event(const SDL_Event& e)
    {
        switch (e.type)
        {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_MOUSE_MOTION:
        {
            float x = 0, y = 0;
            if (SDL_RenderCoordinatesFromWindow(ren, e.motion.x, e.motion.y, &x, &y))
            {
                mouse = { x, y };
            }
            else
            {
                mouse = { e.motion.x, e.motion.y };
            }
            mouseInside = true;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_LEFT)
            {
                float x = e.button.x, y = e.button.y;
                SDL_RenderCoordinatesFromWindow(ren, e.button.x, e.button.y, &x, &y);
                if (settingsOpen && !Contains(SDL_FRect{ ContentX, HeroY, ContentW, FooterY + FooterH - HeroY }, x, y))
                {
                    settingsOpen = false;
                    break;
                }
                Click(x, y);
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.repeat)
                break;
            if (e.key.key == SDLK_ESCAPE)
            {
                if (settingsOpen)
                    settingsOpen = false;
                else if (phase == Phase::Verifying)
                {
                    verify.Cancel();
                    phase = Phase::Idle;
                    phaseStart = now;
                    srcPath.clear();
                    srcSize = 0;
                    Toast("Check cancelled.");
                }
                else
                    running = false;
                break;
            }
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER || e.key.key == SDLK_SPACE)
            {
                if (settingsOpen)
                    settingsOpen = false;
                else if (phase == Phase::Ready)
                    BeginPlay();
                else if (phase == Phase::Failed || phase == Phase::Idle)
                    Browse();
                break;
            }
            if (e.key.key == SDLK_F1 || e.key.key == SDLK_COMMA)
            {
                settingsOpen = !settingsOpen;
                break;
            }
            if (e.key.key == SDLK_F11)
            {
                ToggleFS();
                break;
            }
            break;
        case SDL_EVENT_DROP_BEGIN:
        case SDL_EVENT_DROP_POSITION: dragOver = true; break;
        case SDL_EVENT_DROP_COMPLETE: dragOver = false; break;
        case SDL_EVENT_DROP_FILE:
            dragOver = false;
            if (e.drop.data && *e.drop.data)
                StartVerify(e.drop.data);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH)
            {
                if (settingsOpen)
                    settingsOpen = false;
                else if (phase == Phase::Ready)
                    BeginPlay();
                else
                    Browse();
            }
            else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
            {
                if (settingsOpen)
                    settingsOpen = false;
                else
                    running = false;
            }
            else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START)
            {
                settingsOpen = !settingsOpen;
            }
            break;
        }
    }

    void App::Click(float x, float y)
    {
        mouse = { x, y };
        for (auto it = hs.rbegin(); it != hs.rend(); ++it)
        {
            if (!Contains(it->rect, x, y))
                continue;
            if (!it->enabled && it->act == Act::Export)
            {
                Toast("Export for Xbox is coming in a later update.");
                return;
            }
            if (settingsOpen && it->act != Act::CloseSettings && it->act != Act::ToggleFS && it->act != Act::Browse &&
                it->act != Act::Quit)
                continue;
            switch (it->act)
            {
            case Act::Browse: Browse(); return;
            case Act::Play:
                if (phase == Phase::Ready)
                    BeginPlay();
                else
                    Browse();
                return;
            case Act::Change: Browse(); return;
            case Act::Settings: settingsOpen = true; return;
            case Act::CloseSettings: settingsOpen = false; return;
            case Act::Quit: running = false; return;
            case Act::ToggleFS: ToggleFS(); return;
            case Act::Export: Toast("Export for Xbox is coming in a later update."); return;
            case Act::Cancel:
                verify.Cancel();
                phase = Phase::Idle;
                phaseStart = now;
                srcPath.clear();
                srcSize = 0;
                Toast("Check cancelled.");
                return;
            case Act::GameSel:
                if (it->idx >= 0)
                {
                    auto cat = LauncherGameCatalog();
                    if ((size_t)it->idx < cat.size())
                    {
                        if (cat[it->idx].enabled)
                            Toast("Drop your disc image to add this game.");
                        else
                            Toast(std::string(cat[it->idx].name) + " is coming in a later update.");
                    }
                }
                return;
            default: return;
            }
        }
        // click on bottom drop area while idle
        if (!settingsOpen && phase == Phase::Idle && Contains(SDL_FRect{ ContentX, BarY, 608, BarH - 4 }, x, y))
            Browse();
        if (!settingsOpen && phase == Phase::Ready && Contains(SDL_FRect{ ContentX, BarY, 608, BarH - 4 }, x, y))
            Browse();
    }

    void App::Update(double t, float dt)
    {
        if (phase == Phase::Verifying)
        {
            float target = verify.P();
            dispProg += (target - dispProg) * std::min(1.0f, dt * 12.0f);
            if (target >= 1.0f)
                dispProg = 1.0f;
            if (verify.done.load() && !compSched)
            {
                compSched = true;
                compAt = t + 0.24;
            }
            if (compSched && t >= compAt)
                FinishVerify();
        }
        float tt = settingsOpen ? 1.0f : 0.0f;
        settingsT += (tt - settingsT) * std::min(1.0f, dt * 16.0f);
        if (settingsT < 0.001f)
            settingsT = 0;
        bool handc = false;
        for (const auto& h : hs)
        {
            if (Hover(h))
            {
                handc = true;
                break;
            }
        }
        if (hand && arrow)
            SDL_SetCursor(handc ? hand : arrow);
    }

    void App::StartVerify(const std::string& p)
    {
        verify.Cancel();
        compSched = false;
        dispProg = 0;
        phase = Phase::Verifying;
        phaseStart = now;
        srcPath = p;
        srcSize = 0;
        SDL_PathInfo info;
        if (SDL_GetPathInfo(p.c_str(), &info))
            srcSize = static_cast<uint64_t>(info.size);
        check = {};
        meta = {};
        iconTex = nullptr;
        logoTex = nullptr;
        heroTex = nullptr;
        verify.Start(p, game);
    }

    void App::FinishVerify()
    {
        compSched = false;
        check = verify.check;
        verifyMs = static_cast<long long>((NowSeconds() - phaseStart) * 1000.0);
        if (check.status == GameSourceStatus::Ok)
        {
            meta = verify.meta;
            if (!meta.iconPng.empty())
                iconTex = painter.LoadPng({ meta.iconPng.data(), meta.iconPng.size() });

            // Pull the game's own frontend textures from the verified ISO. They are decoded to
            // PNGs in the game's data folder (never shipped); failure just leaves the procedural art.
            assets.Init(srcPath, meta.titleId);
            DiscFileSystem disc;
            if (disc.Open(srcPath.c_str()))
                assets.ExtractFromDisc(disc, meta);
            logoTex = LoadCachedAsset(assets, LauncherAssets::Role::Logo, painter);
            heroTex = LoadCachedAsset(assets, LauncherAssets::Role::HeroBackground, painter);

            // Remember a verified disc immediately, even if the player closes the launcher without
            // pressing PLAY. The next start can go straight to the game.
            SettingSet(config, game.id, "iso", srcPath);
            Save();
            phase = Phase::Ready;
        }
        else
            phase = Phase::Failed;
        phaseStart = now;
    }

    void App::Browse()
    {
        std::string prev = srcPath.empty() ? SettingGet(config, game.id, "iso") : srcPath;
        std::string p = ChooseIsoDialog(win, game, prev);
        if (!p.empty())
            StartVerify(p);
    }

    void App::BeginPlay()
    {
        if (srcPath.empty())
            return;
        chosen = srcPath;
        SettingSet(config, game.id, "iso", chosen);
        Save();
        running = false;
    }

    void App::ToggleFS()
    {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(win, fullscreen);
        SettingSet(config, "launcher", "fullscreen", fullscreen ? "1" : "0");
        Save();
        Toast(fullscreen ? "Fullscreen enabled" : "Windowed mode");
    }

    void App::Save()
    {
        if (!SaveLauncherConfig(configPath, config))
            Toast("Couldn't save launcher settings");
    }

    void App::Toast(std::string m, double s)
    {
        toast = std::move(m);
        toastUntil = now + s;
    }

    void App::Cap(const std::string& n)
    {
        if (shotsDir.empty())
            return;
        int w = 0, h = 0;
        if (!SDL_GetRenderOutputSize(ren, &w, &h) || w <= 0 || h <= 0)
            return;
        SDL_Surface* surf = SDL_RenderReadPixels(ren, nullptr);
        if (!surf)
            return;
        SDL_Surface* conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surf);
        if (!conv)
            return;
        std::string path = shotsDir + "/" + n + ".png";
        stbi_flip_vertically_on_write(0);
        if (stbi_write_png(path.c_str(), conv->w, conv->h, 4, conv->pixels, conv->pitch))
            fprintf(stderr, "launcher: shot %s\n", path.c_str());
        SDL_DestroySurface(conv);
    }

    void App::PushDrop(const std::string& p)
    {
        SDL_Event ev{};
        ev.type = SDL_EVENT_DROP_FILE;
        ev.drop.windowID = SDL_GetWindowID(win);
        ev.drop.data = p.c_str();
        SDL_PushEvent(&ev);
    }

    void App::Test()
    {
        if (!testMode || tDone)
            return;
        double t = now - testStart;
        if (!tEmpty && t > 0.4)
        {
            Cap("01-no-iso");
            tEmpty = true;
        }
        if (!tDrag && t > 0.8)
        {
            dragOver = true;
            tDrag = true;
        }
        if (tDrag && !tHover && t > 1.05)
        {
            tHover = true;
            dragOver = false;
        }
        if (!testDrop.empty() && !tVer && t > testDropAt - testStart)
        {
            PushDrop(testDrop);
            tVer = true;
        }
        if (phase == Phase::Ready && !tSet && now - phaseStart > 0.8)
        {
            Cap("02-verified");
            settingsOpen = true;
            tSet = true;
        }
        if (phase == Phase::Ready && tSet && settingsT > 0.9 && now - phaseStart > 1.8)
        {
            Cap("03-settings");
            settingsOpen = false;
            tDone = true;
            running = false;
        }
        if (phase == Phase::Failed && !tReady && now - phaseStart > 0.5)
        {
            Cap("03-failed");
            tReady = true;
            tSet = true;
            tDone = true;
            running = false;
        }
    }

    void App::DrawPortalDecor(float cx, float cy, float r, float t)
    {
        painter.Glow(SDL_FRect{ cx - r * 1.6f, cy - r * 1.6f, r * 3.2f, r * 3.2f }, r * 1.6f, Portal, 0.5f);
        painter.Ring(cx, cy, r, 4, Alpha(Portal, 0.9f));
        painter.Ring(cx, cy, r * 0.72f, 2.6f, Alpha(Magic, 0.7f));
        painter.Ring(cx, cy, r * 0.48f, 2.0f, Alpha(Cyan, 0.6f));
        const SDL_Color orbits[] = { Fire, Orange, Gold, Life, Water, Air, Magic, Undead };
        for (int i = 0; i < 8; i++)
        {
            float ang = t * 0.28f + i * (6.283185f / 8.0f);
            float x = cx + std::cos(ang) * (r * 0.84f), y = cy + std::sin(ang) * (r * 0.84f);
            painter.Circle(x, y, 2.8f, Alpha(orbits[i], 0.9f));
            painter.Circle(x, y, 1.6f, Alpha(TextPrimary, 0.6f));
        }
        painter.Triangle(cx, cy - r * 0.26f, cx - r * 0.22f, cy + r * 0.18f, cx + r * 0.22f, cy + r * 0.18f,
            Alpha(TextPrimary, 0.92f));
    }

    void App::Draw()
    {
        hs.clear();
        painter.Background(static_cast<float>(now));
        // sidebar
        painter.RoundRect(SDL_FRect{ 0, 0, SidebarW, DesignH }, 0, Sidebar);
        painter.FillRect(SDL_FRect{ SidebarW - 1, 0, 1, DesignH }, Alpha(Border, 0.7f));
        // logo
        painter.Glow(SDL_FRect{ 26, 20, 40, 40 }, 20, Portal, 0.6f);
        painter.Ring(46, 40, 16, 3, Alpha(Portal, 0.95f));
        painter.Ring(46, 40, 10.5f, 2, Alpha(Magic, 0.7f));
        for (int i = 0; i < 6; i++)
        {
            float ang = now * 0.22f + i * 1.0472f;
            float x = 46 + std::cos(ang) * 12.8f, y = 40 + std::sin(ang) * 12.8f;
            painter.Circle(x, y, 1.8f, Alpha(Tech, 0.9f));
        }
        painter.Text("SKYLANDERS", 78, 24, 20, TextPrimary, true, 0.8f);
        painter.Text("RECOMPILED", 80, 52, 9, Alpha(Cyan, 0.95f), false, 4.6f);
        painter.Text("LAUNCHER", 82, 64, 8, TextDim, false, 3.4f);
        painter.FillRect(SDL_FRect{ 18, 92, SidebarW - 36, 1 }, Alpha(Border, 0.65f));

        widgets->Label("YOUR GAMES", 26, 112, TextDim);
        auto cat = LauncherGameCatalog();
        for (size_t i = 0; i < cat.size(); i++)
        {
            const auto& e = cat[i];
            SDL_FRect r{ 16, 142 + static_cast<float>(i) * 66, SidebarW - 32, 58 };
            SDL_Color ac = FromRgb(e.accent);
            bool sel = std::string(e.id) == game.id;
            bool hov = Hover(Hotspot{ r, Act::GameSel, (int)i, e.enabled });
            // adjust hover
            std::string st;
            if (!e.enabled)
                st = "COMING LATER";
            else if (phase == Phase::Verifying && sel)
                st = "CHECKING";
            else if (phase == Phase::Ready && sel)
                st = "READY";
            else if (phase == Phase::Failed && sel)
                st = "DISC NOT USABLE";
            else
                st = "ADD DISC";
            widgets->SidebarItem(r, e.name, st, ac, e.enabled, sel, hov);
            Add(r, Act::GameSel, (int)i, e.enabled);
        }
        // settings bottom
        SDL_FRect sr{ 16, DesignH - 86, SidebarW - 32, 48 };
        bool sh = false;
        for (auto& h : hs)
        {
            if (h.act == Act::Settings && Hover(h))
            {
                sh = true;
                break;
            }
        }
        // draw gear
        painter.RoundRect(sr, RadiusLg,
            settingsOpen ? Alpha(PanelRaised, 0.9f) : (sh ? Alpha(PanelRaised, 0.7f) : Alpha(Panel, 0.7f)));
        painter.RoundRectOutline(
            sr, RadiusLg, 1.2f, settingsOpen ? BorderBright : (sh ? BorderBright : Border), Alpha(Panel, 0));
        float gx = sr.x + 18, gy = sr.y + 24;
        for (int i = 0; i < 3; i++)
        {
            float y = gy - 7 + i * 7;
            painter.FillRect(
                SDL_FRect{ gx - 7, y, gx + 7 - (gx - 7), 1.8f }, sh || settingsOpen ? TextPrimary : TextSecondary);
            painter.Circle(gx + (i == 0      ? -3
                                    : i == 1 ? 2
                                             : -1),
                y, 2.2f, sh || settingsOpen ? TextPrimary : TextSecondary);
        }
        painter.Text("SETTINGS", sr.x + 38, sr.y + 16, 11,
            settingsOpen ? TextPrimary : (sh ? TextPrimary : TextSecondary), false, 1.6f);
        painter.Text("F1", sr.x + SidebarW - 52, sr.y + 18, 8.5f, TextDim, false, 1.8f);
        Add(sr, Act::Settings);
        painter.TextCentered("NATIVE PC PORT", SidebarW * 0.5f, DesignH - 28, 7.5f, TextDim, false, 2.2f);

        // content
        if (settingsOpen && settingsT > 0.001f)
            DrawSettings();
        else
        {
            DrawHero();
            DrawBottomBar();
        }
        DrawFooter();
        DrawToast();
    }

    void App::DrawHero()
    {
        SDL_FRect hero{ ContentX, HeroY, ContentW, HeroH };
        widgets->Panel(hero, RadiusXl, dragOver && phase == Phase::Idle, dragOver ? Cyan : Portal);
        // The player's own disc supplies this art; until an ISO is verified the panel stays procedural.
        if (heroTex != nullptr)
            painter.DrawTexture(
                heroTex, SDL_FRect{ hero.x + hero.w - 470, hero.y - 24, 494, 494 }, Alpha(Portal, 0.24f));
        if (logoTex != nullptr)
        {
            SDL_FRect art{ ContentRight - 290, HeroY + 270, 280, 140 };
            painter.Glow(art, RadiusXl, Gold, 0.30f);
            painter.DrawTexture(logoTex, art, Alpha(TextPrimary, 0.95f));
            painter.TextRight(
                "ART FROM YOUR DISC", art.x + art.w, art.y + art.h + 4, 10.0f, TextSecondary, false, 0.6f);
        }
        if (dragOver && phase == Phase::Idle)
            painter.DashedRoundRect(SDL_FRect{ hero.x + 10, hero.y + 10, hero.w - 20, hero.h - 20 }, RadiusXl - 4, 1.6f,
                Alpha(Cyan, 0.9f), 16, 10);
        // decor
        DrawPortalDecor(hero.x + hero.w - 132, hero.y + 44, 82, static_cast<float>(now));
        // icon
        SDL_FRect it{ hero.x + 44, hero.y + 70, 168, 168 };
        painter.Glow(it, RadiusXl, Cyan, phase == Phase::Ready ? 0.8f : 0.5f);
        painter.RoundRectGradient(it, RadiusXl, Alpha(Night, 0.98f), Alpha(Twilight, 0.98f));
        painter.RoundRectOutline(it, RadiusXl, 1.6f,
            Alpha(phase == Phase::Ready ? Life : Portal, phase == Phase::Ready ? 0.9f : 0.7f), Alpha(Panel, 0));
        if (iconTex && phase == Phase::Ready)
            painter.DrawTexture(iconTex, SDL_FRect{ it.x + 22, it.y + 22, it.w - 44, it.h - 44 }, TextPrimary);
        else
        {
            painter.Ring(it.x + 84, it.y + 84, 52, 4, Alpha(Portal, 0.9f));
            painter.Ring(it.x + 84, it.y + 84, 34, 3, Alpha(Magic, 0.7f));
            painter.Triangle(
                it.x + 84, it.y + 60, it.x + 66, it.y + 98, it.x + 102, it.y + 98, Alpha(TextPrimary, 0.95f));
        }
        // text
        widgets->Label("SELECTED GAME", ContentX + 244, HeroY + 70, TextDim);
        std::string title = Upper(game.name);
        float ts = 36;
        while (ts > 26 && painter.Measure(title, ts, true, 0.7f) > 680)
            ts -= 2;
        painter.Text(title, ContentX + 246, HeroY + 88, ts, TextPrimary, true, 0.7f);
        widgets->Label(phase == Phase::Ready ? "DISC VERIFIED · READY TO PLAY"
                                             : "DROP YOUR ISO TO VERIFY · RUNS FROM YOUR OWN DISC",
            ContentX + 248, HeroY + 142, TextSecondary);
        // chips
        float cx = ContentX + 248, cy = HeroY + 178;
        auto chip = [&](const std::string& t, SDL_Color c, bool f) {
            float w = painter.Measure(t, 9, 0, 1.8f) + 24;
            widgets->Chip(SDL_FRect{ cx, cy, w, 22 }, t, c, f);
            cx += w + 10;
        };
        chip(game.releases.empty() ? "USA/EUROPE" : Upper(game.releases[0].label), Gold, false);
        if (phase == Phase::Ready)
            chip("VERIFIED", Life, true);
        else if (phase == Phase::Verifying)
            chip("CHECKING", Orange, false);
        else if (phase == Phase::Failed)
            chip("NOT USABLE", Red, false);
        else
            chip("NO DISC", TextDim, false);
        if (srcSize > 0)
            chip(LauncherFormatSize(srcSize), TextDim, false);
        // description
        painter.Text("Magical cartoon adventure · Skylanders style · built from your own disc. No bundled artwork.",
            ContentX + 248, HeroY + 220, 12, TextSecondary);
        painter.Text("Design ready for Giants, Swap Force, Trap Team, SuperChargers, Imaginators.", ContentX + 248,
            HeroY + 248, 11, TextDim);
    }

    void App::DrawBottomBar()
    {
        SDL_FRect bar{ ContentX, BarY, ContentW, BarH };
        widgets->Panel(bar, RadiusXl);
        // drop/play area
        SDL_FRect drop{ ContentX + 16, BarY + 18, 616, BarH - 36 };
        bool dh = Hover(Hotspot{ drop, phase == Phase::Ready ? Act::Browse : Act::Browse, -1, true });
        if (dragOver && phase == Phase::Idle)
            painter.Glow(drop, RadiusLg, Cyan, 0.9f);
        painter.RoundRect(drop, RadiusLg, Alpha(PanelRaised, dh ? 0.95f : 0.78f));
        painter.RoundRectOutline(drop, RadiusLg, 1.4f,
            (dragOver && phase == Phase::Idle) ? Cyan : (dh ? BorderBright : Border), Alpha(Panel, 0));
        if (dragOver && phase == Phase::Idle)
            painter.DashedRoundRect(SDL_FRect{ drop.x + 8, drop.y + 8, drop.w - 16, drop.h - 16 }, RadiusLg - 4, 1.2f,
                Alpha(Cyan, 0.9f), 14, 8);
        // disc
        painter.Circle(drop.x + 38, drop.y + 34, 16, Alpha(Night, 0.9f));
        painter.Ring(drop.x + 38, drop.y + 34, 15, 2,
            Alpha(phase == Phase::Ready ? Life
                                        : (phase == Phase::Verifying ? Orange : (phase == Phase::Failed ? Red : Cyan)),
                0.9f));
        if (phase == Phase::Ready)
        {
            float d = 7;
            painter.FillRect(SDL_FRect{ drop.x + 38 - 2, drop.y + 34 - d / 2, 4, d }, Life);
            painter.FillRect(SDL_FRect{ drop.x + 38 - d / 2, drop.y + 34 - 2, d, 4 }, Life);
        }
        else if (phase == Phase::Verifying)
        {
            float a = now * 4;
            float x1 = drop.x + 38 + std::cos(a) * 9, y1 = drop.y + 34 + std::sin(a) * 9,
                  x2 = drop.x + 38 + std::cos(a + 2.09) * 9, y2 = drop.y + 34 + std::sin(a + 2.09) * 9,
                  x3 = drop.x + 38 + std::cos(a + 4.18) * 9, y3 = drop.y + 34 + std::sin(a + 4.18) * 9;
            painter.Circle(x1, y1, 2, Orange);
            painter.Circle(x2, y2, 2, Orange);
            painter.Circle(x3, y3, 2, Orange);
        }
        else if (phase == Phase::Failed)
        {
            float k = 6;
            painter.FillRect(
                SDL_FRect{ drop.x + 38 - k, drop.y + 34 - k, 2 * k, 2 * k }, Red); /* rough X via circles */
            for (int i = 0; i < 2; i++)
            {
                float ang = i * 1.5708f;
                painter.Circle(drop.x + 38 + std::cos(ang) * 8, drop.y + 34 + std::sin(ang) * 8, 1.6f, Red);
            }
        }
        else
        {
            painter.Ring(drop.x + 38, drop.y + 34, 9, 1.6f, Alpha(Cyan, 0.8f));
            painter.Circle(drop.x + 38, drop.y + 34, 3, Alpha(Gold, 0.9f));
        }
        if (phase == Phase::Idle)
        {
            painter.Text("DROP YOUR ISO HERE", drop.x + 72, drop.y + 18, 16, TextPrimary, true, 0.6f);
            painter.Text(dh ? "CLICK TO BROWSE · DRAG & DROP ANYWHERE" : "DRAG & DROP · OR CLICK BROWSE", drop.x + 74,
                drop.y + 44, 11, TextSecondary);
        }
        else if (phase == Phase::Verifying)
        {
            painter.Text("VERIFYING DISC", drop.x + 72, drop.y + 14, 14, TextPrimary, true, 0.5f);
            std::string st = verify.S();
            painter.Text(painter.Ellipsize(st, 320, 11), drop.x + 74, drop.y + 36, 11, Cyan);
            SDL_FRect pb{ drop.x + 74, drop.y + 54, 520, 8 };
            widgets->ProgressBar(pb, dispProg, Cyan, Magic);
            painter.TextRight(fmt::format("{}%", (int)(dispProg * 100)), pb.x + pb.w, drop.y + 52, 9, TextSecondary);
        }
        else if (phase == Phase::Ready)
        {
            std::string nm = srcPath.empty() ? "verified.iso" : FileNameOnly(srcPath);
            painter.Text(painter.Ellipsize(nm, 420, 14), drop.x + 72, drop.y + 18, 14, TextPrimary, true, 0.4f);
            painter.Text(fmt::format("Verified · {} · Saved to launcher.ini", LauncherFormatSize(srcSize)), drop.x + 74,
                drop.y + 42, 11, Life);
        }
        else if (phase == Phase::Failed)
        {
            painter.Text("DISC CHECK FAILED", drop.x + 72, drop.y + 18, 14, TextPrimary, true, 0.4f);
            std::string m = check.message.empty() ? "Try another image" : check.message;
            painter.Text(painter.Ellipsize(m, 440, 10), drop.x + 74, drop.y + 42, 10, TextSecondary);
        }
        Add(drop, Act::Browse);
        if (phase == Phase::Failed)
        {
            SDL_FRect cb{ drop.x + drop.w - 130, drop.y + 18, 112, 34 };
            widgets->Button(
                cb, "TRY AGAIN", ButtonStyle::Accent, true, Hover(Hotspot{ cb, Act::Browse, -1, true }), 10.5f);
            Add(cb, Act::Browse);
        }
        // play
        SDL_FRect pb{ ContentRight - 264, BarY + 16, 248, PlayButtonH };
        bool pen = phase == Phase::Ready;
        widgets->Button(
            pb, pen ? "PLAY" : "ADD DISC", ButtonStyle::Primary, pen, Hover(Hotspot{ pb, Act::Play, -1, pen }), 18.0f);
        if (!pen)
            painter.TextCentered("Click to choose an ISO", pb.x + pb.w * 0.5f, pb.y + pb.h + 6, 9, TextDim);
        else
            painter.TextCentered("Enter / click to launch", pb.x + pb.w * 0.5f, pb.y + pb.h + 6, 9, Alpha(Life, 0.9f));
        Add(pb, Act::Play, -1, true);
    }

    void App::DrawFooter()
    {
        painter.FillRect(SDL_FRect{ ContentX, FooterY, ContentW, 1 }, Alpha(Border, 0.7f));
        painter.FillRect(SDL_FRect{ ContentX, FooterY, 140, 1 }, Alpha(Portal, 0.9f));
        widgets->Label("SKYLANDERS RECOMPILED · PORTAL LAUNCHER", ContentX, FooterY + 14, TextDim);
        std::string iso = SettingGet(config, game.id, "iso");
        if (!iso.empty())
            painter.Text("ISO: " + LauncherMiddleEllipsis(iso, 68), ContentX, FooterY + 34, 10, TextDim);
        painter.TextRight("F1  SETTINGS", ContentRight, FooterY + 14, 9, TextDim);
        painter.TextRight("HOLD SHIFT / --LAUNCHER TO OPEN AGAIN", ContentRight, FooterY + 34, 8.5f, TextDim);
        // export
        SDL_FRect ex{ ContentRight - 210, FooterY + 16, 194, 38 };
        painter.RoundRect(ex, RadiusLg, Alpha(Panel, 0.6f));
        painter.RoundRectOutline(ex, RadiusLg, 1.1f, Border, Alpha(Panel, 0));
        painter.TextCentered("EXPORT FOR XBOX", ex.x + ex.w * 0.5f - 16, ex.y + 12, 9, TextDim);
        SDL_FRect bd{ ex.x + ex.w - 52, ex.y + 10, 42, 18 };
        painter.RoundRect(bd, RadiusSm, Alpha(Gold, 0.12f));
        painter.RoundRectOutline(bd, RadiusSm, 1, Alpha(Gold, 0.4f), Alpha(Panel, 0));
        painter.TextCentered("SOON", bd.x + 21, bd.y + 5, 7.5f, Alpha(Gold, 0.9f));
        Add(ex, Act::Export, -1, false);
    }

    void App::DrawSettings()
    {
        // header
        widgets->Label("SETTINGS", ContentX, HeroY + 6, TextDim);
        painter.Text("Tweak language, display and game data.", ContentX, HeroY + 28, 12, TextSecondary);
        SDL_FRect back{ ContentRight - 96, HeroY, 80, 34 };
        widgets->Button(
            back, "BACK", ButtonStyle::Secondary, true, Hover(Hotspot{ back, Act::CloseSettings, -1, true }), 10.5f);
        Add(back, Act::CloseSettings);
        // cards left
        SDL_FRect c1{ ContentX, HeroY + 64, 460, 210 };
        widgets->Panel(c1);
        painter.Text("LANGUAGE", c1.x + 18, c1.y + 14, 11, TextPrimary, true, 0.8f);
        painter.Text("From launcher.ini · keys language / voice_language", c1.x + 20, c1.y + 38, 10, TextDim);
        // language row
        SDL_FRect lr{ c1.x + 16, c1.y + 64, c1.w - 32, 44 };
        painter.RoundRect(lr, RadiusLg, Alpha(PanelRaised, 0.7f));
        painter.RoundRectOutline(lr, RadiusLg, 1, Border, Alpha(Panel, 0));
        painter.Text("Text Language", lr.x + 16, lr.y + 14, 11, TextSecondary);
        painter.TextRight(LauncherLanguageLabel(config, game), lr.x + lr.w - 18, lr.y + 16, 11, TextPrimary);
        SDL_FRect vr{ c1.x + 16, c1.y + 116, c1.w - 32, 44 };
        painter.RoundRect(vr, RadiusLg, Alpha(PanelRaised, 0.7f));
        painter.RoundRectOutline(vr, RadiusLg, 1, Border, Alpha(Panel, 0));
        painter.Text("Voice Language", vr.x + 16, vr.y + 14, 11, TextSecondary);
        painter.TextRight(LauncherVoiceLabel(config, game), vr.x + vr.w - 18, vr.y + 16, 11, TextPrimary);
        painter.Text("Cycle with the arrows in a future update · values saved to launcher.ini", c1.x + 18, c1.y + 178,
            9.5f, TextDim);

        SDL_FRect c2{ ContentX, HeroY + 290, 460, 174 };
        widgets->Panel(c2);
        painter.Text("DISPLAY", c2.x + 18, c2.y + 14, 11, TextPrimary, true, 0.8f);
        painter.Text("Windowing and rendering", c2.x + 20, c2.y + 38, 10, TextDim);
        SDL_FRect fr{ c2.x + 16, c2.y + 64, c2.w - 32, 44 };
        painter.RoundRect(fr, RadiusLg, Alpha(PanelRaised, 0.7f));
        painter.RoundRectOutline(fr, RadiusLg, 1, Border, Alpha(Panel, 0));
        painter.Text("Fullscreen", fr.x + 16, fr.y + 14, 11, TextSecondary);
        SDL_FRect tg{ fr.x + fr.w - 72, fr.y + 10, 56, 24 };
        widgets->Toggle(tg, fullscreen, true, Hover(Hotspot{ tg, Act::ToggleFS, -1, true }));
        Add(tg, Act::ToggleFS);
        SDL_FRect rr{ c2.x + 16, c2.y + 114, c2.w - 32, 44 };
        painter.RoundRect(rr, RadiusLg, Alpha(Panel, 0.6f));
        painter.RoundRectOutline(rr, RadiusLg, 1, Border, Alpha(Panel, 0));
        painter.Text("Resolution Scale", rr.x + 16, rr.y + 14, 11, TextDim);
        painter.TextRight("100%", rr.x + rr.w - 70, rr.y + 16, 11, TextDim);
        SDL_FRect sb{ rr.x + rr.w - 56, rr.y + 12, 42, 20 };
        painter.RoundRect(sb, RadiusSm, Alpha(Gold, 0.12f));
        painter.RoundRectOutline(sb, RadiusSm, 1, Alpha(Gold, 0.4f), Alpha(Panel, 0));
        painter.TextCentered("SOON", sb.x + 21, sb.y + 5, 7.5f, Alpha(Gold, 0.9f));

        // right
        SDL_FRect c3{ ContentX + 476, HeroY + 64, ContentW - 476, 242 };
        widgets->Panel(c3);
        painter.Text("GAME DATA", c3.x + 18, c3.y + 14, 11, TextPrimary, true, 0.8f);
        painter.Text("Your ISO stays local · saved in launcher.ini", c3.x + 20, c3.y + 38, 10, TextDim);
        painter.Text("Current ISO", c3.x + 20, c3.y + 74, 10, TextDim);
        std::string iso = SettingGet(config, game.id, "iso");
        painter.Text(painter.Ellipsize(iso.empty() ? "(none selected)" : iso, c3.w - 40, 11), c3.x + 20, c3.y + 94, 11,
            iso.empty() ? TextDim : TextSecondary);
        painter.Text(fmt::format("Status: {}",
                         phase == Phase::Ready
                             ? "Verified and ready"
                             : (phase == Phase::Verifying ? "Verifying"
                                                          : (phase == Phase::Failed ? "Failed check" : "Not checked"))),
            c3.x + 20, c3.y + 122, 11, phase == Phase::Ready ? Life : (phase == Phase::Failed ? Red : TextDim));
        SDL_FRect ch{ c3.x + 18, c3.y + 150, 190, ButtonH };
        widgets->Button(ch, "CHANGE IMAGE", ButtonStyle::Accent, true, Hover(Hotspot{ ch, Act::Browse, -1, true }), 11);
        Add(ch, Act::Browse);
        SDL_FRect qp{ c3.x + 222, c3.y + 150, 190, ButtonH };
        widgets->Button(qp, "QUIT", ButtonStyle::Secondary, true, Hover(Hotspot{ qp, Act::Quit, -1, true }), 11);
        Add(qp, Act::Quit);

        SDL_FRect c4{ ContentX + 476, HeroY + 322, ContentW - 476, 142 };
        widgets->Panel(c4);
        painter.Text("EXPORT FOR XBOX", c4.x + 18, c4.y + 14, 11, TextPrimary, true, 0.8f);
        painter.Text("Produce an installable .msix with your own local build pipeline (disabled).", c4.x + 20,
            c4.y + 38, 10, TextDim);
        SDL_FRect ex2{ c4.x + 18, c4.y + 72, 210, ButtonH };
        widgets->Button(ex2, "EXPORT FOR XBOX", ButtonStyle::Secondary, false, false, 11);
        Add(ex2, Act::Export, -1, false);

        painter.Text("Settings file: " + LauncherMiddleEllipsis(configPath, 92), ContentX, HeroY + 486, 9.5f, TextDim);
        painter.Text("All colours, sizes and fonts are in host/launcher_theme.h · widgets in host/widgets/", ContentX,
            HeroY + 506, 9.5f, TextDim);
    }

    void App::DrawToast()
    {
        if (toast.empty() || now >= toastUntil)
            return;
        float a = std::clamp(static_cast<float>((toastUntil - now) / 0.4), 0.0f, 1.0f);
        float w = painter.Measure(toast, 12) + 56;
        SDL_FRect r{ DesignW * 0.5f - w * 0.5f, 12, w, 38 };
        painter.RoundRect(r, RadiusLg, Alpha(SDL_Color{ 20, 28, 58, 255 }, 0.98f * a));
        painter.RoundRectOutline(r, RadiusLg, 1, Alpha(Cyan, 0.4f * a), Alpha(Panel, 0));
        painter.Circle(r.x + 20, r.y + 19, 3.5f, Alpha(Life, a));
        painter.Text(toast, r.x + 34, r.y + 11, 12, Alpha(TextPrimary, a));
    }
}

std::span<const LauncherGameEntry> LauncherGameCatalog()
{
    static const LauncherGameEntry cat[] = {
        { "ssa", "Spyro's Adventure", "Spyro's Adventure", 0x4AE3FF, true },
        { "giants", "Giants", "Giants", 0xFFC65C, false },
        { "swapforce", "Swap Force", "Swap Force", 0xFF55A8, false },
        { "trapteam", "Trap Team", "Trap Team", 0x6DF0B4, false },
        { "superchargers", "SuperChargers", "SuperChargers", 0xFF8A5C, false },
        { "imaginators", "Imaginators", "Imaginators", 0xB49BFF, false },
    };
    return cat;
}

std::string LauncherLanguageLabel(const LauncherConfig& config, const GameInfo& game)
{
    std::string v = SettingGet(config, "launcher", "language");
    if (v.empty())
        v = SettingGet(config, game.id, "language");
    if (v.empty())
        v = SettingGet(config, "", "language");
    if (v.empty())
        return "English";
    return v;
}

std::string LauncherVoiceLabel(const LauncherConfig& config, const GameInfo& game)
{
    std::string v = SettingGet(config, "launcher", "voice_language");
    if (v.empty())
        v = SettingGet(config, game.id, "voice_language");
    if (v.empty())
        v = SettingGet(config, "", "voice_language");
    if (v.empty())
        return "Same as text";
    return v;
}

bool LauncherFullscreen(const LauncherConfig& config)
{
    std::string v = SettingGet(config, "launcher", "fullscreen");
    if (v.empty())
        v = SettingGet(config, "", "fullscreen");
    return ParseBool(v);
}

std::string LauncherFormatSize(uint64_t bytes)
{
    constexpr double KB = 1024, MB = KB * 1024, GB = MB * 1024;
    if (bytes >= (uint64_t)GB)
        return fmt::format("{:.1f} GB", double(bytes) / GB);
    if (bytes >= (uint64_t)MB)
        return fmt::format("{:.0f} MB", double(bytes) / MB);
    if (bytes >= (uint64_t)KB)
        return fmt::format("{:.0f} KB", double(bytes) / KB);
    return fmt::format("{} bytes", bytes);
}

std::string LauncherMiddleEllipsis(std::string_view text, size_t max)
{
    if (text.size() <= max)
        return std::string(text);
    if (max <= 3)
        return std::string(text.substr(0, max));
    std::vector<size_t> b{ 0 };
    size_t p = 0;
    while (p < text.size())
    {
        unsigned char f = (unsigned char)text[p];
        size_t len = f < 0x80 ? 1 : (f >> 5) == 0x6 ? 2 : (f >> 4) == 0xE ? 3 : 4;
        p = std::min(text.size(), p + len);
        b.push_back(p);
    }
    if (b.size() - 1 <= max)
        return std::string(text);
    size_t k = max - 3, head = (k + 1) / 2, tail = k / 2;
    std::string r(text.substr(0, b[head]));
    r += "...";
    r += text.substr(b[b.size() - 1 - tail]);
    return r;
}

std::string LauncherDisplayName(std::string_view x, const GameInfo& game)
{
    return x.empty() ? std::string(game.name) : std::string(x);
}

std::string RunLauncherWindow(
    const GameInfo& game, const std::string& configPath, LauncherConfig& config, const std::string& previous)
{
    App app(game, configPath, config);
    if (!previous.empty())
        SettingSet(config, game.id, "iso", previous);
    return app.Run();
}
