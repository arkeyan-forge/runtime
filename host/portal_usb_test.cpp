// The USB (Xbox) portals' pure parts and, on macOS, the IOKit transport's device list (no device opened, no game data):
// ./ArkeyanPortalUsbTest
#include "portal_usb.h"
#include "host/windows_devices.h"
#include <cstdio>
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

    // A configuration descriptor: header, then interfaces (number, alternate, class, subclass, protocol) each with
    // their endpoints (address, attributes: 3 interrupt, 2 bulk).
    struct Endpoint
    {
        uint8_t address, attributes;
    };
    void AddInterface(std::vector<uint8_t>& out, uint8_t number, uint8_t alternate, uint8_t cls, uint8_t subclass,
        uint8_t protocol, std::vector<Endpoint> endpoints)
    {
        out.insert(out.end(), { 9, 4, number, alternate, uint8_t(endpoints.size()), cls, subclass, protocol, 0 });
        for (const Endpoint& endpoint : endpoints)
            out.insert(out.end(), { 7, 5, endpoint.address, endpoint.attributes, 32, 0, 1 });
    }
    std::vector<uint8_t> Configuration() { return { 9, 2, 0, 0, 1, 1, 0, 0x80, 50 }; }
}

int main()
{
    using namespace portal_usb;

    // The Xbox 360 portal: one vendor interface (FF 5D 01) with interrupt IN 0x81 and OUT 0x02.
    std::vector<uint8_t> xbox360 = Configuration();
    AddInterface(xbox360, 0, 0, 0xFF, 0x5D, 0x01, { { 0x81, 3 }, { 0x02, 3 } });
    Interface found = FindInterface(xbox360.data(), xbox360.size());
    CHECK(found.number == 0 && found.in == 0x81 && found.out == 0x02 && !found.gip && found.xbox360);
    CHECK(XboxFamilyOf(found) == XboxFamily::Xbox360);

    // Bulk endpoints and alternate settings do not count; a later interface with interrupt endpoints does.
    std::vector<uint8_t> mixed = Configuration();
    AddInterface(mixed, 0, 0, 0xFF, 0x00, 0x00, { { 0x81, 2 }, { 0x01, 2 } });
    AddInterface(mixed, 0, 1, 0xFF, 0x00, 0x00, { { 0x83, 3 }, { 0x03, 3 } });
    AddInterface(mixed, 1, 0, 0xFF, 0x5D, 0x01, { { 0x84, 3 }, { 0x04, 3 } });
    found = FindInterface(mixed.data(), mixed.size());
    CHECK(found.number == 1 && found.in == 0x84 && found.out == 0x04);

    // GIP (FF 47 D0) wins over an earlier interrupt interface.
    std::vector<uint8_t> gip = Configuration();
    AddInterface(gip, 0, 0, 0xFF, 0x5D, 0x01, { { 0x81, 3 }, { 0x01, 3 } });
    AddInterface(gip, 1, 0, 0xFF, 0x47, 0xD0, { { 0x82, 3 }, { 0x02, 3 } });
    found = FindInterface(gip.data(), gip.size());
    CHECK(found.number == 1 && found.gip && found.in == 0x82 && found.out == 0x02);
    CHECK(XboxFamilyOf(found) == XboxFamily::XboxOne);

    // An Xbox 360 vendor interface (FF 5D) wins over an earlier other one; an interface of neither kind is no portal's.
    std::vector<uint8_t> later = Configuration();
    AddInterface(later, 0, 0, 0x03, 0x00, 0x00, { { 0x81, 3 }, { 0x01, 3 } });
    AddInterface(later, 1, 0, 0xFF, 0x5D, 0x81, { { 0x82, 3 }, { 0x02, 3 } });
    found = FindInterface(later.data(), later.size());
    CHECK(found.number == 1 && found.xbox360 && !found.gip);
    std::vector<uint8_t> other = Configuration();
    AddInterface(other, 0, 0, 0xFF, 0x42, 0x00, { { 0x81, 3 }, { 0x01, 3 } });
    found = FindInterface(other.data(), other.size());
    CHECK(found.number == 0 && XboxFamilyOf(found) == XboxFamily::None);

    // The ids known: the Xbox 360 portal, the Xbox One one (Trap Team's), and 24C6:FA00 from Xenia's table.
    CHECK(KnownXboxPortal(0x1430, 0x1F17) == XboxFamily::Xbox360 &&
          KnownXboxPortal(0x1430, 0x09AA) == XboxFamily::XboxOne &&
          KnownXboxPortal(0x1430, 0x09AB) == XboxFamily::XboxOne &&
          KnownXboxPortal(0x24C6, 0xFA00) == XboxFamily::Xbox360 &&
          KnownXboxPortal(0x1430, 0x0150) == XboxFamily::None && KnownXboxPortal(0x1430, 0x1234) == XboxFamily::None);
    CHECK(std::string(XboxFamilyName(XboxFamily::XboxOne)) == "Xbox One portal");

    // Only IN, or a descriptor cut short: no interface; never a read past the end.
    std::vector<uint8_t> onlyIn = Configuration();
    AddInterface(onlyIn, 0, 0, 0xFF, 0x5D, 0x01, { { 0x81, 3 } });
    CHECK(FindInterface(onlyIn.data(), onlyIn.size()).number < 0);
    CHECK(FindInterface(xbox360.data(), xbox360.size() - 7).number < 0);
    CHECK(FindInterface(xbox360.data(), 0).number < 0);

    // Commands: 0B 14, the report, zeros to 32 bytes; a longer report is cut at 30.
    const uint8_t reset[] = { 'R' };
    Xbox360Packet packet = MakeXbox360Packet(Xbox360CommandHeader, reset, sizeof(reset));
    CHECK(packet.size() == 32 && packet[0] == 0x0B && packet[1] == 0x14 && packet[2] == 'R');
    bool zeros = true;
    for (size_t i = 3; i < packet.size(); i++)
        zeros = zeros && packet[i] == 0;
    CHECK(zeros);
    uint8_t longer[40];
    for (size_t i = 0; i < sizeof(longer); i++)
        longer[i] = uint8_t(i + 1);
    packet = MakeXbox360Packet(Xbox360SoundHeader, longer, sizeof(longer));
    CHECK(packet[0] == 0x0B && packet[1] == 0x17 && packet[2] == 1 && packet[31] == 30);

    // Reads: the header is skipped; nothing past it is no report.
    CHECK(Xbox360Report(32).offset == 2 && Xbox360Report(32).size == 30);
    CHECK(Xbox360Report(2).size == 0 && Xbox360Report(0).size == 0);

    // IOKit's results as libusb's codes, and their names in the log.
    CHECK(ErrorFromIoReturn(io_return::Success) == 0);
    CHECK(ErrorFromIoReturn(io_return::Timeout) == ErrorTimeout);
    CHECK(ErrorFromIoReturn(io_return::TransactionTimeout) == ErrorTimeout);
    CHECK(ErrorFromIoReturn(io_return::ExclusiveAccess) == ErrorBusy);
    CHECK(ErrorFromIoReturn(io_return::NoDevice) == ErrorNoDevice);
    CHECK(ErrorFromIoReturn(io_return::NotPermitted) == ErrorAccess);
    CHECK(ErrorFromIoReturn(io_return::PipeStalled) == ErrorPipe);
    CHECK(ErrorFromIoReturn(0xE00002BC) == ErrorIo); // kIOReturnError
    CHECK(std::string(ErrorName(ErrorBusy)).find("another program") != std::string::npos);
    CHECK(EndpointAddress(1, 1) == 0x81 && EndpointAddress(0, 2) == 0x02);

    // What a player's log and probe show of the device: every interface and alternate setting, the endpoints' kinds.
    CHECK(DescribeInterfaces(xbox360.data(), xbox360.size()) ==
          "interface 0 FF/5D/01: 81 interrupt 32 bytes, 02 interrupt 32 bytes");
    CHECK(DescribeInterfaces(mixed.data(), mixed.size()).find("; interface 0 alt 1 ") != std::string::npos);

    // The probe takes only the answer to 'R' (issue #20: any packet, or none, was "ok"), after the header.
    const uint8_t answer[] = { 0x0B, 0x14, 'R', 0x01, 0x28 };
    const uint8_t status[] = { 0x0B, 0x14, 'S', 0x00, 0x00, 0x00, 0x00, 0x01, 0x00 };
    const uint8_t bare[] = { 'R', 0x02, 0x0A, 0x05, 0x08 };
    CHECK(Xbox360ResetAnswer(answer, sizeof(answer)) == 2);
    CHECK(Xbox360ResetAnswer(status, sizeof(status)) == -1);
    CHECK(Xbox360ResetAnswer(bare, sizeof(bare)) == 0);
    CHECK(Xbox360ResetAnswer(answer, 3) == -1 && Xbox360ResetAnswer(answer, 0) == -1);
    CHECK(HexBytes(answer, sizeof(answer)) == "0B 14 52 01 28" &&
          HexBytes(status, sizeof(status), 4) == "0B 14 53 00 ...");
    CHECK(HexBytes(answer, 0).empty());
    // The command frame is the same on Linux, Windows and macOS (one header, the report, zeros to 32 bytes).
    const uint8_t query[] = { 'Q', 0x10, 0x01 };
    Xbox360Packet frame = MakeXbox360Packet(Xbox360CommandHeader, query, sizeof(query));
    CHECK(HexBytes(frame.data(), frame.size(), 6) == "0B 14 51 10 01 00 ...");

#ifndef _WIN32
    // Linux: the kernel driver bound to each interface, read from sysfs (a fake one here, with symbolic links).
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "ArkeyanPortalUsbTest_sys";
        std::filesystem::remove_all(root);
        const std::filesystem::path devices = root / "bus" / "usb" / "devices",
                                    drivers = root / "bus" / "usb" / "drivers";
        auto write = [](const std::filesystem::path& file, const char* text) {
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file) << text << "\n";
        };
        write(devices / "1-3" / "idVendor", "1430");
        write(devices / "1-3" / "idProduct", "1f17");
        write(devices / "1-4" / "idVendor", "1430");
        write(devices / "1-4" / "idProduct", "0150");
        std::filesystem::create_directories(devices / "1-3:1.0");
        std::filesystem::create_directories(devices / "1-3:1.1");
        std::filesystem::create_directories(devices / "1-4:1.0");
        std::filesystem::create_directories(drivers / "xpad");
        std::filesystem::create_directories(drivers / "usbhid");
        std::error_code error;
        std::filesystem::create_directory_symlink(drivers / "xpad", devices / "1-3:1.0" / "driver", error);
        std::filesystem::create_directory_symlink(drivers / "usbhid", devices / "1-4:1.0" / "driver", error);
        CHECK(!error);
        CHECK(LinuxInterfaceDrivers(root, 0x1430, 0x1F17) == "1-3:1.0 xpad, 1-3:1.1 none");
        CHECK(LinuxInterfaceDrivers(root, 0x1430, 0x0150) == "1-4:1.0 usbhid");
        CHECK(LinuxInterfaceDrivers(root, 0x24C6, 0xFA00).empty());
        CHECK(LinuxInterfaceDrivers(root / "missing", 0x1430, 0x1F17).empty());
        std::filesystem::remove_all(root);
    }
