// Linux and Windows: a HID portal's commands as a USB control SET_REPORT and its reports from the interrupt IN endpoint,
// through libusb (Linux, host/portal_hid.cpp, method usb-control) or WinUSB (Windows, a portal a tool such as Zadig
// moved to WinUSB: no HID device is left). hidraw's write goes to the portal's interrupt OUT endpoint, and some Wii
// portals refuse it ("hid-write: Protocol error"): the original Spyro's Adventure one and a Swap Force one (both
// 1430:0150 "Spyro Porta"; the product string does not tell the models apart, the answer to 'R' does), while other Swap
// Force portals ("Spyro Portal") take it; on macOS the same family needed the control request. A class request to an
// interface needs that interface claimed, so on Linux the kernel's HID driver is detached from it (libusb's
// auto-detach) for as long as the portal stays open: the portal's hidraw node goes away meanwhile, and its reports are
// read here. Releasing the interface gives it back to the kernel. UNTESTED ON HARDWARE: a fake libusb checks the calls
// and the bytes (portal_hid_report_test.cpp).
#pragma once
#include "host/portal_hid_report.h"
#include "host/portal_usb.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace portal_hid
{
    struct UsbAddress
    {
        int bus = -1, address = -1, interface = -1; // -1: unknown
    };

    // The USB device and interface behind a hidraw node, from sysfs (`root` is "/sys"; a fake folder in tests):
    // class/hidraw/hidrawN/device is the HID device, inside the USB interface (bInterfaceNumber, hex), inside the USB
    // device (busnum, devnum).
    inline UsbAddress LinuxUsbAddress(const std::filesystem::path& root, const std::string& hidrawPath)
    {
        UsbAddress address;
        const std::string name = std::filesystem::path(hidrawPath).filename().string();
        if (name.rfind("hidraw", 0) != 0)
            return address;
        std::error_code error;
        const std::filesystem::path hid =
            std::filesystem::canonical(root / "class" / "hidraw" / name / "device", error);
        if (error)
            return address;
        auto number = [](const std::filesystem::path& file, int base) {
            std::ifstream in(file);
            std::string text;
            if (!(in >> text))
                return -1;
            char* end = nullptr;
            long value = strtol(text.c_str(), &end, base);
            return *end == '\0' ? int(value) : -1;
        };
        const std::filesystem::path usbInterface = hid.parent_path(), device = usbInterface.parent_path();
        address.interface = number(usbInterface / "bInterfaceNumber", 16);
        address.bus = number(device / "busnum", 10);
        address.address = number(device / "devnum", 10);
        return address;
    }

    // The first interrupt IN endpoint of interface `number` (alternate setting 0) in a raw configuration descriptor;
    // 0 when there is none. `packetSize` gets its wMaxPacketSize.
    inline uint8_t InEndpoint(const uint8_t* descriptor, size_t size, int number, uint16_t* packetSize = nullptr)
    {
        bool inside = false;
        for (size_t offset = 0; offset + 2 <= size && descriptor[offset] >= 2; offset += descriptor[offset])
        {
            const uint8_t* entry = descriptor + offset;
            if (entry[1] == 4 && entry[0] >= 9 && offset + 9 <= size) // interface
                inside = entry[2] == number && entry[3] == 0;
            else if (inside && entry[1] == 5 && entry[0] >= 7 && offset + 7 <= size && (entry[3] & 3) == 3 &&
                     (entry[2] & 0x80) != 0)
            {
                if (packetSize != nullptr)
                    *packetSize = uint16_t(entry[4] | (entry[5] & 0x07) << 8);
                return entry[2];
            }
        }
        return 0;
    }

    class ControlLink
    {
    public:
        // `winusb`: the calls are WinUSB's (Windows): its name in the errors, no udev rule.
        explicit ControlLink(bool winusb = false) : m_name(winusb ? "WinUSB" : "libusb"), m_winusb(winusb) {}
        ControlLink(const ControlLink&) = delete;
        ControlLink& operator=(const ControlLink&) = delete;
        ~ControlLink() { Release(); }

        bool Claimed() const { return m_handle != nullptr; }
        int Interface() const { return m_interface; }
        uint8_t In() const { return m_in; }

        // Opens the portal (`vendor`:`product` at `address`; the first such device when the bus is unknown) and claims
        // its interface, which detaches the kernel's driver from it. Empty, or why not.
        std::string Claim(const portal_usb::Api& usb, const UsbAddress& address, uint16_t vendor, uint16_t product)
        {
            Release();
            portal_usb::Device** devices = nullptr;
            ptrdiff_t count = usb.get_device_list(usb.context, &devices);
            if (count < 0)
                return m_name + ": no device list (" + usb.error_name(int(count)) + ")";
            portal_usb::Device* found = nullptr;
            for (ptrdiff_t i = 0; i < count && found == nullptr; i++)
            {
                portal_usb::DeviceDescriptor descriptor{};
                if (usb.get_device_descriptor(devices[i], &descriptor) != 0 || descriptor.idVendor != vendor ||
                    descriptor.idProduct != product)
                    continue;
                if (address.bus >= 0 && usb.get_bus_number != nullptr && usb.get_device_address != nullptr &&
                    (usb.get_bus_number(devices[i]) != address.bus ||
                        usb.get_device_address(devices[i]) != address.address))
                    continue;
                found = devices[i];
            }
            std::string error;
            portal_usb::Handle* handle = nullptr;
            if (found == nullptr)
                error = m_name + ": the portal is not in the USB device list";
            else if (int result = usb.open(found, &handle); result != 0)
            {
                error = m_name + ": cannot open (" + usb.error_name(result) + ")";
                if (result == portal_usb::ErrorAccess && m_winusb)
                    error += "; another program holds the portal";
                else if (result == portal_usb::ErrorAccess)
                    error += "; Linux needs the portal's udev rule: " + portal_usb::LinuxUdevRule(vendor, product);
            }
            usb.free_device_list(devices, 1);
            if (!error.empty())
                return error;

            const int number = std::max(address.interface, 0);
            uint8_t configuration[512];
            int size = usb.control_transfer(handle, 0x80, 0x06, 0x0200, 0, configuration, sizeof(configuration), 500);
            uint16_t packet = 0;
            const uint8_t in = size > 0 ? InEndpoint(configuration, size_t(size), number, &packet) : 0;
            if (in == 0)
            {
                usb.close(handle);
                return m_name + ": no interrupt IN endpoint on interface " + std::to_string(number);
            }
            usb.set_auto_detach_kernel_driver(handle, 1);
            if (int result = usb.claim_interface(handle, number); result != 0)
            {
                usb.close(handle);
                return m_name + ": cannot claim interface " + std::to_string(number) + " (" + usb.error_name(result) +
                       ")";
            }
            m_usb = &usb;
            m_handle = handle;
            m_interface = number;
            m_in = in;
            m_packet = packet;
            return {};
        }

        // The command (it starts at the letter) as SET_REPORT: 0x21, 0x09, 0x0200, the interface, 32 bytes.
        std::string Send(const uint8_t* command, size_t size)
        {
            SetReport report = MakeSetReport(command, size, m_interface);
            int result = m_usb->control_transfer(m_handle, report.requestType, report.request, report.value,
                report.index, report.data, uint16_t(sizeof(report.data)), 500);
            if (result < 0)
                return m_name + " control transfer: " + m_usb->error_name(result);
            if (result != int(sizeof(report.data)))
                return m_name + " control transfer: " + std::to_string(result) + " of 32 bytes";
            return {};
        }

        // One report from the interrupt IN endpoint, waiting up to `timeoutMs` (at least 1: libusb's 0 waits forever):
        // its size, 0 for none, -1 on an error (`error` says which). A transfer that timed out may still carry data.
        // One packet at most: a transfer longer than the endpoint's packets waits for a second one (issue #20).
        int Read(uint8_t* buffer, int capacity, unsigned timeoutMs, std::string& error)
        {
            int transferred = 0;
            if (m_packet != 0)
                capacity = std::min(capacity, int(m_packet));
            int result =
                m_usb->interrupt_transfer(m_handle, m_in, buffer, capacity, &transferred, std::max(timeoutMs, 1u));
            if (result == 0 || result == portal_usb::ErrorTimeout)
                return transferred;
            error = m_name + " interrupt transfer: " + m_usb->error_name(result);
            return -1;
        }

        // The interface goes back to the kernel's HID driver (Linux: auto-detach re-attaches it on release).
        void Release()
        {
            if (m_handle == nullptr)
                return;
            m_usb->release_interface(m_handle, m_interface);
            m_usb->close(m_handle);
            m_handle = nullptr;
        }

    private:
        const portal_usb::Api* m_usb = nullptr;
        portal_usb::Handle* m_handle = nullptr;
        int m_interface = 0;
        uint8_t m_in = 0;
        uint16_t m_packet = 0; // the IN endpoint's wMaxPacketSize
        std::string m_name;
        bool m_winusb = false;
    };
}
