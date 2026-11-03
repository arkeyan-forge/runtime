#pragma once

#include <string>

struct RuntimeSettings;

// The game log: everything the runtime prints on stderr also goes to Arkeyan.log in the game's data folder
// (datadir.h: %APPDATA%\Skylanders\SSA on Windows, ~/Library/Application Support/Skylanders/SSA on macOS,
// ~/.local/share/Skylanders/SSA on Linux). The previous session's log is kept as Arkeyan.prev.log. SKY_LOG=<file> writes
// that file instead (test runs); an empty SKY_LOG writes none. Each line is in the file as soon as it is printed (a
// crash loses nothing); a terminal still sees it. Call first thing in main, before anything prints.
void LogInit();

// The first lines of the log: version and build, OS, CPU, memory, then the settings. The GPU adapter, backend,
// window size and resolution scale follow from gpu::Init.
void LogHeader(const RuntimeSettings& settings);

// The game log being written and the previous run's (empty when there is none); the F3 menu's report reads them.
std::string LogPath();
std::string PreviousLogPath();

// Read-only identity shared by the log and debug screen. System strings are cached at initialization.
// No account or host name is included. LogBuild identifies this configured binary, including its dirty suffix.
const char* LogBuild();
const char* SystemOsName();
const char* SystemCpuName();
