// The console's network stack, offline: no cable is plugged in. XNet and Winsock start, sockets can be created and
// configured, but there is no address (XNetGetTitleXnAddr reports XNET_GET_XNADDR_NONE) and no traffic. Every NetDll_
// export takes the caller type (XNCALLER_TITLE) first, except the WSA*Event calls. XHttp (the console's HTTP client,
// Swap Force) opens sessions, connections and requests, which are local, and fails each request where a real console
// with no cable fails it: name resolution. Signatures follow Xenia (BSD) and the XDK's WinHTTP-shaped XHttp API.
#include "kernel/objects.h"
#include "memory.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t SocketError = 0xFFFFFFFF; // SOCKET_ERROR, and INVALID_SOCKET as a handle
    constexpr uint32_t WsaEWouldBlock = 10035;
    constexpr uint32_t WsaENotSock = 10038;
    constexpr uint32_t WsaENetDown = 10050;
    constexpr uint32_t WsaENetUnreach = 10051;

    constexpr uint32_t FionBio = 0x8004667E;
    constexpr uint32_t XnAddrNone = 0x00000001; // XNET_GET_XNADDR_NONE
    constexpr uint32_t XnAddrSize = 36;
    constexpr uint32_t WsaDataSize = 400;

    // Win32 errors of the XHttp and WSA*Event calls.
    constexpr uint32_t ErrorInvalidHandle = 6;
    constexpr uint32_t ErrorWinHttpNameNotResolved = 12007;
    constexpr uint32_t ErrorWinHttpIncorrectHandleState = 12019;
    constexpr uint32_t ErrorWinHttpInvalidUrl = 12005;

    constexpr uint32_t KThreadLastError = 0x160; // KTHREAD's last Win32 error, read by XAPI's GetLastError
    constexpr uint32_t XnDnsSize = 0x28; // XNDNS: INT iStatus, UINT cina, IN_ADDR aina[8]
    constexpr uint32_t InvalidStatusCallback = 0xFFFFFFFF; // XHTTP_INVALID_STATUS_CALLBACK

    struct Socket : KernelObject
    {
        bool nonBlocking = false;
    };

    // An XHttp session, connection or request handle.
    struct HttpHandle : KernelObject
    {
        enum Kind { Session, Connection, Request } kind;
        uint32_t statusCallback = 0;

        explicit HttpHandle(Kind kind) : kind(kind) {}
    };

    thread_local uint32_t t_lastError;

    uint32_t Fail(uint32_t error)
    {
        t_lastError = error;
        return SocketError;
    }

    // The console keeps one last error per thread, in its KTHREAD: WSAGetLastError and GetLastError read the same value.
    void SetLastError(const PPCContext& ctx, uint32_t error)
    {
        t_lastError = error;
        if (uint32_t thread = CurrentKThread(ctx.r13.u32))
            g_memory.Write<uint32_t>(thread + KThreadLastError, error);
    }

    // Logs the first call of each import, with its first argument registers.
#define LOG_FIRST_CALL(name) \
    do \
    { \
        static std::atomic_flag reported; \
        if (!reported.test_and_set()) \
        { \
            fprintf(stderr, "[net] " name " (lr=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7=0x%08X)\n", \
                uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); \
        } \
    } while (0)

    std::shared_ptr<HttpHandle> HttpFromHandle(const PPCContext& ctx, uint32_t handle)
    {
        auto http = LookupHandleAs<HttpHandle>(handle);
        if (http == nullptr)
            SetLastError(ctx, ErrorInvalidHandle);
        return http;
    }

    // A new XHttp handle under `parent` (none for a session), or NULL with ERROR_INVALID_HANDLE.
    uint32_t OpenHttp(const PPCContext& ctx, uint32_t parent, HttpHandle::Kind parentKind, HttpHandle::Kind kind)
    {
        if (kind != HttpHandle::Session)
        {
            auto owner = HttpFromHandle(ctx, parent);
            if (owner == nullptr || owner->kind != parentKind)
            {
                SetLastError(ctx, ErrorInvalidHandle);
                return 0;
            }
        }
        return CreateHandle(std::make_shared<HttpHandle>(kind));
    }

    // A call that needs a response: no request was ever sent, so the handle is never in the right state.
    uint32_t HttpRequestCall(const PPCContext& ctx, uint32_t request)
    {
        if (HttpFromHandle(ctx, request) != nullptr)
            SetLastError(ctx, ErrorWinHttpIncorrectHandleState);
        return 0;
    }

    std::shared_ptr<Socket> SocketFromHandle(uint32_t handle)
    {
        auto socket = LookupHandleAs<Socket>(handle);
        if (socket == nullptr)
            t_lastError = WsaENotSock;
        return socket;
    }
}

