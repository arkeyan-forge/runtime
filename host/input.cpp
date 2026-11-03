#include "host/input.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "env.h"

namespace
{
    struct ScriptedPress
    {
        std::chrono::steady_clock::duration at;
        std::chrono::steady_clock::duration length;
        uint16_t button;
        int16_t stickX, stickY; // left stick, for "lup", "ldown", "lleft", "lright"
        uint8_t leftTrigger, rightTrigger; // for "lt", "rt"
    };

    constexpr auto PressLength = std::chrono::milliseconds(150);

    struct User
    {
        GamepadState host;
        uint32_t hostPacket = 0;
        std::vector<ScriptedPress> script;
        bool connected = false;
        uint16_t vibrationLeft = 0, vibrationRight = 0;
        uint32_t vibrationSerial = 0;
    };

    std::mutex g_mutex;
    User g_users[MaxUsers] = { { .connected = true } };
    const auto g_launch = std::chrono::steady_clock::now();

    uint16_t ButtonByName(const std::string& name)
    {
        static const std::pair<const char*, uint16_t> names[] = {
            { "start", Button::Start },
            { "back", Button::Back },
            { "a", Button::A },
            { "b", Button::B },
            { "x", Button::X },
            { "y", Button::Y },
            { "up", Button::DpadUp },
            { "down", Button::DpadDown },
            { "left", Button::DpadLeft },
            { "right", Button::DpadRight },
            { "lb", Button::LeftShoulder },
            { "rb", Button::RightShoulder },
        };
        for (const auto& [text, button] : names)
        {
            if (name == text)
                return button;
        }
        return 0;
    }
}

void SetHostGamepad(const GamepadState& state, int user, bool connected)
{
    std::lock_guard lock(g_mutex);
    User& u = g_users[user];
    u.connected = user == 0 || connected;
    if (memcmp(&state, &u.host, sizeof(state)) != 0)
    {
        u.host = state;
        u.hostPacket++;
    }
}

void SetHostVibration(int user, uint16_t left, uint16_t right)
{
    if (user < 0 || user >= MaxUsers)
        return;
    std::lock_guard lock(g_mutex);
    User& u = g_users[user];
    if (u.vibrationLeft != left || u.vibrationRight != right)
    {
        u.vibrationLeft = left;
        u.vibrationRight = right;
        u.vibrationSerial++;
    }
}

void GetHostVibration(int user, uint16_t& left, uint16_t& right, uint32_t& serial)
{
    std::lock_guard lock(g_mutex);
    const User& u = g_users[user];
    left = u.vibrationLeft;
    right = u.vibrationRight;
    serial = u.vibrationSerial;
}

bool GamepadConnected(int user)
{
    std::lock_guard lock(g_mutex);
    // SKY_PRESS_FILE: the first controller is there from the start (a game that saw no pad at its title may wait for one)
    static const bool pressFile = skyenv::Get("SKY_PRESS_FILE") != nullptr;
    return user >= 0 && user < MaxUsers &&
           (g_users[user].connected || !g_users[user].script.empty() || (user == 0 && pressFile));
}

// SKY_PRESS_FILE=<file>: lines appended to it while the game runs are presses made at once, "<button>[:<hold seconds>]"
// for the first controller, "2 <button>[:<hold>]" for the second (an unattended run steered from outside, by looking
// at the frames it writes; the same names as SKY_PRESS).
static void ReadPressFile()
{
    static const char* path = skyenv::Get("SKY_PRESS_FILE");
    static size_t offset = 0;
    static auto lastCheck = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (path == nullptr || now - lastCheck < std::chrono::milliseconds(100))
        return;
    lastCheck = now;
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
        return;
    fseek(file, 0, SEEK_END);
    size_t size = size_t(ftell(file));
    std::string text(size > offset ? size - offset : 0, '\0');
    fseek(file, long(offset), SEEK_SET);
    size_t got = text.empty() ? 0 : fread(text.data(), 1, text.size(), file);
    fclose(file);
    text.resize(got);
    size_t end = text.rfind('\n');
    if (end == std::string::npos)
        return;
    offset += end + 1;
    size_t start = 0;
    while (start < end)
    {
        size_t stop = text.find('\n', start);
        std::string line = text.substr(start, stop - start);
        start = stop + 1;
        int user = 0;
        if (line.size() > 2 && line[0] == '2' && line[1] == ' ')
        {
            user = 1;
            line.erase(0, 2);
        }
        std::string name = line.substr(0, line.find(':'));
        size_t hold = line.find(':');
        auto length = hold != std::string::npos ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                      std::chrono::duration<double>(atof(line.c_str() + hold + 1)))
                                                : std::chrono::steady_clock::duration(PressLength);
        int16_t x = name == "lleft" ? -32767 : name == "lright" ? 32767 : 0;
        int16_t y = name == "ldown" ? -32767 : name == "lup" ? 32767 : 0;
        g_users[user].script.push_back({ now - g_launch, length, ButtonByName(name), x, y,
            uint8_t(name == "lt" ? 255 : 0), uint8_t(name == "rt" ? 255 : 0) });
    }
}

GamepadState GetGamepad(uint32_t& packetNumber, int user)
{
    std::lock_guard lock(g_mutex);
    ReadPressFile();
    const User& u = g_users[user];
    GamepadState state = u.host;
    packetNumber = u.hostPacket;

    auto now = std::chrono::steady_clock::now() - g_launch;
    for (const auto& press : u.script)
    {
        if (now >= press.at && now < press.at + press.length)
        {
            state.buttons |= press.button;
            if (press.stickX != 0)
                state.thumbLX = press.stickX;
            if (press.stickY != 0)
                state.thumbLY = press.stickY;
            state.leftTrigger = std::max(state.leftTrigger, press.leftTrigger);
            state.rightTrigger = std::max(state.rightTrigger, press.rightTrigger);
            packetNumber += 0x10000; // distinct from host-only states
        }
    }
    return state;
}

void LoadInputScript(const char* script, int user)
{
    std::lock_guard lock(g_mutex);
    std::string text = script;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find(',', start);
        std::string item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
        {
            using std::chrono::duration_cast;
            using Duration = std::chrono::steady_clock::duration;
            auto at = std::chrono::duration<double>(atof(item.c_str() + colon + 1));
            size_t hold = item.find(':', colon + 1);
            Duration length =
                hold != std::string::npos
                    ? duration_cast<Duration>(std::chrono::duration<double>(atof(item.c_str() + hold + 1)))
                    : PressLength;
            std::string name = item.substr(0, colon);
            int16_t x = name == "lleft" ? -32767 : name == "lright" ? 32767 : 0;
            int16_t y = name == "ldown" ? -32767 : name == "lup" ? 32767 : 0;
            g_users[user].script.push_back({ duration_cast<Duration>(at), length, ButtonByName(name), x, y,
                uint8_t(name == "lt" ? 255 : 0), uint8_t(name == "rt" ? 255 : 0) });
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
}
