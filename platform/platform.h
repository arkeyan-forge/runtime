#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// Host operating system services used by the runtime. Everything OS-specific lives behind this header:
// platform_posix.cpp (macOS, Linux) and platform_win32.cpp (Windows desktop, later UWP / Xbox Dev Mode).
// timebase.cpp holds the CPU-specific counter frequency.

// Process-wide setup, first thing in main().
void PlatformInit();

// Fatal faults (bad memory access, illegal instruction, trap) call `onFault` with a short description and the
// faulting address (the data address for memory faults), then the platform prints a backtrace and exits with 1.
void PlatformInstallFaultHandler(void (*onFault)(const char* description, const void* address));

// `size` bytes of zero-filled, readable and writable address space whose pages take host memory once touched.
// Returns nullptr on failure.
uint8_t* PlatformReserveMemory(size_t size);

// Makes the page-aligned range inaccessible: any access faults.
bool PlatformProtectNoAccess(void* address, size_t size);

// Makes the page-aligned range read-only (a write faults), or readable and writable again.
bool PlatformProtectReadOnly(void* address, size_t size, bool readOnly);

// The granularity of PlatformProtectReadOnly (16 KB on Apple silicon, 4 KB elsewhere).
size_t PlatformPageSize();

// Memory faults are first offered to `handled` (write_watch.cpp), on the faulting thread inside the signal handler: it
// returns true when it made the access possible, which is then retried (POSIX: SIGSEGV/SIGBUS; Windows: a vectored
// exception handler, writes only). False if the hook cannot be installed: nothing may then rely on it.
bool PlatformInstallWriteFaultFilter(bool (*handled)(void* address));

// Runs `body` on a new detached host thread with a stack of `stackSize` bytes.
void PlatformStartThread(size_t stackSize, std::function<void()> body);

// Names the calling thread for debuggers and crash reports.
void PlatformSetThreadName(const char* name);

// Desktop Windows: starts a new copy of this program with the same command line and `name`=`value` added to its
// environment (the switch to Vulkan, main.cpp). False when it did not start, and on the other systems.
bool PlatformStartAgain(const char* name, const char* value);

// CPU time (user and system) the whole process has used, in nanoseconds; false where it cannot be read.
bool PlatformProcessCpuNs(uint64_t& ns);
// The calling thread's CPU clock, which PlatformThreadCpuNs reads from any thread while the thread lives (0: none).
uint64_t PlatformThreadCpuClock();
bool PlatformThreadCpuNs(uint64_t clock, uint64_t& ns);

// Asks the scheduler to wake the calling thread on time, every `periodUs` microseconds, for a short computation (the
// emulated vertical blank). macOS: the real-time (time constraint) policy: timed waits otherwise ended 2-8 ms late
// (timer coalescing), 0.03 ms with it. Windows: nothing more than timeBeginPeriod(1) in PlatformInit. Linux: nothing yet.
void PlatformSetThreadTimingCritical(uint32_t periodUs);

// Frequency of the counter the translated mftb instruction reads (__rdtsc in ppc_context.h).
uint64_t PlatformTimeBaseFrequency();

// Read-only file for positional reads, safe to use from several threads at once. -1 is never a valid file.
using PlatformFile = intptr_t;
constexpr PlatformFile InvalidPlatformFile = -1;

// `path` is UTF-8.
PlatformFile PlatformOpenFile(const char* path);
// Bytes read (0 at end of file), -1 on error.
int64_t PlatformReadFile(PlatformFile file, void* buffer, uint32_t size, uint64_t offset);
void PlatformCloseFile(PlatformFile file);
