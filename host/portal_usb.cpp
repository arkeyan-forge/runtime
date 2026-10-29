// Portals that are vendor-specific USB devices, reached with libusb-1.0 (LGPL-2.1). libusb is loaded at run time:
// the build does not depend on it, and a host without it simply has no Xbox portal support.
//   Xbox 360 portals: interrupt endpoints carrying 32-byte reports that start with a 2-byte header (0B 14 on
//   commands), then the protocol report.
//   Xbox One portals: GIP (Xbox One accessory protocol) devices. EXPERIMENTAL, untested on hardware: the protocol
//   reports are assumed to travel in GIP message 0x21 with a 30-byte payload, the one custom message the portal's
//   GIP descriptor declares (docs/runtime/host/portal.md).
#include "host/portal.h"
#include "host/portal_backend.h"
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

namespace
{
    // The part of the libusb-1.0 API used here; its ABI is stable since 1.0.0. Descriptors are read as raw bytes
    // to avoid depending on libusb's structure layouts, except the 18-byte device descriptor (packed by nature).
    struct libusb_context;
    struct libusb_device;
    struct libusb_device_handle;

    struct UsbDeviceDescriptor
    {
        uint8_t bLength, bDescriptorType;
        uint16_t bcdUSB;
        uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
        uint16_t idVendor, idProduct, bcdDevice;
        uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
    };
    static_assert(sizeof(UsbDeviceDescriptor) == 18);

    constexpr int UsbErrorTimeout = -7;

    struct Libusb
    {
        int (*init)(libusb_context**);
        ptrdiff_t (*get_device_list)(libusb_context*, libusb_device***);
        void (*free_device_list)(libusb_device**, int);
        int (*get_device_descriptor)(libusb_device*, UsbDeviceDescriptor*);
        int (*open)(libusb_device*, libusb_device_handle**);
        void (*close)(libusb_device_handle*);
        int (*set_auto_detach_kernel_driver)(libusb_device_handle*, int);
        int (*claim_interface)(libusb_device_handle*, int);
        int (*release_interface)(libusb_device_handle*, int);
        int (*control_transfer)(
            libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, unsigned int);
        int (*interrupt_transfer)(libusb_device_handle*, unsigned char, unsigned char*, int, int*, unsigned int);
        const char* (*error_name)(int);
        libusb_context* context;
    };