PPC_FUNC(__imp__NetDll_XNetStartup)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_XNetCleanup)
{
    ctx.r3.u64 = 0;
}

// WSAStartup(caller, requested version, WSADATA*): Winsock 2.2.
PPC_FUNC(__imp__NetDll_WSAStartup)
{
    uint32_t data = ctx.r5.u32;
    if (data != 0)
    {
        memset(g_memory.Translate(data), 0, WsaDataSize);
        g_memory.Write<uint16_t>(data + 0, 0x0202); // wVersion
        g_memory.Write<uint16_t>(data + 2, 0x0202); // wHighVersion
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_WSACleanup)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_WSAGetLastError)
{
    ctx.r3.u64 = t_lastError;
}

// XNetGetTitleXnAddr(caller, XNADDR*): no cable, so no address.
PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    if (ctx.r4.u32 != 0)
        memset(g_memory.Translate(ctx.r4.u32), 0, XnAddrSize);
    ctx.r3.u64 = XnAddrNone;
}

// XNetRandom(caller, buffer, length): the console's cryptographic random bytes.
PPC_FUNC(__imp__NetDll_XNetRandom)
{
    static std::mutex mutex;
    static std::random_device device;
    auto* bytes = static_cast<uint8_t*>(g_memory.Translate(ctx.r4.u32));
    std::lock_guard lock(mutex);
    for (uint32_t i = 0; i < ctx.r5.u32; i++)
        bytes[i] = uint8_t(device());
    ctx.r3.u64 = 0;
}

// socket(caller, af, type, protocol)
PPC_FUNC(__imp__NetDll_socket)
{
    ctx.r3.u64 = CreateHandle(std::make_shared<Socket>());
}

PPC_FUNC(__imp__NetDll_closesocket)
{
    ctx.r3.u64 = CloseHandle(ctx.r4.u32) ? 0 : Fail(WsaENotSock);
}

// setsockopt(caller, socket, level, name, value, length)
PPC_FUNC(__imp__NetDll_setsockopt)
{
    ctx.r3.u64 = SocketFromHandle(ctx.r4.u32) != nullptr ? 0 : SocketError;
}

// ioctlsocket(caller, socket, command, argument*): FIONBIO switches non-blocking mode.
PPC_FUNC(__imp__NetDll_ioctlsocket)
{
    auto socket = SocketFromHandle(ctx.r4.u32);
    if (socket == nullptr)
    {
        ctx.r3.u64 = SocketError;
        return;
    }
    if (ctx.r5.u32 == FionBio)
        socket->nonBlocking = g_memory.Read<uint32_t>(ctx.r6.u32) != 0;
    ctx.r3.u64 = 0;
}

// bind(caller, socket, address, length)
PPC_FUNC(__imp__NetDll_bind)
{
    ctx.r3.u64 = SocketFromHandle(ctx.r4.u32) != nullptr ? 0 : SocketError;
}

PPC_FUNC(__imp__NetDll_connect)
{
    ctx.r3.u64 = SocketFromHandle(ctx.r4.u32) != nullptr ? Fail(WsaENetUnreach) : SocketError;
}

PPC_FUNC(__imp__NetDll_send)
{
    ctx.r3.u64 = SocketFromHandle(ctx.r4.u32) != nullptr ? Fail(WsaENetDown) : SocketError;
}

PPC_FUNC(__imp__NetDll_sendto)
{
    ctx.r3.u64 = SocketFromHandle(ctx.r4.u32) != nullptr ? Fail(WsaENetDown) : SocketError;
}

// Nothing ever arrives: a non-blocking socket would block, a blocking one would wait forever, so it reports the
// network as down instead.
PPC_FUNC(__imp__NetDll_recv)
{
    auto socket = SocketFromHandle(ctx.r4.u32);
    ctx.r3.u64 = socket == nullptr ? SocketError : Fail(socket->nonBlocking ? WsaEWouldBlock : WsaENetDown);
}

PPC_FUNC(__imp__NetDll_recvfrom)
{
    auto socket = SocketFromHandle(ctx.r4.u32);
    ctx.r3.u64 = socket == nullptr ? SocketError : Fail(socket->nonBlocking ? WsaEWouldBlock : WsaENetDown);
}

// listen(caller, socket, backlog)
PPC_FUNC(__imp__NetDll_listen)
{
    ctx.r3.u64 = SocketFromHandle(ctx.r4.u32) != nullptr ? 0 : SocketError;
}

// accept(caller, socket, address, length*): nothing ever connects, so there is never a client.
PPC_FUNC(__imp__NetDll_accept)
{
    auto socket = SocketFromHandle(ctx.r4.u32);
    if (socket != nullptr && ctx.r6.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r6.u32, 0);
    ctx.r3.u64 = socket == nullptr ? SocketError : Fail(socket->nonBlocking ? WsaEWouldBlock : WsaENetDown);
}

