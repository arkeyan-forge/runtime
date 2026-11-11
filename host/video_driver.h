#pragma once

#include <string_view>

namespace videodriver
{
    inline bool Offscreen(std::string_view driver)
    {
        return driver == "offscreen" || driver == "dummy";
    }

    inline bool Usable(bool linuxDesktop, std::string_view requested, std::string_view selected, int displays)
    {
        return !linuxDesktop || Offscreen(requested) || (displays > 0 && !Offscreen(selected));
    }

    // Keep a working desktop and explicit offscreen test runs. Only Linux retries native video drivers.
    // open(nullptr) uses SDL's current choice; a named attempt must release the failed video subsystem first.
    template <typename Open>
    bool Start(bool linuxDesktop, std::string_view requested, const Open& open)
    {
        if (open(nullptr))
            return true;
        if (!linuxDesktop || Offscreen(requested))
            return false;
        const char* drivers[] = { "wayland", "x11" };
        for (const char* driver : drivers)
            if (requested != driver && open(driver))
                return true;
        return false;
    }
}