    bool LoadLibusb(Libusb& usb)
    {
        const char* names[] = { skyenv::Get("SKY_LIBUSB"), "libusb-1.0.dylib", "/opt/homebrew/lib/libusb-1.0.dylib",
            "/usr/local/lib/libusb-1.0.dylib", "libusb-1.0.so.0", "libusb-1.0.dll" };
        SDL_SharedObject* library = nullptr;
        for (const char* name : names)
        {
            if (name != nullptr && (library = SDL_LoadObject(name)) != nullptr)
                break;
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
        if (!complete || usb.init(&usb.context) != 0)
        {
            SDL_UnloadObject(library);
            return false;
        }
        return true;
    }

    enum class Framing
    {
        Xbox360,
        Gip,
    };

    struct KnownPortal
    {
        uint16_t vendor, product;
    };

    // Xbox 360 portals (Xenia's list; the first one is the portal sold with Spyro's Adventure).
    constexpr KnownPortal Xbox360Portals[] = { { 0x1430, 0x1F17 }, { 0x24C6, 0xFA00 } };
    // Xbox One portals: any Activision device with a GIP interface (the product id is not known yet).
    // SKY_PORTAL_USB=VVVV:PPPP selects another device.
    constexpr uint16_t ActivisionVendor = 0x1430;

    constexpr uint8_t Xbox360Header[2] = { 0x0B, 0x14 };

    // GIP framing: command, options, sequence, LEB128 payload length, payload.
    constexpr uint8_t GipAcknowledge = 0x01;
    constexpr uint8_t GipAnnounce = 0x02;
    constexpr uint8_t GipPower = 0x05;
    constexpr uint8_t GipPortalReport = 0x21;
    constexpr uint8_t GipOptionAcknowledge = 0x10;
    constexpr uint8_t GipOptionInternal = 0x20;
    constexpr uint8_t GipPortalReportSize = 30;

    struct UsbInterface
    {
        int number = -1;
        uint8_t in = 0;
        uint8_t out = 0;
        bool gip = false;
    };

    // Picks the first interface with an interrupt IN and OUT endpoint, preferring the GIP data interface
    // (class FF, subclass 47, protocol D0), from a raw configuration descriptor.
    UsbInterface FindInterface(const uint8_t* descriptor, size_t size)
    {
        UsbInterface best, current;
        bool alternate = false;
        auto consider = [&] {
            if (current.number >= 0 && current.in != 0 && current.out != 0 &&
                (best.number < 0 || (current.gip && !best.gip)))
                best = current;
        };

        for (size_t offset = 0; offset + 2 <= size && descriptor[offset] >= 2; offset += descriptor[offset])
        {
            const uint8_t* entry = descriptor + offset;
            if (entry[1] == 4 && entry[0] >= 9) // interface
            {
                consider();
                current = {};
                alternate = entry[3] != 0;
                if (!alternate)
                {
                    current.number = entry[2];
                    current.gip = entry[5] == 0xFF && entry[6] == 0x47 && entry[7] == 0xD0;
                }
            }
            else if (entry[1] == 5 && entry[0] >= 7 && !alternate && (entry[3] & 3) == 3) // interrupt endpoint
            {
                uint8_t& endpoint = (entry[2] & 0x80) ? current.in : current.out;
                if (endpoint == 0)
                    endpoint = entry[2];
            }
        }
        consider();
        return best;
    }

    class UsbPortal final : public PortalBackend
    {
    public:
        explicit UsbPortal(const Libusb& usb) : m_usb(usb)
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

            // Xbox 360 portals take fixed 32-byte, zero-padded reports.
            uint8_t packet[PortalReportSize]{};
            memcpy(packet, Xbox360Header, sizeof(Xbox360Header));
            memcpy(packet + sizeof(Xbox360Header), command, std::min(size, sizeof(packet) - sizeof(Xbox360Header)));
            return Transfer(m_interface.out, packet, sizeof(packet));
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
            libusb_device** devices = nullptr;
            ptrdiff_t count = m_usb.get_device_list(m_usb.context, &devices);
            for (ptrdiff_t i = 0; i < count && m_handle == nullptr; i++)
            {
                UsbDeviceDescriptor descriptor{};
                if (m_usb.get_device_descriptor(devices[i], &descriptor) != 0)
                    continue;

                bool xbox360 =
                    std::any_of(std::begin(Xbox360Portals), std::end(Xbox360Portals), [&](const KnownPortal& portal) {
                        return portal.vendor == descriptor.idVendor && portal.product == descriptor.idProduct;
                    });
                bool selected = descriptor.idVendor == m_selected.vendor && descriptor.idProduct == m_selected.product;
                if (xbox360 || selected || descriptor.idVendor == ActivisionVendor)
                    TryOpen(devices[i], descriptor, xbox360, selected);
            }
            if (count >= 0)
                m_usb.free_device_list(devices, 1);
            if (m_handle == nullptr && !m_reportedNone)
                fprintf(stderr, "[portal] usb: no Xbox 360 or Xbox One portal found (%d USB devices)\n", int(count));
            m_reportedNone = m_handle == nullptr;
        }

        // Known Xbox 360 portals and the selected device use whatever interface they have; other devices need GIP.
        void TryOpen(libusb_device* device, const UsbDeviceDescriptor& descriptor, bool xbox360, bool selected)
        {
            libusb_device_handle* handle = nullptr;
            if (m_usb.open(device, &handle) != 0)
                return;

            // GET_DESCRIPTOR(configuration 0), read raw.
            uint8_t configuration[512];
            int size = m_usb.control_transfer(handle, 0x80, 0x06, 0x0200, 0, configuration, sizeof(configuration), 500);
            UsbInterface usbInterface = size > 0 ? FindInterface(configuration, size_t(size)) : UsbInterface{};
            if (usbInterface.number < 0 || (!xbox360 && !selected && !usbInterface.gip))
            {
                m_usb.close(handle);
                return;
            }

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
            m_framing = usbInterface.gip ? Framing::Gip : Framing::Xbox360;
            m_lost = false;
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
                if (PortalLogEnabled())
                    fprintf(stderr, "[portal] USB write failed (%s)\n", m_usb.error_name(result));
                return false;
            }
            return true;
        }

