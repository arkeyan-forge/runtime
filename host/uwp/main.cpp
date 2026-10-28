// UWP entry point (Xbox Dev Mode; also runs as a sideloaded Windows app): a CoreApplication view running the same
// runtime as main.cpp. The game's frames go to a D3D12 swap chain on the CoreWindow through gpu/ (host/display.cpp),
// the gamepad comes from Windows.Gaming.Input, sound goes out through XAudio2 (host/audio_xaudio2.cpp). The disc
// image is read from the package's install folder; saves, figure dumps and the log (SSARecomp.log) are in LocalState.
// Raw WinRT ABI through the mingw-w64 headers (no C++/WinRT, no Windows SDK), as in tools/xbox/testapp.

// mingw-w64 (widl) declares IReference<boolean> and IReference<BYTE>, both IReference<unsigned char>, which C++
// rejects as a redefinition. Nothing here uses IReference<boolean>, so skip that declaration.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include "host/audio.h"
#include "host/display.h"
#include "host/input.h"
#include "host/portal.h"
#include "host/toybox.h"
#include "host/uwp/uwp.h"
#include "loader.h"
#include "memory.h"
#include "platform/platform.h"
#include "thread.h"
#include "vfs.h"
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.applicationmodel.core.h>
#include <windows.gaming.input.h>
#include <windows.graphics.display.h>
#include <windows.storage.h>
#include <windows.system.profile.h>
#include <windows.ui.core.h>
#include "host/uwp/winrt_extra.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "../env.h"

using namespace ABI::Windows;
using ApplicationModel::Core::ICoreApplication;
using ApplicationModel::Core::ICoreApplicationView;
using ApplicationModel::Core::IFrameworkView;
using ApplicationModel::Core::IFrameworkViewSource;
using UI::Core::ICoreWindow;

namespace
{
    template <typename T>
    class Ref
    {
    public:
        Ref() = default;
        Ref(const Ref&) = delete;
        Ref& operator=(const Ref&) = delete;
        ~Ref() { Reset(); }

        void Reset()
        {
            if (m_ptr != nullptr)
            {
                m_ptr->Release();
                m_ptr = nullptr;
            }
        }
        T** Put()
        {
            Reset();
            return &m_ptr;
        }
        void** PutVoid() { return reinterpret_cast<void**>(Put()); }
        T* Get() const { return m_ptr; }
        T* operator->() const { return m_ptr; }
        explicit operator bool() const { return m_ptr != nullptr; }

        template <typename U>
        HRESULT As(Ref<U>& out) const
        {
            return m_ptr->QueryInterface(__uuidof(U), out.PutVoid());
        }

    private:
        T* m_ptr = nullptr;
    };

    class HString
    {
    public:
        explicit HString(const wchar_t* text) { WindowsCreateString(text, UINT32(wcslen(text)), &m_value); }
        HString(const HString&) = delete;
        HString& operator=(const HString&) = delete;
        ~HString() { WindowsDeleteString(m_value); }
        operator HSTRING() const { return m_value; }

    private:
        HSTRING m_value = nullptr;
    };

    template <typename T>
    HRESULT GetFactory(const wchar_t* className, Ref<T>& out)
    {
        HString name(className);
        return RoGetActivationFactory(name, __uuidof(T), out.PutVoid());
    }

