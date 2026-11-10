// Portals that are vendor-specific USB devices. Linux and Windows reach them with libusb-1.0 (LGPL-2.1), loaded at run
// time: the build does not depend on it, and a host without it simply has no Xbox portal support. macOS reaches them
// through IOKit (host/portal_usb_mac.cpp): nothing to install.
//   Xbox 360 portals: interrupt endpoints carrying 32-byte reports that start with a 2-byte header (0B 14 on
//   commands), then the protocol report.
//   Xbox One portals: GIP (Xbox One accessory protocol) devices. EXPERIMENTAL, untested on hardware: the protocol
//   reports are assumed to travel in GIP message 0x21 with a 30-byte payload, the one custom message the portal's
//   GIP descriptor declares (docs/runtime/host/portal.md).
#include "host/portal.h"
#include "host/portal_backend.h"
#include "host/portal_usb.h"
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>
#include "env.h"
#include "host/windows_devices.h"
#ifdef _WIN32
#include <windows.h>
#include <cfgmgr32.h>
#ifndef CM_GETIDLIST_FILTER_PRESENT
#define CM_GETIDLIST_FILTER_PRESENT 0x00000100 // Windows 7 and later; older mingw-w64 headers lack it
#endif
#endif

namespace
{
    using portal_usb::Api;
    using portal_usb::DeviceDescriptor;
    using portal_usb::Interface;

#ifdef __APPLE__
    // macOS has no libusb: IOKit answers the same calls (host/portal_usb_mac.cpp).
    bool LoadUsb(Api& usb) { return portal_usb::LoadIokit(usb); }
#else
    bool LoadUsb(Api& usb) { return portal_usb::LoadLibusb(usb); }
#endif
}

#ifndef __APPLE__
bool portal_usb::LoadLibusb(Api& usb)
{
    const char* names[] = { skyenv::Get("SKY_LIBUSB"), "libusb-1.0.so.0", "libusb-1.0.so", "libusb-1.0.dll" };
    SDL_SharedObject* library = nullptr;
    for (const char* name : names)
    {
        if (name != nullptr && (library = SDL_LoadObject(name)) != nullptr)
        {
            usb.from = name;
            break;
        }
    }
    if (library == nullptr)
        return false;

    bool complete = true;
    auto load = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(SDL_LoadFunction(library, name));
        complete = complete && function != nullptr;
    };
    load(usb.init, "libusb_init");
    load(usb.get_device_list, "libusb_get_device_list");
    load(usb.free_device_list, "libusb_free_device_list");
    load(usb.get_device_descriptor, "libusb_get_device_descriptor");
    load(usb.open, "libusb_open");
    load(usb.close, "libusb_close");
    load(usb.set_auto_detach_kernel_driver, "libusb_set_auto_detach_kernel_driver");
    load(usb.claim_interface, "libusb_claim_interface");
    load(usb.release_interface, "libusb_release_interface");
    load(usb.control_transfer, "libusb_control_transfer");
    load(usb.interrupt_transfer, "libusb_interrupt_transfer");
    load(usb.error_name, "libusb_error_name");
    usb.get_bus_number =
        reinterpret_cast<decltype(usb.get_bus_number)>(SDL_LoadFunction(library, "libusb_get_bus_number"));
    usb.get_device_address =
        reinterpret_cast<decltype(usb.get_device_address)>(SDL_LoadFunction(library, "libusb_get_device_address"));
    if (!complete || usb.init(&usb.context) != 0)
    {
        SDL_UnloadObject(library);
        return false;
    }
    return true;
}
#endif

namespace
{
    enum class Framing
    {
        Xbox360,
        Gip,
    };

    struct KnownPortal
    {
        uint16_t vendor, product;
    };

    // The Xbox portals: the ids known (portal_usb::KnownXboxPortal), and any other Activision device whose interface
    // is an Xbox 360 or a GIP one. SKY_PORTAL_USB=VVVV:PPPP selects another device.
    using portal_usb::XboxFamily;
    constexpr uint16_t ActivisionVendor = 0x1430;
    constexpr uint16_t HidPortal = 0x0150; // the HID transport's

