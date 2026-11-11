#pragma once

#include "host/figures/figure_data.h"
#include "host/portal.h"
#include "host/portal_identity.h"
#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// One way of reaching a portal. Reports are bare protocol (command letter first, no Xbox 360 header).
class PortalBackend
{
public:
    virtual ~PortalBackend() = default;
    virtual const char* Name() const = 0;
    // Real portals reconnect here after being unplugged.
    virtual bool IsConnected() = 0;
    // Whether the device is still open, without trying to reopen it: false once it failed or went away.
    virtual bool IsOpen() { return IsConnected(); }
    virtual bool Write(const uint8_t* command, size_t size) = 0;
    virtual size_t Read(uint8_t* report, size_t capacity) = 0;
    // The Traptanium speaker (host/portal_speaker.h): whether this portal can be sent the sound blocks, and one block (the
    // Xbox 360's ADPCM, without its `0B 17` header), sent in the device's own format. Without it the computer plays them.
    virtual bool TakesSound() { return false; }
    virtual bool WriteSound(const uint8_t*, size_t) { return false; }
};

// Real portals. Both rescan for the device while it is unplugged; null when the host API is unavailable.
// Wii, Wii U, PS3, PS4 and PC portals: USB HID (1430:0150), through SDL's hidapi.
std::unique_ptr<PortalBackend> CreateHidPortal();
// Xbox 360 and Xbox One portals: vendor-specific USB, through libusb loaded at run time (Linux), IOKit (macOS), WinUSB
// and Windows.Gaming.Input.Custom (Windows).
std::unique_ptr<PortalBackend> CreateUsbPortal();
namespace portal_usb
{
    struct Api;
}
// Tests: the USB backend over the given calls (a fake device, host/portal_usb_backend_test.cpp).
std::unique_ptr<PortalBackend> CreateUsbPortalWith(const portal_usb::Api& usb);

// --portal-probe (docs/runtime/host/portal_probe.md): each real portal found by a transport, after trying to open it and query it.
struct PortalProbe
{
    const char* kind;       // "hid" or "usb"
    uint16_t vendor, product;
    std::string name;       // the device's product string, or what the model is
    bool opened = false;
    std::string status{};   // "ok" (it answered), or "error: ..."
    // HID: what the device is, each write method tried with the answers to 'R' and 'S', and the one that worked.
    std::string details{}, write{};
    // The portal's answer to 'R' (portal_identity.h): its id as "01 28", the model's name, and what the game makes of
    // it ("works", "warns", "rejected", "unknown"); empty when it did not answer.
    std::string id{}, model{}, verdict{}, verdictReason{};
    std::vector<std::pair<std::string, std::string>> methods{};
};
// Fills the probe's id, model and verdict from the portal's answer to 'R' (`report` starts at the letter).
void NameProbe(PortalProbe& probe, const uint8_t* report, size_t size);
// Each adds the portals it finds to `portals`; returns why the transport itself is unavailable, empty if it is not.
std::string ProbeHidPortals(std::vector<PortalProbe>& portals);
std::string ProbeUsbPortals(std::vector<PortalProbe>& portals);
// Lines for the Test result about what the system has that may be a portal (Windows: host/windows_devices.h).
std::vector<std::string> ProbeNotes();

bool LoadFigure(const std::string& path, FigureData& data);
// Rewrites the first 1 KB of the dump in place.
bool SaveFigure(const std::string& path, const FigureData& data);

class EmulatedPortal final : public PortalBackend
{
public:
    static constexpr int SlotCount = 16; // the status report has 2 bits per slot

    const char* Name() const override { return "emulated"; }
    bool IsConnected() override { return true; }
    // The model that 'R' names (the Xbox 360 Spyro's Adventure portal until told otherwise).
    void SetModel(const PortalModel& model) { m_model = model; }
    const PortalModel& Model() const { return m_model; }
    bool Write(const uint8_t* command, size_t size) override;
    size_t Read(uint8_t* report, size_t capacity) override;
    // The least time between two unprompted status reports of an active portal (a real one sends them at its own pace;
    // 0, the default, answers every read). A title that reads until nothing is left never ends on an endless stream.
    void SetStatusInterval(std::chrono::milliseconds interval)
    {
        std::lock_guard lock(m_mutex);
        m_statusInterval = interval;
    }

    // `path` is where writes are saved; empty keeps them in memory. Takes `wanted` if it is free, else the first free
    // slot. Returns the slot, or -1 if all are taken.
    int Place(const FigureData& data, const std::string& path, int wanted = -1);
    void Remove(int slot);
    // What a real portal does on a reset: it forgets its toys and numbers the ones on it anew from slot 0, in their
    // order, each reported as arriving; the slots they leave are not reported (the fake real portal, tests).
    void Renumber();
    // The slot of the toy whose tag serial (block 0's first 4 bytes) is `serial`, -1 for none.
    int SlotOfSerial(const uint8_t* serial);
    bool IsActive();
    // Path of the figure in `slot`; empty if free or kept in memory only.
    std::string SlotPath(int slot);
    // A figure is in `slot` (a dump, or kept in memory).
    bool Holds(int slot);
    // The mixed portal (MixedPortal): slots a real toy uses, which Place never takes (bit n: slot n).
    void SetReserved(uint32_t slots)
    {
        std::lock_guard lock(m_mutex);
        m_reserved = slots;
    }
    // The status report's 2 bits per slot (present, changed since the last report), the changes then counted as reported.
    uint32_t TakeStatus();
    // The last `C r g b` the title sent, and how many it sent (Giants fades the portal's light all the time).
    struct Light
    {
        uint8_t r = 0, g = 0, b = 0;
    };
    Light LightColor()
    {
        std::lock_guard lock(m_mutex);
        return m_light;
    }
    uint64_t LightCommands()
    {
        std::lock_guard lock(m_mutex);
        return m_lightCommands;
    }

private:
    using Report = std::array<uint8_t, PortalReportSize>;

