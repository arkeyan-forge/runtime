// The USB backend (host/portal_usb.cpp) over a fake Xbox 360 portal behind the libusb-shaped calls: the device found,
// its interface claimed, the start request, the commands framed, the answers read one packet at a time and handed to
// the title without their header (no device, no game data): ./ArkeyanPortalUsbBackendTest
#include "host/portal_backend.h"
#include "host/portal_usb.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

bool PortalLogEnabled() { return false; }
void NameProbe(PortalProbe&, const uint8_t*, size_t) {}

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

    // The portal, as the public notes describe it: one interface FF/5D/01 with interrupt IN 0x81 and OUT 0x02 of 32-byte
    // packets, "0B 14" before the command letter both ways; 'R' answered with 'R' and the portal's id, then 'S' status
    // packets. A USB IN transfer ends on a short packet or when it is full, else at its timeout.
    struct Fake
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<std::vector<uint8_t>> pending;
        std::vector<std::vector<uint8_t>> written;
        int startRequests = 0, claims = 0, releases = 0, largestRead = 0;
        bool open = false;
        bool issue23 = false; // issue #23's portal: its status report ready at the claim, 'R' answered alone
    } g_fake;

    portal_usb::Device* const FakeDevice = reinterpret_cast<portal_usb::Device*>(0x10);
    portal_usb::Handle* const FakeHandle = reinterpret_cast<portal_usb::Handle*>(0x20);

    std::vector<uint8_t> Packet(std::initializer_list<uint8_t> report)
    {
        std::vector<uint8_t> packet(32, 0);
        packet[0] = 0x0B;
        packet[1] = 0x14;
        std::copy(report.begin(), report.end(), packet.begin() + 2);
        return packet;
    }

    int Init(portal_usb::Context** context)
    {
        *context = nullptr;
        return 0;
    }
    portal_usb::Device* g_list[2] = { FakeDevice, nullptr };
    ptrdiff_t List(portal_usb::Context*, portal_usb::Device*** list)
    {
        *list = g_list;
        return 1;
    }
    void FreeList(portal_usb::Device**, int) {}
    int Descriptor(portal_usb::Device*, portal_usb::DeviceDescriptor* descriptor)
    {
        *descriptor = {};
        descriptor->bLength = 18;
        descriptor->idVendor = 0x1430;
        descriptor->idProduct = 0x1F17;
        descriptor->bcdDevice = 0x0100;
        return 0;
    }
    int Open(portal_usb::Device*, portal_usb::Handle** handle)
    {
        *handle = FakeHandle;
        g_fake.open = true;
        return 0;
    }
    void Close(portal_usb::Handle*) { g_fake.open = false; }
    int Detach(portal_usb::Handle*, int) { return 0; }
    int Claim(portal_usb::Handle*, int number)
    {
        std::lock_guard lock(g_fake.mutex);
        g_fake.claims++;
        if (g_fake.issue23 && number == 0) // what the player's portal sent first (SteamOS, 0.9.5)
            g_fake.pending.push_back({ 0x0B, 0x14, 0x53, 0x00, 0x00, 0x00, 0x00, 0xE5, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        return number == 0 ? 0 : portal_usb::ErrorNotFound;
    }
    int Release(portal_usb::Handle*, int)
    {
        g_fake.releases++;
        return 0;
    }
    int Control(portal_usb::Handle*, uint8_t type, uint8_t request, uint16_t value, uint16_t index, unsigned char* data,
        uint16_t length, unsigned int)
    {
        if (type == 0x80 && request == 0x06 && value == 0x0200)
        {
            const uint8_t configuration[] = { 9, 2, 32, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 2, 0xFF, 0x5D, 0x01, 0, 7, 5,
                0x81, 3, 32, 0, 4, 7, 5, 0x02, 3, 32, 0, 8 };
            const size_t size = std::min<size_t>(length, sizeof(configuration));
            memcpy(data, configuration, size);
            return int(size);
        }
        if (type == 0xC1 && request == 0x01 && value == 0x0100 && index == 0)
        {
            std::lock_guard lock(g_fake.mutex);
            g_fake.startRequests++;
            memset(data, 0, length);
            return length;
        }
        return portal_usb::ErrorPipe;
    }
    int Interrupt(portal_usb::Handle*, unsigned char endpoint, unsigned char* data, int length, int* transferred,
        unsigned int timeout)
    {
        *transferred = 0;
        std::unique_lock lock(g_fake.mutex);
        if (endpoint == 0x02)
        {
            g_fake.written.emplace_back(data, data + length);
            if (length == 32 && data[0] == 0x0B && data[1] == 0x14 && data[2] == 'R')
            {
                g_fake.pending.push_back(Packet({ 'R', 0x01, 0x28 }));
                if (!g_fake.issue23)
                    g_fake.pending.push_back(Packet({ 'S', 0x01, 0, 0, 0, 0x01 }));
                g_fake.ready.notify_all();
            }
            *transferred = length;
            return 0;
        }
        if (endpoint != 0x81)
            return portal_usb::ErrorNotFound;
        g_fake.largestRead = std::max(g_fake.largestRead, length);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        while (*transferred < length)
        {
            if (g_fake.pending.empty() &&
                !g_fake.ready.wait_until(lock, deadline, [] { return !g_fake.pending.empty(); }))
                return portal_usb::ErrorTimeout; // what had arrived stays in `transferred`
            std::vector<uint8_t> packet = g_fake.pending.front();
            g_fake.pending.pop_front();
            const int size = std::min(int(packet.size()), length - *transferred);
            memcpy(data + *transferred, packet.data(), size_t(size));
            *transferred += size;
            if (size < 32)
                break;
        }
        return 0;
    }
    const char* Name(int error) { return portal_usb::ErrorName(error); }

    // The next report the title would get, waiting up to 2 s.
    size_t NextReport(PortalBackend& portal, uint8_t* report, size_t capacity)
    {
        for (int i = 0; i < 200; i++)
        {
            if (size_t size = portal.Read(report, capacity))
                return size;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return 0;
    }
}

int main()
{
    portal_usb::Api usb{};
    usb.init = Init;
    usb.get_device_list = List;
    usb.free_device_list = FreeList;
    usb.get_device_descriptor = Descriptor;
    usb.open = Open;
    usb.close = Close;
    usb.set_auto_detach_kernel_driver = Detach;
    usb.claim_interface = Claim;
    usb.release_interface = Release;
    usb.control_transfer = Control;
    usb.interrupt_transfer = Interrupt;
    usb.error_name = Name;
    usb.from = "fake";

    {
        std::unique_ptr<PortalBackend> portal = CreateUsbPortalWith(usb);
        CHECK(portal->IsConnected() && g_fake.open && g_fake.claims == 1);
        CHECK(std::string(portal->Name()) == "Xbox 360 USB");
        {
            std::lock_guard lock(g_fake.mutex);
            CHECK(g_fake.startRequests == 1); // made once, after the claim
        }

        // The title's 'R' goes out framed; the answer comes back without its header, then the status.
        const uint8_t reset[] = { 'R' };
        CHECK(portal->Write(reset, sizeof(reset)));
        {
            std::lock_guard lock(g_fake.mutex);
            CHECK(g_fake.written.size() == 1 && g_fake.written[0].size() == 32 && g_fake.written[0][0] == 0x0B &&
                  g_fake.written[0][1] == 0x14 && g_fake.written[0][2] == 'R' && g_fake.written[0][3] == 0);
        }
        uint8_t report[64]{};
        size_t size = NextReport(*portal, report, sizeof(report));
        CHECK(size == 30 && report[0] == 'R' && report[1] == 0x01 && report[2] == 0x28);
        size = NextReport(*portal, report, sizeof(report));
        CHECK(size == 30 && report[0] == 'S' && report[1] == 0x01);
        {
            std::lock_guard lock(g_fake.mutex);
            CHECK(g_fake.largestRead == 32); // one packet per transfer (0.9.5 asked for 64 and lost them)
        }
        // The speaker's blocks go under their own header.
        const uint8_t sound[4] = { 1, 2, 3, 4 };
        CHECK(portal->TakesSound() && portal->WriteSound(sound, sizeof(sound)));
        {
            std::lock_guard lock(g_fake.mutex);
            CHECK(g_fake.written.back()[1] == 0x17 && g_fake.written.back()[2] == 1);
        }
    }
    CHECK(!g_fake.open && g_fake.releases == 1); // released and closed with the backend

    {
        // Issue #23, replayed through the game's backend: the status report waiting at the claim reaches the title
        // first, then the answer to its 'R' (0.9.5 lost both: one packet each, inside 64-byte reads).
        g_fake.pending.clear();
        g_fake.written.clear();
        g_fake.issue23 = true;
        std::unique_ptr<PortalBackend> portal = CreateUsbPortalWith(usb);
        CHECK(portal->IsConnected());
        uint8_t report[64]{};
        size_t size = NextReport(*portal, report, sizeof(report));
        CHECK(size == 30 && report[0] == 'S' && report[1] == 0 && report[5] == 0xE5 && report[6] == 0x01);
        const uint8_t reset[] = { 'R' };
        CHECK(portal->Write(reset, sizeof(reset)));
        size = NextReport(*portal, report, sizeof(report));
        CHECK(size == 30 && report[0] == 'R' && report[1] == 0x01 && report[2] == 0x28);
    }

    printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
