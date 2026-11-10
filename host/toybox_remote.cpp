// The game's end of the Toy Box link (toybox_remote.h).
#include "host/toybox_remote.h"
#include "host/env.h"
#include "host/toybox_socket.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#define SKY_GETPID _getpid
#else
#include <unistd.h>
#define SKY_GETPID getpid
#endif

namespace toyboxremote
{
    namespace
    {
        std::atomic<bool> g_started{ false }, g_connected{ false }, g_hello{ false }, g_stop{ false };
        std::mutex g_mutex; // the queues and the client handle
        std::deque<toyboxlink::Message> g_commands;
        std::string g_outgoing;
        toyboxsocket::Handle g_client = toyboxsocket::Invalid;
        std::string g_file, g_token, g_game;

        void Drop(toyboxsocket::Handle& client, const char* why)
        {
            std::lock_guard lock(g_mutex);
            if (client == toyboxsocket::Invalid)
                return;
            toyboxsocket::Close(client);
            client = toyboxsocket::Invalid;
            g_client = toyboxsocket::Invalid;
            g_outgoing.clear();
            g_commands.clear();
            if (g_connected.exchange(false))
                fprintf(stderr, "[toybox-link] the window left (%s)\n", why);
        }

        void Refuse(toyboxsocket::Handle handle, const std::vector<std::string>& why)
        {
            toyboxsocket::Send(handle, toyboxlink::Format({ "refused", why }));
            toyboxsocket::Close(handle);
        }

        void Serve(toyboxsocket::Handle listener)
        {
            toyboxsocket::Handle client = toyboxsocket::Invalid;
            toyboxlink::LineBuffer buffer;
            bool greeted = false;
            bool unknownLogged = false;
            std::chrono::steady_clock::time_point acceptedAt{};
            char data[4096];
            while (!g_stop)
            {
                toyboxsocket::Handle handles[2] = { listener, client };
                unsigned ready = toyboxsocket::Wait(handles, 2, 25);
                if (g_stop)
                    break;
                // A connection that does not say hello within 5 s is not a Toy Box window: it does not keep the place.
                if (client != toyboxsocket::Invalid && !greeted &&
                    std::chrono::steady_clock::now() - acceptedAt > std::chrono::seconds(5))
                {
                    toyboxsocket::Close(client);
                    client = toyboxsocket::Invalid;
                }
                if ((ready & 1) != 0)
                {
                    toyboxsocket::Handle accepted = toyboxsocket::Accept(listener);
                    if (accepted != toyboxsocket::Invalid && client != toyboxsocket::Invalid)
                    {
                        Refuse(accepted, { "busy" }); // one window at a time
                        fprintf(stderr, "[toybox-link] a second window was refused: one is connected\n");
                    }
                    else if (accepted != toyboxsocket::Invalid)
                    {
                        client = accepted;
                        acceptedAt = std::chrono::steady_clock::now();
                        buffer = {};
                        greeted = false;
                    }
                }
                if ((ready & 2) != 0 && client != toyboxsocket::Invalid)
                {
                    long got = toyboxsocket::Receive(client, data, sizeof(data));
                    if (got <= 0)
                    {
                        Drop(client, got == 0 ? "closed" : "error");
                        continue;
                    }
                    buffer.Append(data, size_t(got));
                    std::string line;
                    while (client != toyboxsocket::Invalid && buffer.Next(line))
                    {
                        toyboxlink::Message message;
                        if (!toyboxlink::Parse(line, message))
                            continue;
                        if (!greeted)
                        {
                            long long version = message.fields.empty() ? 0 : atoll(message.fields[0].c_str());
                            if (message.verb != "hello" || message.fields.size() < 2)
                            {
                                Refuse(client, { "version", std::to_string(toyboxlink::Version) });
                                client = toyboxsocket::Invalid;
                                break;
                            }
                            if (version != toyboxlink::Version)
                            {
                                fprintf(stderr, "[toybox-link] refused a window of link version %lld (this game: %d)\n",
                                    version, toyboxlink::Version);
                                Refuse(client, { "version", std::to_string(toyboxlink::Version) });
                                client = toyboxsocket::Invalid;
                                break;
                            }
                            if (message.fields[1] != g_token)
                            {
                                fprintf(stderr, "[toybox-link] refused a connection with a wrong token\n");
                                Refuse(client, { "token" });
                                client = toyboxsocket::Invalid;
                                break;
                            }
                            greeted = true;
                            {
                                std::lock_guard lock(g_mutex);
                                g_client = client;
                                g_outgoing.clear();
                            }
                            toyboxsocket::Send(client,
                                toyboxlink::Format({ "hello", { std::to_string(toyboxlink::Version), g_game } }));
                            g_connected = true;
                            g_hello = true;
                            fprintf(stderr, "[toybox-link] a window connected (%s)\n",
                                message.fields.size() > 2 ? message.fields[2].c_str() : "?");
                            continue;
                        }
                        if (message.verb == "put" || message.verb == "off" || message.verb == "swap" ||
                            message.verb == "clear")
                        {
                            std::lock_guard lock(g_mutex);
                            g_commands.push_back(std::move(message));
                        }
                        else if (!unknownLogged)
                        {
                            unknownLogged = true;
                            fprintf(stderr, "[toybox-link] ignored the message \"%s\" (newer window?)\n",
                                message.verb.c_str());
                        }
                    }
                    if (buffer.Overflow())
                        Drop(client, "a line too long");
                }
                std::string out;
                {
                    std::lock_guard lock(g_mutex);
                    out.swap(g_outgoing);
                }
                if (!out.empty() && client != toyboxsocket::Invalid && greeted && !toyboxsocket::Send(client, out))
                    Drop(client, "it stopped reading");
            }
            toyboxsocket::Close(listener);
        }
    }

