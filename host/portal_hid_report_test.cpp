// The HID portals' control SET_REPORT, the order of the write methods, SDL's macOS device path, and Linux's control link
// through a fake libusb and a fake sysfs (no device, no game data): ./ArkeyanPortalHidReportTest
#include "portal_hid_control.h"
#include "portal_hid_report.h"
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    int g_checks;
    int g_failures;

    void Check(bool condition, const char* what, int line)
    {
        g_checks++;
        if (!condition)
        {
            g_failures++;
            fprintf(stderr, "FAIL line %d: %s\n", line, what);
        }
    }
#define CHECK(condition) Check((condition), #condition, __LINE__)

    // A fake libusb: two devices (an Xbox 360 portal, then the Wii portal at bus 1, address 7, interface 0 with its
    // endpoints 81 IN and 01 OUT); every call is written down.
    struct FakeUsb
    {
        std::vector<std::string> calls;
        struct Control
        {
            uint8_t requestType, request;
            uint16_t value, index, length;
            std::vector<uint8_t> data;
        };
        std::vector<Control> controls;
        int openResult = 0, claimResult = 0, controlResult = -100; // -100: the request's length
        int readResult = 0, readSize = 0, readLength = 0;
    } g_usb;

    portal_usb::Device* const XboxDevice = reinterpret_cast<portal_usb::Device*>(0x10);
    portal_usb::Device* const WiiDevice = reinterpret_cast<portal_usb::Device*>(0x20);
    portal_usb::Handle* const WiiHandle = reinterpret_cast<portal_usb::Handle*>(0x30);
    portal_usb::Device* g_list[] = { XboxDevice, WiiDevice, nullptr };

    portal_usb::Api FakeApi()
    {
        portal_usb::Api usb{};
        usb.get_device_list = [](portal_usb::Context*, portal_usb::Device*** list) -> ptrdiff_t {
            *list = g_list;
            return 2;
        };
        usb.free_device_list = [](portal_usb::Device**, int) { g_usb.calls.push_back("free_device_list"); };
        usb.get_device_descriptor = [](portal_usb::Device* device, portal_usb::DeviceDescriptor* descriptor) {
            *descriptor = {};
            descriptor->idVendor = 0x1430;
            descriptor->idProduct = device == WiiDevice ? 0x0150 : 0x1F17;
            return 0;
        };
        usb.get_bus_number = [](portal_usb::Device*) -> uint8_t { return 1; };
        usb.get_device_address = [](portal_usb::Device* device) -> uint8_t { return device == WiiDevice ? 7 : 3; };
        usb.open = [](portal_usb::Device* device, portal_usb::Handle** handle) {
            g_usb.calls.push_back(device == WiiDevice ? "open wii" : "open other");
            *handle = WiiHandle;
            return g_usb.openResult;
        };
        usb.close = [](portal_usb::Handle*) { g_usb.calls.push_back("close"); };
        usb.set_auto_detach_kernel_driver = [](portal_usb::Handle*, int on) {
            g_usb.calls.push_back("auto_detach " + std::to_string(on));
            return 0;
        };
        usb.claim_interface = [](portal_usb::Handle*, int number) {
            g_usb.calls.push_back("claim " + std::to_string(number));
            return g_usb.claimResult;
        };
        usb.release_interface = [](portal_usb::Handle*, int number) {
            g_usb.calls.push_back("release " + std::to_string(number));
            return 0;
        };
        usb.control_transfer = [](portal_usb::Handle*, uint8_t requestType, uint8_t request, uint16_t value,
                                   uint16_t index, unsigned char* data, uint16_t length, unsigned int) {
            if (requestType == 0x80 && request == 0x06) // GET_DESCRIPTOR (configuration)
            {
                const uint8_t configuration[] = { 9, 2, 32, 0, 1, 1, 0, 0x80, 50, // configuration
                    9, 4, 0, 0, 2, 3, 0, 0, 0, // interface 0, HID
                    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 29, 0, // HID descriptor
                    7, 5, 0x81, 3, 64, 0, 1, // interrupt IN 81
                    7, 5, 0x01, 3, 64, 0, 1 }; // interrupt OUT 01
                memcpy(data, configuration, std::min<size_t>(length, sizeof(configuration)));
                return int(std::min<size_t>(length, sizeof(configuration)));
            }
            g_usb.controls.push_back(
                { requestType, request, value, index, length, std::vector<uint8_t>(data, data + length) });
            return g_usb.controlResult == -100 ? int(length) : g_usb.controlResult;
        };
        usb.interrupt_transfer = [](portal_usb::Handle*, unsigned char endpoint, unsigned char* data, int length,
                                     int* done, unsigned int timeout) {
            g_usb.readLength = length;
            g_usb.calls.push_back("interrupt " + std::to_string(endpoint) + " " + std::to_string(timeout));
            for (int i = 0; i < g_usb.readSize; i++)
                data[i] = uint8_t(i == 0 ? 'R' : i);
            *done = g_usb.readSize;
            return g_usb.readResult;
        };
        usb.error_name = [](int error) {
            return error == portal_usb::ErrorAccess ? "LIBUSB_ERROR_ACCESS" : "LIBUSB_ERROR";
        };
        return usb;
    }

    bool Calls(const std::vector<std::string>& expected)
    {
        bool same = g_usb.calls == expected;
        if (!same)
            for (const std::string& call : g_usb.calls)
                fprintf(stderr, "  call: %s\n", call.c_str());
        g_usb.calls.clear();
        return same;
    }

    void WriteFile(const std::filesystem::path& file, const char* text)
    {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file) << text << "\n";
    }

    void ControlLinkChecks(const std::filesystem::path& scratch)
    {
        using portal_hid::WriteMethod;
        // The order: a new portal on Linux tries hidapi's write, then the control request; once the control request
        // worked it goes first. Windows and macOS keep theirs.
        auto onLinux = portal_hid::TryOrder(portal_hid::LinuxMethods, WriteMethod::HidWrite);
        CHECK(onLinux.size() == 2 && onLinux[0] == WriteMethod::HidWrite && onLinux[1] == WriteMethod::ControlReport);
        onLinux = portal_hid::TryOrder(portal_hid::LinuxMethods, WriteMethod::ControlReport);
        CHECK(onLinux[0] == WriteMethod::ControlReport && onLinux[1] == WriteMethod::HidWrite);
        auto windows = portal_hid::TryOrder(portal_hid::WindowsMethods, WriteMethod::OutputReport);
        CHECK(windows[0] == WriteMethod::OutputReport && windows[1] == WriteMethod::HidWrite);
        auto mac = portal_hid::TryOrder(portal_hid::MacMethods, WriteMethod::HidWrite);
        CHECK(mac[0] == WriteMethod::HidWrite && mac[1] == WriteMethod::ControlReport);
        auto unknown = portal_hid::TryOrder(portal_hid::WindowsMethods, WriteMethod::ControlReport);
        CHECK(unknown[0] == WriteMethod::OutputReport && unknown[1] == WriteMethod::HidWrite);
        CHECK(std::string(portal_hid::MethodName(WriteMethod::ControlReport)) == "usb-control");

        // sysfs: /dev/hidraw2 is the HID device 0003:1430:0150.0004 in interface 1-2:1.0 of device 1-2 (bus 1, address 7).
        namespace fs = std::filesystem;
        const fs::path sys = scratch / "sys";
        fs::remove_all(sys);
        const fs::path device = sys / "devices" / "pci0000:00" / "usb1" / "1-2";
        const fs::path hid = device / "1-2:1.0" / "0003:1430:0150.0004";
        fs::create_directories(hid);
        fs::create_directories(sys / "class" / "hidraw" / "hidraw2");
        fs::create_symlink(hid, sys / "class" / "hidraw" / "hidraw2" / "device");
        WriteFile(device / "busnum", "1");
        WriteFile(device / "devnum", "7");
        WriteFile(device / "1-2:1.0" / "bInterfaceNumber", "00");
        portal_hid::UsbAddress address = portal_hid::LinuxUsbAddress(sys, "/dev/hidraw2");
        CHECK(address.bus == 1 && address.address == 7 && address.interface == 0);
        CHECK(portal_hid::LinuxUsbAddress(sys, "/dev/hidraw9").bus == -1); // not in sysfs
        CHECK(portal_hid::LinuxUsbAddress(sys, "DevSrvsID:42").bus == -1); // not a hidraw node
        fs::remove_all(sys);

        // Claim: the Wii portal by its address, its IN endpoint from the descriptor, the kernel's driver detached.
        const portal_usb::Api usb = FakeApi();
        portal_hid::ControlLink link;
        CHECK(link.Claim(usb, address, 0x1430, 0x0150).empty());
        CHECK(Calls({ "open wii", "free_device_list", "auto_detach 1", "claim 0" }));
        CHECK(link.Claimed() && link.Interface() == 0 && link.In() == 0x81);

        // The reset: SET_REPORT 21 09 0200, interface 0, 32 bytes, 'R' then zeros.
        const uint8_t reset[] = { 'R' };
        CHECK(link.Send(reset, sizeof(reset)).empty());
        CHECK(g_usb.controls.size() == 1);
        if (!g_usb.controls.empty())
        {
            const FakeUsb::Control& control = g_usb.controls[0];
            CHECK(control.requestType == 0x21 && control.request == 0x09 && control.value == 0x0200);
            CHECK(control.index == 0 && control.length == 32 && control.data.size() == 32);
            CHECK(control.data[0] == 'R' && control.data[1] == 0 && control.data[31] == 0);
        }
        g_usb.controlResult = -9;
        CHECK(link.Send(reset, sizeof(reset)) == "libusb control transfer: LIBUSB_ERROR");
        g_usb.controlResult = 5;
        CHECK(link.Send(reset, sizeof(reset)) == "libusb control transfer: 5 of 32 bytes");
        g_usb.controlResult = -100;

        // Reports from endpoint 81; 0 ms becomes 1 (libusb's 0 is forever); a timeout with data is data.
        uint8_t report[64];
        std::string error;
        g_usb.readSize = 32;
        CHECK(link.Read(report, sizeof(report), 0, error) == 32 && report[0] == 'R');
        CHECK(Calls({ "interrupt 129 1" }));
        g_usb.readResult = portal_usb::ErrorTimeout;
        g_usb.readSize = 0;
        CHECK(link.Read(report, sizeof(report), 50, error) == 0 && error.empty());
        g_usb.readSize = 32;
        CHECK(link.Read(report, sizeof(report), 50, error) == 32);
        // One packet at most (the endpoint's 64 bytes), whatever the buffer.
        uint8_t large[128];
        CHECK(link.Read(large, sizeof(large), 50, error) == 32 && g_usb.readLength == 64);
        g_usb.readResult = portal_usb::ErrorNoDevice;
        CHECK(link.Read(report, sizeof(report), 50, error) == -1 && error == "libusb interrupt transfer: LIBUSB_ERROR");
        g_usb.readResult = 0;
        g_usb.calls.clear();

        // Release: the interface back to the kernel, then the handle.
        link.Release();
        CHECK(Calls({ "release 0", "close" }));
        CHECK(!link.Claimed());
        link.Release();
        CHECK(Calls({}));

        // Failures: no access names the udev rule; a busy interface is closed again.
        g_usb.openResult = portal_usb::ErrorAccess;
        error = link.Claim(usb, address, 0x1430, 0x0150);
        CHECK(error.rfind("libusb: cannot open (LIBUSB_ERROR_ACCESS); Linux needs the portal's udev rule: ", 0) == 0);
        CHECK(error.find("ATTRS{idProduct}==\"0150\"") != std::string::npos);
        CHECK(!link.Claimed());
        CHECK(Calls({ "open wii", "free_device_list" })); // nothing to close: the open failed
        g_usb.openResult = 0;
        g_usb.claimResult = portal_usb::ErrorBusy;
        CHECK(link.Claim(usb, address, 0x1430, 0x0150) == "libusb: cannot claim interface 0 (LIBUSB_ERROR)");
        CHECK(Calls({ "open wii", "free_device_list", "auto_detach 1", "claim 0", "close" }));
        g_usb.claimResult = 0;
        // An unknown address takes the first Wii portal; another product is not in the list.
        CHECK(link.Claim(usb, {}, 0x1430, 0x0150).empty() && link.Claimed());
        link.Release();
        CHECK(link.Claim(usb, address, 0x1430, 0x0999) == "libusb: the portal is not in the USB device list");
        g_usb.calls.clear();

        // Windows: the same link over WinUSB (a portal a tool moved to WinUSB): its name in the errors, no udev rule.
        portal_hid::ControlLink winusb(true);
        CHECK(winusb.Claim(usb, {}, 0x1430, 0x0999) == "WinUSB: the portal is not in the USB device list");
        g_usb.openResult = portal_usb::ErrorAccess;
        CHECK(winusb.Claim(usb, {}, 0x1430, 0x0150) ==
              "WinUSB: cannot open (LIBUSB_ERROR_ACCESS); another program holds the portal");
        g_usb.openResult = 0;
        CHECK(winusb.Claim(usb, {}, 0x1430, 0x0150).empty() && winusb.In() == 0x81 && winusb.Interface() == 0);
        g_usb.controlResult = 5;
        const uint8_t status[] = { 'S' };
        CHECK(winusb.Send(status, sizeof(status)) == "WinUSB control transfer: 5 of 32 bytes");
        g_usb.controlResult = -100;
        winusb.Release();
        g_usb.calls.clear();

        // The IN endpoint's packet size from the descriptor.
        const uint8_t small[] = { 9, 2, 25, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 1, 3, 0, 0, 0, 7, 5, 0x81, 3, 32, 0, 1 };
        uint16_t packet = 0;
        CHECK(portal_hid::InEndpoint(small, sizeof(small), 0, &packet) == 0x81 && packet == 32);
    }
}