        void ReadLoop()
        {
            uint8_t packet[64];
            while (!m_lost)
            {
                int size = 0;
                int result = m_usb.interrupt_transfer(m_handle, m_interface.in, packet, sizeof(packet), &size, 100);
                if (result == UsbErrorTimeout)
                    continue;
                if (result != 0)
                {
                    fprintf(stderr, "[portal] USB read failed (%s)\n", m_usb.error_name(result));
                    m_lost = true;
                    break;
                }

                if (m_framing == Framing::Gip)
                    HandleGip(packet, size_t(size));
                else if (size > int(sizeof(Xbox360Header)))
                    Queue(packet + sizeof(Xbox360Header), size_t(size) - sizeof(Xbox360Header));
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

        Libusb m_usb;
        KnownPortal m_selected{};
        libusb_device_handle* m_handle = nullptr;
        UsbInterface m_interface;
        Framing m_framing = Framing::Xbox360;
        std::thread m_reader;
        std::atomic<bool> m_lost = false;
        std::mutex m_mutex;
        std::deque<std::vector<uint8_t>> m_reports;
        std::atomic<uint8_t> m_sequence = 1;
        std::chrono::steady_clock::time_point m_nextScan{};
        bool m_reportedNone = false; // rescans log "none found" once
    };
}

std::unique_ptr<PortalBackend> CreateUsbPortal()
{
    Libusb usb{};
    if (!LoadLibusb(usb))
        return nullptr;
    return std::make_unique<UsbPortal>(usb);
}

namespace
{
    // Reset, in the Xbox 360 framing; any report back is an answer.
    std::string Query(
        const Libusb& usb, libusb_device_handle* handle, const UsbInterface& usbInterface, PortalProbe& probe)
    {
        uint8_t packet[PortalReportSize] = { Xbox360Header[0], Xbox360Header[1], 'R' };
        int transferred = 0;
        int result = usb.interrupt_transfer(handle, usbInterface.out, packet, sizeof(packet), &transferred, 500);
        if (result != 0)
            return std::string("error: write failed (") + usb.error_name(result) + ")";
        uint8_t reply[64];
        result = usb.interrupt_transfer(handle, usbInterface.in, reply, sizeof(reply), &transferred, 1000);
        if (result == UsbErrorTimeout)
            return "error: no answer within 1 s";
        if (result != 0)
            return std::string("error: read failed (") + usb.error_name(result) + ")";
        // The answer carries the 2-byte header (0B 14) before the 'R'.
        for (int i = 0; i < 3 && i < transferred; i++)
        {
            if (reply[i] == 'R')
            {
                NameProbe(probe, reply + i, size_t(transferred - i));
                break;
            }
        }
        return "ok";
    }
}

std::string ProbeUsbPortals(std::vector<PortalProbe>& portals)
{
    Libusb usb{};
    if (!LoadLibusb(usb))
        return "libusb-1.0 not found (set SKY_LIBUSB to its path)";

    libusb_device** devices = nullptr;
    ptrdiff_t count = usb.get_device_list(usb.context, &devices);
    for (ptrdiff_t i = 0; i < count; i++)
    {
        UsbDeviceDescriptor descriptor{};
        if (usb.get_device_descriptor(devices[i], &descriptor) != 0)
            continue;
        bool xbox360 =
            std::any_of(std::begin(Xbox360Portals), std::end(Xbox360Portals), [&](const KnownPortal& portal) {
                return portal.vendor == descriptor.idVendor && portal.product == descriptor.idProduct;
            });
        // The HID portals (1430:0150) are the HID transport's.
        if (!xbox360 && (descriptor.idVendor != ActivisionVendor || descriptor.idProduct == 0x0150))
            continue;

        PortalProbe probe{ "usb", descriptor.idVendor, descriptor.idProduct,
            xbox360 ? "Xbox 360 portal" : "Activision device" };
        libusb_device_handle* handle = nullptr;
        int result = usb.open(devices[i], &handle);
        if (result != 0)
        {
            probe.status = std::string("error: cannot open (") + usb.error_name(result) + ")";
            portals.push_back(probe);
            continue;
        }
        probe.opened = true;
        uint8_t configuration[512];
        int size = usb.control_transfer(handle, 0x80, 0x06, 0x0200, 0, configuration, sizeof(configuration), 500);
        UsbInterface usbInterface = size > 0 ? FindInterface(configuration, size_t(size)) : UsbInterface{};
        if (usbInterface.gip)
            probe.name = "Xbox One portal (experimental)";
        if (usbInterface.number < 0 || (!xbox360 && !usbInterface.gip))
            probe.status = "error: no portal interface";
        else if (usb.set_auto_detach_kernel_driver(handle, 1);
            (result = usb.claim_interface(handle, usbInterface.number)) != 0)
            probe.status = std::string("error: cannot claim interface (") + usb.error_name(result) + ")";
        else
        {
            probe.status =
                usbInterface.gip ? "ok" : Query(usb, handle, usbInterface, probe); // GIP: opened and claimed only
            usb.release_interface(handle, usbInterface.number);
        }
        usb.close(handle);
        portals.push_back(probe);
    }
    if (count >= 0)
        usb.free_device_list(devices, 1);
    return {};
}
