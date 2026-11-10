// A HID portal command as a USB control SET_REPORT (macOS and Linux, host/portal_hid.cpp), the ways a command can be
// sent and the order they are tried in, and the device behind SDL's macOS path. No IOKit or libusb here, so it is tested
// on every host (portal_hid_report_test.cpp).
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace portal_hid
{
    constexpr size_t CommandSize = 32;

    // HID class SET_REPORT: host to device, class, to the interface (0x21), request 0x09, value = report type
    // (2, output) << 8 | report id (0), index = the interface. The data is the command without the report id byte.
    struct SetReport
    {
        uint8_t requestType = 0x21;
        uint8_t request = 0x09;
        uint16_t value = 0x0200;
        uint16_t index = 0;
        uint8_t data[CommandSize]{};
    };

    // How a command reaches the portal (the comment above WriteMethod's use in host/portal_hid.cpp says why).
    enum class WriteMethod
    {
        OutputReport, // HidD_SetOutputReport (Windows)
        ControlReport, // SET_REPORT on the default pipe: IOUSBDeviceInterface (macOS), libusb (Linux)
        HidWrite, // SDL_hid_write
    };

    inline const char* MethodName(WriteMethod method)
    {
        switch (method)
        {
        case WriteMethod::OutputReport: return "output-report";
        case WriteMethod::ControlReport: return "usb-control";
        default: return "hid-write";
        }
    }

    // Each system's methods, in the order a new portal tries them. Linux: hidapi's write first (the Wii Swap Force
    // portal takes it), then the control request, which detaches the kernel's HID driver from the portal's interface.
    constexpr WriteMethod WindowsMethods[] = { WriteMethod::OutputReport, WriteMethod::HidWrite };
    constexpr WriteMethod MacMethods[] = { WriteMethod::ControlReport, WriteMethod::HidWrite };
    constexpr WriteMethod LinuxMethods[] = { WriteMethod::HidWrite, WriteMethod::ControlReport };

    // The order of one write: the method that worked last, then the others in the system's order.
    template <size_t N>
    inline std::array<WriteMethod, N> TryOrder(const WriteMethod (&methods)[N], WriteMethod last)
    {
        std::array<WriteMethod, N> order{};
        std::copy(std::begin(methods), std::end(methods), order.begin());
        if (auto found = std::find(order.begin(), order.end(), last); found != order.end())
            std::rotate(order.begin(), found, found + 1);
        return order;
    }

    // `command` starts at the letter; zero-padded to 32 bytes, cut beyond.
    inline SetReport MakeSetReport(const uint8_t* command, size_t size, int interface)
    {
        SetReport report;
        report.index = uint16_t(std::max(interface, 0));
        memcpy(report.data, command, std::min(size, CommandSize));
        return report;
    }

    // SDL's hidapi names a macOS device "DevSrvsID:<registry entry id>"; 0 when `path` is not such a name.
    inline uint64_t MacRegistryEntryId(const char* path)
    {
        constexpr char Prefix[] = "DevSrvsID:";
        if (path == nullptr || strncmp(path, Prefix, sizeof(Prefix) - 1) != 0)
            return 0;
        const char* digits = path + sizeof(Prefix) - 1;
        if (*digits < '0' || *digits > '9')
            return 0;
        char* end = nullptr;
        unsigned long long id = strtoull(digits, &end, 10);
        return *end == '\0' ? uint64_t(id) : 0;
    }
}