    void Start(const std::string& folder, const std::string& game)
    {
        if (const char* off = skyenv::Get("SKY_TOYBOX_LINK"); off != nullptr && std::string(off) == "0")
            return;
        if (g_started.exchange(true) || !toyboxsocket::Startup())
            return;
        int port = 0;
        toyboxsocket::Handle listener = toyboxsocket::Listen(port);
        if (listener == toyboxsocket::Invalid)
        {
            fprintf(stderr, "[toybox-link] cannot listen: no Toy Box window for this game\n");
            return;
        }
        g_token = toyboxlink::NewToken();
        g_game = game;
        std::filesystem::path file = std::filesystem::path(folder) / toyboxlink::FileName;
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << toyboxlink::FormatEndpoint({ toyboxlink::Version, port, g_token, uint32_t(SKY_GETPID()) });
            if (!out)
            {
                fprintf(
                    stderr, "[toybox-link] cannot write %s: no Toy Box window for this game\n", file.string().c_str());
                toyboxsocket::Close(listener);
                return;
            }
        }
        // Only this user reads the token.
        std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, error);
        g_file = file.string();
        std::thread(Serve, listener).detach();
        atexit(Stop);
        fprintf(stderr, "[toybox-link] listening on the loopback (link version %d)\n", toyboxlink::Version);
    }

    bool Connected() { return g_connected.load(std::memory_order_relaxed); }

    bool Next(toyboxlink::Message& command)
    {
        std::lock_guard lock(g_mutex);
        if (g_commands.empty())
            return false;
        command = std::move(g_commands.front());
        g_commands.pop_front();
        return true;
    }

    bool TakeHello() { return g_hello.exchange(false); }

    void Send(const std::vector<toyboxlink::Message>& messages)
    {
        if (!Connected())
            return;
        std::lock_guard lock(g_mutex);
        for (const toyboxlink::Message& message : messages)
            g_outgoing += toyboxlink::Format(message);
    }

    void Stop()
    {
        if (!g_started || g_stop.exchange(true))
            return;
        {
            std::lock_guard lock(g_mutex);
            if (g_client != toyboxsocket::Invalid)
                toyboxsocket::Send(g_client, g_outgoing + toyboxlink::Format({ "bye", {} }));
            g_outgoing.clear();
        }
        std::error_code error;
        if (!g_file.empty())
            std::filesystem::remove(g_file, error);
    }
}
