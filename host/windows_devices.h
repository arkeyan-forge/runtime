// Windows: the present devices that look like a portal, read from the configuration manager (read only: nothing is
// opened). The portal's Test result and the game log list them, with their driver, so a portal Arkeyan does not reach
// still shows what Windows made of it; the audio output skips the outputs that sit on one (host/audio_device.h).
// Elsewhere the lists are empty. The rule is pure, tested by portal_usb_test.cpp.
#pragma once
#include <cctype>
#include <string>
#include <vector>

namespace windows_devices
{
    struct Device
    {
        std::string instanceId;  // "USB\VID_24C6&PID_FA00\6&2A1F3C&0&3"
        std::string name;        // the friendly name, else the description
        std::string service;     // the driver service ("HidUsb", "xboxgip", "WinUSB"), empty when none
        std::string hardwareIds; // its hardware ids, comma-separated
        std::string portalNode;  // the node that looks like a portal: itself, or a parent (an audio output on one)
        bool sure = false;       // Portal::Sure, else Maybe
    };

    // Whether a device is a portal: Sure for Activision's (1430), the Xbox One portal (24C6:FA00) and a name that says
    // so; Maybe for the other devices of the Xbox One accessory makers (24C6, 0E6F: their pads too), listed only.
    enum class Portal
    {
        No,
        Maybe,
        Sure,
    };
    inline Portal LooksLikePortal(
        const std::string& instanceId, const std::string& hardwareIds, const std::string& name)
    {
        auto upper = [](std::string text) {
            for (char& c : text)
                c = char(std::toupper(static_cast<unsigned char>(c)));
            return text;
        };
        const std::string ids = upper(instanceId) + "," + upper(hardwareIds), words = upper(name);
        auto has = [&](const char* text) { return ids.find(text) != std::string::npos; };
        for (const char* word : { "PORTAL", "SKYLANDER", "SPYRO", "TRAPTANIUM" })
        {
            if (words.find(word) != std::string::npos)
                return Portal::Sure;
        }
        if (has("VID_1430") || has("VID&00021430") || has("VID_24C6&PID_FA00"))
            return Portal::Sure;
        if (has("VID_24C6") || has("VID_0E6F") || has("VID&000224C6") || has("VID&00020E6F"))
            return Portal::Maybe;
        return Portal::No;
    }

    // One line for the log and the Test result.
    inline std::string Describe(const Device& device)
    {
        std::string line = device.instanceId + " \"" + device.name + "\", driver " +
                           (device.service.empty() ? std::string("(none)") : device.service);
        if (!device.hardwareIds.empty() && device.hardwareIds.find(device.instanceId) == std::string::npos)
            line += ", ids " + device.hardwareIds;
        if (device.portalNode != device.instanceId)
            line += ", on " + device.portalNode;
        return line;
    }

    // An audio output's node ("SWD\MMDEVAPI\{0.0.0.00000000}.{...}"): its name is the output's name.
    inline bool IsAudioOutput(const Device& device)
    {
        return device.instanceId.rfind("SWD\\MMDEVAPI\\{0.0.0.", 0) == 0;
    }

    // Every present device that looks like a portal, and every one under such a device. Empty off Windows.
    std::vector<Device> PortalLikeDevices();

    // The names of the audio outputs on a portal (for host/audio_device.h). Empty off Windows.
    std::vector<std::string> PortalAudioOutputNames();
}
