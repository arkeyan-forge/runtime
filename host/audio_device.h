// Which output device the game plays on (host/audio.cpp): the system's default, unless it is a portal's. A Traptanium
// portal with a speaker can show up as an audio device (Windows names it the default output when the Xbox One portal is
// plugged in); its speaker is the portal driver's business, so the game's sound never goes there. Pure, tested by
// audio_device_test.cpp.
#pragma once
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace audio_device
{
    struct Output
    {
        uint32_t id = 0; // SDL's physical device id
        std::string name;
    };

    // A portal's own device, from its name ("Speakers (Skylanders Portal)"), or one of `portalNames`: the names of the
    // outputs the system says sit on a portal's USB device (Windows: host/windows_devices.h), whatever they are called.
    inline bool IsPortal(const std::string& name, const std::vector<std::string>& portalNames = {})
    {
        for (const std::string& known : portalNames)
        {
            if (known == name)
                return true;
        }
        std::string lower = name;
        for (char& c : lower)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        for (const char* word : { "portal", "skylander", "spyro", "traptanium" })
        {
            if (lower.find(word) != std::string::npos)
                return true;
        }
        return false;
    }

    struct Choice
    {
        uint32_t id = 0; // 0: no output the game may use
        std::string name; // the chosen output
        bool isDefault = false; // it is the system's default
        std::vector<std::string> skipped; // the portals' outputs passed over
    };

    // The default when it is not a portal's, else the first output that is not one; never a portal's.
    inline Choice Choose(const std::string& defaultName, const std::vector<Output>& outputs,
        const std::vector<std::string>& portalNames = {})
    {
        Choice choice;
        for (const Output& output : outputs)
        {
            if (IsPortal(output.name, portalNames))
                choice.skipped.push_back(output.name);
        }
        if (!defaultName.empty() && !IsPortal(defaultName, portalNames))
        {
            for (const Output& output : outputs)
            {
                if (output.name == defaultName)
                {
                    choice.id = output.id;
                    choice.name = output.name;
                    choice.isDefault = true;
                    return choice;
                }
            }
        }
        for (const Output& output : outputs)
        {
            if (!IsPortal(output.name, portalNames))
            {
                choice.id = output.id;
                choice.name = output.name;
                return choice;
            }
        }
        return choice;
    }
}
