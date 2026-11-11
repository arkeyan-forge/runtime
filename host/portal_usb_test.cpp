// The USB (Xbox) portals' pure parts and, on macOS, the IOKit transport's device list (no device opened, no game data):
// ./ArkeyanPortalUsbTest
#include "portal_usb.h"
#include "host/windows_devices.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
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

    // A fake Xbox 360 portal behind the libusb-shaped calls, speaking the protocol as the public notes describe it:
    // interrupt IN 0x81 and OUT 0x02, 32-byte packets, "0B 14" before the command letter both ways; 'R' is answered
    // with 'R' and the portal's id, 'A' with 'A', then 'S' status packets. Its IN endpoint behaves as USB does: a
    // transfer ends on a short packet or when it is full; at its timeout it returns a timeout with what it holds.
    struct FakePortal
    {
        std::vector<std::vector<uint8_t>> pending; // packets the portal has ready
        std::vector<std::vector<uint8_t>> written;
        bool stallsStart = false; // the start request (C1 01 0100) is stalled
        bool waitsForStart = false; // answers nothing until the start request came
        bool started = false;
        bool answersOnly = false; // 'R' gets its answer and no status after it (issue #23's portal)
        int startRequests = 0;
    } g_fake;

    // What issue #23's Xbox 360 portal (SteamOS, 0.9.5) sent first: its status report, no figure, counter E5, active.
    const std::vector<uint8_t> Issue23Status = [] {
        std::vector<uint8_t> packet(32, 0);
        const uint8_t head[] = { 0x0B, 0x14, 0x53, 0x00, 0x00, 0x00, 0x00, 0xE5, 0x01 };
        std::copy(std::begin(head), std::end(head), packet.begin());
        return packet;
    }();

    std::vector<uint8_t> FakePacket(std::initializer_list<uint8_t> report)
    {
        std::vector<uint8_t> packet(32, 0);
        packet[0] = 0x0B;
        packet[1] = 0x14;
        std::copy(report.begin(), report.end(), packet.begin() + 2);
        return packet;
    }

    int FakeControl(portal_usb::Handle*, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
        unsigned char* data, uint16_t length, unsigned int)
    {
        if (type == 0xC1 && request == 0x01 && value == 0x0100 && index == 0)
        {
            g_fake.startRequests++;
            if (g_fake.stallsStart)
                return portal_usb::ErrorPipe;
            g_fake.started = true;
            const uint8_t reply[20] = { 0x00, 0x14, 0x01 };
            memcpy(data, reply, std::min<size_t>(length, sizeof(reply)));
            return std::min<int>(length, int(sizeof(reply)));
        }
        return portal_usb::ErrorPipe;
    }

    int FakeInterrupt(
        portal_usb::Handle*, unsigned char endpoint, unsigned char* data, int length, int* transferred, unsigned int)
    {
        *transferred = 0;
        if (endpoint == 0x02)
        {
            std::vector<uint8_t> packet(data, data + length);
            g_fake.written.push_back(packet);
            if (length == 32 && data[0] == 0x0B && data[1] == 0x14 && (g_fake.started || !g_fake.waitsForStart))
            {
                if (data[2] == 'R')
                    g_fake.pending.push_back(FakePacket({ 'R', 0x01, 0x28 }));
                else if (data[2] == 'A')
                    g_fake.pending.push_back(FakePacket({ 'A', 0x01 }));
                if (!g_fake.answersOnly)
                    g_fake.pending.push_back(FakePacket({ 'S', 0x01, 0, 0, 0, 0x01 }));
            }
            *transferred = length;
            return 0;
        }
        if (endpoint != 0x81)
            return portal_usb::ErrorNotFound;
        while (!g_fake.pending.empty() && *transferred < length)
        {
            std::vector<uint8_t>& packet = g_fake.pending.front();
            const int size = std::min(int(packet.size()), length - *transferred);
            memcpy(data + *transferred, packet.data(), size_t(size));
            *transferred += size;
            const bool shortPacket = size < 32;
            g_fake.pending.erase(g_fake.pending.begin());
            if (shortPacket)
                return 0;
        }
        return *transferred == length ? 0 : portal_usb::ErrorTimeout;
    }

    portal_usb::Api FakeApi()
    {
        portal_usb::Api usb{};
        usb.control_transfer = FakeControl;
        usb.interrupt_transfer = FakeInterrupt;
        return usb;
    }
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

    CHECK(found.inSize == 32 && found.outSize == 32);

    // One packet per IN transfer: a 64-byte read of 32-byte packets waits for a second one (issue #20).
    CHECK(ReadLength(found, 64) == 32 && ReadLength(found, 16) == 16 && ReadLength(Interface{}, 64) == 32);
    CHECK(Received(0, 32) == 32 && Received(ErrorTimeout, 32) == 32 && Received(ErrorTimeout, 0) == 0 &&
          Received(ErrorPipe, 0) == ErrorPipe && Received(ErrorNoDevice, 5) == ErrorNoDevice);

    {
        // The fake portal: the start request, then 'R' and its answer, one packet per read.
        Api usb = FakeApi();
        g_fake = {};
        CHECK(StartXbox360(usb, nullptr, 0) == "start request C1 01 0100: 00 14 01 (20 bytes, zeros after)");
        const uint8_t reset[] = { 'R' };
        Xbox360Packet command = MakeXbox360Packet(Xbox360CommandHeader, reset, 1);
        int transferred = 0;
        CHECK(usb.interrupt_transfer(nullptr, found.out, command.data(), 32, &transferred, 100) == 0 &&
              transferred == 32);
        // What 0.9.5 asked for (64 bytes) joins two packets into one "report" (the second is lost), and with one
        // packet ready ends in a timeout holding it, which it dropped.
        uint8_t reply[64]{};
        CHECK(usb.interrupt_transfer(nullptr, found.in, reply, 64, &transferred, 100) == 0 && transferred == 64 &&
              reply[2] == 'R' && reply[34] == 'S');
        g_fake.pending = { FakePacket({ 'R', 0x01, 0x28 }) };
        CHECK(usb.interrupt_transfer(nullptr, found.in, reply, 64, &transferred, 100) == ErrorTimeout &&
              transferred == 32 && reply[2] == 'R');
        g_fake.pending = { FakePacket({ 'R', 0x01, 0x28 }), FakePacket({ 'S', 0x01 }) };
        CHECK(ReadPacket(usb, nullptr, found, reply, sizeof(reply), 100) == 32 && reply[2] == 'R');
        CHECK(Xbox360ResetAnswer(reply, 32) == 2 && Xbox360Report(32).size == 30);
        CHECK(ReadPacket(usb, nullptr, found, reply, sizeof(reply), 100) == 32 && reply[2] == 'S');
        CHECK(ReadPacket(usb, nullptr, found, reply, sizeof(reply), 100) == 0);
        // A short packet ends a transfer too; a timeout that holds a packet still gives it.
        g_fake.pending = { std::vector<uint8_t>{ 0x0B, 0x14, 'Q' } };
        CHECK(ReadPacket(usb, nullptr, found, reply, sizeof(reply), 100) == 3);
        Interface wide = found;
        wide.inSize = 64;
        g_fake.pending = { FakePacket({ 'S' }) };
        CHECK(ReadPacket(usb, nullptr, wide, reply, sizeof(reply), 100) == 32 && reply[2] == 'S');

        // A portal that stalls the start request is only logged; one that waits for it answers after it.
        g_fake = {};
        g_fake.stallsStart = true;
        CHECK(StartXbox360(usb, nullptr, 0).find("stalled (the portal does not need it") != std::string::npos);
        g_fake = {};
        g_fake.waitsForStart = true;
        CHECK(usb.interrupt_transfer(nullptr, 0x02, command.data(), 32, &transferred, 100) == 0);
        CHECK(ReadPacket(usb, nullptr, found, reply, sizeof(reply), 100) == 0);
        StartXbox360(usb, nullptr, 0);
        CHECK(usb.interrupt_transfer(nullptr, 0x02, command.data(), 32, &transferred, 100) == 0);
        CHECK(ReadPacket(usb, nullptr, found, reply, sizeof(reply), 100) == 32 && reply[2] == 'R');
        CHECK(g_fake.startRequests == 1 && g_fake.written.size() == 2);
    }

    {
        // Issue #23, replayed: the portal's status report is waiting when the probe writes 'R', and the answer comes
        // after it. 0.9.5's 64-byte read took both packets as one and judged it by the first: the player's line.
        Api usb = FakeApi();
        usb.error_name = ErrorName;
        g_fake = {};
        g_fake.answersOnly = true;
        g_fake.pending = { Issue23Status };
        const uint8_t reset[] = { 'R' };
        Xbox360Packet command = MakeXbox360Packet(Xbox360CommandHeader, reset, 1);
        int transferred = 0;
        CHECK(usb.interrupt_transfer(nullptr, found.out, command.data(), 32, &transferred, 100) == 0);
        uint8_t reply[64]{};
        CHECK(usb.interrupt_transfer(nullptr, found.in, reply, 64, &transferred, 100) == 0 && transferred == 64);
        CHECK(Xbox360ResetAnswer(reply, size_t(transferred)) == -1 &&
              HexBytes(reply, size_t(transferred)) == "0B 14 53 00 00 00 00 E5 01 00 00 00 00 00 00 00 ...");
        CHECK(reply[34] == 'R'); // the answer was there, in the second half

        // Now: one packet per read, the status passed over, the answer matched.
        g_fake = {};
        g_fake.answersOnly = true;
        g_fake.pending = { Issue23Status };
        std::string steps;
        std::vector<uint8_t> answer;
        CHECK(QueryXbox360Reset(usb, nullptr, found, steps, answer) == "ok");
        CHECK(answer.size() == 30 && answer[0] == 'R' && answer[1] == 0x01 && answer[2] == 0x28);
        CHECK(steps.find("; start request C1 01 0100: ") == 0);
        CHECK(steps.find("; out 1: 0B 14 52 (32 bytes, zeros after)") != std::string::npos);
        CHECK(steps.find("; in 1: 0B 14 53 00 00 00 00 E5 01 (32 bytes, zeros after) at ") != std::string::npos);
        CHECK(steps.find("; in 2: 0B 14 52 01 28 (32 bytes, zeros after) at ") != std::string::npos);
        CHECK(g_fake.startRequests == 1 && g_fake.written.size() == 1);

        // A portal that only sends its status: the status says so, with the first packet.
        g_fake = {};
        g_fake.answersOnly = true;
        g_fake.stallsStart = true;
        g_fake.pending = { Issue23Status };
        steps.clear();
        answer.clear();
        usb.interrupt_transfer = [](portal_usb::Handle* handle, unsigned char endpoint, unsigned char* data, int length,
                                     int* transferred, unsigned int timeout) {
            if (endpoint == 0x02) // taken, never answered
                return *transferred = length, 0;
            return FakeInterrupt(handle, endpoint, data, length, transferred, timeout);
        };
        CHECK(QueryXbox360Reset(usb, nullptr, found, steps, answer) ==
              "error: no 'R' answer within 1 s (1 other packets, the first: 0B 14 53 00 00 00 00 E5 01 00 00 00 00 00 "
              "00 00 ...)");
        CHECK(answer.empty() && steps.find("stalled") != std::string::npos);
    }

    {
        // The log's packets: the bytes up to the last non-zero one; 8 each way, then none (all with SKY_PORTAL_LOG).
        const uint8_t packet[32] = { 0x0B, 0x14, 'R', 0x01, 0x28 };
        CHECK(PacketHex(packet, 32) == "0B 14 52 01 28 (32 bytes, zeros after)");
        CHECK(PacketHex(packet, 5) == "0B 14 52 01 28 (5 bytes)");
        const uint8_t zeros[4] = {};
        CHECK(PacketHex(zeros, 4) == "zeros (4 bytes)");
        PacketTrace trace;
        CHECK(trace.Line(true, packet, 32) == "out 1: 0B 14 52 01 28 (32 bytes, zeros after)");
        CHECK(trace.Line(false, packet, 5) == "in 1: 0B 14 52 01 28 (5 bytes)");
        for (int i = 2; i < PacketTrace::Logged; i++)
            trace.Line(true, packet, 32);
        CHECK(trace.Line(true, packet, 32).find("(later packets: SKY_PORTAL_LOG=1)") != std::string::npos);
        CHECK(trace.Line(true, packet, 32).empty() && !trace.Line(true, packet, 32, true).empty());
        trace.Reset();
        CHECK(trace.Line(true, packet, 32).rfind("out 1: ", 0) == 0);
    }

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
        WindowsUsbNode hid{ 0x1430, 0x0150, -1, "HidUsb", "" }, winusb{ 0x1430, 0x0150, -1, "WinUSB", "" },
            none{ 0x1430, 0x0150, -1, "", "" }, composite{ 0x1430, 0x0150, -1, "usbccgp", "" },
            xbox{ 0x1430, 0x1F17, 0, "xusb22", "" }, xboxWinusb{ 0x1430, 0x1F17, 0, "WinUSB", "" },
            xboxNone{ 0x1430, 0x1F17, -1, "", "" }, xboxLibusbK{ 0x1430, 0x1F17, -1, "libusbK", "" };
        CHECK(WindowsDriverProblem(hid, false).empty() && WindowsDriverProblem(composite, false).empty());
        // A HID portal on WinUSB is used through it (0.9.6); on libusb0 or libusbK (a Trap Team Wii portal Zadig moved
        // to libusb0, which came back after an uninstall) the way back, with the driver package's name when known.
        CHECK(WindowsDriverProblem(winusb, false).empty() && WindowsDriverProblem(winusb, true).empty());
        CHECK(WindowsDriverProblem(none, false).find("no driver") == 0);
        WindowsUsbNode libusb0{ 0x1430, 0x0150, -1, "libusb0", "oem42.inf" },
            libusbK{ 0x1430, 0x0150, -1, "libusbK", "" };
        const std::string back = WindowsDriverProblem(libusb0, false);
        CHECK(back.find("it uses the libusb0 driver, which a tool such as Zadig installed") == 0);
        CHECK(
            back.find("Uninstall device, tick \"Attempt to remove the driver for this device\"") != std::string::npos);
        CHECK(back.find("3. if it is back on libusb0: in a Command Prompt run as administrator, pnputil /enum-drivers "
                        "(the driver is oem42.inf), then pnputil /delete-driver oem42.inf /uninstall /force") !=
              std::string::npos);
        CHECK(back.find("choose WinUSB for it in Zadig") != std::string::npos &&
              back == WindowsDriverProblem(libusb0, true));
        CHECK(WindowsDriverProblem(libusbK, false)
                  .find("find the libusbK driver's oemNN.inf), then pnputil "
                        "/delete-driver oemNN.inf /uninstall /force") != std::string::npos);
        libusbK.inf = "libusbk.inf"; // not a third-party package's published name: never offered for deletion
        CHECK(WindowsDriverProblem(libusbK, false).find("delete-driver oemNN.inf") != std::string::npos);
        // Xbox 360 portals: WinUSB is opened without libusb; any other driver needs the one-time WinUSB step.
        CHECK(WindowsDriverProblem(xboxWinusb, false).empty() && WindowsDriverProblem(xboxWinusb, true).empty());
        CHECK(WindowsDriverProblem(xbox, false).find("an Xbox 360 portal on the xusb22 driver") == 0 &&
              WindowsDriverProblem(xbox, true).find("WinUsb Device") != std::string::npos);
        CHECK(WindowsDriverProblem(xboxNone, false).find("an Xbox 360 portal with no driver") == 0);
        CHECK(WindowsDriverProblem(xboxLibusbK, true).empty() &&
              WindowsDriverProblem(xboxLibusbK, false)
                      .find("which Arkeyan cannot open (it opens WinUSB devices): use "
                            "WinUSB instead, once, in Device Manager") != std::string::npos);
        for (const WindowsUsbNode& each : { winusb, none, libusb0, libusbK, xbox, xboxNone, xboxLibusbK })
            CHECK(WindowsDriverProblem(each, false).find("SKY_LIBUSB") == std::string::npos); // never on Windows
        // The Xbox One portal: Windows' own Xbox driver is the route (Windows.Gaming.Input.Custom), WinUSB too.
        WindowsUsbNode xboxOne{ 0x1430, 0x09AA, -1, "xboxgip", "" }, xboxOneWinusb{ 0x1430, 0x09AA, -1, "WinUSB", "" },
            xboxOneLibusb0{ 0x1430, 0x09AA, -1, "libusb0", "" };
        CHECK(WindowsDriverProblem(xboxOne, false).empty() && WindowsDriverProblem(xboxOneWinusb, false).empty());
        CHECK(WindowsDriverProblem(xboxOneLibusb0, false).find("To give it back to Windows' own Xbox driver: 1. ") !=
                  std::string::npos &&
              WindowsDriverProblem(xboxOneLibusb0, true).empty());
    }

    {
        // Windows' WinUSB transport: its errors as libusb's codes, the interface classes it opens a device through, the
        // revision in a hardware id.
        CHECK(ErrorFromWin32(0) == 0 && ErrorFromWin32(win32_error::SemTimeout) == ErrorTimeout &&
              ErrorFromWin32(win32_error::GenFailure) == ErrorPipe &&
              ErrorFromWin32(win32_error::AccessDenied) == ErrorBusy &&
              ErrorFromWin32(win32_error::DeviceNotConnected) == ErrorNoDevice && ErrorFromWin32(87) == ErrorIo);
        const std::string zadig = std::string("{6b7bb2fd-1a43-4f23-a0ec-9b3a7d3c5a42}\0", 39) +
                                  std::string("bad\0", 4) + std::string("{6B7BB2FD-1A43-4F23-A0EC-9B3A7D3C5A42}\0", 39);
        std::vector<std::string> classes = WinusbInterfaceClasses(zadig);
        CHECK(classes.size() == 3 && classes[0] == "{6B7BB2FD-1A43-4F23-A0EC-9B3A7D3C5A42}" &&
              classes[1] == "{A5DCBF10-6530-11D2-901F-00C04FB951ED}" &&
              classes[2] == "{DEE824EF-729B-4A0E-9C14-B7117D33A817}");
        CHECK(WinusbInterfaceClasses("").size() == 2 && WinusbInterfaceClasses("{1234}").size() == 2);
        CHECK(WindowsRevision("USB\\VID_1430&PID_1F17&REV_0100&MI_00") == 0x0100 &&
              WindowsRevision("usb\\vid_1430&pid_09aa&rev_0227") == 0x0227 &&
              WindowsRevision("USB\\VID_1430&PID_1F17") == 0 && WindowsRevision("USB\\VID_1&REV_01") == 0);
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
