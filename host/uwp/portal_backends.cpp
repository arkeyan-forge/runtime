// Real portals on Xbox (UWP). One backend tries every route the console may allow, all at once; the first portal
// that connects is used (docs/history/handoffs/handoff_portal_xbox.md):
//   - Xbox One portal (wired, a GIP accessory): Windows.Gaming.Input.Custom, a controller factory registered for the
//     portal's GIP interface. The protocol reports travel in GIP message 0x21 with a 30-byte payload, as in
//     host/portal_usb.cpp; the console's GIP driver does the framing, power and acknowledgements.
//   - HID portals (Wii, Wii U, PS3, PS4, PC; 1430:0150): Windows.Devices.HumanInterfaceDevice (the manifest's
//     humaninterfacedevice capability), and the same factory registered for the hardware id 1430:0150 in case the
//     console offers the portal as a game controller instead (HidGameControllerProvider).
//   - Xbox 360 portals: no route (no driver for them on the console); they are only reported in the log.
// Everything is logged ([portal] ...): the HID and USB devices the app can see, access granted or denied, the
// first reports each way. The protocol itself is the same as on desktop (host/portal_hid.cpp, host/portal_usb.cpp).
// Raw WinRT ABI through the mingw-w64 headers plus host/uwp/winrt_portal.h, as in host/uwp/main.cpp.

// See host/uwp/main.cpp: mingw-w64 declares IReference<boolean> twice.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include "host/portal.h"
#include "host/portal_backend.h"
#include "host/uwp/winrt_portal.h"
#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>
#include <robuffer.h>
#include <windows.devices.enumeration.h>
#include <windows.gaming.input.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace ABI::Windows;
namespace Custom = Gaming::Input::Custom;
namespace Hid = Devices::HumanInterfaceDevice;

namespace
{
    // A COM reference (copyable, unlike main.cpp's: the transports are handed between threads).
    template <typename T>
    class Ref
    {
    public:
        Ref() = default;
        Ref(T* ptr) : m_ptr(ptr)
        {
            if (m_ptr != nullptr)
                m_ptr->AddRef();
        }
        Ref(const Ref& other) : Ref(other.m_ptr) {}
        Ref& operator=(const Ref& other)
        {
            if (other.m_ptr != nullptr)
                other.m_ptr->AddRef();
            Reset();
            m_ptr = other.m_ptr;
            return *this;
        }
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
        explicit HString(const std::wstring& text) { WindowsCreateString(text.c_str(), UINT32(text.size()), &m_value); }
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