    // GIP framing: command, options, sequence, LEB128 payload length, payload.
    constexpr uint8_t GipAcknowledge = 0x01;
    constexpr uint8_t GipAnnounce = 0x02;
    constexpr uint8_t GipPower = 0x05;
    constexpr uint8_t GipPortalReport = 0x21;
    constexpr uint8_t GipOptionAcknowledge = 0x10;
    constexpr uint8_t GipOptionInternal = 0x20;
    constexpr uint8_t GipPortalReportSize = 30;

    class UsbPortal final : public PortalBackend
    {
    public:
        explicit UsbPortal(const Api& usb) : m_usb(usb)
        {
            if (const char* selected = skyenv::Get("SKY_PORTAL_USB"))
            {
                unsigned vendor = 0, product = 0;
                if (sscanf(selected, "%x:%x", &vendor, &product) == 2)
                    m_selected = { uint16_t(vendor), uint16_t(product) };
            }
        }
        ~UsbPortal() override { Close(); }

        const char* Name() const override
        {
            return m_framing == Framing::Gip ? "Xbox One USB (experimental)" : "Xbox 360 USB";
        }

        bool IsConnected() override
        {
            if (m_handle != nullptr && m_lost)
                Close();
            if (m_handle == nullptr && std::chrono::steady_clock::now() >= m_nextScan)
            {
                m_nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                Open();
            }
            return m_handle != nullptr;
        }

        bool Write(const uint8_t* command, size_t size) override
        {
            if (m_handle == nullptr)
                return false;

            if (m_framing == Framing::Gip)
            {
                uint8_t payload[GipPortalReportSize]{};
                memcpy(payload, command, std::min<size_t>(size, sizeof(payload)));
                return SendGip(GipPortalReport, 0, NextSequence(), payload, sizeof(payload));
            }

            auto packet = portal_usb::MakeXbox360Packet(portal_usb::Xbox360CommandHeader, command, size);
            return Transfer(m_interface.out, packet.data(), int(packet.size()));
        }

        // The Xbox 360 portal takes the blocks as the console sends them. The Xbox One portal's sound message is not
        // known: the computer plays the sound there.
        bool TakesSound() override { return m_handle != nullptr && m_framing == Framing::Xbox360; }

