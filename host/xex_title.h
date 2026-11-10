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
// The security info's region mask: the consoles the disc runs on (0x000000FF North America, 0x0000FF00 Japan and Asia,
// 0x00FF0000 Europe and Australia, 0xFFFFFFFF region free).
std::optional<uint32_t> ReadRegion(const std::vector<uint8_t>& xex);
// Whether XenonUtils' image loader (Image::ParseImage), which trusts the header, can read this file: the header, the
// security and file format infos, the first compressed block and the import table are in bounds. A disc check calls it
// before decrypting an unverified default.xex, so a damaged or invented file is refused instead of crashing the loader.
// (The compressed blocks after the first are checked by their hash chain inside the loader.)
bool XexImageReadable(const std::vector<uint8_t>& xex);

// Name and icon from the XDBF resource of a verified default.xex. Game content: keep it in memory only.
struct TitleMetadata
{
    std::string name;
    std::vector<uint8_t> iconPng;
    uint32_t titleId = 0;
};
TitleMetadata ReadTitleMetadata(const std::vector<uint8_t>& xex);
