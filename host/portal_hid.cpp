// Portals that are USB HID devices (Wii, Wii U, PS3, PS4, PC): 32-byte input reports on the interrupt IN endpoint,
// commands as 32-byte output reports (SET_REPORT on the control endpoint). Reached through SDL's hidapi; the commands go
// on Windows through hid.dll's HidD_SetOutputReport, on macOS as a USB control request to the device, on Linux through
// hidapi's write or, when the portal refuses it, a USB control request through libusb (WriteMethod below).
//
// The portal's reports are on a vendor-defined usage page (FF00), and SDL's hidapi by default enumerates game
// controllers only (SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS: it drops every device whose usage page is known and is
// not a joystick or gamepad). Windows always knows the usage page, so the portal was never listed there; the hint is
// lifted for the portal's own enumeration. Every step is logged ("[portal] hid: ...") so a log shows where it stops.
#include "host/portal_adpcm.h"
#include "host/portal_backend.h"
#include "host/portal_hid_report.h"
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <hidsdi.h>
#endif
#ifdef __linux__
#include "host/portal_hid_control.h"
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#endif

namespace
{
    constexpr uint16_t PortalVendor = 0x1430; // Activision
    constexpr uint16_t PortalProduct = 0x0150; // every HID portal model, Spyro's Adventure to Imaginators
    static_assert(portal_hid::CommandSize == PortalReportSize);

    // Every HID interface of the portal, whatever its usage page (see the top of this file).
    SDL_hid_device_info* EnumeratePortals()
    {
        SDL_SetHintWithPriority(SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS, "0", SDL_HINT_OVERRIDE);
        SDL_hid_device_info* devices = SDL_hid_enumerate(PortalVendor, PortalProduct);
        SDL_ResetHint(SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS);
        return devices;
    }

    std::string Utf8(const wchar_t* text)
    {
        if (text == nullptr)
            return {};
        char* converted = SDL_iconv_string(
            "UTF-8", "WCHAR_T", reinterpret_cast<const char*>(text), (SDL_wcslen(text) + 1) * sizeof(wchar_t));
        std::string result = converted != nullptr ? converted : "";
        SDL_free(converted);
        return result;
    }

    std::string Describe(const SDL_hid_device_info& device)
    {
        char text[512];
        snprintf(text, sizeof(text), "%04X:%04X \"%s\", usage page %04X usage %04X, interface %d, path %s",
            device.vendor_id, device.product_id, Utf8(device.product_string).c_str(), device.usage_page, device.usage,
            device.interface_number, device.path);
        return text;
    }

    // How a command reaches the portal. The portal has an interrupt OUT endpoint but takes its commands as output
    // reports over the control pipe (SET_REPORT): on Windows, WriteFile (hidapi's write) goes to the OUT endpoint and
    // fails or times out, HidD_SetOutputReport sends SET_REPORT (public reports of the PC portal, and the logs of the
    // first Windows PC). On macOS the HID driver's output report (hidapi's write, IOHIDDeviceSetReport) fails too
    // ("device not responding", 0xE00002ED, on a Spyro's Adventure Wii portal), and the same 32 bytes as a USB control
    // SET_REPORT to the device are answered. Linux: hidapi's write (hidraw) works on a Wii Swap Force portal and fails
    // on the original Wii portal ("Protocol error"); the control request through libusb is tried next
    // (host/portal_hid_control.h).
    using portal_hid::MethodName;
    using portal_hid::WriteMethod;

#ifdef _WIN32
    constexpr auto& Methods = portal_hid::WindowsMethods;

    std::string LastError(const char* what)
    {
        char text[64];
        snprintf(text, sizeof(text), "%s: error 0x%lX", what, static_cast<unsigned long>(GetLastError()));
        return text;
    }

    // hid.dll, loaded on first use (no import: the kit's link line stays as it is).
    struct HidDll
    {
        BOOLEAN(__stdcall* SetOutputReport)(HANDLE, PVOID, ULONG) = nullptr;
        BOOLEAN(__stdcall* GetPreparsedData)(HANDLE, PHIDP_PREPARSED_DATA*) = nullptr;
        BOOLEAN(__stdcall* FreePreparsedData)(PHIDP_PREPARSED_DATA) = nullptr;
        NTSTATUS(__stdcall* GetCaps)(PHIDP_PREPARSED_DATA, PHIDP_CAPS) = nullptr;