// getsockopt(caller, socket, level, name, value, length*): every option reads as zero.
PPC_FUNC(__imp__NetDll_getsockopt)
{
    if (SocketFromHandle(ctx.r4.u32) == nullptr)
    {
        ctx.r3.u64 = SocketError;
        return;
    }
    if (ctx.r8.u32 != 0 && ctx.r9.u32 != 0)
        memset(g_memory.Translate(ctx.r8.u32), 0, g_memory.Read<uint32_t>(ctx.r9.u32));
    ctx.r3.u64 = 0;
}

// inet_addr(caller, "a.b.c.d"): the address in network order (big-endian, so the guest sees a.b.c.d as one number),
// or INADDR_NONE.
PPC_FUNC(__imp__NetDll_inet_addr)
{
    unsigned a, b, c, d;
    char end;
    const char* text = ctx.r4.u32 != 0 ? static_cast<const char*>(g_memory.Translate(ctx.r4.u32)) : "";
    bool valid = sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &end) == 4 && a < 256 && b < 256 && c < 256 && d < 256;
    ctx.r3.u64 = valid ? (a << 24 | b << 16 | c << 8 | d) : 0xFFFFFFFF;
}

// XNetGetEthernetLinkStatus(caller): XNET_ETHERNET_LINK_* flags, none without a cable.
PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus)
{
    ctx.r3.u64 = 0;
}

// WSACreateEvent(): a manual-reset event, not signaled, as Xenia (BSD) answers. The WSA*Event calls take no caller.
PPC_FUNC(__imp__NetDll_WSACreateEvent)
{
    ctx.r3.u64 = CreateHandle(std::make_shared<Event>(true, false));
}

// WSACloseEvent(event): TRUE, or FALSE with ERROR_INVALID_HANDLE.
PPC_FUNC(__imp__NetDll_WSACloseEvent)
{
    bool closed = LookupHandleAs<Event>(ctx.r3.u32) != nullptr && CloseHandle(ctx.r3.u32);
    if (!closed)
        SetLastError(ctx, ErrorInvalidHandle);
    ctx.r3.u64 = closed ? 1 : 0;
}

// WSAResetEvent(event)
PPC_FUNC(__imp__NetDll_WSAResetEvent)
{
    auto event = LookupHandleAs<Event>(ctx.r3.u32);
    if (event == nullptr)
    {
        SetLastError(ctx, ErrorInvalidHandle);
        ctx.r3.u64 = 0;
        return;
    }
    {
        std::lock_guard lock(g_kernelLock);
        event->signaled = false;
        event->SyncHeader();
    }
    ctx.r3.u64 = 1;
}

// XNetXnAddrToMachineId(caller, XNADDR*, machine id out): there is no network, so no machine to name.
PPC_FUNC(__imp__NetDll_XNetXnAddrToMachineId)
{
    if (ctx.r5.u32 != 0)
        g_memory.Write<uint64_t>(ctx.r5.u32, 0);
    ctx.r3.u64 = WsaENetDown;
}

// XNetDnsLookup(caller, host name, event, XNDNS** out): like Xenia (BSD), the lookup completes at once, failed (the
// status is WSAENETDOWN, not WSAEINPROGRESS), with no address; the event is signaled. XNetDnsRelease frees it.
PPC_FUNC(__imp__NetDll_XNetDnsLookup)
{
    uint32_t event = ctx.r5.u32;
    uint32_t out = ctx.r6.u32;
    LOG_FIRST_CALL("XNetDnsLookup");
    if (out == 0)
    {
        ctx.r3.u64 = WsaENetDown;
        return;
    }
    uint32_t dns = g_virtualHeap4K.Alloc(XnDnsSize);
    if (dns != 0)
        g_memory.Write<uint32_t>(dns, WsaENetDown);
    g_memory.Write<uint32_t>(out, dns);
    if (auto signal = event != 0 ? LookupHandleAs<Event>(event) : nullptr)
    {
        std::lock_guard lock(g_kernelLock);
        signal->signaled = true;
        signal->SyncHeader();
        signal->WakeSleepers();
    }
    ctx.r3.u64 = dns != 0 ? 0 : WsaENetDown;
}

