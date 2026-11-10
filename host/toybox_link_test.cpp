// The Toy Box link (host/toybox_link.h, toybox_socket.h, toybox_remote.h): the messages, the lines, the link file, and
// the game's end served on the loopback to a fake window. No game data. ./ArkeyanToyboxLinkTest
#include "host/toybox_link.h"
#include "host/toybox_remote.h"
#include "host/toybox_socket.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* what)
    {
        if (!condition)
        {
            fprintf(stderr, "FAIL: %s\n", what);
            g_failures++;
        }
    }

    // The lines a fake window receives within `timeoutMs`, until `until` is seen (empty: any line).
    std::vector<toyboxlink::Message> Receive(
        toyboxsocket::Handle handle, toyboxlink::LineBuffer& buffer, const std::string& until, int timeoutMs = 2000)
    {
        std::vector<toyboxlink::Message> got;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end)
        {
            std::string line;
            while (buffer.Next(line))
            {
                toyboxlink::Message message;
                if (toyboxlink::Parse(line, message))
                {
                    got.push_back(message);
                    if (until.empty() || message.verb == until)
                        return got;
                }
            }
            if (toyboxsocket::Wait(&handle, 1, 50) == 0)
                continue;
            char data[1024];
            long size = toyboxsocket::Receive(handle, data, sizeof(data));
            if (size <= 0)
                break;
            buffer.Append(data, size_t(size));
        }
        return got;
    }

    void Messages()
    {
        toyboxlink::Message message;
        std::string line = toyboxlink::Format({ "put", { "1", "/tmp/My Figures/Spyro.sky" } });
        Check(line == "put\t1\t/tmp/My Figures/Spyro.sky\n", "put is written as tab-separated fields");
        Check(toyboxlink::Parse(line.substr(0, line.size() - 1), message) && message.verb == "put" &&
                  message.fields.size() == 2 && message.fields[1] == "/tmp/My Figures/Spyro.sky",
            "put is read back, spaces kept");
        Check(toyboxlink::Parse("swap\r", message) && message.verb == "swap" && message.fields.empty(),
            "a CR before the end is dropped");
        Check(toyboxlink::Parse("seat\t0\tvirtual\t\t\t1", message) && message.fields.size() == 5 &&
                  message.fields[2].empty(),
            "empty fields are kept");
        Check(!toyboxlink::Parse("", message), "an empty line is no message");
        Check(toyboxlink::Format({ "put", { "1", "a\tb" } }).empty(), "a field with a tab is not sent");
        Check(toyboxlink::Format({ "put", { "1", "a\nb" } }).empty(), "a field with a line break is not sent");
        Check(toyboxlink::Format({ "", {} }).empty(), "no verb, no line");
        Check(toyboxlink::Format({ "put", { std::string(toyboxlink::MaxLine, 'x') } }).empty(), "too long, not sent");

        // Seats both ways.
        toyboxlink::Seat seats[3];
        seats[1].kind = toyboxlink::Seat::Kind::Virtual;
        seats[1].path = "/c/Spyro.sky";
        seats[1].taken = true;
        seats[2].kind = toyboxlink::Seat::Kind::Real;
        seats[2].type = 30;
        seats[2].variant = 0x1801;
        seats[2].known = true;
        for (int i = 0; i < 3; i++)
        {
            toyboxlink::Message written = toyboxlink::SeatMessage(i, seats[i]), read;
            int index = -1;
            toyboxlink::Seat back;
            std::string text = toyboxlink::Format(written);
            Check(!text.empty() && toyboxlink::Parse(text.substr(0, text.size() - 1), read) &&
                      toyboxlink::ParseSeat(read, index, back) && index == i && back == seats[i],
                "a seat line is read back as written");
        }
        int index = 0;
        toyboxlink::Seat seat;
        toyboxlink::Parse("seat\t0\tvirtual\t\t\t1", message);
        Check(!toyboxlink::ParseSeat(message, index, seat), "a virtual seat needs its path");
        toyboxlink::Parse("seat\t0\treal\t70000\t0\t1", message);
        Check(!toyboxlink::ParseSeat(message, index, seat), "a toy type above 65535 is refused");
        toyboxlink::Parse("seat\t-1\tempty", message);
        Check(!toyboxlink::ParseSeat(message, index, seat), "a negative seat is refused");
        toyboxlink::Parse("seat\t0\tlater\tthing", message);
        Check(!toyboxlink::ParseSeat(message, index, seat), "an unknown kind is refused");

        // Lines across reads, and a line that never ends.
        toyboxlink::LineBuffer buffer;
        buffer.Append("swa", 3);
        std::string got;
        Check(!buffer.Next(got), "half a line is not a line");
        buffer.Append("p\nclear\nof", 10);
        Check(buffer.Next(got) && got == "swap" && buffer.Next(got) && got == "clear" && !buffer.Next(got),
            "lines split across reads come out whole");
        toyboxlink::LineBuffer endless;
        std::string chunk(1000, 'x');
        for (int i = 0; i < 5; i++)
            endless.Append(chunk.data(), chunk.size());
        Check(endless.Overflow() && !endless.Next(got), "a line past MaxLine is an overflow");

        // The link file.
        toyboxlink::Endpoint endpoint{ 1, 50123, toyboxlink::NewToken(), 4242 }, back;
        Check(endpoint.token.size() == 32 && endpoint.token != toyboxlink::NewToken(),
            "tokens are 32 digits, new each time");
        Check(toyboxlink::ParseEndpoint(toyboxlink::FormatEndpoint(endpoint), back) && back.port == 50123 &&
                  back.token == endpoint.token && back.pid == 4242 && back.version == 1,
            "the link file is read back");
        Check(!toyboxlink::ParseEndpoint("skytoybox 1 0 abcdefabcdefabcdef 1", back), "port 0 is refused");
        Check(!toyboxlink::ParseEndpoint("skytoybox 1 80 short 1", back), "a short token is refused");
        Check(!toyboxlink::ParseEndpoint("other 1 80 abcdefabcdefabcdef 1", back), "another file is refused");
    }

    toyboxsocket::Handle Window(const toyboxlink::Endpoint& endpoint, int version, const std::string& token,
        toyboxlink::LineBuffer& buffer, std::vector<toyboxlink::Message>& answer)
    {
        toyboxsocket::Handle handle = toyboxsocket::Connect(endpoint.port, 1000);
        if (handle == toyboxsocket::Invalid)
            return handle;
        toyboxsocket::Send(handle, toyboxlink::Format({ "hello", { std::to_string(version), token, "test" } }));
        answer = Receive(handle, buffer, "");
        return handle;
    }

    void Server()
    {
        Check(toyboxsocket::Startup(), "sockets start");
        Check(toyboxsocket::Connect(1, 200) == toyboxsocket::Invalid, "nothing listens on port 1");

        std::filesystem::path folder =
            std::filesystem::temp_directory_path() / ("sky_toybox_link_test_" + toyboxlink::NewToken());
        toyboxremote::Start(folder.string(), "giants");
        std::filesystem::path file = folder / toyboxlink::FileName;
        std::stringstream text;
        {
            std::ifstream in(file); // closed at once: Windows cannot remove a file still open
            text << in.rdbuf();
        }
        toyboxlink::Endpoint endpoint;
        Check(toyboxlink::ParseEndpoint(text.str(), endpoint) && endpoint.version == toyboxlink::Version,
            "the game writes its link file");
#if !defined(_WIN32)
        auto perms = std::filesystem::status(file).permissions();
        Check((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
                  std::filesystem::perms::none,
            "only its owner reads the link file");
#endif

        // A window of another version is told the game's, and closed.
        toyboxlink::LineBuffer buffer;
        std::vector<toyboxlink::Message> answer;
        toyboxsocket::Handle old = Window(endpoint, toyboxlink::Version + 1, endpoint.token, buffer, answer);
        Check(old != toyboxsocket::Invalid && answer.size() == 1 && answer[0].verb == "refused" &&
                  answer[0].fields.size() == 2 && answer[0].fields[0] == "version" &&
                  answer[0].fields[1] == std::to_string(toyboxlink::Version),
            "another version is refused with the game's version");
        toyboxsocket::Close(old);
        // A wrong token.
        buffer = {};
        toyboxsocket::Handle stranger =
            Window(endpoint, toyboxlink::Version, "0123456789abcdef0123456789abcdef", buffer, answer);
        Check(answer.size() == 1 && answer[0].verb == "refused" && answer[0].fields.size() == 1 &&
                  answer[0].fields[0] == "token",
            "a wrong token is refused");
        toyboxsocket::Close(stranger);
        Check(!toyboxremote::Connected(), "no window connected yet");

        // The window.
        buffer = {};
        toyboxsocket::Handle window = Window(endpoint, toyboxlink::Version, endpoint.token, buffer, answer);
        Check(answer.size() == 1 && answer[0].verb == "hello" && answer[0].fields.size() == 2 &&
                  answer[0].fields[1] == "giants",
            "the right window is greeted with the game's id");
        for (int i = 0; i < 50 && !toyboxremote::Connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(toyboxremote::Connected() && toyboxremote::TakeHello() && !toyboxremote::TakeHello(),
            "the game knows a window came, once");

        // A second window is refused while the first is there.
        toyboxlink::LineBuffer secondBuffer;
        toyboxsocket::Handle second = Window(endpoint, toyboxlink::Version, endpoint.token, secondBuffer, answer);
        Check(answer.size() == 1 && answer[0].verb == "refused" && answer[0].fields.size() == 1 &&
                  answer[0].fields[0] == "busy",
            "one window at a time");
        toyboxsocket::Close(second);

        // Commands in order, unknown verbs left out.
        toyboxsocket::Send(window, "put\t0\t/c/Spyro.sky\nfly\taway\noff\t1\nswap\nclear\n");
        std::vector<std::string> verbs;
        toyboxlink::Message command;
        for (int i = 0; i < 100 && verbs.size() < 4; i++)
        {
            while (toyboxremote::Next(command))
                verbs.push_back(command.verb + (command.fields.empty() ? "" : " " + command.fields[0]));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(verbs == std::vector<std::string>{ "put 0", "off 1", "swap", "clear" }, "the commands arrive in order");

        // The seats go out.
        toyboxlink::Seat seat;
        seat.kind = toyboxlink::Seat::Kind::Virtual;
        seat.path = "/c/Spyro.sky";
        toyboxremote::Send({ toyboxlink::SeatMessage(0, seat), { "sync", { "1" } } });
        answer = Receive(window, buffer, "sync");
        int index = -1;
        toyboxlink::Seat back;
        Check(answer.size() == 2 && toyboxlink::ParseSeat(answer[0], index, back) && back == seat &&
                  answer[1].verb == "sync",
            "the window gets the seats");

        // The window goes, another comes.
        toyboxsocket::Close(window);
        for (int i = 0; i < 100 && toyboxremote::Connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(!toyboxremote::Connected(), "the game sees the window go");
        buffer = {};
        window = Window(endpoint, toyboxlink::Version, endpoint.token, buffer, answer);
        Check(answer.size() == 1 && answer[0].verb == "hello", "a new window can come after");

        // A line that never ends drops the window.
        std::string flood(toyboxlink::MaxLine + 10, 'x');
        toyboxsocket::Send(window, flood);
        for (int i = 0; i < 100 && toyboxremote::Connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Check(!toyboxremote::Connected(), "a window that floods is dropped");
        toyboxsocket::Close(window);

        // The game ends: bye, and the file goes.
        buffer = {};
        window = Window(endpoint, toyboxlink::Version, endpoint.token, buffer, answer);
        for (int i = 0; i < 100 && !toyboxremote::Connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        toyboxremote::Stop();
        answer = Receive(window, buffer, "bye");
        Check(!answer.empty() && answer.back().verb == "bye", "the window is told the game ends");
        Check(!std::filesystem::exists(file), "the link file is removed at the end");
        toyboxsocket::Close(window);
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
}

int main()
{
    Messages();
    Server();
    if (g_failures == 0)
        printf("ArkeyanToyboxLinkTest: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