        static const HidDll& Get()
        {
            static const HidDll dll = [] {
                HidDll d;
                if (HMODULE module = LoadLibraryW(L"hid.dll"))
                {
                    d.SetOutputReport =
                        reinterpret_cast<decltype(d.SetOutputReport)>(GetProcAddress(module, "HidD_SetOutputReport"));
                    d.GetPreparsedData =
                        reinterpret_cast<decltype(d.GetPreparsedData)>(GetProcAddress(module, "HidD_GetPreparsedData"));
                    d.FreePreparsedData = reinterpret_cast<decltype(d.FreePreparsedData)>(
                        GetProcAddress(module, "HidD_FreePreparsedData"));
                    d.GetCaps = reinterpret_cast<decltype(d.GetCaps)>(GetProcAddress(module, "HidP_GetCaps"));
                }
                return d;
            }();
            return dll;
        }
    };

    // A handle of our own on the device, for HidD_SetOutputReport (SDL's is not reachable), and the output report's
    // length with its report id byte, as the HID descriptor gives it.
    struct OutputHandle
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        size_t length = 0;
        std::string error; // why it is not open

        void Open(const char* path)
        {
            int size = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
            std::wstring wide(size_t(std::max(size, 1)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), size);
            const HidDll& hid = HidDll::Get();
            if (hid.SetOutputReport == nullptr)
            {
                error = "hid.dll has no HidD_SetOutputReport";
                return;
            }
            handle = CreateFileW(wide.c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                error = LastError("CreateFile");
                return;
            }
            PHIDP_PREPARSED_DATA data = nullptr;
            HIDP_CAPS caps{};
            if (hid.GetPreparsedData != nullptr && hid.GetCaps != nullptr && hid.GetPreparsedData(handle, &data))
            {
                if (hid.GetCaps(data, &caps) == HIDP_STATUS_SUCCESS)
                    length = caps.OutputReportByteLength;
                if (hid.FreePreparsedData != nullptr)
                    hid.FreePreparsedData(data);
            }
            if (length == 0)
                length = 1 + PortalReportSize;
        }

        void Close()
        {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    };
#elif defined(__APPLE__)
    constexpr auto& Methods = portal_hid::MacMethods;

    std::string IoError(const char* what, int32_t result)
    {
        char text[64];
        snprintf(text, sizeof(text), "%s: 0x%08X", what, static_cast<unsigned>(result));
        return text;
    }

    // The portal's USB device, for the control SET_REPORT: the HID service SDL opened (its path holds the service's
    // registry id), then up through its USB interface (the request's index) to the device. Not opened with
    // USBDeviceOpen: the default pipe takes class requests without it, so another program holding the portal is no bar.
    struct OutputHandle
    {
        IOUSBDeviceInterface182** device = nullptr;
        int interface = 0;
        size_t length = 1 + PortalReportSize;
        std::string error; // why there is no device

