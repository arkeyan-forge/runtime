// Windows (desktop): the Xbox One portal through Windows' own Xbox driver, with Windows.Gaming.Input.Custom: a game
// controller factory registered for the portal's GIP interface, as the Xbox (UWP) build does (host/uwp/portal_backends.cpp).
// Windows does the GIP framing, the power and the acknowledgements; the protocol reports travel in GIP message 0x21 with a
// 30-byte payload (as in host/portal_usb.cpp). Nothing to install and no driver change. Untested on hardware [?]: whether
// Windows calls a desktop program's factory is what the log's "[portal] gip: ..." lines tell.
// Raw WinRT ABI through the mingw-w64 headers plus host/uwp/winrt_portal.h.

// See host/uwp/main.cpp: mingw-w64 declares IReference<boolean> twice.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include "host/portal_gip_win.h"
#include "host/portal.h"
#include "host/portal_usb.h"
// COM interfaces have no virtual destructor by design (Release deletes).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#include "host/uwp/winrt_portal.h"
#pragma clang diagnostic pop
#include <roapi.h>
#include <winstring.h>
#include <windows.gaming.input.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using namespace ABI::Windows;
namespace Custom = Gaming::Input::Custom;

namespace
{
    // The Xbox One portal's GIP interface, declared in its GIP descriptor next to the device type
    // "Activision.Xbox.Skylanders.Portal" (a capture of the portal published by the Santroller project).
    constexpr GUID PortalGipInterface = { 0x1f7788a2, 0xb860, 0x9b40,
        { 0x36, 0x26, 0xec, 0x56, 0xc7, 0xc4, 0x52, 0x56 } };
    constexpr BYTE GipPortalReport = 0x21;
    constexpr size_t GipPortalReportSize = 30;

    template <typename T>
    void Release(T*& object)
    {
        if (object != nullptr)
            object->Release();
        object = nullptr;
    }

    // The system's objects call back into these. They are kept until exit (a factory cannot be unregistered), so the
    // reference counts only guard against an early delete.
    class Route;