        bool WriteSound(const uint8_t* block, size_t size) override
        {
            if (!TakesSound())
                return false;
            auto packet = portal_usb::MakeXbox360Packet(portal_usb::Xbox360SoundHeader, block, size);
            return Transfer(m_interface.out, packet.data(), int(packet.size()));
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

    private:
        void Open()
        {
            portal_usb::Device** devices = nullptr;
            ptrdiff_t count = m_usb.get_device_list(m_usb.context, &devices);
            for (ptrdiff_t i = 0; i < count && m_handle == nullptr; i++)
            {
                DeviceDescriptor descriptor{};
                if (m_usb.get_device_descriptor(devices[i], &descriptor) != 0)
                    continue;

                const XboxFamily known = portal_usb::KnownXboxPortal(descriptor.idVendor, descriptor.idProduct);
                bool selected = descriptor.idVendor == m_selected.vendor && descriptor.idProduct == m_selected.product;
                if (known != XboxFamily::None || selected ||
                    (descriptor.idVendor == ActivisionVendor && descriptor.idProduct != HidPortal))
                    TryOpen(devices[i], descriptor, known, selected);
            }
            if (count >= 0)
                m_usb.free_device_list(devices, 1);
            if (m_handle == nullptr && !m_reportedNone)
                fprintf(stderr, "[portal] usb: no Xbox 360 or Xbox One portal found (%d USB devices)\n", int(count));
            m_reportedNone = m_handle == nullptr;
        }

        // Known Xbox portals and the selected device use whatever interface they have; other Activision devices need
        // an Xbox 360 or a GIP interface.
        void TryOpen(portal_usb::Device* device, const DeviceDescriptor& descriptor, XboxFamily known, bool selected)
        {
            const bool xbox360 = known != XboxFamily::None;
            portal_usb::Handle* handle = nullptr;
            if (int result = m_usb.open(device, &handle); result != 0)
            {
                // Logged on the first scan only (rescans every 2 s while none is connected).
                if ((xbox360 || selected) && !m_reportedNone)
                    LogOpenFailure(descriptor, result);
                return;
            }

            // GET_DESCRIPTOR(configuration 0), read raw.
            uint8_t configuration[512];
            int size = m_usb.control_transfer(handle, 0x80, 0x06, 0x0200, 0, configuration, sizeof(configuration), 500);
            Interface usbInterface = size > 0 ? portal_usb::FindInterface(configuration, size_t(size)) : Interface{};
            const XboxFamily family = known != XboxFamily::None ? known : portal_usb::XboxFamilyOf(usbInterface);
            if (usbInterface.number < 0 || (family == XboxFamily::None && !selected))
            {
                // An Activision device that is no portal we know: its interfaces, once, so a player's log names it.
                const uint32_t id = uint32_t(descriptor.idVendor) << 16 | descriptor.idProduct;
                if (std::find(m_unknown.begin(), m_unknown.end(), id) == m_unknown.end())
                {
                    m_unknown.push_back(id);
                    fprintf(stderr, "[portal] usb: %04X:%04X is no portal we know: %s\n", descriptor.idVendor,
                        descriptor.idProduct,
                        size > 0 ? portal_usb::DescribeInterfaces(configuration, size_t(size)).c_str()
                                 : "its configuration could not be read");
                }
                m_usb.close(handle);
                return;
            }

            // What the device is, for a player's log (logged once per connection): its interfaces, and on Linux the kernel
            // driver each one had before the claim (xpad takes vendor 1430's Xbox 360 class interfaces).
            fprintf(stderr, "[portal] usb: %04X:%04X %s\n", descriptor.idVendor, descriptor.idProduct,
                portal_usb::DescribeInterfaces(configuration, size_t(size)).c_str());
#ifdef __linux__
            if (std::string drivers =
                    portal_usb::LinuxInterfaceDrivers("/sys", descriptor.idVendor, descriptor.idProduct);
                !drivers.empty())
                fprintf(stderr, "[portal] usb: kernel drivers before the claim: %s\n", drivers.c_str());
#endif
            m_usb.set_auto_detach_kernel_driver(handle, 1);
            int result = m_usb.claim_interface(handle, usbInterface.number);
            if (result != 0)
            {
                fprintf(stderr, "[portal] USB portal %04X:%04X: cannot claim interface %d (%s)\n", descriptor.idVendor,
                    descriptor.idProduct, usbInterface.number, m_usb.error_name(result));
                m_usb.close(handle);
                return;
            }

            m_handle = handle;
            m_interface = usbInterface;
            m_framing = family == XboxFamily::XboxOne || (family == XboxFamily::None && usbInterface.gip)
                            ? Framing::Gip
                            : Framing::Xbox360;
            m_lost = false;
            m_wroteFailed = m_readOne = m_wroteOne = m_saidSilent = false;
            m_firstWrite = std::chrono::steady_clock::time_point{};
            m_reports.clear();
            m_reader = std::thread([this] { ReadLoop(); });
            fprintf(stderr, "[portal] %s portal %04X:%04X connected (interface %d, endpoints %02X/%02X)\n", Name(),
                descriptor.idVendor, descriptor.idProduct, usbInterface.number, usbInterface.in, usbInterface.out);

            if (m_framing == Framing::Gip)
                PowerOn();
        }

        void Close()
        {
            if (m_handle == nullptr)
                return;

            m_lost = true;
            if (m_reader.joinable())
                m_reader.join();
            m_usb.release_interface(m_handle, m_interface.number);
            m_usb.close(m_handle);
            m_handle = nullptr;
            fprintf(stderr, "[portal] USB portal disconnected\n");
        }

        bool Transfer(uint8_t endpoint, uint8_t* data, int size)
        {
            int transferred = 0;
            int result = m_usb.interrupt_transfer(m_handle, endpoint, data, size, &transferred, 100);
            if (result != 0 || transferred != size)
            {
                // The first failure of a connection always (a portal that takes no command is never seen by the game).
                if (PortalLogEnabled() || !m_wroteFailed)
                    fprintf(stderr, "[portal] USB write failed (%s, %d of %d bytes)\n", m_usb.error_name(result),
                        transferred, size);
                m_wroteFailed = true;
                return false;
            }
            if (!m_wroteOne)
            {
                // The first command the portal took, with its bytes: the framing a player's log can be checked against.
                m_wroteOne = true;
                m_firstWrite = std::chrono::steady_clock::now();
                fprintf(stderr, "[portal] usb: first write accepted on endpoint %02X (%d bytes: %s)\n", endpoint, size,
                    portal_usb::HexBytes(data, size_t(size)).c_str());
            }
            return true;
        }

        void LogOpenFailure(const DeviceDescriptor& descriptor, int result)
        {
            fprintf(stderr, "[portal] USB portal %04X:%04X: cannot open (%s)\n", descriptor.idVendor,
                descriptor.idProduct, m_usb.error_name(result));
#ifdef __linux__
            if (result == portal_usb::ErrorAccess)
                fprintf(stderr,
                    "[portal] usb: Linux needs this udev rule to open the portal, in "
                    "/etc/udev/rules.d/70-skylanders-portal.rules (then unplug and replug it): %s\n",
                    portal_usb::LinuxUdevRule(descriptor.idVendor, descriptor.idProduct).c_str());
#endif
        }

        // The first packet of a connection, of any size, header included: proof that the portal talks.
        void LogFirstReport(const uint8_t* packet, size_t size)
        {
            m_readOne = true;
            fprintf(stderr, "[portal] usb: first report (%zu bytes): %s\n", size,
                portal_usb::HexBytes(packet, size).c_str());
        }

        // A portal that took commands for 5 s and sent nothing back (issue #20: an Xbox 360 portal on Linux), said once.
        void CheckSilence()
        {
            if (m_readOne || m_saidSilent || !m_wroteOne ||
                std::chrono::steady_clock::now() - m_firstWrite.load() < std::chrono::seconds(5))
                return;
            m_saidSilent = true;
            fprintf(stderr,
                "[portal] usb: nothing came from endpoint %02X in 5 s, while the portal took the commands sent to "
                "%02X: "
                "it does not answer (docs/runtime/host/portal.md, \"An Xbox 360 portal that takes commands and never "
                "answers\")\n",
                m_interface.in, m_interface.out);
        }

        void ReadLoop()
        {
            uint8_t packet[64];
            while (!m_lost)
            {
                int size = 0;
                int result = m_usb.interrupt_transfer(m_handle, m_interface.in, packet, sizeof(packet), &size, 100);
                if (result == portal_usb::ErrorTimeout || (result == 0 && size == 0))
                {
                    CheckSilence();
                    continue;
                }
                if (result != 0)
                {
                    fprintf(stderr, "[portal] USB read failed (%s)\n", m_usb.error_name(result));
                    m_lost = true;
                    break;
                }

                if (!m_readOne)
                    LogFirstReport(packet, size_t(size));
                if (m_framing == Framing::Gip)
                    HandleGip(packet, size_t(size));
                else if (portal_usb::Span report = portal_usb::Xbox360Report(size_t(size)); report.size > 0)
                    Queue(packet + report.offset, report.size);
            }
        }

        void Queue(const uint8_t* report, size_t size)
        {
            std::lock_guard lock(m_mutex);
            if (m_reports.size() >= 64)
                m_reports.pop_front();
            m_reports.emplace_back(report, report + size);
        }

        // GIP sequence numbers skip 0. Used by the title's thread and the reader (power-on, acknowledgements).
        uint8_t NextSequence()
        {
            uint8_t sequence = m_sequence++;
            return sequence != 0 ? sequence : m_sequence++;
        }

        bool SendGip(uint8_t command, uint8_t options, uint8_t sequence, const uint8_t* payload, size_t size)
        {
            uint8_t packet[64] = { command, options, sequence, uint8_t(size) };
            memcpy(packet + 4, payload, size);
            return Transfer(m_interface.out, packet, int(4 + size));
        }

        void PowerOn()
        {
            const uint8_t on = 0x00;
            SendGip(GipPower, GipOptionInternal, NextSequence(), &on, 1);
        }

        void HandleGip(const uint8_t* packet, size_t size)
        {
            if (size < 4)
                return;

            uint8_t command = packet[0], options = packet[1], sequence = packet[2];
            size_t length = 0, offset = 3;
            for (int shift = 0; offset < size && shift < 28; shift += 7)
            {
                uint8_t byte = packet[offset++];
                length |= size_t(byte & 0x7F) << shift;
                if ((byte & 0x80) == 0)
                    break;
            }
            length = std::min(length, size - offset);

            if (options & GipOptionAcknowledge)
            {
                const uint8_t acknowledge[9] = { 0, command, uint8_t(options & GipOptionInternal), uint8_t(length),
                    uint8_t(length >> 8) };
                SendGip(GipAcknowledge, GipOptionInternal, sequence, acknowledge, sizeof(acknowledge));
            }

            if (command == GipPortalReport && !(options & GipOptionInternal))
            {
                Queue(packet + offset, length);
                return;
            }
            if (command == GipAnnounce && (options & GipOptionInternal))
                PowerOn();

            if (PortalLogEnabled())
            {
                fprintf(stderr, "[portal] GIP %02X %02X %02X:", command, options, sequence);
                for (size_t i = offset; i < offset + length; i++)
                    fprintf(stderr, " %02X", packet[i]);
                fprintf(stderr, "\n");
            }
        }

        Api m_usb;
        KnownPortal m_selected{};
        portal_usb::Handle* m_handle = nullptr;
        Interface m_interface;
        Framing m_framing = Framing::Xbox360;
        std::thread m_reader;
        std::atomic<bool> m_lost = false;
        std::mutex m_mutex;
        std::deque<std::vector<uint8_t>> m_reports;
        std::atomic<uint8_t> m_sequence = 1;
        std::chrono::steady_clock::time_point m_nextScan{};
        bool m_reportedNone = false;    // rescans log "none found" once
        std::vector<uint32_t> m_unknown; // the Activision devices logged as no portal (vendor << 16 | product)
        bool m_wroteFailed = false; // logged once per connection
        std::atomic<bool> m_readOne = false;
        std::atomic<bool> m_wroteOne = false, m_saidSilent = false;
        std::atomic<std::chrono::steady_clock::time_point> m_firstWrite{};
    };
}

#ifdef _WIN32
namespace
{
    // Every present USB node of vendor 1430 and of 24C6:FA00 (portal_usb::KnownXboxPortal), with its driver service, read
    // from the configuration manager (read only: nothing is opened).
    std::vector<portal_usb::WindowsUsbNode> WindowsPortalNodes()
    {
        std::vector<portal_usb::WindowsUsbNode> nodes;
        const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
        std::vector<char> list;
        for (int attempt = 0; attempt < 3; attempt++) // a device plugged in between the two calls makes the list longer
        {
            ULONG size = 0;
            if (CM_Get_Device_ID_List_SizeA(&size, "USB", flags) != CR_SUCCESS || size == 0)
                return nodes;
            list.assign(size, '\0');
            const CONFIGRET result = CM_Get_Device_ID_ListA("USB", list.data(), size, flags);
            if (result == CR_SUCCESS)
                break;
            if (result != CR_BUFFER_SMALL || attempt == 2)
                return nodes;
        }
        for (const char* id = list.data(); id < list.data() + list.size() && *id != '\0'; id += strlen(id) + 1)
        {
            portal_usb::WindowsUsbNode node;
            if (!portal_usb::ParseWindowsUsbInstance(id, node) ||
                !(node.vendor == ActivisionVendor ||
                    portal_usb::KnownXboxPortal(node.vendor, node.product) != XboxFamily::None))
                continue;
            DEVINST instance = 0;
            char service[128] = "";
            ULONG length = sizeof(service);
            if (CM_Locate_DevNodeA(&instance, const_cast<char*>(id), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
                CM_Get_DevNode_Registry_PropertyA(instance, CM_DRP_SERVICE, nullptr, service, &length, 0) == CR_SUCCESS)
                node.service = service;
            nodes.push_back(node);
        }
        return nodes;
    }

    // The game's log: each portal Windows has, its driver, and what is wrong with it for Arkeyan (issue #19).
    void LogWindowsPortalDrivers(bool libusb)
    {
        for (const std::string& line : ProbeNotes())
            fprintf(stderr, "[portal] %s\n", line.c_str());
        for (const portal_usb::WindowsUsbNode& node : WindowsPortalNodes())
        {
            char where[16] = "";
            if (node.interfaceNumber >= 0)
                snprintf(where, sizeof(where), " interface %d", node.interfaceNumber);
            fprintf(stderr, "[portal] windows: USB %04X:%04X%s, driver %s\n", node.vendor, node.product, where,
                node.service.empty() ? "(none)" : node.service.c_str());
            if (std::string problem = portal_usb::WindowsDriverProblem(node, libusb); !problem.empty())
                fprintf(stderr, "[portal] windows: %04X:%04X: %s\n", node.vendor, node.product, problem.c_str());
        }
    }
}
#endif

std::vector<std::string> ProbeNotes()
{
    std::vector<std::string> notes;
#ifdef _WIN32
    // Every device that looks like a portal, whatever its driver: a portal Arkeyan cannot reach still shows there.
    for (const windows_devices::Device& device : windows_devices::PortalLikeDevices())
        notes.push_back("windows device: " + windows_devices::Describe(device));
    if (notes.empty())
        notes.push_back("windows device: none looks like a portal (USB 1430:*, 24C6:*, 0E6F:*, a name with "
                        "\"Portal\" or \"Skylanders\")");
#endif
    return notes;
}

std::unique_ptr<PortalBackend> CreateUsbPortal()
{
    Api usb{};
    const bool loaded = LoadUsb(usb);
#ifdef _WIN32
    LogWindowsPortalDrivers(loaded);
#endif
    if (!loaded)
        return nullptr;
    fprintf(stderr, "[portal] usb: %s\n", usb.from);
    return std::make_unique<UsbPortal>(usb);
}

namespace
{
    // Reset, in the Xbox 360 framing: "ok" only when the 'R' answer comes back (0.9.4 took any packet, and nothing back
    // as well, for an answer); else the first packet that came, which tells the framing the portal uses.
    std::string Query(const Api& usb, portal_usb::Handle* handle, const Interface& usbInterface, PortalProbe& probe)
    {
        const uint8_t reset = 'R';
        auto packet = portal_usb::MakeXbox360Packet(portal_usb::Xbox360CommandHeader, &reset, 1);
        int transferred = 0;
        int result =
            usb.interrupt_transfer(handle, usbInterface.out, packet.data(), int(packet.size()), &transferred, 500);
        if (result != 0)
            return std::string("error: write failed (") + usb.error_name(result) + ")";
        // The answer carries the 2-byte header before the 'R'; a report of another kind may come first (status).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        std::string first;
        int packets = 0;
        for (auto now = std::chrono::steady_clock::now(); now < deadline; now = std::chrono::steady_clock::now())
        {
            uint8_t reply[64];
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            result = usb.interrupt_transfer(
                handle, usbInterface.in, reply, sizeof(reply), &transferred, unsigned(std::max<long long>(left, 1)));
            if (result == portal_usb::ErrorTimeout)
                break;
            if (result != 0)
                return std::string("error: read failed (") + usb.error_name(result) + ")";
            if (packets++ == 0)
                first = portal_usb::HexBytes(reply, size_t(transferred));
            if (const int at = portal_usb::Xbox360ResetAnswer(reply, size_t(transferred)); at >= 0)
            {
                NameProbe(probe, reply + at, size_t(transferred - at));
                return "ok";
            }
        }
        return packets == 0 ? "error: no 'R' answer within 1 s (nothing came back)"
                            : "error: no 'R' answer within 1 s (" + std::to_string(packets) +
                                  " other packets, the first: " + first + ")";
    }
}

std::string ProbeUsbPortals(std::vector<PortalProbe>& portals)
{
    Api usb{};
    const bool loaded = LoadUsb(usb);
#ifdef _WIN32
    // A portal on a driver Arkeyan cannot reach it through is still a portal: listed with what to do (issue #19).
    std::vector<std::pair<uint16_t, uint16_t>> listed;
    for (const portal_usb::WindowsUsbNode& node : WindowsPortalNodes())
    {
        const std::string problem = portal_usb::WindowsDriverProblem(node, loaded);
        if (problem.empty() ||
            std::find(listed.begin(), listed.end(), std::pair(node.vendor, node.product)) != listed.end())
            continue;
        listed.emplace_back(node.vendor, node.product);
        const XboxFamily family = portal_usb::KnownXboxPortal(node.vendor, node.product);
        PortalProbe probe{ "usb", node.vendor, node.product,
            node.vendor == ActivisionVendor && node.product == HidPortal ? "Portal of Power"
            : family != XboxFamily::None                                 ? portal_usb::XboxFamilyName(family)
                                                                         : "Activision device" };
        probe.status = "error: " + problem;
        probe.details = "driver " + (node.service.empty() ? std::string("(none)") : node.service);
        portals.push_back(probe);
    }
#endif
    if (!loaded)
        return "libusb-1.0 not found (set SKY_LIBUSB to its path)";

    portal_usb::Device** devices = nullptr;
    ptrdiff_t count = usb.get_device_list(usb.context, &devices);
    for (ptrdiff_t i = 0; i < count; i++)
    {
        DeviceDescriptor descriptor{};
        if (usb.get_device_descriptor(devices[i], &descriptor) != 0)
            continue;
        const XboxFamily known = portal_usb::KnownXboxPortal(descriptor.idVendor, descriptor.idProduct);
        // The HID portals (1430:0150) are the HID transport's.
        if (known == XboxFamily::None && (descriptor.idVendor != ActivisionVendor || descriptor.idProduct == HidPortal))
            continue;
#ifdef _WIN32
        if (std::find(listed.begin(), listed.end(), std::pair(descriptor.idVendor, descriptor.idProduct)) !=
            listed.end())
            continue; // already said why it cannot be used
#endif

        PortalProbe probe{ "usb", descriptor.idVendor, descriptor.idProduct,
            known != XboxFamily::None ? portal_usb::XboxFamilyName(known) : "Activision device" };
        portal_usb::Handle* handle = nullptr;
        int result = usb.open(devices[i], &handle);
        if (result != 0)
        {
            probe.status = std::string("error: cannot open (") + usb.error_name(result) + ")";
#ifdef __linux__
            if (result == portal_usb::ErrorAccess) // the kit's README.txt has the rule
                probe.status += "; Linux needs the portal's udev rule: " +
                                portal_usb::LinuxUdevRule(descriptor.idVendor, descriptor.idProduct);
#endif
            portals.push_back(probe);
            continue;
        }
        probe.opened = true;
        uint8_t configuration[512];
        int size = usb.control_transfer(handle, 0x80, 0x06, 0x0200, 0, configuration, sizeof(configuration), 500);
        Interface usbInterface = size > 0 ? portal_usb::FindInterface(configuration, size_t(size)) : Interface{};
        if (size > 0)
            probe.details = portal_usb::DescribeInterfaces(configuration, size_t(size));
#ifdef __linux__
        if (std::string drivers = portal_usb::LinuxInterfaceDrivers("/sys", descriptor.idVendor, descriptor.idProduct);
            !drivers.empty())
            probe.details += "; kernel drivers: " + drivers;
#endif
        const XboxFamily family = known != XboxFamily::None ? known : portal_usb::XboxFamilyOf(usbInterface);
        const bool gip = family == XboxFamily::XboxOne || (family == XboxFamily::None && usbInterface.gip);
        if (gip)
            probe.name = "Xbox One portal (experimental)";
        else if (family == XboxFamily::Xbox360)
            probe.name = "Xbox 360 portal";
        if (usbInterface.number < 0 || family == XboxFamily::None)
            probe.status = "error: no portal interface";
        else if (usb.set_auto_detach_kernel_driver(handle, 1);
            (result = usb.claim_interface(handle, usbInterface.number)) != 0)
            probe.status = std::string("error: cannot claim interface (") + usb.error_name(result) + ")";
        else
        {
            probe.status = gip ? "ok" : Query(usb, handle, usbInterface, probe); // GIP: opened and claimed only
            usb.release_interface(handle, usbInterface.number);
        }
        usb.close(handle);
        portals.push_back(probe);
    }
    if (count >= 0)
        usb.free_device_list(devices, 1);
    return {};
}