#endif

    // Windows: the device nodes and what each driver means to Arkeyan (issue #19: portals bound to WinUSB by a tool).
    {
        WindowsUsbNode node;
        CHECK(ParseWindowsUsbInstance("USB\\VID_1430&PID_0150\\6&2A5F&0&1", node) && node.vendor == 0x1430 &&
              node.product == 0x0150 && node.interfaceNumber == -1);
        CHECK(ParseWindowsUsbInstance("usb\\vid_1430&pid_1f17&mi_00\\7&1", node) && node.product == 0x1F17 &&
              node.interfaceNumber == 0);
        CHECK(!ParseWindowsUsbInstance("HID\\VID_1430&PID_0150\\1", node) &&
              !ParseWindowsUsbInstance("USB\\ROOT_HUB30", node));
        WindowsUsbNode hid{ 0x1430, 0x0150, -1, "HidUsb" }, winusb{ 0x1430, 0x0150, -1, "WinUSB" },
            none{ 0x1430, 0x0150, -1, "" }, composite{ 0x1430, 0x0150, -1, "usbccgp" },
            xbox{ 0x1430, 0x1F17, 0, "xusb22" }, xboxWinusb{ 0x1430, 0x1F17, 0, "WinUSB" };
        CHECK(WindowsDriverProblem(hid, false).empty() && WindowsDriverProblem(composite, false).empty());
        CHECK(WindowsDriverProblem(winusb, false).find("it uses the WinUSB driver, not Windows' own HID driver") == 0);
        CHECK(WindowsDriverProblem(winusb, true).find("uninstall the device and its driver") != std::string::npos);
        CHECK(WindowsDriverProblem(none, false).find("no driver") == 0);
        CHECK(WindowsDriverProblem(xbox, false).find("libusb-1.0, which Windows does not have") != std::string::npos);
        CHECK(WindowsDriverProblem(xbox, true).find("WinUSB, libusbK or libusb0 devices only") != std::string::npos);
        CHECK(WindowsDriverProblem(xboxWinusb, true).empty());
        // The Xbox One portal, which Windows' own Xbox driver holds: named, and the next version's.
        WindowsUsbNode xboxOne{ 0x1430, 0x09AA, -1, "xboxgip" };
        CHECK(WindowsDriverProblem(xboxOne, false).find("an Xbox One portal (driver xboxgip)") == 0 &&
              WindowsDriverProblem(xboxOne, true).find("from the next version") != std::string::npos);
    }

    // Linux: the rule the log gives for a device libusb cannot open.
    CHECK(LinuxUdevRule(0x1430, 0x1F17) ==
          "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"1430\", ATTRS{idProduct}==\"1f17\", MODE=\"0660\", TAG+=\"uaccess\"");