    std::string Utf8(const std::wstring& text)
    {
        if (text.empty())
            return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
        std::string result(size_t(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    std::wstring LocalStatePath()
    {
        Ref<Storage::IApplicationDataStatics> statics;
        Ref<Storage::IApplicationData> data;
        Ref<Storage::IStorageFolder> folder;
        Ref<Storage::IStorageItem> item;
        HSTRING path = nullptr;
        if (FAILED(GetFactory(L"Windows.Storage.ApplicationData", statics)) || FAILED(statics->get_Current(data.Put()))
            || FAILED(data->get_LocalFolder(folder.Put())) || FAILED(folder.As(item)) || FAILED(item->get_Path(&path)))
            return {};
        UINT32 length = 0;
        const wchar_t* text = WindowsGetStringRawBuffer(path, &length);
        std::wstring result(text, length);
        WindowsDeleteString(path);
        return result;
    }

    // The collection shared by every Skylanders app of the same publisher (docs/runtime/host/collection.md): the publisher cache
    // folder "Skylanders" (declared in AppxManifest.xml), else empty.
    std::wstring PublisherFolder()
    {
        Ref<Storage::IApplicationDataStatics> statics;
        Ref<Storage::IApplicationData> data;
        Ref<Storage::IApplicationData3> shared;
        Ref<Storage::IStorageFolder> folder;
        Ref<Storage::IStorageItem> item;
        HString name(L"Skylanders");
        HSTRING path = nullptr;
        if (FAILED(GetFactory(L"Windows.Storage.ApplicationData", statics)) || FAILED(statics->get_Current(data.Put()))
            || FAILED(data.As(shared)) || FAILED(shared->GetPublisherCacheFolder(name, folder.Put())) || FAILED(folder.As(item))
            || FAILED(item->get_Path(&path)))
            return {};
        UINT32 length = 0;
        const wchar_t* text = WindowsGetStringRawBuffer(path, &length);
        std::wstring result(text, length);
        WindowsDeleteString(path);
        return result;
    }

    std::wstring InstallFolder()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring folder = path;
        return folder.substr(0, folder.find_last_of(L'\\'));
    }

    // The default resolution scale: 2 on Xbox Series X|S, 1 on Xbox One (and on a PC, where DeviceForm is not "Xbox").
    uint32_t DefaultScale()
    {
        Ref<System::Profile::IAnalyticsInfoStatics> analytics;
        HSTRING form = nullptr;
        if (FAILED(GetFactory(L"Windows.System.Profile.AnalyticsInfo", analytics)) || FAILED(analytics->get_DeviceForm(&form)))
            return 1;
        UINT32 length = 0;
        std::wstring text(WindowsGetStringRawBuffer(form, &length));
        WindowsDeleteString(form);
        fprintf(stderr, "[uwp] device form %s\n", Utf8(text).c_str());
        return text.find(L"Xbox Series") != std::wstring::npos ? 2 : 1;
    }

    // The disc image the package carries (tools/xbox/package.sh names it game.iso), else any .iso next to it.
    std::wstring FindDiscImage(const std::wstring& folder)
    {
        std::wstring preferred = folder + L"\\game.iso";
        if (GetFileAttributesW(preferred.c_str()) != INVALID_FILE_ATTRIBUTES)
            return preferred;
        WIN32_FIND_DATAW data;
        HANDLE find = FindFirstFileExW((folder + L"\\*.iso").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE)
            return {};
        FindClose(find);
        return folder + L"\\" + data.cFileName;
    }

    void OnFault(const char* description, const void* hostAddress)
    {
        auto* address = static_cast<const uint8_t*>(hostAddress);
        if (address >= g_memory.base && address < g_memory.base + 0x100000000ull)
            fprintf(stderr, "\nFault (%s) at guest address 0x%08X\n", description, uint32_t(address - g_memory.base));
        else
            fprintf(stderr, "\nFault (%s) at host address %p\n", description, hostAddress);
        PrintGuestLocation();
    }

    int16_t Axis(double value)
    {
        return int16_t(std::lround(std::clamp(value, -1.0, 1.0) * 32767.0));
    }

    GamepadState ReadGamepad(Gaming::Input::IGamepadStatics* gamepads)
    {
        GamepadState state;
        Ref<Foundation::Collections::IVectorView<Gaming::Input::Gamepad*>> list;
        Ref<Gaming::Input::IGamepad> gamepad;
        UINT32 count = 0;
        Gaming::Input::GamepadReading reading{};
        if (gamepads == nullptr || FAILED(gamepads->get_Gamepads(list.Put())) || FAILED(list->get_Size(&count)) || count == 0
            || FAILED(list->GetAt(0, gamepad.Put())) || FAILED(gamepad->GetCurrentReading(&reading)))
            return state;

        using namespace Gaming::Input;
        static const std::pair<GamepadButtons, uint16_t> buttons[] = {
            { GamepadButtons_DPadUp, Button::DpadUp }, { GamepadButtons_DPadDown, Button::DpadDown },
            { GamepadButtons_DPadLeft, Button::DpadLeft }, { GamepadButtons_DPadRight, Button::DpadRight },
            { GamepadButtons_Menu, Button::Start }, { GamepadButtons_View, Button::Back },
            { GamepadButtons_LeftThumbstick, Button::LeftThumb }, { GamepadButtons_RightThumbstick, Button::RightThumb },
            { GamepadButtons_LeftShoulder, Button::LeftShoulder }, { GamepadButtons_RightShoulder, Button::RightShoulder },
            { GamepadButtons_A, Button::A }, { GamepadButtons_B, Button::B }, { GamepadButtons_X, Button::X },
            { GamepadButtons_Y, Button::Y },
        };
        for (const auto& [gamepadButton, button] : buttons)
        {
            if (unsigned(reading.Buttons) & unsigned(gamepadButton))
                state.buttons |= button;
        }
        state.thumbLX = Axis(reading.LeftThumbstickX);
        state.thumbLY = Axis(reading.LeftThumbstickY);
        state.thumbRX = Axis(reading.RightThumbstickX);
        state.thumbRY = Axis(reading.RightThumbstickY);
        state.leftTrigger = uint8_t(std::lround(std::clamp(reading.LeftTrigger, 0.0, 1.0) * 255.0));
        state.rightTrigger = uint8_t(std::lround(std::clamp(reading.RightTrigger, 0.0, 1.0) * 255.0));
        return state;
    }

    // COM objects the system calls back into.
    template <typename Interface>
    class Unknown : public Interface
    {
    public:
        virtual ~Unknown() = default;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (iid == __uuidof(Interface) || iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject)
                || (std::is_base_of_v<IInspectable, Interface> && iid == __uuidof(IInspectable)))
            {
                *out = static_cast<Interface*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG refs = --m_refs;
            if (refs == 0)
                delete this;
            return refs;
        }

    private:
        std::atomic<ULONG> m_refs{ 1 };
    };

    template <typename Interface>
    class Inspectable : public Unknown<Interface>
    {
    public:
        HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override
        {
            *count = 0;
            *iids = nullptr;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override
        {
            *name = nullptr;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override
        {
            *level = BaseTrust;
            return S_OK;
        }
    };

    std::atomic<bool> g_closed;

    class ClosedHandler final : public Unknown<Foundation::ITypedEventHandler<UI::Core::CoreWindow*, UI::Core::CoreWindowEventArgs*>>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(ICoreWindow*, UI::Core::ICoreWindowEventArgs*) override
        {
            g_closed = true;
            return S_OK;
        }
    };

    // On Xbox an unhandled back request (gamepad B) suspends the app to Home; B is a game button, so take it.
    class BackHandler final : public Unknown<UI::Core::IBackRequestedEventHandler>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, UI::Core::IBackRequestedEventArgs* args) override
        {
            args->put_Handled(TRUE);
            return S_OK;
        }
    };