// XNetDnsRelease(caller, XNDNS*)
PPC_FUNC(__imp__NetDll_XNetDnsRelease)
{
    if (ctx.r4.u32 != 0)
        g_virtualHeap4K.Free(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// XHttpStartup(caller, reserved, reserved pointer) and XHttpShutdown(caller): the client itself is always there.
PPC_FUNC(__imp__NetDll_XHttpStartup)
{
    SetLastError(ctx, 0);
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__NetDll_XHttpShutdown)
{
    ctx.r3.u64 = 1;
}

// XHttpOpen(caller, user agent, access type, proxy name, proxy bypass, flags): a session handle.
PPC_FUNC(__imp__NetDll_XHttpOpen)
{
    LOG_FIRST_CALL("XHttpOpen");
    ctx.r3.u64 = OpenHttp(ctx, 0, HttpHandle::Session, HttpHandle::Session);
}

// XHttpConnect(caller, session, server name, port, flags): a connection handle (nothing is sent yet).
PPC_FUNC(__imp__NetDll_XHttpConnect)
{
    ctx.r3.u64 = OpenHttp(ctx, ctx.r4.u32, HttpHandle::Session, HttpHandle::Connection);
}

// XHttpOpenRequest(caller, connection, verb, object name, version, referrer, accept types, flags): a request handle.
PPC_FUNC(__imp__NetDll_XHttpOpenRequest)
{
    ctx.r3.u64 = OpenHttp(ctx, ctx.r4.u32, HttpHandle::Connection, HttpHandle::Request);
}

// XHttpSendRequest(caller, request, headers, headers length, optional data, its length, total length, context): with
// no cable the server name never resolves. It fails at once (FALSE, ERROR_WINHTTP_NAME_NOT_RESOLVED) even for an
// asynchronous session, which WinHTTP allows; no status callback is ever made, so nothing waits for one.
PPC_FUNC(__imp__NetDll_XHttpSendRequest)
{
    LOG_FIRST_CALL("XHttpSendRequest");
    if (HttpFromHandle(ctx, ctx.r4.u32) != nullptr)
        SetLastError(ctx, ErrorWinHttpNameNotResolved);
    ctx.r3.u64 = 0;
}

// XHttpReceiveResponse(caller, request, reserved)
PPC_FUNC(__imp__NetDll_XHttpReceiveResponse)
{
    ctx.r3.u64 = HttpRequestCall(ctx, ctx.r4.u32);
}

// XHttpQueryHeaders(caller, request, info level, name, buffer, buffer length*, index*)
PPC_FUNC(__imp__NetDll_XHttpQueryHeaders)
{
    if (ctx.r8.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r8.u32, 0);
    ctx.r3.u64 = HttpRequestCall(ctx, ctx.r4.u32);
}

// XHttpReadData(caller, request, buffer, bytes to read, bytes read*)
PPC_FUNC(__imp__NetDll_XHttpReadData)
{
    if (ctx.r7.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r7.u32, 0);
    ctx.r3.u64 = HttpRequestCall(ctx, ctx.r4.u32);
}

// XHttpWriteData(caller, request, buffer, bytes to write, bytes written*)
PPC_FUNC(__imp__NetDll_XHttpWriteData)
{
    if (ctx.r7.u32 != 0)
        g_memory.Write<uint32_t>(ctx.r7.u32, 0);
    ctx.r3.u64 = HttpRequestCall(ctx, ctx.r4.u32);
}

// XHttpCloseHandle(caller, handle)
PPC_FUNC(__imp__NetDll_XHttpCloseHandle)
{
    bool closed = HttpFromHandle(ctx, ctx.r4.u32) != nullptr && CloseHandle(ctx.r4.u32);
    ctx.r3.u64 = closed ? 1 : 0;
}

// XHttpSetStatusCallback(caller, handle, callback, notification flags, reserved): the previous callback. It is kept
// but never called.
PPC_FUNC(__imp__NetDll_XHttpSetStatusCallback)
{
    auto http = HttpFromHandle(ctx, ctx.r4.u32);
    if (http == nullptr)
    {
        ctx.r3.u64 = InvalidStatusCallback;
        return;
    }
    uint32_t previous = http->statusCallback;
    http->statusCallback = ctx.r5.u32;
    ctx.r3.u64 = previous;
}

// XHttpDoWork(caller, session, maximum time): there is never any pending work.
PPC_FUNC(__imp__NetDll_XHttpDoWork)
{
    ctx.r3.u64 = HttpFromHandle(ctx, ctx.r4.u32) != nullptr ? 1 : 0;
}

// XHttpCrackUrl(caller, url, url length, flags, URL_COMPONENTS*): not parsed. It fails with ERROR_WINHTTP_INVALID_URL,
// which stops the title before it builds a request that could only fail later.
PPC_FUNC(__imp__NetDll_XHttpCrackUrl)
{
    LOG_FIRST_CALL("XHttpCrackUrl");
    SetLastError(ctx, ErrorWinHttpInvalidUrl);
    ctx.r3.u64 = 0;
}
