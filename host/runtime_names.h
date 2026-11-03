#pragma once
#include <filesystem>

namespace runtime_names
{
    inline constexpr char Log[] = "Arkeyan.log";
    inline constexpr char PreviousLog[] = "Arkeyan.prev.log";
    inline constexpr char LegacyLog[] = "SSARecomp.log";
    inline constexpr char LegacyPreviousLog[] = "SSARecomp.prev.log";

    // Read logs in place, including an old game started outside the launcher after a new one.
    inline std::filesystem::path ReadLog(const std::filesystem::path& folder, bool previous = false)
    {
        namespace fs = std::filesystem;
        const fs::path current = folder / (previous ? PreviousLog : Log);
        const fs::path legacy = folder / (previous ? LegacyPreviousLog : LegacyLog);
        std::error_code error;
        if (!fs::is_regular_file(current, error))
            return fs::is_regular_file(legacy, error) ? legacy : current;
        const auto currentTime = fs::last_write_time(current, error);
        if (error)
            return current;
        const auto legacyTime = fs::last_write_time(legacy, error);
        return !error && legacyTime > currentTime ? legacy : current;
    }
}
