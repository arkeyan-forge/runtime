#pragma once

struct RuntimeSettings;

// The game log: everything the runtime prints on stderr also goes to SSARecomp.log in the game's data folder
// (datadir.h: %APPDATA%\Skylanders\SSA on Windows, ~/Library/Application Support/Skylanders/SSA on macOS,
// ~/.local/share/Skylanders/SSA on Linux). The previous session's log is kept as SSARecomp.prev.log. SSA_LOG=<file> writes
// that file instead (test runs); an empty SSA_LOG writes none. Each line is in the file as soon as it is printed (a
// crash loses nothing); a terminal still sees it. Call first thing in main, before anything prints.
void LogInit();

// The first lines of the log: version and build, OS, CPU, memory, then the settings. The GPU adapter, backend,
// window size and resolution scale follow from gpu::Init.
void LogHeader(const RuntimeSettings& settings);
