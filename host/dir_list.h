#pragma once
#include <filesystem>
#include <system_error>
#include <vector>

// The entries of one folder, read without ever throwing. A range-for over directory_iterator(path, error) checks the
// error only when the iterator is built: operator++ throws when a later read fails (a folder the system refuses to
// list, one that vanishes mid-walk). Here a failed read just ends the list; what was read so far is kept.
inline std::vector<std::filesystem::directory_entry> ListDirectory(const std::filesystem::path& folder, std::error_code& error)
{
    std::vector<std::filesystem::directory_entry> entries;
    std::filesystem::directory_iterator it(folder, error), end;
    for (; !error && it != end; it.increment(error))
        entries.push_back(*it);
    return entries;
}