    // The view's Activated event (dispatched by ProcessEvents in Run): the window is activated there, as UWP apps must,
    // which takes the system's splash screen down and lets the CoreWindow's swap chain reach the display.
    std::atomic<bool> g_activated;

    class ActivatedHandler final : public Unknown<Foundation::ITypedEventHandler<ApplicationModel::Core::CoreApplicationView*,
        ApplicationModel::Activation::IActivatedEventArgs*>>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(ICoreApplicationView* view, ApplicationModel::Activation::IActivatedEventArgs* args) override
        {
            ApplicationModel::Activation::ActivationKind kind{};
            if (args != nullptr)
                args->get_Kind(&kind);
            Ref<ICoreWindow> window;
            if (view != nullptr && SUCCEEDED(view->get_CoreWindow(window.Put())) && window)
                window->Activate();
            fprintf(stderr, "[uwp] view activated (kind %d)\n", int(kind));
            g_activated = true;
            return S_OK;
        }
    };

    class VisibilityHandler final : public Unknown<Foundation::ITypedEventHandler<UI::Core::CoreWindow*, UI::Core::VisibilityChangedEventArgs*>>
    {
    public:
        HRESULT STDMETHODCALLTYPE Invoke(ICoreWindow*, UI::Core::IVisibilityChangedEventArgs* args) override
        {
            boolean visible = false;
            if (args != nullptr)
                args->get_Visible(&visible);
            fprintf(stderr, "[uwp] window %s\n", visible ? "visible" : "hidden");
            return S_OK;
        }
    };

    class View final : public Inspectable<IFrameworkView>
    {
    public:
        HRESULT STDMETHODCALLTYPE Initialize(ICoreApplicationView* view) override
        {
            EventRegistrationToken token;
            auto* activated = new ActivatedHandler();
            HRESULT result = view->add_Activated(activated, &token);
            activated->Release();
            if (FAILED(result))
                fprintf(stderr, "[uwp] cannot subscribe to Activated: 0x%08lX\n", static_cast<unsigned long>(result));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetWindow(ICoreWindow* window) override
        {
            m_window = window;
            m_window->AddRef();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE Load(HSTRING) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE Uninitialize() override { return S_OK; }

        HRESULT STDMETHODCALLTYPE Run() override
        {
            EventRegistrationToken token;
            auto* closed = new ClosedHandler();
            m_window->add_Closed(closed, &token);
            closed->Release();

            Ref<UI::Core::ISystemNavigationManagerStatics> navigationStatics;
            Ref<UI::Core::ISystemNavigationManager> navigation;
            if (SUCCEEDED(GetFactory(L"Windows.UI.Core.SystemNavigationManager", navigationStatics))
                && SUCCEEDED(navigationStatics->GetForCurrentView(navigation.Put())))
            {
                auto* back = new BackHandler();
                navigation->add_BackRequested(back, &token);
                back->Release();
            }
            else
            {
                fprintf(stderr, "[uwp] SystemNavigationManager unavailable: B may send the app to Home\n");
            }

            auto* visibility = new VisibilityHandler();
            m_window->add_VisibilityChanged(visibility, &token);
            visibility->Release();

            // Wait for the Activated event (it activates the window) before the heavy work; activate it ourselves if
            // it does not come.
            Ref<UI::Core::ICoreDispatcher> dispatcher;
            m_window->get_Dispatcher(dispatcher.Put());
            for (int waited = 0; !g_activated && waited < 5000; waited += 10)
            {
                dispatcher->ProcessEvents(UI::Core::CoreProcessEventsOption_ProcessAllIfPresent);
                if (!g_activated)
                    Sleep(10);
            }
            if (!g_activated)
            {
                fprintf(stderr, "[uwp] no Activated event after 5 s: activating the window anyway\n");
                m_window->Activate();
            }
            UINT32 width, height;
            SwapChainSize(width, height);
            UwpSetCoreWindow(reinterpret_cast<IUnknown*>(m_window), width, height);

            // The swap chain first, and a black frame at once: the system's splash screen stays up until the app
            // presents (the disc image and the game take a few seconds more).
            UwpApplySettings(Utf8(LocalStatePath()) + "\\", DefaultScale());
            SDL_Window* window = CreateDisplayWindow("Skylanders: Spyro's Adventure", 1280, 720);
            ServiceGamePresents(0); // this (UI) thread issues every Present from now on: the console shows no other
            PresentDisplayFrame();

            std::wstring disc = FindDiscImage(InstallFolder());
            if (disc.empty())
            {
                fprintf(stderr, "[uwp] no disc image (game.iso) in %s\n", Utf8(InstallFolder()).c_str());
                return S_OK;
            }
            std::string discPath = Utf8(disc);
            fprintf(stderr, "[uwp] disc image %s, swap chain %ux%u\n", discPath.c_str(), width, height);

            PlatformInit();
            PlatformInstallFaultHandler(OnFault);
            if (!g_disc.Open(discPath.c_str()))
            {
                fprintf(stderr, "%s is not an Xbox 360 game disc image\n", discPath.c_str());
                return S_OK;
            }
            g_memory.Reserve();
            g_module = LoadXex(g_disc.ReadAll("default.xex"));

            HostAudioInit();
            // Figures: the publisher's shared collection, with LocalState\figures (older builds) copied into it once;
            // LocalState\figures itself when the publisher folder is unavailable.
            if (skyenv::Get("SKY_COLLECTION") == nullptr)
            {
                std::string localFigures = Utf8(LocalStatePath()) + "\\figures";
                std::wstring publisher = PublisherFolder();
                _putenv_s("SKY_COLLECTION", publisher.empty() ? localFigures.c_str() : (Utf8(publisher) + "\\Collection").c_str());
                if (!publisher.empty())
                    _putenv_s("SKY_MIGRATE_FROM", localFigures.c_str());
                fprintf(stderr, "[uwp] figure collection %s\n", skyenv::Get("SKY_COLLECTION"));
            }
            PortalInit();
            ToyboxInit(window);
            auto mainThread = CreateGuestThread(g_module.entryPoint, 0, 0, g_module.stackSize, false, 0);

            Ref<Gaming::Input::IGamepadStatics> gamepads;
            if (FAILED(GetFactory(L"Windows.Gaming.Input.Gamepad", gamepads)))
                fprintf(stderr, "[uwp] Windows.Gaming.Input unavailable: no gamepad\n");

            while (!g_closed)
            {
                {
                    std::lock_guard lock(g_kernelLock);
                    if (mainThread->exited)
                        break;
                }
                dispatcher->ProcessEvents(UI::Core::CoreProcessEventsOption_ProcessAllIfPresent);
                // The title gets no input while the Toy Box (View + Y) or the wheel (View held) is open.
                GamepadState state = ReadGamepad(gamepads.Get());
                ToyboxFilterGamepad(state);
                SetHostGamepad(state);
                ToyboxUpdate();
                // Movies and the first frames are presented here; once the game renders, its frames are presented
                // here too (gpu::ServicePresents), as Microsoft's template presents from the UI thread.
                if (IsGameRendering())
                    ServiceGamePresents(16);
                else
                    PresentDisplayFrame(); // paced by vsync
            }

            if (g_closed)
                std::_Exit(0);
            fprintf(stderr, "Main thread exited with 0x%08X\n", mainThread->exitCode);
            return S_OK;
        }

    private:
        // The window's bounds are in view pixels: scaled by the display's DPI (200% on Xbox, 960x540 -> 1920x1080).
        void SwapChainSize(UINT32& width, UINT32& height)
        {
            Foundation::Rect bounds{};
            m_window->get_Bounds(&bounds);
            FLOAT dpi = 96.0f;
            Ref<Graphics::Display::IDisplayInformationStatics> displayStatics;
            Ref<Graphics::Display::IDisplayInformation> display;
            if (SUCCEEDED(GetFactory(L"Windows.Graphics.Display.DisplayInformation", displayStatics))
                && SUCCEEDED(displayStatics->GetForCurrentView(display.Put())))
                display->get_LogicalDpi(&dpi);
            width = UINT32(std::lround(bounds.Width * dpi / 96.0f));
            height = UINT32(std::lround(bounds.Height * dpi / 96.0f));
            if (width == 0 || height == 0)
            {
                width = 1920;
                height = 1080;
            }
        }

        ICoreWindow* m_window = nullptr;
    };

    class ViewSource final : public Inspectable<IFrameworkViewSource>
    {
    public:
        HRESULT STDMETHODCALLTYPE CreateView(IFrameworkView** view) override
        {
            *view = new View();
            return S_OK;
        }
    };
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    RoInitialize(RO_INIT_MULTITHREADED);

    // Everything the runtime prints goes to LocalState\SSARecomp.log (Device Portal: File explorer, LocalAppData).
    std::wstring localState = LocalStatePath();
    if (!localState.empty())
    {
        _wfreopen((localState + L"\\SSARecomp.log").c_str(), L"w", stderr);
        setvbuf(stderr, nullptr, _IONBF, 0); // complete at all times: the log can be downloaded while the game runs
        UwpSetPreferencePath(Utf8(localState) + "\\");
    }

    Ref<ICoreApplication> application;
    HRESULT result = GetFactory(L"Windows.ApplicationModel.Core.CoreApplication", application);
    if (SUCCEEDED(result))
    {
        auto* source = new ViewSource();
        result = application->Run(source);
        source->Release();
    }
    if (FAILED(result))
        fprintf(stderr, "[uwp] CoreApplication.Run failed: 0x%08lX\n", static_cast<unsigned long>(result));
    RoUninitialize();
    return 0;
}