    class Sink final : public Custom::IGameControllerInputSink, public Custom::IGipGameControllerInputSink
    {
    public:
        explicit Sink(Route& route) : m_route(route) {}
        ~Sink() = default;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IAgileObject) ||
                iid == __uuidof(Custom::IGameControllerInputSink))
                *out = static_cast<Custom::IGameControllerInputSink*>(this);
            else if (iid == __uuidof(Custom::IGipGameControllerInputSink))
                *out = static_cast<Custom::IGipGameControllerInputSink*>(this);
            else
            {
                *out = nullptr;
                return E_NOINTERFACE;
            }
            AddRef();
            return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG refs = --m_refs;
            if (refs == 0)
                delete this;
            return refs;
        }
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
        HRESULT STDMETHODCALLTYPE OnInputResumed(UINT64) override
        {
            fprintf(stderr, "[portal] gip: input resumed\n");
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnInputSuspended(UINT64) override
        {
            fprintf(stderr, "[portal] gip: input suspended (Windows stops input while the game is not in front)\n");
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnKeyReceived(UINT64, BYTE key, boolean pressed) override
        {
            fprintf(stderr, "[portal] gip: key %02X %s\n", key, pressed ? "pressed" : "released");
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnMessageReceived(
            UINT64, Custom::GipMessageClass messageClass, BYTE id, BYTE sequence, UINT32 size, BYTE* message) override;

    private:
        Route& m_route;
        std::atomic<ULONG> m_refs{ 1 };
    };

    class Factory final : public Custom::ICustomGameControllerFactory
    {
    public:
        explicit Factory(Route& route) : m_route(route) {}
        ~Factory() = default;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IAgileObject) ||
                iid == __uuidof(Custom::ICustomGameControllerFactory))
            {
                *out = static_cast<Custom::ICustomGameControllerFactory*>(this);
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
        HRESULT STDMETHODCALLTYPE CreateGameController(
            Custom::IGameControllerProvider* provider, IInspectable** value) override;
        HRESULT STDMETHODCALLTYPE OnGameControllerAdded(Gaming::Input::IGameController*) override
        {
            fprintf(stderr, "[portal] gip: game controller added\n");
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnGameControllerRemoved(Gaming::Input::IGameController*) override;

    private:
        Route& m_route;
        std::atomic<ULONG> m_refs{ 1 };
    };

    // The one route of the process: the factory is registered once, from a thread kept in the multithreaded apartment
    // (Windows calls the agile factory and sink from its own threads; the backend's thread then calls SendMessage).
    class Route
    {
    public:
        static Route& Get()
        {
            static Route* route = new Route; // never deleted: Windows keeps the factory
            return *route;
        }

        // Why the route cannot work, empty when the factory is registered.
        std::string Problem()
        {
            std::lock_guard lock(m_mutex);
            return m_problem;
        }

        bool Connected()
        {
            std::lock_guard lock(m_mutex);
            return m_provider != nullptr;
        }

        // Waits up to `ms` for Windows to hand over a portal.
        bool WaitConnected(unsigned ms)
        {
            std::unique_lock lock(m_mutex);
            return m_changed.wait_for(lock, std::chrono::milliseconds(ms), [this] { return m_provider != nullptr; });
        }

        // The message class of the portal's reports is not documented: Command first, then the others; the first one
        // Windows accepts is kept.
        bool Send(const uint8_t* command, size_t size)
        {
            Custom::IGipGameControllerProvider* provider = nullptr;
            {
                std::lock_guard lock(m_mutex);
                provider = m_provider;
                if (provider != nullptr)
                    provider->AddRef();
            }
            if (provider == nullptr)
                return false;
            uint8_t payload[GipPortalReportSize]{};
            memcpy(payload, command, std::min(size, sizeof(payload)));
            HRESULT result = provider->SendMessage(m_class, GipPortalReport, sizeof(payload), payload);
            if (FAILED(result) && !m_classFound)
            {
                fprintf(stderr, "[portal] gip: message %02X class %d failed (0x%08lX)\n", GipPortalReport,
                    int(m_class.load()), result);
                for (Custom::GipMessageClass other : { Custom::GipMessageClass_LowLatency,
                         Custom::GipMessageClass_StandardLatency, Custom::GipMessageClass_Command })
                {
                    if (other == m_class)
                        continue;
                    result = provider->SendMessage(other, GipPortalReport, sizeof(payload), payload);
                    fprintf(
                        stderr, "[portal] gip: message %02X class %d: 0x%08lX\n", GipPortalReport, int(other), result);
                    if (SUCCEEDED(result))
                    {
                        m_class = other;
                        break;
                    }
                }
            }
            provider->Release();
            if (FAILED(result))
                return false;
            m_classFound = true;
            if (std::string line = m_trace.Line(true, payload, sizeof(payload), PortalLogEnabled()); !line.empty())
                fprintf(stderr, "[portal] gip: %s\n", line.c_str());
            return true;
        }

        size_t Read(uint8_t* report, size_t capacity)
        {
            std::lock_guard lock(m_mutex);
            if (m_reports.empty())
                return 0;
            const std::vector<uint8_t>& next = m_reports.front();
            const size_t size = std::min(next.size(), capacity);
            memcpy(report, next.data(), size);
            m_reports.pop_front();
            return size;
        }

        void Attach(Custom::IGipGameControllerProvider* provider)
        {
            std::lock_guard lock(m_mutex);
            Release(m_provider);
            m_provider = provider;
            m_reports.clear();
            m_trace.Reset();
            m_changed.notify_all();
        }

        void Detach()
        {
            std::lock_guard lock(m_mutex);
            Release(m_provider);
            m_changed.notify_all();
        }

        void Queue(Custom::GipMessageClass messageClass, BYTE id, BYTE sequence, const uint8_t* message, UINT32 size)
        {
            if (id != GipPortalReport)
            {
                if (m_otherLogged++ < portal_usb::PacketTrace::Logged || PortalLogEnabled())
                    fprintf(stderr, "[portal] gip: message %02X class %d sequence %u: %s\n", id, int(messageClass),
                        sequence, portal_usb::PacketHex(message, size).c_str());
                return;
            }
            if (std::string line = m_trace.Line(false, message, size, PortalLogEnabled()); !line.empty())
                fprintf(stderr, "[portal] gip: %s\n", line.c_str());
            std::lock_guard lock(m_mutex);
            if (m_reports.size() >= 64)
                m_reports.pop_front();
            m_reports.emplace_back(message, message + size);
            m_changed.notify_all();
        }

        // Waits up to `ms` for a report (the probe).
        size_t WaitRead(uint8_t* report, size_t capacity, unsigned ms)
        {
            {
                std::unique_lock lock(m_mutex);
                m_changed.wait_for(lock, std::chrono::milliseconds(ms), [this] { return !m_reports.empty(); });
            }
            return Read(report, capacity);
        }

    private:
        Route()
        {
            std::thread([this] { Run(); }).detach();
            std::unique_lock lock(m_mutex);
            m_changed.wait_for(lock, std::chrono::seconds(5), [this] { return m_started; });
        }

        void Run()
        {
            HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);
            std::string problem;
            if (FAILED(result) && result != RPC_E_CHANGED_MODE)
                problem = "Windows Runtime unavailable (RoInitialize 0x" + Hex(result) + ")";
            Custom::IGameControllerFactoryManagerStatics* manager = nullptr;
            if (problem.empty())
            {
                HSTRING name = nullptr;
                const wchar_t className[] = L"Windows.Gaming.Input.Custom.GameControllerFactoryManager";
                WindowsCreateString(className, UINT32(wcslen(className)), &name);
                result = RoGetActivationFactory(
                    name, __uuidof(Custom::IGameControllerFactoryManagerStatics), reinterpret_cast<void**>(&manager));
                WindowsDeleteString(name);
                if (FAILED(result))
                    problem = "Windows.Gaming.Input.Custom unavailable (0x" + Hex(result) + ")";
            }
            if (problem.empty())
            {
                result = manager->RegisterCustomFactoryForGipInterface(new Factory(*this), PortalGipInterface);
                if (FAILED(result))
                    problem = "the factory for the portal's GIP interface was refused (0x" + Hex(result) + ")";
                Release(manager);
            }
            fprintf(stderr,
                "[portal] gip: Windows.Gaming.Input.Custom factory for the Xbox One portal's GIP interface "
                "{1F7788A2-B860-9B40-3626-EC56C7C45256}: %s\n",
                problem.empty() ? "registered" : problem.c_str());
            {
                std::lock_guard lock(m_mutex);
                m_problem = problem;
                m_started = true;
                m_changed.notify_all();
            }
            // The thread stays in the multithreaded apartment for the process's life, which keeps the apartment (and
            // the objects Windows calls) alive for the other threads.
            for (;;)
                Sleep(INFINITE);
        }

        static std::string Hex(HRESULT result)
        {
            char text[16];
            snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(result));
            return text;
        }

        std::mutex m_mutex;
        std::condition_variable m_changed;
        bool m_started = false;
        std::string m_problem;
        Custom::IGipGameControllerProvider* m_provider = nullptr;
        std::deque<std::vector<uint8_t>> m_reports;
        std::atomic<Custom::GipMessageClass> m_class{ Custom::GipMessageClass_Command };
        std::atomic<bool> m_classFound{ false };
        std::atomic<int> m_otherLogged{ 0 };
        portal_usb::PacketTrace m_trace;
    };

    HRESULT Sink::OnMessageReceived(
        UINT64, Custom::GipMessageClass messageClass, BYTE id, BYTE sequence, UINT32 size, BYTE* message)
    {
        m_route.Queue(messageClass, id, sequence, message, size);
        return S_OK;
    }

    HRESULT Factory::CreateGameController(Custom::IGameControllerProvider* provider, IInspectable** value)
    {
        UINT16 vendor = 0, product = 0;
        Custom::GameControllerVersionInfo firmware{}, hardware{};
        provider->get_HardwareVendorId(&vendor);
        provider->get_HardwareProductId(&product);
        provider->get_FirmwareVersionInfo(&firmware);
        provider->get_HardwareVersionInfo(&hardware);
        fprintf(stderr, "[portal] gip: Windows hands over %04X:%04X (firmware %u.%u.%u.%u, hardware %u.%u.%u.%u)\n",
            vendor, product, firmware.Major, firmware.Minor, firmware.Build, firmware.Revision, hardware.Major,
            hardware.Minor, hardware.Build, hardware.Revision);
        Custom::IGipGameControllerProvider* gip = nullptr;
        if (SUCCEEDED(
                provider->QueryInterface(__uuidof(Custom::IGipGameControllerProvider), reinterpret_cast<void**>(&gip))))
        {
            m_route.Attach(gip);
            fprintf(stderr, "[portal] Xbox One portal %04X:%04X connected (GIP, Windows.Gaming.Input.Custom)\n", vendor,
                product);
        }
        else
            fprintf(stderr, "[portal] gip: that controller has no GIP provider: ignored\n");
        *value = static_cast<Custom::IGameControllerInputSink*>(new Sink(m_route));
        return S_OK;
    }

    HRESULT Factory::OnGameControllerRemoved(Gaming::Input::IGameController*)
    {
        fprintf(stderr, "[portal] gip: the Xbox One portal went away\n");
        m_route.Detach();
        return S_OK;
    }

    class GipPortal final : public PortalBackend
    {
    public:
        const char* Name() const override { return "Xbox One (GIP, Windows)"; }
        bool IsConnected() override { return Route::Get().Connected(); }
        bool Write(const uint8_t* command, size_t size) override { return Route::Get().Send(command, size); }
        size_t Read(uint8_t* report, size_t capacity) override { return Route::Get().Read(report, capacity); }
    };
}