#ifdef __APPLE__
    // The IOKit transport lists this Mac's USB devices (none opened) and frees the list.
    Api usb{};
    CHECK(LoadIokit(usb));
    Device** devices = nullptr;
    ptrdiff_t count = usb.get_device_list(usb.context, &devices);
    CHECK(count >= 0 && devices != nullptr);
    int described = 0;
    for (ptrdiff_t i = 0; i < count; i++)
    {
        DeviceDescriptor descriptor{};
        described += usb.get_device_descriptor(devices[i], &descriptor) == 0 && descriptor.bLength == 18;
    }
    CHECK(described == count);
    usb.free_device_list(devices, 1);
    printf("IOKit: %d USB devices listed\n", int(count));
#endif

    {
        // Windows: what the device list shows (any 1430, 24C6, 0E6F, or a portal's name) and what the audio skips.
        using windows_devices::LooksLikePortal;
        using windows_devices::Portal;
        CHECK(LooksLikePortal("USB\\VID_1430&PID_0150\\6&1", "", "USB Input Device") == Portal::Sure);
        CHECK(LooksLikePortal("HID\\VID_1430&PID_0150&MI_00\\7&2", "", "") == Portal::Sure);
        CHECK(LooksLikePortal("USB\\VID_24C6&PID_FA00\\6&3", "", "Xbox Gaming Device") == Portal::Sure);
        CHECK(LooksLikePortal("USB\\VID_24C6&PID_581A\\6&4", "", "Xbox Gaming Device") == Portal::Maybe);
        CHECK(LooksLikePortal("USB\\VID_0E6F&PID_02A4\\6&5", "", "") == Portal::Maybe);
        CHECK(LooksLikePortal("BTHENUM\\{1}_VID&00021430_PID&1F17\\8&6", "", "") == Portal::Sure);
        CHECK(LooksLikePortal("SWD\\MMDEVAPI\\{0.0.0.00000000}.{AB}", "", "Speakers (Skylanders Portal)") ==
              Portal::Sure);
        CHECK(LooksLikePortal("XBOXGIP\\X\\1", "GIP\\VID_1430&PID_1F00", "Gip device") == Portal::Sure);
        CHECK(LooksLikePortal("USB\\VID_045E&PID_02EA\\6&7", "", "Xbox Controller") == Portal::No);
        CHECK(LooksLikePortal("SWD\\MMDEVAPI\\{0.0.0.00000000}.{CD}", "", "Speakers (Realtek(R) Audio)") == Portal::No);
        windows_devices::Device output{ "SWD\\MMDEVAPI\\{0.0.0.00000000}.{AB}", "Speakers (Portal)", "", "",
            "USB\\VID_24C6&PID_FA00\\6&3", true };
        CHECK(windows_devices::IsAudioOutput(output));
        CHECK(windows_devices::Describe(output) == "SWD\\MMDEVAPI\\{0.0.0.00000000}.{AB} \"Speakers (Portal)\", "
                                                   "driver (none), on USB\\VID_24C6&PID_FA00\\6&3");
        windows_devices::Device portal{ "USB\\VID_1430&PID_0150\\6&1", "USB Input Device", "HidUsb",
            "USB\\VID_1430&PID_0150&REV_0100, USB\\VID_1430&PID_0150", "USB\\VID_1430&PID_0150\\6&1", true };
        CHECK(!windows_devices::IsAudioOutput(portal));
        CHECK(windows_devices::Describe(portal) ==
              "USB\\VID_1430&PID_0150\\6&1 \"USB Input Device\", driver HidUsb, ids USB\\VID_1430&PID_0150&REV_0100, "
              "USB\\VID_1430&PID_0150");
    }

    printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