        void Open(const char* path)
        {
            error.clear(); // a portal plugged in again
            interface = 0;
            uint64_t id = portal_hid::MacRegistryEntryId(path);
            if (id == 0)
            {
                error = std::string("not a macOS HID path: ") + path;
                return;
            }
            io_registry_entry_t entry = IOServiceGetMatchingService(MACH_PORT_NULL, IORegistryEntryIDMatching(id));
            while (entry != 0 && !IOObjectConformsTo(entry, kIOUSBDeviceClassName))
            {
                if (IOObjectConformsTo(entry, "IOUSBHostInterface"))
                    if (CFTypeRef number =
                            IORegistryEntryCreateCFProperty(entry, CFSTR(kUSBInterfaceNumber), nullptr, 0))
                    {
                        if (CFGetTypeID(number) == CFNumberGetTypeID())
                            CFNumberGetValue(static_cast<CFNumberRef>(number), kCFNumberIntType, &interface);
                        CFRelease(number);
                    }
                io_registry_entry_t parent = 0;
                if (IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent) != KERN_SUCCESS)
                    parent = 0;
                IOObjectRelease(entry);
                entry = parent;
            }
            if (entry == 0)
            {
                error = "no USB device above the HID service";
                return;
            }
            IOCFPlugInInterface** plugin = nullptr;
            SInt32 score = 0;
            kern_return_t result = IOCreatePlugInInterfaceForService(
                entry, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin, &score);
            IOObjectRelease(entry);
            if (result != KERN_SUCCESS || plugin == nullptr)
            {
                error = IoError("IOCreatePlugInInterfaceForService", result);
                return;
            }
            HRESULT query = (*plugin)->QueryInterface(
                plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182), reinterpret_cast<LPVOID*>(&device));
            IODestroyPlugInInterface(plugin);
            if (query != S_OK || device == nullptr)
            {
                device = nullptr;
                error = IoError("QueryInterface", query);
            }
        }

        void Close()
        {
            if (device != nullptr)
                (*device)->Release(device);
            device = nullptr;
        }
    };
#elif defined(__linux__)
    constexpr auto& Methods = portal_hid::LinuxMethods;

    // libusb, loaded once, the first time a portal needs the control request.
    const portal_usb::Api* Libusb()
    {
        static portal_usb::Api usb{};
        static const bool loaded = portal_usb::LoadLibusb(usb);
        return loaded ? &usb : nullptr;
    }

    // The hidraw node SDL opened, to find its USB device, and the control link once a command needed it.
    struct OutputHandle
    {
        size_t length = 1 + PortalReportSize;
        std::string error;
        std::string path;
        portal_hid::ControlLink link;

        void Open(const char* hidraw)
        {
            error.clear();
            path = hidraw != nullptr ? hidraw : "";
        }
        void Close() { link.Release(); }

        // The first usb-control command claims the portal's interface: its hidraw node goes away until Close.
        bool Claim(std::string& why)
        {
            if (link.Claimed())
                return true;
            const portal_usb::Api* usb = Libusb();
            if (usb == nullptr)
                return why = "libusb-1.0 not found (set SKY_LIBUSB to its path)", false;
            why = link.Claim(*usb, portal_hid::LinuxUsbAddress("/sys", path), PortalVendor, PortalProduct);
            if (!why.empty())
                return false;
            fprintf(stderr,
                "[portal] hid: usb-control: interface %d claimed through libusb (the kernel's HID driver is detached "
                "from it while the portal is open; reports from endpoint %02X)\n",
                link.Interface(), link.In());
            return true;
        }
    };
#else
    constexpr WriteMethod Methods[] = { WriteMethod::HidWrite };

    struct OutputHandle
    {
        size_t length = 1 + PortalReportSize;
        std::string error;
        void Open(const char*) {}
        void Close() {}
    };