    // Takes ownership of `text`.
    std::wstring TakeString(HSTRING text)
    {
        UINT32 size = 0;
        const wchar_t* chars = WindowsGetStringRawBuffer(text, &size);
        std::wstring result(chars, size);
        WindowsDeleteString(text);
        return result;
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

    std::string Hex(const uint8_t* data, size_t size)
    {
        std::string text;
        char byte[4];
        for (size_t i = 0; i < size; i++)
        {
            snprintf(byte, sizeof(byte), " %02X", data[i]);
            text += byte;
        }
        return text;
    }

    constexpr uint16_t ActivisionVendor = 0x1430;
    constexpr uint16_t HidPortalProduct = 0x0150;
    // The Xbox One portal's GIP interface, declared in its GIP descriptor next to the device type
    // "Activision.Xbox.Skylanders.Portal" (from a capture of the portal published by the Santroller project).
    constexpr GUID PortalGipInterface = { 0x1f7788a2, 0xb860, 0x9b40,
        { 0x36, 0x26, 0xec, 0x56, 0xc7, 0xc4, 0x52, 0x56 } };
    constexpr BYTE GipPortalReport = 0x21;
    constexpr size_t GipPortalReportSize = 30;

    // Device interface classes: HID collections, and USB devices (only listed, for the log).
    const wchar_t HidInterfaces[] = L"System.Devices.InterfaceClassGuid:=\"{4D1E55B2-F16F-11CF-88CB-001111000030}\"";
    const wchar_t UsbDevices[] = L"System.Devices.InterfaceClassGuid:=\"{A5DCBF10-6530-11D2-901F-00C04FB951ED}\"";
    const wchar_t Enabled[] = L" AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True";

    constexpr int LoggedReports = 8;   // reports logged each way per connection
    constexpr int LoggedDevices = 24;  // devices listed per class
    constexpr auto RescanPeriod = std::chrono::seconds(2);

    // Waits for an asynchronous operation by polling it (the callers are worker threads or a bounded startup wait).
    HRESULT Wait(IUnknown* operation, DWORD timeoutMs)
    {
        Ref<IAsyncInfo> info;
        HRESULT result = operation->QueryInterface(__uuidof(IAsyncInfo), info.PutVoid());
        if (FAILED(result))
            return result;

        ULONGLONG start = GetTickCount64();
        AsyncStatus status = Started;
        while (SUCCEEDED(info->get_Status(&status)) && status == Started)
        {
            if (GetTickCount64() - start > timeoutMs)
            {
                info->Cancel();
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            Sleep(1);
        }
        if (status == Completed)
            return S_OK;
        HRESULT error = E_FAIL;
        info->get_ErrorCode(&error);
        return status == Canceled ? E_ABORT : error;
    }

    uint8_t* BufferBytes(Storage::Streams::IBuffer* buffer, UINT32& length)
    {
        length = 0;
        Ref<::Windows::Storage::Streams::IBufferByteAccess> access;
        BYTE* bytes = nullptr;
        if (buffer == nullptr || FAILED(buffer->get_Length(&length)) ||
            FAILED(
                buffer->QueryInterface(__uuidof(::Windows::Storage::Streams::IBufferByteAccess), access.PutVoid())) ||
            FAILED(access->Buffer(&bytes)))
        {
            length = 0;
            return nullptr;
        }
        return bytes;
    }

    struct DeviceEntry
    {
        std::wstring id;
        std::wstring name;
    };

    // Device interfaces matching an AQS filter. False if the enumeration itself failed.
    bool FindDevices(const std::wstring& filter, std::vector<DeviceEntry>& devices, HRESULT& result)
    {
        devices.clear();
        Ref<Devices::Enumeration::IDeviceInformationStatics> statics;
        Ref<Foundation::IAsyncOperation<Devices::Enumeration::DeviceInformationCollection*>> operation;
        Ref<Foundation::Collections::IVectorView<Devices::Enumeration::DeviceInformation*>> list;
        UINT32 count = 0;
        if (FAILED(result = GetFactory(L"Windows.Devices.Enumeration.DeviceInformation", statics)) ||
            FAILED(result = statics->FindAllAsyncAqsFilter(HString(filter), operation.Put())) ||
            FAILED(result = Wait(operation.Get(), 10000)) || FAILED(result = operation->GetResults(list.Put())) ||
            FAILED(result = list->get_Size(&count)))
            return false;

        for (UINT32 i = 0; i < count; i++)
        {
            Ref<Devices::Enumeration::IDeviceInformation> device;
            HSTRING id = nullptr, name = nullptr;
            if (FAILED(list->GetAt(i, device.Put())) || FAILED(device->get_Id(&id)))
                continue;
            device->get_Name(&name);
            devices.push_back({ TakeString(id), TakeString(name) });
        }
        return true;
    }

    bool Contains(std::wstring text, const wchar_t* part)
    {
        std::transform(text.begin(), text.end(), text.begin(), towupper);
        return text.find(part) != std::wstring::npos;
    }

    // COM objects the system calls back into (as in host/uwp/main.cpp). They are owned by the portal, which is kept
    // until exit, so their reference counts only guard against early deletion.
    template <typename Interface>
    class Unknown : public Interface
    {
    public:
        virtual ~Unknown() = default;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (iid == __uuidof(Interface) || iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) ||
                (std::is_base_of_v<IInspectable, Interface> && iid == __uuidof(IInspectable)))
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

    enum class Transport
    {
        None,
        HidDevice,     // Windows.Devices.HumanInterfaceDevice
        Gip,           // Windows.Gaming.Input.Custom, GIP provider (Xbox One portal)
        HidController, // Windows.Gaming.Input.Custom, HID provider
    };

    const char* TransportName(Transport transport)
    {
        switch (transport)
        {
        case Transport::HidDevice: return "HID portal (Windows.Devices.HumanInterfaceDevice)";
        case Transport::Gip: return "Xbox One portal (GIP, Windows.Gaming.Input.Custom)";
        case Transport::HidController: return "HID portal (Windows.Gaming.Input.Custom)";
        default: return "none";
        }
    }

    class UwpPortal;

    class HidReportHandler final : public Unknown<Hid::IHidInputReportReceivedHandler>
    {
    public:
        explicit HidReportHandler(UwpPortal& portal) : m_portal(portal) {}
        HRESULT STDMETHODCALLTYPE Invoke(Hid::IHidDevice*, Hid::IHidInputReportReceivedEventArgs* args) override;

    private:
        UwpPortal& m_portal;
    };

    // What CreateGameController returns: the sink the console delivers the device's input to.
    class ControllerSink final : public Custom::IGameControllerInputSink,
                                 public Custom::IGipGameControllerInputSink,
                                 public Custom::IHidGameControllerInputSink
    {
    public:
        ControllerSink(UwpPortal& portal, Transport transport) : m_portal(portal), m_transport(transport) {}
        virtual ~ControllerSink() = default;

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
        {
            if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IAgileObject) ||
                iid == __uuidof(Custom::IGameControllerInputSink))
                *out = static_cast<Custom::IGameControllerInputSink*>(this);
            else if (iid == __uuidof(Custom::IGipGameControllerInputSink))
                *out = static_cast<Custom::IGipGameControllerInputSink*>(this);
            else if (iid == __uuidof(Custom::IHidGameControllerInputSink))
                *out = static_cast<Custom::IHidGameControllerInputSink*>(this);
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
            fprintf(stderr, "[portal] %s: input resumed\n", TransportName(m_transport));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnInputSuspended(UINT64) override
        {
            fprintf(stderr, "[portal] %s: input suspended (the app lost focus)\n", TransportName(m_transport));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnKeyReceived(UINT64, BYTE keyCode, boolean isPressed) override
        {
            fprintf(stderr, "[portal] GIP key %02X %s\n", keyCode, isPressed ? "pressed" : "released");
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnMessageReceived(UINT64, Custom::GipMessageClass messageClass, BYTE messageId,
            BYTE sequenceId, UINT32 size, BYTE* message) override;
        HRESULT STDMETHODCALLTYPE OnInputReportReceived(UINT64, BYTE reportId, UINT32 size, BYTE* report) override;

    private:
        UwpPortal& m_portal;
        Transport m_transport;
        std::atomic<ULONG> m_refs{ 1 };
        std::atomic<int> m_logged{ 0 };
    };

    class ControllerFactory final : public Unknown<Custom::ICustomGameControllerFactory>
    {
    public:
        explicit ControllerFactory(UwpPortal& portal) : m_portal(portal) {}

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
            fprintf(stderr, "[portal] game controller added\n");
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnGameControllerRemoved(Gaming::Input::IGameController*) override;

    private:
        UwpPortal& m_portal;
    };

    // The portal's state. The system keeps the controller factory (it cannot be unregistered), so this is never
    // deleted; UwpPortalBackend is what the portal layer owns.
    class UwpPortal final : public PortalBackend
    {
    public:
        UwpPortal()
        {
            m_reportHandler = new HidReportHandler(*this);
            m_factory = new ControllerFactory(*this);
            RegisterFactory();
            std::thread([this] { ScanLoop(); }).detach();
        }

        const char* Name() const override { return "real (UWP)"; }

        bool IsConnected() override
        {
            std::lock_guard lock(m_mutex);
            return m_transport != Transport::None;
        }

        bool Write(const uint8_t* command, size_t size) override
        {
            Transport transport;
            Ref<Hid::IHidDevice> device;
            Ref<Custom::IGipGameControllerProvider> gip;
            Ref<Custom::IHidGameControllerProvider> hidController;
            {
                std::lock_guard lock(m_mutex);
                transport = m_transport;
                device = m_device;
                gip = m_gip;
                hidController = m_hidController;
            }

            bool sent = false;
            if (transport == Transport::HidDevice)
                sent = WriteHidDevice(device, command, size);
            else if (transport == Transport::Gip)
                sent = WriteGip(gip, command, size);
            else if (transport == Transport::HidController)
            {
                uint8_t report[PortalReportSize]{};
                memcpy(report, command, std::min(size, sizeof(report)));
                HRESULT result = hidController->SendOutputReport(0, sizeof(report), report);
                sent = SUCCEEDED(result);
                if (!sent)
                    fprintf(stderr, "[portal] HID output report failed (0x%08lX)\n", result);
            }
            if (sent)
                LogReport("out", transport, command, std::min(size, PortalReportSize), m_loggedOut);
            return sent;
        }

        size_t Read(uint8_t* report, size_t capacity) override
        {
            std::lock_guard lock(m_mutex);
            if (m_reports.empty())
                return 0;

            const std::vector<uint8_t>& next = m_reports.front();
            size_t size = std::min(next.size(), capacity);
            memcpy(report, next.data(), size);
            m_reports.pop_front();
            return size;
        }

        // Waits a little at startup for a portal that is already plugged in: the controller factory and the HID
        // enumeration answer asynchronously. True once one is connected.
        bool WaitForPortal()
        {
            ULONGLONG start = GetTickCount64();
            while (GetTickCount64() - start < 4000)
            {
                if (IsConnected())
                    return true;
                if (m_firstScanDone && GetTickCount64() - start > 1500)
                    break;
                Sleep(20);
            }
            return IsConnected();
        }

        // The portal layer chose another portal: stop looking (the factory stays registered, its callbacks only log).
        void Stop()
        {
            m_stopped = true;
            Ref<Hid::IHidDevice> device;
            {
                std::lock_guard lock(m_mutex);
                m_gip.Reset();
                m_hidController.Reset();
                device = m_device;
                m_device.Reset();
                m_transport = Transport::None;
            }
            if (device)
            {
                device->remove_InputReportReceived(m_deviceToken);
                CloseHid(device);
            }
        }

        // From the transports' callbacks. Reports of a transport other than the active one are dropped (the same
        // portal may be reachable two ways).
        void Queue(Transport transport, const uint8_t* report, size_t size)
        {
            // HID reports may start with their report id, 0 (protocol reports start with a command letter).
            if (transport != Transport::Gip && size > 0 && report[0] == 0)
            {
                report++;
                size--;
            }
            if (size == 0)
                return;
            {
                std::lock_guard lock(m_mutex);
                if (transport != m_transport)
                    return;
                if (m_reports.size() >= 64)
                    m_reports.pop_front();
                m_reports.emplace_back(report, report + size);
            }
            LogReport("in", transport, report, size, m_loggedIn);
        }

        void AttachGip(Ref<Custom::IGipGameControllerProvider> gip)
        {
            if (m_stopped)
                return;
            std::lock_guard lock(m_mutex);
            m_gip = gip;
            Activate(Transport::Gip);
        }

        void AttachHidController(Ref<Custom::IHidGameControllerProvider> hidController)
        {
            if (m_stopped)
                return;
            std::lock_guard lock(m_mutex);
            m_hidController = hidController;
            Activate(Transport::HidController);
        }

        void DetachControllers()
        {
            std::lock_guard lock(m_mutex);
            m_gip.Reset();
            m_hidController.Reset();
            if (m_transport == Transport::Gip || m_transport == Transport::HidController)
                Deactivate();
        }

    private:
        // Called with m_mutex held.
        void Activate(Transport transport)
        {
            if (m_transport != Transport::None)
            {
                fprintf(stderr, "[portal] %s also available; keeping %s\n", TransportName(transport),
                    TransportName(m_transport));
                return;
            }
            m_transport = transport;
            m_reports.clear();
            m_loggedIn = m_loggedOut = 0;
            fprintf(stderr, "[portal] connected: %s\n", TransportName(transport));
        }

        // Called with m_mutex held: falls back on another transport still attached.
        void Deactivate()
        {
            fprintf(stderr, "[portal] disconnected: %s\n", TransportName(m_transport));
            m_transport = Transport::None;
            if (m_device)
                Activate(Transport::HidDevice);
            else if (m_gip)
                Activate(Transport::Gip);
            else if (m_hidController)
                Activate(Transport::HidController);
        }

        void LogReport(
            const char* direction, Transport transport, const uint8_t* report, size_t size, std::atomic<int>& logged)
        {
            int index = logged++;
            if (index < LoggedReports || PortalLogEnabled())
                fprintf(stderr, "[portal] %s %s:%s\n", transport == Transport::Gip ? "GIP" : "HID", direction,
                    Hex(report, size).c_str());
            if (index == LoggedReports && !PortalLogEnabled())
                fprintf(stderr, "[portal] (only the first %d reports each way are logged)\n", LoggedReports);
        }

        void RegisterFactory()
        {
            Ref<Custom::IGameControllerFactoryManagerStatics> manager;
            HRESULT result = GetFactory(L"Windows.Gaming.Input.Custom.GameControllerFactoryManager", manager);
            if (FAILED(result))
            {
                fprintf(
                    stderr, "[portal] Windows.Gaming.Input.Custom unavailable (0x%08lX): no Xbox One portal\n", result);
                return;
            }
            result = manager->RegisterCustomFactoryForGipInterface(m_factory, PortalGipInterface);
            fprintf(stderr,
                "[portal] GIP factory for the Xbox One portal interface {1F7788A2-B860-9B40-3626-EC56C7C45256}: %s "
                "(0x%08lX)\n",
                SUCCEEDED(result) ? "registered" : "failed", result);
            result = manager->RegisterCustomFactoryForHardwareId(m_factory, ActivisionVendor, HidPortalProduct);
            fprintf(stderr, "[portal] game controller factory for HID %04X:%04X: %s (0x%08lX)\n", ActivisionVendor,
                HidPortalProduct, SUCCEEDED(result) ? "registered" : "failed", result);
        }

        // HID portals through Windows.Devices.HumanInterfaceDevice, rescanned while none is open. The first pass also
        // lists what the console lets the app see.
        void ScanLoop()
        {
            RoInitialize(RO_INIT_MULTITHREADED);
            LogDevices();
            while (!m_stopped)
            {
                bool open;
                {
                    std::lock_guard lock(m_mutex);
                    open = bool(m_device);
                }
                if (!open)
                    ScanHid();
                m_firstScanDone = true;
                std::this_thread::sleep_for(RescanPeriod);
            }
        }

        void LogDevices()
        {
            const struct
            {
                const char* kind;
                const wchar_t* filter;
            } classes[] = { { "HID", HidInterfaces }, { "USB", UsbDevices } };

            for (const auto& deviceClass : classes)
            {
                std::vector<DeviceEntry> devices;
                HRESULT result = S_OK;
                if (!FindDevices(deviceClass.filter, devices, result))
                {
                    fprintf(stderr, "[portal] cannot list %s devices (0x%08lX)\n", deviceClass.kind, result);
                    continue;
                }
                fprintf(stderr, "[portal] %s devices visible to the app: %zu\n", deviceClass.kind, devices.size());
                for (size_t i = 0; i < devices.size() && i < LoggedDevices; i++)
                {
                    const DeviceEntry& device = devices[i];
                    fprintf(stderr, "[portal]   %s (%s)\n", Utf8(device.id).c_str(), Utf8(device.name).c_str());
                    if (Contains(device.id, L"VID_1430&PID_1F17") || Contains(device.id, L"VID_24C6&PID_FA00"))
                        fprintf(stderr,
                            "[portal]   ^ an Xbox 360 portal: the console has no driver for it, use another portal\n");
                    else if (Contains(device.id, L"VID_1430&PID_09AA") || Contains(device.id, L"VID_1430&PID_09AB"))
                        fprintf(stderr, "[portal]   ^ the Xbox One portal (reached through GIP, not this device)\n");
                }
            }
        }

        void ScanHid()
        {
            std::wstring filter = std::wstring(HidInterfaces) + Enabled +
                                  L" AND System.DeviceInterface.Hid.VendorId:=" + std::to_wstring(ActivisionVendor) +
                                  L" AND System.DeviceInterface.Hid.ProductId:=" + std::to_wstring(HidPortalProduct);
            std::vector<DeviceEntry> devices;
            HRESULT result = S_OK;
            if (!FindDevices(filter, devices, result))
            {
                if (!m_firstScanDone)
                    fprintf(stderr, "[portal] HID portal search failed (0x%08lX)\n", result);
                return;
            }
            if (devices.empty() && !m_firstScanDone)
                fprintf(stderr, "[portal] no HID portal (%04X:%04X) visible; still looking every 2 s\n",
                    ActivisionVendor, HidPortalProduct);
            for (const DeviceEntry& device : devices)
            {
                if (OpenHid(device))
                    break;
            }
        }

        bool OpenHid(const DeviceEntry& entry)
        {
            // Failures are logged once per device, not at every rescan.
            bool quiet = m_failedDevices.count(entry.id) != 0;
            if (!quiet)
                fprintf(stderr, "[portal] HID portal found: %s\n", Utf8(entry.id).c_str());

            Ref<Hid::IHidDeviceStatics> statics;
            Ref<Hid::IAsyncOperationHidDevice> operation;
            Ref<Hid::IHidDevice> device;
            HRESULT result = GetFactory(L"Windows.Devices.HumanInterfaceDevice.HidDevice", statics);
            if (SUCCEEDED(result))
                result = statics->FromIdAsync(HString(entry.id), Storage::FileAccessMode_ReadWrite, operation.Put());
            if (SUCCEEDED(result))
                result = Wait(operation.Get(), 10000);
            if (SUCCEEDED(result))
                result = operation->GetResults(device.Put());
            if (FAILED(result) || !device)
            {
                if (!quiet)
                {
                    if (SUCCEEDED(result))
                        fprintf(stderr,
                            "[portal] HID portal: access denied (no device returned). The manifest's "
                            "humaninterfacedevice capability must allow it (vidpid:1430 0150, usage:ff00 *)\n");
                    else
                        fprintf(stderr, "[portal] HID portal: cannot open (0x%08lX%s)\n", result,
                            result == E_ACCESSDENIED ? ", access denied" : "");
                }
                m_failedDevices.insert(entry.id);
                return false;
            }

            UINT16 vendor = 0, product = 0, version = 0, usagePage = 0, usage = 0;
            device->get_VendorId(&vendor);
            device->get_ProductId(&product);
            device->get_Version(&version);
            device->get_UsagePage(&usagePage);
            device->get_UsageId(&usage);
            UINT32 outputSize = OutputReportSize(device);
            fprintf(stderr,
                "[portal] HID portal opened: %04X:%04X version %04X, usage page %04X usage %04X, output report %u "
                "bytes\n",
                vendor, product, version, usagePage, usage, outputSize);

            EventRegistrationToken token{};
            result = device->add_InputReportReceived(m_reportHandler, &token);
            if (FAILED(result))
            {
                fprintf(stderr, "[portal] HID portal: cannot receive its input reports (0x%08lX)\n", result);
                CloseHid(device);
                m_failedDevices.insert(entry.id);
                return false;
            }

            std::lock_guard lock(m_mutex);
            if (m_stopped)
            {
                device->remove_InputReportReceived(token);
                CloseHid(device);
                return true;
            }
            m_device = device;
            m_deviceToken = token;
            m_failedDevices.erase(entry.id);
            Activate(Transport::HidDevice);
            return true;
        }

        static UINT32 OutputReportSize(const Ref<Hid::IHidDevice>& device)
        {
            Ref<Hid::IHidOutputReport> report;
            Ref<Storage::Streams::IBuffer> buffer;
            UINT32 length = 0;
            if (SUCCEEDED(device->CreateOutputReport(report.Put())) && SUCCEEDED(report->get_Data(buffer.Put())))
                buffer->get_Length(&length);
            return length;
        }

        static void CloseHid(const Ref<Hid::IHidDevice>& device)
        {
            Ref<Foundation::IClosable> closable;
            if (SUCCEEDED(device.As(closable)))
                closable->Close();
        }

        // Called by the title's thread: SendOutputReportAsync is awaited, as SDL_hid_write blocks on desktop.
        bool WriteHidDevice(const Ref<Hid::IHidDevice>& device, const uint8_t* command, size_t size)
        {
            Ref<Hid::IHidOutputReport> report;
            Ref<Storage::Streams::IBuffer> buffer;
            Ref<Foundation::IAsyncOperation<UINT32>> operation;
            UINT32 length = 0;
            HRESULT result = device->CreateOutputReport(report.Put());
            if (SUCCEEDED(result))
                result = report->get_Data(buffer.Put());
            uint8_t* bytes = SUCCEEDED(result) ? BufferBytes(buffer.Get(), length) : nullptr;
            if (bytes != nullptr && length > 1)
            {
                // Report id 0 first, then the zero-padded command.
                memset(bytes, 0, length);
                memcpy(bytes + 1, command, std::min<size_t>(size, length - 1));
                result = report->put_Data(buffer.Get());
                if (SUCCEEDED(result))
                    result = device->SendOutputReportAsync(report.Get(), operation.Put());
                if (SUCCEEDED(result))
                    result = Wait(operation.Get(), 1000);
            }
            else if (SUCCEEDED(result))
                result = E_UNEXPECTED;
            if (SUCCEEDED(result))
                return true;

            fprintf(stderr, "[portal] HID output report failed (0x%08lX): closing the portal\n", result);
            {
                std::lock_guard lock(m_mutex);
                if (m_device.Get() != device.Get())
                    return false;
                m_device.Reset();
                if (m_transport == Transport::HidDevice)
                    Deactivate();
            }
            // Outside the lock: the report handler may be waiting for it.
            device->remove_InputReportReceived(m_deviceToken);
            CloseHid(device);
            return false;
        }

        // The message class of the portal's reports is not documented: Command first, then the others; the first
        // one the console accepts is kept.
        bool WriteGip(const Ref<Custom::IGipGameControllerProvider>& gip, const uint8_t* command, size_t size)
        {
            uint8_t payload[GipPortalReportSize]{};
            memcpy(payload, command, std::min(size, sizeof(payload)));
            HRESULT result = gip->SendMessage(m_gipClass, GipPortalReport, sizeof(payload), payload);
            if (SUCCEEDED(result))
            {
                m_gipClassFound = true;
                return true;
            }

            fprintf(stderr, "[portal] GIP message %02X class %d failed (0x%08lX)\n", GipPortalReport,
                int(m_gipClass.load()), result);
            if (m_gipClassFound)
                return false;
            for (Custom::GipMessageClass other : { Custom::GipMessageClass_Command, Custom::GipMessageClass_LowLatency,
                     Custom::GipMessageClass_StandardLatency })
            {
                if (other == m_gipClass)
                    continue;
                result = gip->SendMessage(other, GipPortalReport, sizeof(payload), payload);
                fprintf(stderr, "[portal] GIP message %02X class %d: 0x%08lX\n", GipPortalReport, int(other), result);
                if (SUCCEEDED(result))
                {
                    m_gipClass = other;
                    m_gipClassFound = true;
                    return true;
                }
            }
            return false;
        }

        std::mutex m_mutex; // the transports, the active one and the report queue
        Transport m_transport = Transport::None;
        Ref<Hid::IHidDevice> m_device;
        EventRegistrationToken m_deviceToken{};
        Ref<Custom::IGipGameControllerProvider> m_gip;
        Ref<Custom::IHidGameControllerProvider> m_hidController;
        std::deque<std::vector<uint8_t>> m_reports;

        std::atomic<Custom::GipMessageClass> m_gipClass{ Custom::GipMessageClass_Command };
        std::atomic<bool> m_gipClassFound{ false };
        std::atomic<int> m_loggedIn{ 0 }, m_loggedOut{ 0 };
        std::atomic<bool> m_firstScanDone{ false };
        std::atomic<bool> m_stopped{ false };
        std::set<std::wstring> m_failedDevices; // scanner thread only

        HidReportHandler* m_reportHandler;
        ControllerFactory* m_factory;
    };

    HRESULT HidReportHandler::Invoke(Hid::IHidDevice*, Hid::IHidInputReportReceivedEventArgs* args)
    {
        Ref<Hid::IHidInputReport> report;
        Ref<Storage::Streams::IBuffer> buffer;
        UINT32 length = 0;
        if (SUCCEEDED(args->get_Report(report.Put())) && SUCCEEDED(report->get_Data(buffer.Put())))
        {
            if (const uint8_t* bytes = BufferBytes(buffer.Get(), length))
                m_portal.Queue(Transport::HidDevice, bytes, length);
        }
        return S_OK;
    }

    HRESULT ControllerSink::OnMessageReceived(
        UINT64, Custom::GipMessageClass messageClass, BYTE messageId, BYTE sequenceId, UINT32 size, BYTE* message)
    {
        if (messageId == GipPortalReport)
        {
            m_portal.Queue(Transport::Gip, message, size);
            return S_OK;
        }
        if (m_logged++ < LoggedReports || PortalLogEnabled())
            fprintf(stderr, "[portal] GIP message %02X class %d sequence %u:%s\n", messageId, int(messageClass),
                sequenceId, Hex(message, size).c_str());
        return S_OK;
    }

    HRESULT ControllerSink::OnInputReportReceived(UINT64, BYTE reportId, UINT32 size, BYTE* report)
    {
        if (reportId != 0 && m_logged++ < LoggedReports)
            fprintf(stderr, "[portal] HID input report id %u (%u bytes)\n", reportId, size);
        m_portal.Queue(Transport::HidController, report, size);
        return S_OK;
    }

    HRESULT ControllerFactory::CreateGameController(Custom::IGameControllerProvider* provider, IInspectable** value)
    {
        UINT16 vendor = 0, product = 0;
        Custom::GameControllerVersionInfo firmware{}, hardware{};
        provider->get_HardwareVendorId(&vendor);
        provider->get_HardwareProductId(&product);
        provider->get_FirmwareVersionInfo(&firmware);
        provider->get_HardwareVersionInfo(&hardware);
        fprintf(stderr, "[portal] game controller %04X:%04X (firmware %u.%u.%u.%u, hardware %u.%u.%u.%u)\n", vendor,
            product, firmware.Major, firmware.Minor, firmware.Build, firmware.Revision, hardware.Major, hardware.Minor,
            hardware.Build, hardware.Revision);

        Ref<Custom::IGameControllerProvider> source(provider);
        Ref<Custom::IGipGameControllerProvider> gip;
        Ref<Custom::IHidGameControllerProvider> hid;
        Transport transport = Transport::None;
        if (SUCCEEDED(source.As(gip)))
        {
            transport = Transport::Gip;
            m_portal.AttachGip(gip);
        }
        else if (SUCCEEDED(source.As(hid)))
        {
            UINT16 usagePage = 0, usage = 0;
            hid->get_UsagePage(&usagePage);
            hid->get_UsageId(&usage);
            fprintf(stderr, "[portal] HID game controller, usage page %04X usage %04X\n", usagePage, usage);
            transport = Transport::HidController;
            m_portal.AttachHidController(hid);
        }
        else
            fprintf(stderr, "[portal] game controller of an unexpected kind: ignored\n");

        auto* sink = new ControllerSink(m_portal, transport);
        *value = static_cast<Custom::IGameControllerInputSink*>(sink);
        return S_OK;
    }

    HRESULT ControllerFactory::OnGameControllerRemoved(Gaming::Input::IGameController*)
    {
        fprintf(stderr, "[portal] game controller removed\n");
        m_portal.DetachControllers();
        return S_OK;
    }

    class UwpPortalBackend final : public PortalBackend
    {
    public:
        explicit UwpPortalBackend(UwpPortal& portal) : m_portal(portal) {}
        ~UwpPortalBackend() override { m_portal.Stop(); }

        const char* Name() const override { return m_portal.Name(); }
        bool IsConnected() override { return m_portal.IsConnected(); }
        bool Write(const uint8_t* command, size_t size) override { return m_portal.Write(command, size); }
        size_t Read(uint8_t* report, size_t capacity) override { return m_portal.Read(report, capacity); }

    private:
        UwpPortal& m_portal;
    };
}

// Every UWP route is in this one backend. In auto mode it is kept only if a portal answers within a few seconds.
std::unique_ptr<PortalBackend> CreateHidPortal()
{
    auto* portal = new UwpPortal;
    bool connected = portal->WaitForPortal();
    fprintf(stderr, "[portal] real portal %s at startup\n", connected ? "found" : "not found");
    return std::make_unique<UwpPortalBackend>(*portal);
}

// Xbox 360 portals have no route on the console, and the Xbox One portal is in CreateHidPortal (GIP).
std::unique_ptr<PortalBackend> CreateUsbPortal() { return nullptr; }

// --portal-probe is a desktop command (the console has no command line).
std::string ProbeHidPortals(std::vector<PortalProbe>&) { return "not on this platform"; }

std::string ProbeUsbPortals(std::vector<PortalProbe>&) { return "not on this platform"; }

std::vector<std::string> ProbeNotes() { return {}; }
