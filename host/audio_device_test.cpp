// The output device choice (host/audio_device.h) on fake device names: the default unless it is a portal's, never a
// portal's. No audio device needed: ./ArkeyanAudioDeviceTest
#include "host/audio_device.h"
#include <cstdio>

namespace
{
    int g_checks = 0, g_failures = 0;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        g_checks++;                                                                                                    \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            g_failures++;                                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                       \
        }                                                                                                              \
    } while (0)
}

int main()
{
    using namespace audio_device;
    const std::vector<Output> outputs = { { 2, "Speakers (Realtek(R) Audio)" }, { 4, "Speakers (Skylanders Portal)" },
        { 6, "Headphones (USB Audio)" } };

    // The names.
    CHECK(IsPortal("Speakers (Skylanders Portal)"));
    CHECK(IsPortal("Headset Earphone (SPYRO PORTA)"));
    CHECK(IsPortal("Traptanium speaker"));
    CHECK(!IsPortal("Speakers (Realtek(R) Audio)"));
    CHECK(!IsPortal("MacBook Pro Speakers"));
    CHECK(!IsPortal("Headset Earphone (Xbox Controller)"));
    // A name the system ties to a portal's USB device, whatever it says.
    CHECK(IsPortal("Headset Earphone (Xbox Controller)", { "Headset Earphone (Xbox Controller)" }));

    // The default is no portal: it is chosen, the portal is still named as skipped.
    Choice choice = Choose("Speakers (Realtek(R) Audio)", outputs);
    CHECK(choice.id == 2 && choice.isDefault && choice.skipped.size() == 1 &&
          choice.skipped[0] == "Speakers (Skylanders Portal)");

    // The portal became the default (plugged in on Windows): the first other output instead.
    choice = Choose("Speakers (Skylanders Portal)", outputs);
    CHECK(choice.id == 2 && !choice.isDefault && choice.name == "Speakers (Realtek(R) Audio)");

    // The same through the system's list of portal outputs.
    const std::vector<Output> generic = { { 3, "Headset Earphone (Xbox Controller)" }, { 5, "Speakers (Realtek)" } };
    choice = Choose("Headset Earphone (Xbox Controller)", generic, { "Headset Earphone (Xbox Controller)" });
    CHECK(choice.id == 5 && !choice.isDefault && choice.skipped.size() == 1);
    choice = Choose("Headset Earphone (Xbox Controller)", generic);
    CHECK(choice.id == 3 && choice.isDefault && choice.skipped.empty());

    // A default that changed to another real output is followed.
    choice = Choose("Headphones (USB Audio)", outputs);
    CHECK(choice.id == 6 && choice.isDefault);

    // Only a portal: nothing (the game runs silent rather than through the portal).
    choice = Choose("Speakers (Skylanders Portal)", { { 4, "Speakers (Skylanders Portal)" } });
    CHECK(choice.id == 0 && choice.skipped.size() == 1);
    choice = Choose("", {});
    CHECK(choice.id == 0 && choice.skipped.empty());

    // A default not in the list (the driver names it apart): the first output that is no portal.
    choice = Choose("Default", outputs);
    CHECK(choice.id == 2 && !choice.isDefault);

    printf("audio device test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