#endif

    // Report id 0 first, then the command zero-padded: 1 + 32 bytes, the output report's exact length that Windows wants.
    bool SendCommand(WriteMethod method, SDL_hid_device* device, OutputHandle& output, const uint8_t* command,
        size_t size, std::string& error)
    {
        uint8_t report[1 + 64]{};
        memcpy(report + 1, command, std::min(size, PortalReportSize));
#ifdef _WIN32
        if (method == WriteMethod::OutputReport)
        {
            size_t length = std::clamp(output.length, size_t(1 + PortalReportSize), sizeof(report));
            if (output.handle == INVALID_HANDLE_VALUE)
                return error = output.error, false;
            if (!HidDll::Get().SetOutputReport(output.handle, report, ULONG(length)))
                return error = LastError("HidD_SetOutputReport"), false;
            return true;
        }
#endif
#ifdef __linux__
        if (method == WriteMethod::ControlReport)
        {
            if (!output.Claim(error))
                return false;
            error = output.link.Send(command, size);
            return error.empty();
        }
#endif
#ifdef __APPLE__
        if (method == WriteMethod::ControlReport)
        {
            if (output.device == nullptr)
                return error = output.error, false;
            portal_hid::SetReport setReport = portal_hid::MakeSetReport(command, size, output.interface);
            IOUSBDevRequestTO request{};
            request.bmRequestType = setReport.requestType;
            request.bRequest = setReport.request;
            request.wValue = setReport.value;
            request.wIndex = setReport.index;
            request.wLength = sizeof(setReport.data);
            request.pData = setReport.data;
            request.noDataTimeout = request.completionTimeout = 500; // ms; a gone portal does not hold the thread
            IOReturn result = (*output.device)->DeviceRequestTO(output.device, &request);
            if (result != kIOReturnSuccess)
                return error = IoError("DeviceRequest", result), false;
            return true;
        }
#endif
        if (SDL_hid_write(device, report, 1 + PortalReportSize) < 0)
            return error = SDL_GetError(), false;
        return true;
    }

    // Whether the control link holds the portal's interface (Linux): SDL's handle on the hidraw node is then gone.
    bool Linked(const OutputHandle& output)
    {
#ifdef __linux__
        return output.link.Claimed();
#else
        (void)output;
        return false;
#endif
    }

    // One report, waiting up to `timeoutMs`: from the control link while it holds the interface, else from SDL's hidapi.
    // Its size, 0 for none, -1 on an error (`error` says which).
    int ReadReport(SDL_hid_device* device, OutputHandle& output, uint8_t* buffer, size_t capacity, int timeoutMs,
        std::string& error)
    {
#ifdef __linux__
        if (output.link.Claimed())
            return output.link.Read(buffer, int(capacity), unsigned(std::max(timeoutMs, 1)), error);
#endif
        (void)output;
        int size = SDL_hid_read_timeout(device, buffer, capacity, timeoutMs);
        if (size < 0)
            error = SDL_GetError();
        return size;
    }

    class HidPortal final : public PortalBackend
    {
    public:
        ~HidPortal() override { Close(); }

        const char* Name() const override { return "USB HID"; }

        // AsyncPortal (host/portal_async.h) calls this on its own thread and spaces out the attempts.
        bool IsConnected() override
        {
            if (m_device == nullptr)
                Open();
            return m_device != nullptr;
        }

        bool IsOpen() override { return m_device != nullptr; }

        // Tries the method that worked last first, then the others; keeps the first that works.
        bool Write(const uint8_t* command, size_t size) override
        {
            if (m_device == nullptr)
                return false;
            char letter = size > 0 ? char(command[0]) : '?';
            // The speaker on: a new line for the decoder (WriteSound).
            if (letter == 'M' && size > 1 && command[1] != 0)
            {
                m_decoder.Reset();
                m_samples = 0;
            }
            std::string errors;
            for (WriteMethod method : portal_hid::TryOrder(Methods, m_method))
            {
                std::string error;
                if (SendCommand(method, m_device, m_output, command, size, error))
                {
                    if (!m_wrote || method != m_method)
                        fprintf(stderr, "[portal] hid: wrote '%c' with %s (%zu-byte output report)\n", letter,
                            MethodName(method), m_output.length);
                    m_method = method;
                    m_wrote = true;
                    m_said.clear(); // working: the next failure is news
                    return true;
                }
                errors += std::string(errors.empty() ? "" : "; ") + MethodName(method) + ": " + error;
            }
            Say("[portal] hid: write of '" + std::string(1, letter) + "' failed (" + errors + ")");
            Close();
            return false;
        }

        // HID portals take the speaker's sound as 16-bit PCM at 8 kHz, 32 samples (64 bytes, little-endian) per write on the
        // interrupt OUT endpoint, where the console's ADPCM is decoded first. A portal that refuses it (Windows sends only
        // reports of the descriptor's 33 bytes) leaves the sound to the computer.
        bool TakesSound() override { return m_device != nullptr && !m_soundRefused && !Linked(m_output); }

        bool WriteSound(const uint8_t* block, size_t size) override
        {
            if (!TakesSound())
                return false;
            for (size_t i = 0; i < size * 2; i++)
            {
                const uint8_t byte = block[i / 2];
                const bool low = (i % 2 == 0) == portal_adpcm::LowNibbleFirst;
                const int16_t sample = m_decoder.Decode(low ? byte & 15 : byte >> 4);
                m_pcm[1 + m_samples * 2] = uint8_t(sample);
                m_pcm[2 + m_samples * 2] = uint8_t(uint16_t(sample) >> 8);
                if (++m_samples < SoundBlockSamples)
                    continue;
                m_samples = 0;
                if (SDL_hid_write(m_device, m_pcm, sizeof(m_pcm)) < 0)
                {
                    fprintf(stderr,
                        "[portal] hid: the portal refused a sound block (%s): the computer plays the sound\n",
                        SDL_GetError());
                    m_soundRefused = true;
                    return false;
                }
            }
            return true;
        }

        size_t Read(uint8_t* report, size_t capacity) override
        {
            if (m_device == nullptr)
                return 0;

            uint8_t buffer[64];
            std::string error;
            int size = ReadReport(m_device, m_output, buffer, sizeof(buffer), 0, error);
            if (size < 0)
            {
                Say("[portal] hid: read failed (" + error + ")");
                Close();
                return 0;
            }
            if (size > 0 && !m_read)
            {
                fprintf(stderr, "[portal] hid: read the first report, '%c' (%d bytes)\n", buffer[0], size);
                m_read = true;
            }

            size_t copied = std::min(size_t(size), capacity);
            memcpy(report, buffer, copied);
            return copied;
        }

    private:
        // A portal that opens and then fails at once repeats the same lines: each is logged once until a write works.
        void Say(const std::string& line)
        {
            if (m_said.insert(line).second)
                fprintf(stderr, "%s\n", line.c_str());
        }

        void Open()
        {
            SDL_hid_device_info* devices = EnumeratePortals();
            int count = 0;
            for (SDL_hid_device_info* device = devices; device != nullptr; device = device->next)
                count++;
            // Rescans while unplugged only log a change.
            if (count != m_lastCount)
            {
                if (count == 0)
                    fprintf(stderr, "[portal] hid: enumerated %04X:%04X: none found\n", PortalVendor, PortalProduct);
                for (SDL_hid_device_info* device = devices; device != nullptr; device = device->next)
                    Say("[portal] hid: enumerated " + Describe(*device));
            }
            for (SDL_hid_device_info* device = devices; device != nullptr && m_device == nullptr; device = device->next)
            {
                m_device = SDL_hid_open_path(device->path);
                if (m_device != nullptr)
                {
                    m_output.Open(device->path);
                    char line[160];
                    snprintf(line, sizeof(line),
                        "[portal] hid: opened %04X:%04X (interface %d, output report %zu bytes%s%s)", device->vendor_id,
                        device->product_id, device->interface_number, m_output.length,
                        m_output.error.empty() ? "" : "; no output-report handle: ", m_output.error.c_str());
                    Say(line);
                }
                else if (count != m_lastCount)
                {
                    fprintf(stderr, "[portal] hid: cannot open %s (%s)\n", device->path, SDL_GetError());
#ifdef __linux__
                    // /dev/hidraw* belongs to root: the player's account needs a udev rule (the kit's README.txt).
                    fprintf(stderr,
                        "[portal] hid: Linux needs this udev rule to open the portal, in "
                        "/etc/udev/rules.d/70-skylanders-portal.rules (then unplug and replug it): "
                        "SUBSYSTEM==\"hidraw\", ATTRS{idVendor}==\"1430\", ATTRS{idProduct}==\"0150\", MODE=\"0660\", "
                        "TAG+=\"uaccess\"\n");
#endif
                }
            }
            m_lastCount = count;
            m_wrote = m_read = m_soundRefused = false;
            m_samples = 0;
            SDL_hid_free_enumeration(devices);
        }

        void Close()
        {
            if (m_device == nullptr)
                return;
            SDL_hid_close(m_device);
            m_output.Close();
            m_device = nullptr;
        }

        SDL_hid_device* m_device = nullptr;
        OutputHandle m_output;
        WriteMethod m_method = Methods[0];
        int m_lastCount = -1;
        bool m_wrote = false, m_read = false;
        static constexpr size_t SoundBlockSamples = 32;
        portal_adpcm::Decoder m_decoder;
        uint8_t m_pcm[1 + SoundBlockSamples * 2]{}; // report id 0, then the samples
        size_t m_samples = 0;
        bool m_soundRefused = false;
        std::set<std::string> m_said;
    };
}

