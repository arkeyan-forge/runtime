// The USB portals' shared parts (host/portal_usb.cpp): the calls the backend makes, which libusb-1.0 answers on Linux
// and Windows and IOKit on macOS (host/portal_usb_mac.cpp), and the pure parts: the interface and endpoints read from a
// raw configuration descriptor, the Xbox 360 framing, the error names, the Linux udev rule. No libusb or IOKit here,
// so it is tested on every host (portal_usb_test.cpp).
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace portal_usb
{
    struct Context;
    struct Device;
    struct Handle;

    struct DeviceDescriptor
    {
        uint8_t bLength, bDescriptorType;
        uint16_t bcdUSB;
        uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
        uint16_t idVendor, idProduct, bcdDevice;
        uint8_t iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
    };
    static_assert(sizeof(DeviceDescriptor) == 18);

    // libusb's codes; the IOKit transport returns the same ones.
    enum Error : int
    {
        ErrorIo = -1,
        ErrorAccess = -3,
        ErrorNoDevice = -4,
        ErrorNotFound = -5,
        ErrorBusy = -6,
        ErrorTimeout = -7,
        ErrorPipe = -9,
        ErrorNotSupported = -12,
        ErrorOther = -99,
    };

    // The part of the libusb-1.0 API the backend uses (its ABI is stable since 1.0.0). Descriptors are read as raw
    // bytes, except the 18-byte device descriptor (packed by nature).
    struct Api
    {
        int (*init)(Context**);
        ptrdiff_t (*get_device_list)(Context*, Device***);
        void (*free_device_list)(Device**, int);
        int (*get_device_descriptor)(Device*, DeviceDescriptor*);
        int (*open)(Device*, Handle**);
        void (*close)(Handle*);
        int (*set_auto_detach_kernel_driver)(Handle*, int);
        int (*claim_interface)(Handle*, int);
        int (*release_interface)(Handle*, int);
        int (*control_transfer)(Handle*, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char*, uint16_t, unsigned int);
        int (*interrupt_transfer)(Handle*, unsigned char, unsigned char*, int, int*, unsigned int);
        const char* (*error_name)(int);
        // Optional (null when missing): the HID portals' control link picks its device by them (host/portal_hid_control.h).
        uint8_t (*get_bus_number)(Device*);
        uint8_t (*get_device_address)(Device*);
        Context* context;
        const char* from; // where it was loaded from, for the log
    };

#ifdef __APPLE__
    bool LoadIokit(Api& usb); // host/portal_usb_mac.cpp
#else
    bool LoadLibusb(Api& usb); // host/portal_usb.cpp: libusb-1.0 loaded at run time (SKY_LIBUSB, then the usual names)
#endif

    // The names of the codes above, for the IOKit transport's log.
    inline const char* ErrorName(int error)
    {
        switch (error)
        {
        case 0: return "success";
        case ErrorIo: return "I/O error";
        case ErrorAccess: return "access denied";
        case ErrorNoDevice: return "no device (unplugged)";
        case ErrorNotFound: return "not found";
        case ErrorBusy: return "busy (another program holds it)";
        case ErrorTimeout: return "timeout";
        case ErrorPipe: return "stalled";
        case ErrorNotSupported: return "not supported";
        default: return "error";
        }
    }

    // macOS: an IOReturn (IOKit/IOReturn.h, IOKit/usb/USB.h; checked against them in portal_usb_mac.cpp) as a code above.
    namespace io_return
    {
        constexpr uint32_t Success = 0;
        constexpr uint32_t NoDevice = 0xE00002C0;
        constexpr uint32_t NotPrivileged = 0xE00002C1;
        constexpr uint32_t ExclusiveAccess = 0xE00002C5;
        constexpr uint32_t Unsupported = 0xE00002C7;
        constexpr uint32_t Timeout = 0xE00002D6;
        constexpr uint32_t NotPermitted = 0xE00002E2;
        constexpr uint32_t Aborted = 0xE00002EB;
        constexpr uint32_t NotResponding = 0xE00002ED;
        constexpr uint32_t PipeStalled = 0xE000404F;
        constexpr uint32_t TransactionTimeout = 0xE0004051;
    }
    inline int ErrorFromIoReturn(uint32_t result)
    {
        switch (result)
        {
        case io_return::Success: return 0;
        case io_return::NoDevice:
        case io_return::NotResponding: return ErrorNoDevice;
        case io_return::NotPrivileged:
        case io_return::NotPermitted: return ErrorAccess;
        case io_return::ExclusiveAccess: return ErrorBusy;
        case io_return::Unsupported: return ErrorNotSupported;
        case io_return::Timeout:
        case io_return::TransactionTimeout: return ErrorTimeout;
        case io_return::PipeStalled: return ErrorPipe;
        default: return ErrorIo;
        }
    }

    // An endpoint's address from IOKit's pipe properties: its number, with 0x80 for IN (kUSBIn = 1).
    inline uint8_t EndpointAddress(uint8_t direction, uint8_t number)
    {
        return uint8_t((number & 0x0F) | (direction == 1 ? 0x80 : 0));
    }

    struct Interface
    {
        int number = -1;
        uint8_t in = 0;
        uint8_t out = 0;
        bool gip = false; // the Xbox One's GIP data interface: class FF, subclass 47, protocol D0
        bool xbox360 = false; // the Xbox 360's vendor interface: class FF, subclass 5D
    };

    // Picks the first interface with an interrupt IN and OUT endpoint, preferring the GIP data interface, then an Xbox
    // 360 one, from a raw configuration descriptor. Alternate settings are skipped.
    inline Interface FindInterface(const uint8_t* descriptor, size_t size)
    {
        Interface best, current;
        bool alternate = false;
        auto consider = [&] {
            auto rank = [](const Interface& i) { return i.gip ? 2 : i.xbox360 ? 1 : 0; };
            if (current.number >= 0 && current.in != 0 && current.out != 0 &&
                (best.number < 0 || rank(current) > rank(best)))
                best = current;
        };

        for (size_t offset = 0; offset + 2 <= size && descriptor[offset] >= 2; offset += descriptor[offset])
        {
            const uint8_t* entry = descriptor + offset;
            if (entry[1] == 4 && entry[0] >= 9 && offset + 9 <= size) // interface
            {
                consider();
                current = {};
                alternate = entry[3] != 0;
                if (!alternate)
                {
                    current.number = entry[2];
                    current.gip = entry[5] == 0xFF && entry[6] == 0x47 && entry[7] == 0xD0;
                    current.xbox360 = entry[5] == 0xFF && entry[6] == 0x5D;
                }
            }
            else if (entry[1] == 5 && entry[0] >= 7 && offset + 7 <= size && !alternate &&
                     (entry[3] & 3) == 3) // interrupt endpoint
            {
                uint8_t& endpoint = (entry[2] & 0x80) ? current.in : current.out;
                if (endpoint == 0)
                    endpoint = entry[2];
            }
        }
        consider();
        return best;
    }

    // The Xbox portals: by id when it is known, else by what an Activision device's interface is (docs/runtime/host/portal.md).
    enum class XboxFamily
    {
        None,
        Xbox360,
        XboxOne,
    };
    inline XboxFamily KnownXboxPortal(uint16_t vendor, uint16_t product)
    {
        if (vendor == 0x1430 && product == 0x1F17)
            return XboxFamily::Xbox360; // Spyro's Adventure to Trap Team (Trap Team's with a speaker)
        if (vendor == 0x1430 && product == 0x09AA)
            return XboxFamily::XboxOne; // Trap Team's (2026-11-10); Windows names it "Xbox Gaming Device"
        if (vendor == 0x1430 && product == 0x09AB)
            return XboxFamily::XboxOne; // Santroller's capture (GIP message 0x21, 30 bytes: the framing used here)
        if (vendor == 0x24C6 && product == 0xFA00)
            return XboxFamily::Xbox360; // only in Xenia's table of Xbox 360 portals; never seen here
        return XboxFamily::None;
    }
    inline XboxFamily XboxFamilyOf(const Interface& usbInterface)
    {
        return usbInterface.gip ? XboxFamily::XboxOne : usbInterface.xbox360 ? XboxFamily::Xbox360 : XboxFamily::None;
    }
    inline const char* XboxFamilyName(XboxFamily family)
    {
        return family == XboxFamily::XboxOne   ? "Xbox One portal"
               : family == XboxFamily::Xbox360 ? "Xbox 360 portal"
                                               : "";
    }

    // Xbox 360 portals take fixed 32-byte, zero-padded reports: a 2-byte header (0B 14 on commands, 0B 17 on the
    // speaker's sound blocks), then the protocol report, cut at 30 bytes.
    constexpr size_t Xbox360PacketSize = 32;
    constexpr uint8_t Xbox360CommandHeader[2] = { 0x0B, 0x14 };
    constexpr uint8_t Xbox360SoundHeader[2] = { 0x0B, 0x17 };
    using Xbox360Packet = std::array<uint8_t, Xbox360PacketSize>;

    inline Xbox360Packet MakeXbox360Packet(const uint8_t (&header)[2], const uint8_t* report, size_t size)
    {
        Xbox360Packet packet{};
        packet[0] = header[0];
        packet[1] = header[1];
        memcpy(packet.data() + 2, report, std::min(size, Xbox360PacketSize - 2));
        return packet;
    }

    // What the portal sends carries a 2-byte header too, which the title skips: the protocol report's offset and size
    // in a packet read from the IN endpoint (size 0: nothing past the header).
    struct Span
    {
        size_t offset, size;
    };
    inline Span Xbox360Report(size_t packetSize) { return packetSize > 2 ? Span{ 2, packetSize - 2 } : Span{ 2, 0 }; }

    // Up to `most` bytes as "0B 14 52 01 28" (the log, the probe).
    inline std::string HexBytes(const uint8_t* bytes, size_t size, size_t most = 16)
    {
        std::string text;
        for (size_t i = 0; i < std::min(size, most); i++)
        {
            char byte[4];
            snprintf(byte, sizeof(byte), i == 0 ? "%02X" : " %02X", bytes[i]);
            text += byte;
        }
        if (size > most)
            text += " ...";
        return text;
    }

    // Where the answer to 'R' starts in a packet read from an Xbox 360 portal: after the 2-byte header, or (another
    // framing, never seen) at the start or after one byte; -1 when the packet is not one.
    inline int Xbox360ResetAnswer(const uint8_t* packet, size_t size)
    {
        for (int at : { 2, 0, 1 })
            if (size >= size_t(at) + 3 && packet[at] == 'R')
                return at;
        return -1;
    }

    // Every interface and alternate setting of a raw configuration descriptor, for the log and the probe:
    // "interface 0 FF/5D/01: 81 interrupt 32 bytes, 02 interrupt 32 bytes; interface 1 alt 1 ...".
    inline std::string DescribeInterfaces(const uint8_t* descriptor, size_t size)
    {
        static const char* const Kinds[] = { "control", "isochronous", "bulk", "interrupt" };
        std::string text;
        bool first = true;
        for (size_t offset = 0; offset + 2 <= size && descriptor[offset] >= 2; offset += descriptor[offset])
        {
            const uint8_t* entry = descriptor + offset;
            char part[64];
            if (entry[1] == 4 && entry[0] >= 9 && offset + 9 <= size)
            {
                if (entry[3] != 0)
                    snprintf(part, sizeof(part), "%sinterface %u alt %u %02X/%02X/%02X", text.empty() ? "" : "; ",
                        entry[2], entry[3], entry[5], entry[6], entry[7]);
                else
                    snprintf(part, sizeof(part), "%sinterface %u %02X/%02X/%02X", text.empty() ? "" : "; ", entry[2],
                        entry[5], entry[6], entry[7]);
                text += part;
                first = true;
            }
            else if (entry[1] == 5 && entry[0] >= 7 && offset + 7 <= size)
            {
                snprintf(part, sizeof(part), "%s%02X %s %u bytes", first ? ": " : ", ", entry[2], Kinds[entry[3] & 3],
                    unsigned(entry[4] | entry[5] << 8));
                text += part;
                first = false;
            }
        }
        return text;
    }

    // Linux: the kernel driver bound to each interface of the USB devices `vendor:product`, from sysfs (`root` is "/sys"; a
    // fake folder in tests): "1-3:1.0 xpad, 1-3:1.1 none". The xpad driver takes vendor 1430's Xbox 360 class interfaces
    // as gamepads; libusb detaches it when the portal is claimed.
    inline std::string LinuxInterfaceDrivers(const std::filesystem::path& root, uint16_t vendor, uint16_t product)
    {
        std::error_code error;
        const std::filesystem::path devices = root / "bus" / "usb" / "devices";
        auto hex = [](const std::filesystem::path& file) {
            std::ifstream in(file);
            std::string text;
            return in >> text ? int(strtol(text.c_str(), nullptr, 16)) : -1;
        };
        std::vector<std::string> found;
        for (std::filesystem::directory_iterator it(devices, error), end; !error && it != end; it.increment(error))
        {
            const std::string name = it->path().filename().string();
            if (name.find(':') != std::string::npos || hex(it->path() / "idVendor") != vendor ||
                hex(it->path() / "idProduct") != product)
                continue;
            for (std::filesystem::directory_iterator inner(devices, error), stop; !error && inner != stop;
                inner.increment(error))
            {
                const std::string interface = inner->path().filename().string();
                if (interface.rfind(name + ":", 0) != 0)
                    continue;
                std::error_code linkError;
                const std::filesystem::path driver = std::filesystem::read_symlink(inner->path() / "driver", linkError);
                found.push_back(interface + " " + (linkError ? std::string("none") : driver.filename().string()));
            }
        }
        std::sort(found.begin(), found.end());
        std::string text;
        for (const std::string& entry : found)
            text += (text.empty() ? "" : ", ") + entry;
        return text;
    }

    // Windows: a USB device node ("USB\VID_1430&PID_0150\..." or a composite device's interface,
    // "USB\VID_1430&PID_0150&MI_00\...") and the driver service bound to it (CM_DRP_SERVICE: "HidUsb", "WinUSB"...).
    struct WindowsUsbNode
    {
        uint16_t vendor = 0, product = 0;
        int interfaceNumber = -1; // the MI_ number, -1 for the device itself
        std::string service;
    };
    inline bool ParseWindowsUsbInstance(const std::string& id, WindowsUsbNode& node)
    {
        std::string upper = id;
        for (char& c : upper)
            c = char(toupper(static_cast<unsigned char>(c)));
        auto field = [&](const char* name, int digits, int base) {
            const size_t at = upper.find(name);
            if (at == std::string::npos || at + strlen(name) + size_t(digits) > upper.size())
                return -1;
            const std::string text = upper.substr(at + strlen(name), size_t(digits));
            if (text.find_first_not_of(base == 16 ? "0123456789ABCDEF" : "0123456789") != std::string::npos)
                return -1;
            return int(strtol(text.c_str(), nullptr, base));
        };
        const int vendor = field("VID_", 4, 16), product = field("PID_", 4, 16);
        if (upper.rfind("USB\\", 0) != 0 || vendor < 0 || product < 0)
            return false;
        node.vendor = uint16_t(vendor);
        node.product = uint16_t(product);
        node.interfaceNumber = field("&MI_", 2, 16);
        return true;
    }

    // Whether Arkeyan can reach the portal through the driver Windows bound to it; empty when it can (issue #19: the
    // portals a player had bound to WinUSB or libusbK with a tool such as Zadig, as some emulators ask, are no HID
    // devices any more). `libusb`: libusb-1.0.dll was found.
    inline std::string WindowsDriverProblem(const WindowsUsbNode& node, bool libusb)
    {
        std::string service = node.service;
        for (char& c : service)
            c = char(tolower(static_cast<unsigned char>(c)));
        const bool hid = node.vendor == 0x1430 && node.product == 0x0150;
        const XboxFamily family = KnownXboxPortal(node.vendor, node.product);
        const bool xbox = family == XboxFamily::Xbox360;
        const bool libusbDriver = service == "winusb" || service == "libusbk" || service == "libusb0";
        if (service == "usbccgp")
            return {}; // a composite device: its interfaces carry the drivers
        if (service.empty())
            return "no driver is installed for it (Device Manager shows it with a warning)";
        if (hid)
            return service == "hidusb"
                       ? std::string()
                       : "it uses the " + node.service +
                             " driver, not Windows' own HID driver, so Arkeyan (which reads this portal "
                             "as a HID device) cannot see it. In Device Manager, uninstall the device "
                             "and its driver, then unplug the portal and plug it in again";
        // Windows' own Xbox driver holds it (GIP); reaching it without libusb is the next version's (portal.md).
        if (family == XboxFamily::XboxOne)
            return "an Xbox One portal (driver " + node.service +
                   "): Arkeyan talks to Xbox One portals on Windows from the next version (they work on macOS and "
                   "Linux); until then, use another portal or the virtual one";
        if (xbox && !libusb)
            return "an Xbox portal (driver " + node.service +
                   "): Arkeyan reaches Xbox portals through libusb-1.0, which Windows does not have (SKY_LIBUSB)";
        if (xbox && !libusbDriver)
            return "an Xbox portal on the " + node.service +
                   " driver: libusb opens WinUSB, libusbK or libusb0 devices only";
        return {};
    }

    // Linux: /dev/bus/usb/* belongs to root, so libusb cannot open an Xbox portal without this rule (the kit's
    // README.txt gives it with the HID portals' one).
    inline std::string LinuxUdevRule(uint16_t vendor, uint16_t product)
    {
        char rule[160];
        snprintf(rule, sizeof(rule),
            "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"%04x\", ATTRS{idProduct}==\"%04x\", MODE=\"0660\", "
            "TAG+=\"uaccess\"",
            vendor, product);
        return rule;
    }
}
