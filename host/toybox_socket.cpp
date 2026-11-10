// Loopback sockets for the Toy Box link (toybox_socket.h). Shared word for word with the launcher.
#include "toybox_socket.h"
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketLength = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketLength = socklen_t;
#endif

namespace toyboxsocket
{
    namespace
    {
#if defined(_WIN32)
        SOCKET Raw(Handle handle) { return SOCKET(handle); }
        Handle Wrap(SOCKET raw) { return raw == INVALID_SOCKET ? Invalid : Handle(raw); }
        void CloseRaw(SOCKET raw) { closesocket(raw); }
#else
        int Raw(Handle handle) { return int(handle); }
        Handle Wrap(int raw) { return raw < 0 ? Invalid : Handle(raw); }
        void CloseRaw(int raw) { close(raw); }
#endif

        sockaddr_in Loopback(int port)
        {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(uint16_t(port));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            return address;
        }

        // A socket that is not inherited by the processes the game starts, never raises SIGPIPE, and gives up a send the
        // peer does not read for a second.
        void Prepare(Handle handle)
        {
#if defined(_WIN32)
            SetHandleInformation(reinterpret_cast<HANDLE>(Raw(handle)), HANDLE_FLAG_INHERIT, 0);
            DWORD timeout = 1000;
            setsockopt(Raw(handle), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
            fcntl(Raw(handle), F_SETFD, FD_CLOEXEC);
            timeval timeout{ 1, 0 };
            setsockopt(Raw(handle), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
            int noSignal = 1;
            setsockopt(Raw(handle), SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
#endif
            int on = 1;
            setsockopt(Raw(handle), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
        }
    }

    bool Startup()
    {
#if defined(_WIN32)
        static const bool started = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        return started;
#else
        return true;
#endif
    }

    Handle Listen(int& port)
    {
        port = 0;
        Handle listener = Wrap(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (listener == Invalid)
            return Invalid;
        Prepare(listener);
        sockaddr_in address = Loopback(0);
        SocketLength length = sizeof(address);
        if (bind(Raw(listener), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(Raw(listener), 4) != 0 ||
            getsockname(Raw(listener), reinterpret_cast<sockaddr*>(&address), &length) != 0)
        {
            CloseRaw(Raw(listener));
            return Invalid;
        }
        port = ntohs(address.sin_port);
        return listener;
    }

    Handle Accept(Handle listener)
    {
        sockaddr_in peer{};
        SocketLength length = sizeof(peer);
        Handle accepted = Wrap(accept(Raw(listener), reinterpret_cast<sockaddr*>(&peer), &length));
        if (accepted == Invalid)
            return Invalid;
        // Only this computer: the listener is bound to the loopback address, and a peer from elsewhere is refused anyway.
        if (peer.sin_family != AF_INET || (ntohl(peer.sin_addr.s_addr) >> 24) != 127)
        {
            CloseRaw(Raw(accepted));
            return Invalid;
        }
        Prepare(accepted);
        return accepted;
    }

    Handle Connect(int port, int timeoutMs)
    {
        if (port <= 0 || port > 65535)
            return Invalid;
        Handle handle = Wrap(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (handle == Invalid)
            return Invalid;
        Prepare(handle);
        sockaddr_in address = Loopback(port);
        // Non-blocking while connecting, so a stale port cannot hold the caller.
#if defined(_WIN32)
        u_long nonBlocking = 1;
        ioctlsocket(Raw(handle), FIONBIO, &nonBlocking);
#else
        int flags = fcntl(Raw(handle), F_GETFL, 0);
        fcntl(Raw(handle), F_SETFL, flags | O_NONBLOCK);
#endif
        bool connected = connect(Raw(handle), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (!connected)
        {
#if defined(_WIN32)
            bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
            WSAPOLLFD entry{ Raw(handle), POLLWRNORM, 0 };
            if (pending && WSAPoll(&entry, 1, timeoutMs) == 1 && (entry.revents & POLLWRNORM) != 0)
#else
            bool pending = errno == EINPROGRESS;
            pollfd entry{ Raw(handle), POLLOUT, 0 };
            if (pending && poll(&entry, 1, timeoutMs) == 1 && (entry.revents & POLLOUT) != 0)
#endif
            {
                int error = 0;
                SocketLength length = sizeof(error);
                connected =
                    getsockopt(Raw(handle), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 &&
                    error == 0;
            }
        }
        if (!connected)
        {
            CloseRaw(Raw(handle));
            return Invalid;
        }
#if defined(_WIN32)
        nonBlocking = 0;
        ioctlsocket(Raw(handle), FIONBIO, &nonBlocking);
#else
        fcntl(Raw(handle), F_SETFL, flags);
#endif
        return handle;
    }

    unsigned Wait(const Handle* handles, int count, int timeoutMs)
    {
#if defined(_WIN32)
        std::vector<WSAPOLLFD> entries;
#else
        std::vector<pollfd> entries;
#endif
        std::vector<int> owners;
        for (int i = 0; i < count && i < 32; i++)
        {
            if (handles[i] == Invalid)
                continue;
#if defined(_WIN32)
            entries.push_back({ Raw(handles[i]), POLLRDNORM, 0 });
#else
            entries.push_back({ Raw(handles[i]), POLLIN, 0 });
#endif
            owners.push_back(i);
        }
        if (entries.empty())
            return 0;
#if defined(_WIN32)
        int ready = WSAPoll(entries.data(), ULONG(entries.size()), timeoutMs);
#else
        int ready = poll(entries.data(), nfds_t(entries.size()), timeoutMs);
#endif
        unsigned mask = 0;
        for (size_t i = 0; ready > 0 && i < entries.size(); i++)
        {
            if (entries[i].revents != 0)
                mask |= 1u << owners[i];
        }
        return mask;
    }

    long Receive(Handle handle, char* buffer, size_t capacity)
    {
        long got = long(recv(Raw(handle), buffer, int(capacity), 0));
        return got < 0 ? -1 : got;
    }

    bool Send(Handle handle, std::string_view data)
    {
#if defined(MSG_NOSIGNAL)
        constexpr int Flags = MSG_NOSIGNAL;
#else
        constexpr int Flags = 0;
#endif
        while (!data.empty())
        {
            long sent = long(send(Raw(handle), data.data(), int(data.size()), Flags));
            if (sent <= 0)
                return false;
            data.remove_prefix(size_t(sent));
        }
        return true;
    }

    void Close(Handle handle)
    {
        if (handle != Invalid)
            CloseRaw(Raw(handle));
    }
}