std::unique_ptr<PortalBackend> CreateHidPortal()
{
    if (SDL_hid_init() != 0)
    {
        fprintf(stderr, "[portal] hid: SDL_hid_init failed (%s)\n", SDL_GetError());
        return nullptr;
    }
    return std::make_unique<HidPortal>();
}

namespace
{
    // Sends `letter` and waits up to 1 s for the report that answers it (the portal may stream status reports
    // meanwhile). Returns "ok, <reply bytes> in <n> ms" or "error: ...".
    std::string Query(SDL_hid_device* device, OutputHandle& output, WriteMethod method, char letter,
        std::vector<uint8_t>* answer = nullptr)
    {
        uint8_t report[64];
        std::string error;
        while (ReadReport(device, output, report, sizeof(report), 0, error) > 0)
        {}
        error.clear();
        const uint8_t command[] = { uint8_t(letter) };
        uint64_t start = SDL_GetTicks();
        if (!SendCommand(method, device, output, command, sizeof(command), error))
            return "error: write failed (" + error + ")";
        while (SDL_GetTicks() < start + 1000)
        {
            int size = ReadReport(device, output, report, sizeof(report), 50, error);
            if (size < 0)
                return "error: read failed (" + error + ")";
            if (size > 0 && report[0] == uint8_t(letter))
            {
                if (answer != nullptr)
                    answer->assign(report, report + size);
                char text[96];
                int length = snprintf(text, sizeof(text), "ok, '%c'", letter);
                for (int i = 1; i < std::min(size, 9); i++)
                    length += snprintf(text + length, sizeof(text) - length, " %02X", report[i]);
                snprintf(text + length, sizeof(text) - length, " in %llu ms",
                    static_cast<unsigned long long>(SDL_GetTicks() - start));
                return text;
            }
        }
        return std::string("error: no '") + letter + "' answer within 1 s";
    }
}