int main(int argc, char** argv)
{
    // 'A' 01: the request the Wii portal answers with 41 01 FF; the rest of the 32 bytes is zero.
    const uint8_t activate[] = { 'A', 0x01 };
    portal_hid::SetReport report = portal_hid::MakeSetReport(activate, sizeof(activate), 0);
    CHECK(report.requestType == 0x21);
    CHECK(report.request == 0x09);
    CHECK(report.value == 0x0200);
    CHECK(report.index == 0);
    CHECK(sizeof(report.data) == 32);
    CHECK(report.data[0] == 'A' && report.data[1] == 0x01);
    bool zero = true;
    for (size_t i = 2; i < sizeof(report.data); i++)
        zero = zero && report.data[i] == 0;
    CHECK(zero);

    // A full block write ('W', slot, block, 16 bytes) fits; anything beyond 32 bytes is cut; the interface is the index.
    uint8_t longer[40];
    for (size_t i = 0; i < sizeof(longer); i++)
        longer[i] = uint8_t(i + 1);
    report = portal_hid::MakeSetReport(longer, sizeof(longer), 2);
    CHECK(report.index == 2);
    CHECK(report.data[0] == 1 && report.data[31] == 32);
    CHECK(portal_hid::MakeSetReport(activate, sizeof(activate), -1).index == 0); // hidapi's "unknown interface"

    CHECK(portal_hid::MacRegistryEntryId("DevSrvsID:4296106855") == 4296106855ull);
    CHECK(portal_hid::MacRegistryEntryId("DevSrvsID:") == 0);
    CHECK(portal_hid::MacRegistryEntryId("DevSrvsID:12x") == 0);
    CHECK(portal_hid::MacRegistryEntryId("DevSrvsID:-5") == 0);
    CHECK(portal_hid::MacRegistryEntryId("/dev/hidraw3") == 0);
    CHECK(portal_hid::MacRegistryEntryId(nullptr) == 0);

    // A scratch folder for the fake sysfs: the argument, else one in the system's temporary folder.
    const std::filesystem::path scratch =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::temp_directory_path() / ("arkeyan-hid-test-" + std::to_string(time(nullptr)));
    ControlLinkChecks(scratch);
    if (argc <= 1)
        std::filesystem::remove_all(scratch);

    printf("ArkeyanPortalHidReportTest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