    struct Slot
    {
        std::optional<FigureData> data;
        std::string path;
        bool changed = false; // reported once in the next status report (placed or removed)
    };

    Report StatusReport();
    uint32_t StatusBits(); // under m_mutex

    std::mutex m_mutex;
    PortalModel m_model = EmulatedPortalModel("ssa");
    Slot m_slots[SlotCount];
    std::deque<Report> m_responses;
    bool m_active = false;
    std::chrono::milliseconds m_statusInterval{ 0 };
    std::chrono::steady_clock::time_point m_lastStatus{};
    uint8_t m_statusCounter = 0;
    Light m_light;
    uint64_t m_lightCommands = 0;
    bool m_unknownSeen[256] = {};
    uint32_t m_reserved = 0;
};

// A real portal and the Toy Box's virtual seats as one portal for the game (docs/runtime/host/portal.md, "Mixed portal"): player
// 1 on a real figure and player 2 on a virtual one, or the reverse. The real portal's toys keep their slot when it is free
// and move to a free one when a virtual figure has it; virtual figures sit in the slots the real portal leaves free. The
// status reports the game reads are the real portal's with the virtual slots added; a block read or written goes to the
// portal that holds the slot. Everything else (reset, activate, lights, the speaker) is the real portal's.
// A real portal forgets its toys on a reset and numbers them anew, each reported as arriving again (issue #20), while a
// title resets the portal in bursts when it loads (Giants): a toy the game had read would come back as a new one after each
// reset, and the game would ask for a Skylander forever. So a reset holds the game's real toys in their slots, present and
// unchanged; each toy the real portal then reports is identified by its tag serial (block 0, asked here and never shown to
// the game) and goes back to the game's slot that had it, or arrives as a new toy; a held slot nobody comes back to
// within the hold time (3 s) is reported gone. Blocks asked of a held slot meanwhile get no answer (the title asks again).
class MixedPortal final : public PortalBackend
{
public:
    // `virtualPart` is never activated: it only answers the blocks of its slots and gives its status bits.
    // `holdTime`: how long a reset holds the game's real toys (shorter in the tests).
    MixedPortal(std::unique_ptr<PortalBackend> real, EmulatedPortal& virtualPart,
        std::chrono::milliseconds holdTime = HoldTime);
    const char* Name() const override { return "mixed"; }
    bool IsConnected() override { return m_real->IsConnected(); }
    bool IsOpen() override { return m_real->IsOpen(); }
    bool Write(const uint8_t* command, size_t size) override;
    size_t Read(uint8_t* report, size_t capacity) override;
    bool TakesSound() override { return m_real->TakesSound(); }
    bool WriteSound(const uint8_t* block, size_t size) override { return m_real->WriteSound(block, size); }
    // The game's slot holds a toy of the real portal.
    bool IsRealSlot(int slot);
    // The real portal's slot behind the game's, -1 for none (tests).
    int RealSlotOf(int slot);

    // The game's slot is held after a reset, waiting for its toy to come back (tests).
    bool IsHeld(int slot);
    static constexpr std::chrono::milliseconds HoldTime{ 3000 };

private:
    using Report = std::array<uint8_t, PortalReportSize>;
    using Clock = std::chrono::steady_clock;
    static constexpr int Slots = EmulatedPortal::SlotCount;
    void MergeStatus(uint8_t* report); // under m_mutex, as the rest below
    void Map(int real, int game);
    int FreeGameSlot(int wanted, uint32_t virtualBits);
    void HoldRealToys();
    bool Identify(const uint8_t* report, size_t size); // a block 0 answer asked by Identify's query: taken, not shown
    std::unique_ptr<PortalBackend> m_real;
    EmulatedPortal& m_virtual;
    std::mutex m_mutex;
    int m_gameSlot[Slots]; // per real slot: the game's slot, -1
    int m_realSlot[Slots]; // per game slot: the real slot, -1
    std::deque<Report> m_answers; // the virtual slots' answers, read first
    // Per game slot: the tag serial the game read from its real toy (block 0), and whether it is held after a reset.
    struct Toy
    {
        bool known = false, held = false, arrived = false; // arrived: report its arrival in the next status
        uint8_t serial[4] = {};
    };
    Toy m_toys[Slots];
    std::chrono::milliseconds m_holdTime;
    Clock::time_point m_holdUntil{};
    // Per real slot: a toy present and not mapped yet, being identified (when its block 0 was last asked; 0: not asked).
    bool m_identifying[Slots] = {};
    Clock::time_point m_asked[Slots] = {};
};

// Tests (SKY_PORTAL=fake-real): a "real" portal that is an emulated one holding the dumps of SKY_PORTAL_FAKE_REAL (comma
// separated), with a real portal's pace, run behind AsyncPortal as a HID portal is. It stands for the hardware in the mixed
// portal's checks (ArkeyanPortalTest, tools/regress/toybox_window.sh); the dumps are never written.
std::unique_ptr<PortalBackend> CreateFakeRealPortal(const std::string& dumps);