std::unique_ptr<PortalBackend> CreateGipPortal(unsigned waitMs)
{
    Route& route = Route::Get();
    if (!route.Problem().empty())
        return nullptr;
    if (waitMs != 0 && !route.WaitConnected(waitMs))
        fprintf(
            stderr, "[portal] gip: Windows handed over no Xbox One portal within %u ms (still listening)\n", waitMs);
    return std::make_unique<GipPortal>();
}

std::string ProbeGipPortal(PortalProbe& probe, std::string& steps)
{
    Route& route = Route::Get();
    if (std::string problem = route.Problem(); !problem.empty())
        return "error: " + problem;
    if (!route.WaitConnected(3000))
        return "error: Windows handed the portal to no program within 3 s (Windows.Gaming.Input.Custom)";
    probe.opened = true;
    steps += "; handed over by Windows (GIP, Windows.Gaming.Input.Custom)";
    uint8_t report[64];
    while (route.Read(report, sizeof(report)) != 0)
    {}
    const uint8_t reset = 'R';
    if (!route.Send(&reset, 1))
        return "error: GIP message 21 refused";
    steps += "; out: 52 (GIP message 21, 30 bytes)";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (int packets = 0; std::chrono::steady_clock::now() < deadline;)
    {
        const size_t size = route.WaitRead(report, sizeof(report), 200);
        if (size == 0)
            continue;
        if (packets++ < 4)
            steps += "; in: " + portal_usb::PacketHex(report, size);
        if (report[0] == 'R' && size >= 3)
        {
            NameProbe(probe, report, size);
            return "ok";
        }
    }
    return "error: no 'R' answer within 1 s";
}
