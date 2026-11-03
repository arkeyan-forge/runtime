#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A default.xex's execution info and dashboard resources, read from the player's own file (the launcher checks a disc with
// them, Arkeyan-Builder takes the game's icon from them). Bounds-checked: the file comes from the player.
struct ExecutionInfo
{
    uint32_t mediaId;
    uint32_t version;
    uint32_t titleId;
};
std::optional<ExecutionInfo> ReadExecutionInfo(const std::vector<uint8_t>& xex);

// Name and icon from the XDBF resource of a verified default.xex. Game content: keep it in memory only.
struct TitleMetadata
{
    std::string name;
    std::vector<uint8_t> iconPng;
    uint32_t titleId = 0;
};
TitleMetadata ReadTitleMetadata(const std::vector<uint8_t>& xex);
