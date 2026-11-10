// Exercise the real startup hook with a recording input sink. No window, audio device or game.
#include "host/gamemenu.h"
#include "host/launcher.h"
#include "host/settings.h"
#include <cstdio>

namespace
{
    int calls;
    std::string keys[3];
    bool tab;
}

void HostAudioSetVolume(uint32_t) {}
speakers::Mode HostAudioLayout() { return speakers::Stereo; }
bool portalSound = true;
void PortalSetSound(bool on) { portalSound = on; }
bool PortalGameHasSpeaker() { return true; }
extern const GameInfo g_gameInfo;
const GameInfo g_gameInfo = { "test", "Test", {}, "" };
void gamemenu::InitDebugScreen(const char*, const char*, SDL_Window*) {}
void ToyboxSetKeys(const std::string (&saved)[3], bool alias)
{
    calls++;
    for (int i = 0; i < 3; i++)
        keys[i] = saved[i];
    tab = alias;
}

int main()
{
    RuntimeSettings settings;
    gamemenu::Init(settings, nullptr, "test");
    bool defaults = calls == 1 && keys[0] == "F6" && keys[1] == "F7" && keys[2] == "F8" && tab && portalSound &&
                    gamemenu::Current().portalSound && gamemenu::Current().portalSoundShown;
    settings.toyboxKeys[0] = "O";
    settings.toyboxKeys[1] = "P";
    settings.toyboxKeys[2] = "U";
    settings.toyboxTab = false;
    settings.debugScreen = 2;
    settings.reduceMotion = true;
    settings.portalSound = false;
    gamemenu::Init(settings, nullptr, "test");
    bool saved = calls == 2 && keys[0] == "O" && keys[1] == "P" && keys[2] == "U" && !tab;
    bool merged = gamemenu::Current().debugScreen == 2 && gamemenu::Current().reduceMotion &&
                  gamemenu::Current().version == "test" && !gamemenu::Current().portalSound && !portalSound;
    puts(defaults && saved && merged
             ? "PASS: startup applies Toy Box keys, debug screen, version, reduced motion and portal sound"
             : "FAIL: startup menu values");
    return defaults && saved && merged ? 0 : 1;
}
