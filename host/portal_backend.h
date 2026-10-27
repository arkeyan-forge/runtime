#pragma once

#include "host/figures/figure_data.h"
#include "host/portal.h"
#include "host/portal_identity.h"
#include <array>
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
};

// Real portals. Both rescan for the device while it is unplugged; null when the host API is unavailable.
// Wii, Wii U, PS3, PS4 and PC portals: USB HID (1430:0150), through SDL's hidapi.
std::unique_ptr<PortalBackend> CreateHidPortal();
// Xbox 360 and Xbox One portals: vendor-specific USB, through libusb loaded at run time.
std::unique_ptr<PortalBackend> CreateUsbPortal();

// --portal-probe (docs/runtime/host/portal_probe.md): each real portal found by a transport, after trying to open it and query it.
struct PortalProbe
{
    const char* kind;       // "hid" or "usb"
    uint16_t vendor, product;
    std::string name;       // the device's product string, or what the model is
    bool opened = false;
    std::string status;     // "ok" (it answered), or "error: ..."
    // HID: what the device is, each write method tried with the answers to 'R' and 'S', and the one that worked.
    std::string details, write;
    // The portal's answer to 'R' (portal_identity.h): its id as "01 28", the model's name, and what the game makes of
    // it ("works", "warns", "rejected", "unknown"); empty when it did not answer.
    std::string id, model, verdict, verdictReason;
    std::vector<std::pair<std::string, std::string>> methods;
};
// Fills the probe's id, model and verdict from the portal's answer to 'R' (`report` starts at the letter).
void NameProbe(PortalProbe& probe, const uint8_t* report, size_t size);
// Each adds the portals it finds to `portals`; returns why the transport itself is unavailable, empty if it is not.
std::string ProbeHidPortals(std::vector<PortalProbe>& portals);
std::string ProbeUsbPortals(std::vector<PortalProbe>& portals);

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

    // `path` is where writes are saved; empty keeps them in memory. Takes `wanted` if it is free, else the first free
    // slot. Returns the slot, or -1 if all are taken.
    int Place(const FigureData& data, const std::string& path, int wanted = -1);
    void Remove(int slot);
    bool IsActive();
    // Path of the figure in `slot`; empty if free or kept in memory only.
    std::string SlotPath(int slot);
    // The last `C r g b` the title sent, and how many it sent (Giants fades the portal's light all the time).
    struct Light { uint8_t r = 0, g = 0, b = 0; };
    Light LightColor() { std::lock_guard lock(m_mutex); return m_light; }
    uint64_t LightCommands() { std::lock_guard lock(m_mutex); return m_lightCommands; }

private:
    using Report = std::array<uint8_t, PortalReportSize>;

    struct Slot
    {
        std::optional<FigureData> data;
        std::string path;
        bool changed = false; // reported once in the next status report (placed or removed)
    };

    Report StatusReport();

    std::mutex m_mutex;
    PortalModel m_model = EmulatedPortalModel("ssa");
    Slot m_slots[SlotCount];
    std::deque<Report> m_responses;
    bool m_active = false;
    uint8_t m_statusCounter = 0;
    Light m_light;
    uint64_t m_lightCommands = 0;
    bool m_unknownSeen[256] = {};
};
