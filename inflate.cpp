// The game's zlib inflate (1.2.x, statically linked) replaced by the host's. Levels and frontend screens are
// zlib streams (mission.str and friends) that the loader inflates 16 KB at a time on the game thread; translated,
// the game's inflate manages ~30 MB/s and was most of every load (level_027: ~2.7 s of ~3.4 s).
//
// The game only uses inflateInit_ (-> inflateInit2_), inflate and inflateEnd, so the stream state can live on the
// host: each guest z_stream gets a host one, keyed by its address; the guest's `state` field just marks it as open.
#include "game_hooks.h"
#include "memory.h"
#include <memory>
#include <mutex>
#include <ppc_config.h>
#include <ppc_context.h>
#include <unordered_map>

#ifdef SSA_INFLATE_MINIZ
// No system zlib (Windows): miniz's inflate, public domain, from the tree. Only the declarations here; CMake compiles
// miniz.c itself. Its stream is zlib's, field for field, so the hooks below are the same for both.
#define MINIZ_HEADER_FILE_ONLY
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.c>
namespace
{
    using z_stream = mz_stream;
    using Bytef = unsigned char;
    constexpr int Z_OK = MZ_OK, Z_STREAM_ERROR = MZ_STREAM_ERROR, Z_VERSION_ERROR = MZ_VERSION_ERROR;
    constexpr Bytef* Z_NULL = nullptr;
    int inflateInit2(z_stream* stream, int windowBits) { return mz_inflateInit2(stream, windowBits); }
    int inflate(z_stream* stream, int flush) { return mz_inflate(stream, flush); }
    int inflateEnd(z_stream* stream) { return mz_inflateEnd(stream); }
}
#else
#include <zlib.h>
#endif

namespace
{
    // Guest z_stream (32-bit, big-endian).
    namespace Stream
    {
        constexpr uint32_t NextIn = 0x00;
        constexpr uint32_t AvailIn = 0x04;
        constexpr uint32_t TotalIn = 0x08;
        constexpr uint32_t NextOut = 0x0C;
        constexpr uint32_t AvailOut = 0x10;
        constexpr uint32_t TotalOut = 0x14;
        constexpr uint32_t Message = 0x18;
        constexpr uint32_t State = 0x1C;
        constexpr uint32_t DataType = 0x2C;
        constexpr uint32_t Adler = 0x30;
        constexpr uint32_t Size = 0x38;
    }

    std::mutex g_streamsMutex;
    std::unordered_map<uint32_t, std::unique_ptr<z_stream>> g_streams; // by guest z_stream address

    z_stream* HostStream(uint32_t stream)
    {
        std::lock_guard lock(g_streamsMutex);
        auto it = g_streams.find(stream);
        return it != g_streams.end() ? it->second.get() : nullptr;
    }

    void CloseStream(uint32_t stream)
    {
        std::lock_guard lock(g_streamsMutex);
        auto it = g_streams.find(stream);
        if (it == g_streams.end())
            return;
        inflateEnd(it->second.get());
        g_streams.erase(it);
    }
}

// inflateInit2_(strm, windowBits, version, stream_size); inflateInit_ (0x825F23B8) branches here with windowBits 15.
#ifdef GAME_HOOK_inflateInit2_
PPC_FUNC(GAME_HOOK(inflateInit2_))
{
    uint32_t stream = ctx.r3.u32;
    int32_t windowBits = ctx.r4.s32;
    uint32_t version = ctx.r5.u32;
    if (version == 0 || g_memory.Read<uint8_t>(version) != '1' || ctx.r6.u32 != Stream::Size)
    {
        ctx.r3.s64 = Z_VERSION_ERROR;
        return;
    }
    if (stream == 0)
    {
        ctx.r3.s64 = Z_STREAM_ERROR;
        return;
    }

    CloseStream(stream); // initialised again without inflateEnd
    auto host = std::make_unique<z_stream>();
    int result = inflateInit2(host.get(), windowBits);
    if (result == Z_OK)
    {
        std::lock_guard lock(g_streamsMutex);
        g_streams[stream] = std::move(host);
    }
    g_memory.Write<uint32_t>(stream + Stream::TotalIn, 0);
    g_memory.Write<uint32_t>(stream + Stream::TotalOut, 0);
    g_memory.Write<uint32_t>(stream + Stream::Message, 0);
    g_memory.Write<uint32_t>(stream + Stream::State, result == Z_OK ? stream : 0);
    g_memory.Write<uint32_t>(stream + Stream::Adler, 1);
    ctx.r3.s64 = result;
}
#endif

// inflate(strm, flush)
#ifdef GAME_HOOK_inflate
PPC_FUNC(GAME_HOOK(inflate))
{
    uint32_t stream = ctx.r3.u32;
    z_stream* host = stream != 0 ? HostStream(stream) : nullptr;
    if (host == nullptr)
    {
        ctx.r3.s64 = Z_STREAM_ERROR;
        return;
    }

    uint32_t nextIn = g_memory.Read<uint32_t>(stream + Stream::NextIn);
    uint32_t availIn = g_memory.Read<uint32_t>(stream + Stream::AvailIn);
    uint32_t nextOut = g_memory.Read<uint32_t>(stream + Stream::NextOut);
    uint32_t availOut = g_memory.Read<uint32_t>(stream + Stream::AvailOut);
    host->next_in = nextIn != 0 ? static_cast<Bytef*>(g_memory.Translate(nextIn)) : Z_NULL;
    host->avail_in = availIn;
    host->next_out = nextOut != 0 ? static_cast<Bytef*>(g_memory.Translate(nextOut)) : Z_NULL;
    host->avail_out = availOut;

    int result = inflate(host, ctx.r4.s32);

    uint32_t consumed = availIn - host->avail_in;
    uint32_t produced = availOut - host->avail_out;
    g_memory.Write<uint32_t>(stream + Stream::NextIn, nextIn + consumed);
    g_memory.Write<uint32_t>(stream + Stream::AvailIn, host->avail_in);
    g_memory.Write<uint32_t>(stream + Stream::TotalIn, g_memory.Read<uint32_t>(stream + Stream::TotalIn) + consumed);
    g_memory.Write<uint32_t>(stream + Stream::NextOut, nextOut + produced);
    g_memory.Write<uint32_t>(stream + Stream::AvailOut, host->avail_out);
    g_memory.Write<uint32_t>(stream + Stream::TotalOut, g_memory.Read<uint32_t>(stream + Stream::TotalOut) + produced);
    g_memory.Write<uint32_t>(stream + Stream::DataType, uint32_t(host->data_type));
    g_memory.Write<uint32_t>(stream + Stream::Adler, uint32_t(host->adler));
    ctx.r3.s64 = result;
}
#endif

// inflateEnd(strm)
#ifdef GAME_HOOK_inflateEnd
PPC_FUNC(GAME_HOOK(inflateEnd))
{
    uint32_t stream = ctx.r3.u32;
    if (stream == 0 || HostStream(stream) == nullptr)
    {
        ctx.r3.s64 = Z_STREAM_ERROR;
        return;
    }
    CloseStream(stream);
    g_memory.Write<uint32_t>(stream + Stream::State, 0);
    ctx.r3.s64 = Z_OK;
}
#endif
