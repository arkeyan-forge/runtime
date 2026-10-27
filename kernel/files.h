#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Drive names ("save:") that XamContentCreateEx mounts on host directories. Guest paths under a mounted root are read
// and written on the host; every other path is on the read-only disc. Names are case-insensitive. Mounting fails if
// the name is already mounted, unmounting if it is not.
bool MountContentRoot(const std::string& name, const std::filesystem::path& directory);
bool UnmountContentRoot(const std::string& name);

// A guest file name as a host one that is valid on every host OS: characters Windows forbids and non-ASCII ones become
// '_'. Empty for names that cannot be stored ("", ".", "..").
std::filesystem::path HostFileName(std::string_view name);

// A UTF-8 path from the environment or SDL.
std::filesystem::path Utf8Path(std::string_view path);
