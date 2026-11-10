#pragma once

// What the debug screen reads of the machine at each refresh (docs/runtime/host/gamemenu.md): CPU time of the process and
// of the threads frame_stats.h registers, the thermal state and the power source. Read only while the screen is on; a
// value a platform cannot give stays unknown (the screen says n/a). No allocation of our own.
#include <cstddef>
#include <cstdint>

namespace machine
{
    constexpr int ThreadCount =
        6; // the kinds of gpu::framestats::Thread: game, renderer, main, textures, shaders, audio

    // The process's CPU time and each kind's (its threads' sum, nanoseconds).
    struct Cpu
    {
        bool processKnown = false;
        uint64_t processNs = 0;
        bool threadKnown[ThreadCount]{};
        uint64_t threadNs[ThreadCount]{};
        uint32_t threads[ThreadCount]{}; // how many of each kind were read
    };
    void ReadCpu(Cpu& cpu);

    // The runtime's own state the Full screen shows, read in one place so that ArkeyanDebugScreenTest can give its own.
    struct Runtime
    {
        uint32_t guestThreads = 0;            // running now
        uint64_t heapUsed = 0, heapSize = 0;  // the console's physical memory, allocated and in all
        bool texturesKnown = false;           // false: the renderer was busy at the read
        size_t textures = 0;                  // host textures alive
        size_t packFiles = 0;                 // texture pack files indexed
        uint32_t voices = 0, underruns = 0;   // audio clients playing, empty-queue outputs since the start
        char input[48]{};                     // the pad in use (its type, never its name) or the keyboard
        char glyphs[24]{};                    // the button pictures shown
        uint32_t seatToys[8]{};               // the toy type on each portal seat the title read, 0: none or unknown
        uint32_t seats = 0;                   // how many entries of seatToys are filled
    };
    void ReadRuntime(Runtime& runtime);

    // The whole machine: CPU ticks busy and in all since the start (their change gives the system's CPU use) and the
    // RAM in use (not free or reclaimable cache) and in all.
    struct System
    {
        bool cpuKnown = false, ramKnown = false;
        uint64_t cpuBusy = 0, cpuTotal = 0;
        uint64_t ramUsed = 0, ramTotal = 0;
    };
    System ReadSystem();

    // macOS: its thermal pressure level; Linux: the hottest thermal zone; Windows: unknown (no public API without WMI).
    enum class Thermal
    {
        Unknown,
        Nominal,
        Moderate,
        Heavy,
        Critical
    };
    struct Heat
    {
        Thermal level = Thermal::Unknown;
        int celsius = -1000; // below -273: unknown
    };
    Heat ReadHeat();

    struct Power
    {
        bool known = false;
        bool battery = false; // running on the battery (else plugged in)
        int percent = -1;     // the battery's charge, -1: none or unknown
    };
    Power ReadPower();
}