std::string ProbeHidPortals(std::vector<PortalProbe>& portals)
{
    if (SDL_hid_init() != 0)
        return std::string("SDL_hid_init failed: ") + SDL_GetError();

    SDL_hid_device_info* devices = EnumeratePortals();
    for (SDL_hid_device_info* device = devices; device != nullptr; device = device->next)
    {
        PortalProbe probe{ "hid", device->vendor_id, device->product_id, Utf8(device->product_string) };
        if (SDL_hid_device* handle = SDL_hid_open_path(device->path))
        {
            probe.opened = true;
            OutputHandle output;
            output.Open(device->path);
            char details[96];
            snprintf(details, sizeof(details), "interface %d, usage page %04X, output report %zu bytes",
                device->interface_number, device->usage_page, output.length);
            probe.details = details;
            if (!output.error.empty())
                probe.details += " (no output-report handle: " + output.error + ")";
            // Each method in turn: reset ('R', answered by an 'R' report), then status ('S').
            for (WriteMethod method : Methods)
            {
                std::vector<uint8_t> answer;
                std::string reset = Query(handle, output, method, 'R', &answer);
                if (probe.id.empty())
                    NameProbe(probe, answer.data(), answer.size());
                std::string status = reset.rfind("ok", 0) == 0 ? Query(handle, output, method, 'S') : "not tried";
                probe.methods.push_back({ MethodName(method), "R: " + reset + "; S: " + status });
                if (probe.write.empty() && reset.rfind("ok", 0) == 0)
                    probe.write = MethodName(method);
                if (probe.status.empty() || probe.write == MethodName(method))
                    probe.status = probe.write.empty() ? reset : "ok";
            }
            output.Close();
            SDL_hid_close(handle);
        }
        else
            probe.status = std::string("error: cannot open (") + SDL_GetError() + ")";
        portals.push_back(probe);
    }
    SDL_hid_free_enumeration(devices);
    return {};
}
