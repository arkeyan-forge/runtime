#include "kernel/files.h"
#include "game.h"
#include "gpu/draw.h"
#include "host/collection/save_backup.h"
#include "kernel/objects.h"
#include "kernel/overlapped.h"
#include "thread.h"
#include "vfs.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <ppc_config.h>
#include <ppc_context.h>
#include "../host/env.h"

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t StatusBufferOverflow = 0x80000005;
    constexpr uint32_t StatusNoMoreFiles = 0x80000006;
    constexpr uint32_t StatusNoSuchFile = 0xC000000F;
    constexpr uint32_t StatusInvalidParameter = 0xC000000D;
    constexpr uint32_t StatusInvalidDeviceRequest = 0xC0000010;
    constexpr uint32_t StatusEndOfFile = 0xC0000011;
    constexpr uint32_t StatusAccessDenied = 0xC0000022;
    constexpr uint32_t StatusObjectNameInvalid = 0xC0000033;
    constexpr uint32_t StatusObjectNameNotFound = 0xC0000034;
    constexpr uint32_t StatusObjectNameCollision = 0xC0000035;
    constexpr uint32_t StatusObjectPathNotFound = 0xC000003A;
    constexpr uint32_t StatusDiskFull = 0xC000007F;
    constexpr uint32_t StatusFileIsADirectory = 0xC00000BA;
    constexpr uint32_t StatusNotADirectory = 0xC0000103;

    // Create dispositions, and what IO_STATUS_BLOCK.Information reports after a create.
    constexpr uint32_t FileSupersede = 0;
    constexpr uint32_t FileOpen = 1;
    constexpr uint32_t FileCreate = 2;
    constexpr uint32_t FileOpenIf = 3;
    constexpr uint32_t FileOverwrite = 4;
    constexpr uint32_t FileOverwriteIf = 5;
    constexpr uint32_t FileSuperseded = 0;
    constexpr uint32_t FileOpened = 1;
    constexpr uint32_t FileCreated = 2;
    constexpr uint32_t FileOverwritten = 3;

    // Create options.
    constexpr uint32_t FileDirectoryFile = 0x1;
    constexpr uint32_t FileNonDirectoryFile = 0x40;
    constexpr uint32_t FileDeleteOnClose = 0x1000;

    constexpr uint32_t WriteAccess = 0x40000000 | 0x10000000 | 0x2 | 0x4; // GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA

    constexpr uint32_t FileStandardInformation = 5;
    constexpr uint32_t FileInternalInformation = 6;
    constexpr uint32_t FileRenameInformation = 10;
    constexpr uint32_t FileDispositionInformation = 13;
    constexpr uint32_t FilePositionInformation = 14;
    constexpr uint32_t FileAllocationInformation = 19;
    constexpr uint32_t FileEndOfFileInformation = 20;
    constexpr uint32_t FileNetworkOpenInformation = 34;
    constexpr uint32_t FileFsSizeInformation = 3;

    constexpr uint32_t FileAttributeReadOnly = 0x01;
    constexpr uint32_t FileAttributeDirectory = 0x10;
    constexpr uint32_t FileAttributeNormal = 0x80;

    std::mutex g_mountLock;
    std::unordered_map<std::string, fs::path> g_mounts; // lowercase drive name -> host directory

    std::string Lowercase(std::string text)
    {
        for (char& c : text)
            c = char(tolower(static_cast<unsigned char>(c)));
        return text;
    }

    struct File : KernelObject
    {
        std::string path; // disc-relative for disc files, the full guest path for content ones
        uint64_t position = 0;

        // A disc file or directory...
        const DiscFileSystem::Entry* entry = nullptr;

        // ...or, when entry is null, a content file or directory on the host.
        fs::path hostPath;
        fs::path tempPath; // while a save is written: the temporary its stream writes, committed over hostPath on close
        bool directory = false;
        bool writable = false;
        bool deleteOnClose = false;
        std::fstream stream; // closed for directories
        std::vector<std::pair<std::string, uint32_t>> listing; // NtQueryDirectoryFile: names left, then their attributes
        size_t listed = 0;
        bool listingStarted = false;
        std::mutex lock;     // guards stream and hostPath

        bool OnHost() const { return entry == nullptr; }

        // Puts the temporary in place of the save (save_backup.h): synced, the previous version kept as a backup, then
        // renamed. The old save stays byte for byte as it was when anything fails.
        void CommitWritesLocked()
        {
            if (tempPath.empty())
                return;
            stream.flush();
            stream.close();
            savebackup::Commit commit = savebackup::CommitFile(tempPath, hostPath, savebackup::Root(SSA_GAME_ID));
            if (!commit.ok)
            {
                fprintf(stderr, "[saves] %s: %s\n", path.c_str(), commit.error.c_str());
                std::error_code error;
                fs::remove(tempPath, error);
            }
            else if (commit.backedUp)
            {
                fprintf(stderr, "[saves] backup %s\n", commit.backupStamp.c_str());
            }
            tempPath.clear();
        }

        void CommitWrites()
        {
            std::lock_guard lockGuard(lock);
            CommitWritesLocked();
        }

        ~File() override
        {
            if (deleteOnClose)
            {
                stream.close();
                std::error_code error;
                if (!tempPath.empty())
                    fs::remove(tempPath, error);
                fs::remove(hostPath, error);
            }
            else
            {
                CommitWrites();
            }
        }
    };

    // Where a guest path lives: under a mounted content root, or on the disc.
    struct Location
    {
        bool content = false;
        bool valid = true; // false for content paths that leave their root
        fs::path hostPath;
    };

    Location Locate(std::string path)
    {
        for (char& c : path)
            c = c == '/' ? '\\' : c;
        if (path.starts_with("\\??\\"))
            path.erase(0, 4);

        Location location;
        size_t colon = path.find(':');
        if (colon == std::string::npos || path.find('\\') < colon)
            return location;
        {
            std::lock_guard lock(g_mountLock);
            auto mount = g_mounts.find(Lowercase(path.substr(0, colon)));
            if (mount == g_mounts.end())
                return location;
            location.content = true;
            location.hostPath = mount->second;
        }

        for (size_t start = colon + 1; start <= path.size();)
        {
            size_t end = std::min(path.find('\\', start), path.size());
            std::string_view component(path.data() + start, end - start);
            if (!component.empty() && component != ".")
            {
                fs::path name = HostFileName(component);
                if (name.empty())
                {
                    location.valid = false;
                    return location;
                }
                location.hostPath /= name;
            }
            start = end + 1;
        }
        return location;
    }

    // Giants' levels are `level\<name>.arc` and `.bld` pairs (no `levels\<name>\` folders).
    bool IsGiants() { return std::string_view(::game::Current().info.id) == "giants"; }
    bool IsSwapForce() { return std::string_view(::game::Current().info.id) == "swapforce"; }

    // SKY_LEVEL=<level> (test aid): the files of the game's first level (game::Adapter::firstLevel) are those of
    // <level>, so a new game starts there. [SSA] level_027 (levels\level_027\..., audio\levels\level_027.bnk,
    // audio\level_027.lxb, text\<lang>\level_027.lxb) -> e.g. level_001, challenge_level_005, pvp_level_002, level_hub.
    // [Giants] level\level_213_tutotial.arc and .bld -> e.g. level_203_the_machine, pvp_level_200_two_towers, title.
    // [SwapForce] no file redirect (a level's archive holds its map under the map's own path): the zone lookup is
    // redirected instead (engines/alchemy/zone.cpp).
    std::string RedirectLevel(std::string path)
    {
        static const std::string level = [] {
            const char* value = skyenv::Get("SKY_LEVEL");
            std::string name = value != nullptr ? value : "";
            for (char& c : name)
                c = char(tolower(c));
            return name;
        }();
        if (level.empty())
            return path;
        const auto [first, follows] = ::game::Current().firstLevel;
        if (first.empty())
            return path;
        const size_t nameAt = first.rfind('\\') + 1; // where the level's name starts in `first`
        for (size_t at = path.find(first); at != std::string::npos; at = path.find(first, at + 1))
        {
            char next = at + first.size() < path.size() ? path[at + first.size()] : '\0';
            if (follows.find(next) != std::string_view::npos)
                path.replace(at + nameAt, first.size() - nameAt, level);
        }
        return path;
    }

    // Every disc device the title can name (game:, d:, \Device\Cdrom0, symbolic links) is the same disc.
    std::string DiscPath(std::string path)
    {
        for (char& c : path)
            c = c == '/' ? '\\' : char(tolower(c));
        if (path.starts_with("\\??\\"))
            path.erase(0, 4);
        if (auto colon = path.find(':'); colon != std::string::npos)
        {
            path.erase(0, colon + 1);
        }
        else
        {
            for (std::string_view device : { "\\device\\cdrom0", "\\sysroot\\game" })
            {
                if (path.starts_with(device))
                {
                    path.erase(0, device.size());
                    break;
                }
            }
        }
        return RedirectLevel(std::move(path));
    }

    std::string GuestString(uint32_t ansiString)
    {
        uint16_t length = g_memory.Read<uint16_t>(ansiString);
        return std::string(static_cast<const char*>(g_memory.Translate(g_memory.Read<uint32_t>(ansiString + 4))), length);
    }

    // The full guest path named by an OBJECT_ATTRIBUTES (root directory handle + ANSI name).
    std::string ObjectPath(uint32_t objectAttributes)
    {
        std::string path = GuestString(g_memory.Read<uint32_t>(objectAttributes + 4));
        uint32_t root = g_memory.Read<uint32_t>(objectAttributes);
        if (auto directory = root != 0 ? LookupHandleAs<File>(root) : nullptr)
            path = directory->path + "\\" + path;
        return path;
    }

    void WriteIoStatus(uint32_t ioStatusBlock, uint32_t status, uint32_t information)
    {
        if (ioStatusBlock == 0)
            return;
        g_memory.Write<uint32_t>(ioStatusBlock, status);
        g_memory.Write<uint32_t>(ioStatusBlock + 4, information);
    }

    struct Attributes
    {
        uint64_t size = 0;
        uint64_t time = 0; // 100 ns intervals since 1601, 0 when unknown
        uint32_t attributes = 0;
        bool directory = false;
    };

    Attributes DiscAttributes(const DiscFileSystem::Entry& entry)
    {
        return { entry.size, 0, entry.directory ? FileAttributeDirectory : FileAttributeReadOnly, entry.directory };
    }

    std::optional<Attributes> HostAttributes(const fs::path& path)
    {
        std::error_code error;
        auto status = fs::status(path, error);
        if (!fs::exists(status))
            return std::nullopt;

        Attributes attributes;
        attributes.directory = fs::is_directory(status);
        attributes.attributes = attributes.directory ? FileAttributeDirectory : FileAttributeNormal;
        if (!attributes.directory)
            attributes.size = fs::file_size(path, error);
        if (error)
            attributes.size = 0;

        auto written = fs::last_write_time(path, error);
        if (!error)
        {
            auto age = std::chrono::duration_cast<std::chrono::nanoseconds>(fs::file_time_type::clock::now() - written);
            attributes.time = GuestSystemTime() - age.count() / 100;
        }
        return attributes;
    }

    std::optional<Attributes> FileAttributes(File& file)
    {
        if (!file.OnHost())
            return DiscAttributes(*file.entry);

        std::lock_guard lock(file.lock);
        if (file.stream.is_open())
            file.stream.flush();
        return HostAttributes(file.tempPath.empty() ? file.hostPath : file.tempPath);
    }

    // FILE_NETWORK_OPEN_INFORMATION: creation, access, write and change times, allocation size, end of file, attributes.
    void WriteNetworkOpenInformation(uint32_t info, const Attributes& attributes)
    {
        memset(g_memory.Translate(info), 0, 0x38);
        for (uint32_t time = 0; time < 4; time++)
            g_memory.Write<uint64_t>(info + time * 8, attributes.time);
        g_memory.Write<uint64_t>(info + 0x20, attributes.size);
        g_memory.Write<uint64_t>(info + 0x28, attributes.size);
        g_memory.Write<uint32_t>(info + 0x30, attributes.attributes);
    }

    uint32_t OpenDiscFile(const std::string& path, uint32_t desiredAccess, File& file)
    {
        file.entry = g_disc.Find(path);
        if (file.entry == nullptr)
            return StatusObjectNameNotFound;
        if ((desiredAccess & WriteAccess) && !file.entry->directory)
            return StatusAccessDenied;
        return StatusSuccess;
    }

    uint32_t OpenHostFile(const fs::path& path, uint32_t desiredAccess, uint32_t disposition, uint32_t options,
        File& file, uint32_t& information)
    {
        std::error_code error;
        auto status = fs::status(path, error);
        bool exists = fs::exists(status);
        if (!exists && !fs::is_directory(path.parent_path(), error))
            return StatusObjectPathNotFound;

        file.hostPath = path;

        if (fs::is_directory(status) || (!exists && (options & FileDirectoryFile)))
        {
            if (options & FileNonDirectoryFile)
                return StatusFileIsADirectory;
            if (exists && disposition == FileCreate)
                return StatusObjectNameCollision;
            if (!exists && disposition != FileCreate && disposition != FileOpenIf)
                return StatusObjectNameNotFound;
            if (!exists && !fs::create_directory(path, error))
                return StatusAccessDenied;
            file.directory = true;
            information = exists ? FileOpened : FileCreated;
            return StatusSuccess;
        }
        if (options & FileDirectoryFile)
            return StatusNotADirectory;

        bool truncate = false;
        switch (disposition)
        {
        case FileOpen:
            if (!exists)
                return StatusObjectNameNotFound;
            information = FileOpened;
            break;
        case FileCreate:
            if (exists)
                return StatusObjectNameCollision;
            information = FileCreated;
            break;
        case FileOpenIf:
            information = exists ? FileOpened : FileCreated;
            break;
        case FileOverwrite:
            if (!exists)
                return StatusObjectNameNotFound;
            truncate = true;
            information = FileOverwritten;
            break;
        case FileOverwriteIf:
        case FileSupersede:
            truncate = exists;
            information = !exists ? FileCreated : disposition == FileSupersede ? FileSuperseded : FileOverwritten;
            break;
        default:
            return StatusInvalidParameter;
        }

        file.writable = (desiredAccess & WriteAccess) != 0;
        fs::path openPath = path;
        if (file.writable)
        {
            // A save is written through a temporary in its own folder, synced and renamed over the old file when the
            // handle closes: a write that is interrupted leaves the old save untouched (save_backup.h).
            fs::path temp = savebackup::TempPath(path);
            if (truncate || !exists)
            {
                std::ofstream create(temp, std::ios::binary | std::ios::trunc);
                if (!create)
                    return StatusAccessDenied;
            }
            else if (!savebackup::CopyFileAtomic(path, temp))
            {
                return StatusAccessDenied;
            }
            file.tempPath = temp;
            openPath = temp;
        }
        else if (truncate || !exists)
        {
            std::ofstream create(path, std::ios::binary | std::ios::trunc);
            if (!create)
                return StatusAccessDenied;
        }

        file.stream.open(openPath, std::ios::binary | std::ios::in | (file.writable ? std::ios::out : std::ios::openmode()));
        if (!file.stream.is_open() && !file.tempPath.empty())
        {
            std::error_code error;
            fs::remove(file.tempPath, error);
        }
        return file.stream.is_open() ? StatusSuccess : StatusAccessDenied;
    }

    uint32_t OpenFile(uint32_t handlePointer, uint32_t objectAttributes, uint32_t ioStatusBlock, uint32_t desiredAccess,
        uint32_t disposition, uint32_t options)
    {
        std::string path = ObjectPath(objectAttributes);
        auto file = std::make_shared<File>();
        uint32_t information = FileOpened;
        uint32_t status;

        Location location = Locate(path);
        if (location.content)
        {
            file->path = path;
            status = location.valid
                ? OpenHostFile(location.hostPath, desiredAccess, disposition, options, *file, information)
                : StatusObjectNameInvalid;
        }
        else
        {
            file->path = DiscPath(path);
            status = OpenDiscFile(file->path, desiredAccess, *file);
        }

        fprintf(stderr, "[file] open %s -> 0x%08X\n", file->path.c_str(), status);
        WriteIoStatus(ioStatusBlock, status, status == StatusSuccess ? information : 0);
        if (status != StatusSuccess)
            return status;

        file->deleteOnClose = location.content && (options & FileDeleteOnClose);
        g_memory.Write<uint32_t>(handlePointer, CreateHandle(file));

        // A level or the frontend starts loading (Lux: levels\<name>\mission.str, the hub's mission_stage<n>.str; Giants:
        // level\<name>.arc; Swap Force: archives\common_ents_zone.pak, opened by every zone load, the menu background's
        // too): the loading screen waits here until the pipeline list is built, so the level's first frames build none of it.
        if (!location.content && (IsGiants() ? (file->path.starts_with("\\level\\") && file->path.ends_with(".arc"))
            : IsSwapForce() ? file->path == "\\archives\\common_ents_zone.pak"
            : (file->path.starts_with("\\levels\\") && file->path.ends_with(".str") && file->path.find("\\mission", 8) != std::string::npos)))
            gpu::WaitForPipelineList(file->path.c_str());
        return status;
    }

    // NtReadFile/NtWriteFile(handle, event, apc routine, apc context, io status, buffer, length, byte offset):
    // transfers complete synchronously. ReadFileEx-style callers get their completion routine as an APC on this thread
    // (XAPI tags the routine pointer's low bit); the others may wait on the completion event.
    void CompleteTransfer(PPCContext& ctx, uint32_t status, uint32_t transferred)
    {
        uint32_t event = ctx.r4.u32;
        uint32_t apcRoutine = ctx.r5.u32 & ~1u;
        uint32_t ioStatusBlock = ctx.r7.u32;

        WriteIoStatus(ioStatusBlock, status, transferred);
        if (apcRoutine != 0)
        {
            if (auto thread = CurrentGuestThread(ctx.r13.u32))
                QueueUserApc(*thread, { apcRoutine, ctx.r6.u32, ioStatusBlock, 0 });
        }
        SignalEventHandle(event);
        ctx.r3.u64 = status;
    }

    // A negative byte offset (FILE_USE_FILE_POINTER_POSITION) or none means the current position.
    uint64_t TransferPosition(const File& file, uint32_t byteOffset)
    {
        int64_t position = byteOffset != 0 ? g_memory.Read<int64_t>(byteOffset) : -1;
        return position < 0 ? file.position : uint64_t(position);
    }

    bool TraceFiles()
    {
        static const bool trace = skyenv::Get("SKY_TRACE_FILES") != nullptr;
        return trace;
    }
}

bool MountContentRoot(const std::string& name, const fs::path& directory)
{
    std::lock_guard lock(g_mountLock);
    return g_mounts.emplace(Lowercase(name), directory).second;
}

bool UnmountContentRoot(const std::string& name)
{
    std::lock_guard lock(g_mountLock);
    return g_mounts.erase(Lowercase(name)) != 0;
}

fs::path HostFileName(std::string_view name)
{
    if (name.empty() || name == "." || name == "..")
        return {};

    std::u8string result;
    for (char c : name)
    {
        bool forbidden = static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F
            || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
        result += forbidden ? u8'_' : char8_t(c);
    }
    return fs::path(result);
}

fs::path Utf8Path(std::string_view path)
{
    return fs::path(std::u8string(path.begin(), path.end()));
}

PPC_FUNC(__imp__NtCreateFile)
{
    uint32_t options = g_memory.Read<uint32_t>(ctx.r1.u32 + 0x54); // ninth argument, on the stack
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r4.u32, ctx.r10.u32, options);
}

PPC_FUNC(__imp__NtOpenFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r4.u32, FileOpen, ctx.r8.u32);
}

PPC_FUNC(__imp__NtReadFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    uint32_t buffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;

    if (file == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    uint64_t position = TransferPosition(*file, ctx.r10.u32);
    uint32_t read = 0;
    if (!file->OnHost())
    {
        read = g_disc.Read(*file->entry, position, g_memory.Translate(buffer), length);
    }
    else if (!file->directory)
    {
        std::lock_guard lock(file->lock);
        file->stream.clear();
        file->stream.seekg(std::streamoff(position));
        file->stream.read(static_cast<char*>(g_memory.Translate(buffer)), length);
        read = uint32_t(file->stream.gcount());
    }
    file->position = position + read;

    if (TraceFiles())
    {
        fprintf(stderr, "[file] read %s @%llu len %u -> %u (event 0x%X apc 0x%X)\n", file->path.c_str(),
            (unsigned long long)position, length, read, ctx.r4.u32, ctx.r5.u32);
    }

    uint32_t status = file->directory ? StatusInvalidDeviceRequest
        : read == 0 && length != 0 ? StatusEndOfFile : StatusSuccess;
    CompleteTransfer(ctx, status, read);
}

PPC_FUNC(__imp__NtWriteFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    uint32_t buffer = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;

    if (file == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    uint64_t position = TransferPosition(*file, ctx.r10.u32);
    uint32_t status = StatusSuccess;
    if (!file->OnHost() || !file->writable)
    {
        status = StatusAccessDenied;
    }
    else
    {
        std::lock_guard lock(file->lock);
        file->stream.clear();
        file->stream.seekp(std::streamoff(position));
        file->stream.write(static_cast<const char*>(g_memory.Translate(buffer)), length);
        if (!file->stream.flush())
            status = StatusDiskFull;
    }
    uint32_t written = status == StatusSuccess ? length : 0;
    file->position = position + written;

    if (TraceFiles())
    {
        fprintf(stderr, "[file] write %s @%llu len %u -> 0x%08X (event 0x%X apc 0x%X)\n", file->path.c_str(),
            (unsigned long long)position, length, status, ctx.r4.u32, ctx.r5.u32);
    }

    CompleteTransfer(ctx, status, written);
}

// NtWriteFileGather(handle, event, apc routine, apc context, io status, segments, length, byte offset): writes `length`
// bytes taken page by page (4 KB) from the zero-terminated array of 8-byte FILE_SEGMENT_ELEMENTs (a pointer each).
PPC_FUNC(__imp__NtWriteFileGather)
{
    constexpr uint32_t SegmentSize = 0x1000;
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    uint32_t segments = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;

    if (file == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    uint64_t position = TransferPosition(*file, ctx.r10.u32);
    uint32_t status = StatusSuccess;
    uint32_t written = 0;
    if (!file->OnHost() || !file->writable)
    {
        status = StatusAccessDenied;
    }
    else
    {
        std::lock_guard lock(file->lock);
        file->stream.clear();
        file->stream.seekp(std::streamoff(position));
        while (written < length)
        {
            uint32_t buffer = uint32_t(g_memory.Read<uint64_t>(segments));
            if (buffer == 0)
                break;
            uint32_t chunk = std::min(SegmentSize, length - written);
            file->stream.write(static_cast<const char*>(g_memory.Translate(buffer)), chunk);
            written += chunk;
            segments += 8;
        }
        if (!file->stream.flush())
        {
            status = StatusDiskFull;
            written = 0;
        }
    }
    file->position = position + written;

    if (TraceFiles())
    {
        fprintf(stderr, "[file] write gather %s @%llu len %u -> 0x%08X\n", file->path.c_str(),
            (unsigned long long)position, length, status);
    }

    CompleteTransfer(ctx, status, written);
}

PPC_FUNC(__imp__NtQueryInformationFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    uint32_t ioStatusBlock = ctx.r4.u32;
    uint32_t info = ctx.r5.u32;
    uint32_t infoClass = ctx.r7.u32;

    if (file == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    std::optional<Attributes> attributes = FileAttributes(*file);
    if (!attributes)
    {
        WriteIoStatus(ioStatusBlock, StatusObjectNameNotFound, 0);
        ctx.r3.u64 = StatusObjectNameNotFound;
        return;
    }

    uint32_t size;
    switch (infoClass)
    {
    case FileStandardInformation:
        memset(g_memory.Translate(info), 0, 0x18);
        g_memory.Write<uint64_t>(info + 0x00, attributes->size); // AllocationSize
        g_memory.Write<uint64_t>(info + 0x08, attributes->size); // EndOfFile
        g_memory.Write<uint32_t>(info + 0x10, 1);                // NumberOfLinks
        g_memory.Write<uint8_t>(info + 0x14, file->deleteOnClose);
        g_memory.Write<uint8_t>(info + 0x15, attributes->directory);
        size = 0x18;
        break;
    case FileInternalInformation:
        g_memory.Write<uint64_t>(info, file->OnHost() ? std::hash<std::string>()(file->path) : file->entry->offset);
        size = 8;
        break;
    case FilePositionInformation:
        g_memory.Write<uint64_t>(info, file->position);
        size = 8;
        break;
    case FileNetworkOpenInformation:
        WriteNetworkOpenInformation(info, *attributes);
        size = 0x38;
        break;
    default:
        fprintf(stderr, "[file] NtQueryInformationFile: unsupported class %u\n", infoClass);
        WriteIoStatus(ioStatusBlock, StatusInvalidParameter, 0);
        ctx.r3.u64 = StatusInvalidParameter;
        return;
    }

    WriteIoStatus(ioStatusBlock, StatusSuccess, size);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtSetInformationFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    uint32_t ioStatusBlock = ctx.r4.u32;
    uint32_t info = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    if (file == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    // Disc files are never writable, and only content files can be deleted or renamed.
    uint32_t status = StatusSuccess;
    std::error_code error;
    std::lock_guard lock(file->lock);
    switch (infoClass)
    {
    case FilePositionInformation:
        file->position = g_memory.Read<uint64_t>(info);
        break;
    case FileEndOfFileInformation:
        if (!file->writable)
        {
            status = StatusAccessDenied;
            break;
        }
        file->stream.flush();
        fs::resize_file(file->tempPath.empty() ? file->hostPath : file->tempPath, g_memory.Read<uint64_t>(info), error);
        status = error ? StatusDiskFull : StatusSuccess;
        break;
    case FileAllocationInformation:
        // The host file system allocates on its own.
        status = file->writable ? StatusSuccess : StatusAccessDenied;
        break;
    case FileDispositionInformation:
        if (!file->OnHost())
        {
            status = StatusAccessDenied;
            break;
        }
        file->deleteOnClose = g_memory.Read<uint8_t>(info) != 0;
        break;
    case FileRenameInformation:
    {
        // FILE_RENAME_INFORMATION: BOOLEAN ReplaceIfExists, HANDLE RootDirectory, ANSI_STRING FileName.
        std::string name = GuestString(info + 8);
        Location target = Locate(name);
        if (!file->OnHost())
        {
            status = StatusAccessDenied;
            break;
        }
        if (!target.content || !target.valid)
        {
            status = StatusObjectNameInvalid;
            break;
        }
        if (fs::exists(target.hostPath, error) && g_memory.Read<uint8_t>(info) == 0)
        {
            status = StatusObjectNameCollision;
            break;
        }
        file->CommitWritesLocked(); // the temporary is put in place first, then the save is renamed
        fs::rename(file->hostPath, target.hostPath, error);
        if (error)
        {
            status = StatusAccessDenied;
            break;
        }
        file->hostPath = target.hostPath;
        file->path = name;
        break;
    }
    default:
        fprintf(stderr, "[file] NtSetInformationFile: unsupported class %u\n", infoClass);
        status = StatusInvalidParameter;
        break;
    }

    WriteIoStatus(ioStatusBlock, status, status == StatusSuccess ? length : 0);
    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    std::string path = ObjectPath(ctx.r3.u32);
    std::optional<Attributes> attributes;

    Location location = Locate(path);
    if (location.content)
    {
        if (location.valid)
            attributes = HostAttributes(location.hostPath);
    }
    else if (const DiscFileSystem::Entry* entry = g_disc.Find(DiscPath(path)))
    {
        attributes = DiscAttributes(*entry);
    }

    if (!attributes)
    {
        ctx.r3.u64 = StatusObjectNameNotFound;
        return;
    }

    WriteNetworkOpenInformation(ctx.r4.u32, *attributes);
    ctx.r3.u64 = StatusSuccess;
}

// FILE_FS_SIZE_INFORMATION: total and available allocation units, sectors per unit, bytes per sector. The disc has no
// free space; content files report their host volume.
PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    uint32_t ioStatusBlock = ctx.r4.u32;
    uint32_t info = ctx.r5.u32;
    uint32_t infoClass = ctx.r7.u32;

    if (file == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }
    if (infoClass != FileFsSizeInformation)
    {
        fprintf(stderr, "[file] NtQueryVolumeInformationFile: unsupported class %u\n", infoClass);
        WriteIoStatus(ioStatusBlock, StatusInvalidParameter, 0);
        ctx.r3.u64 = StatusInvalidParameter;
        return;
    }

    constexpr uint32_t BytesPerSector = 512;
    constexpr uint32_t SectorsPerUnit = 32;
    fs::space_info space{};
    if (file->OnHost())
    {
        std::error_code error;
        space = fs::space(file->tempPath.empty() ? file->hostPath : file->tempPath, error);
        if (error)
            space = {};
    }

    g_memory.Write<uint64_t>(info + 0x00, space.capacity / (BytesPerSector * SectorsPerUnit));
    g_memory.Write<uint64_t>(info + 0x08, space.available / (BytesPerSector * SectorsPerUnit));
    g_memory.Write<uint32_t>(info + 0x10, SectorsPerUnit);
    g_memory.Write<uint32_t>(info + 0x14, BytesPerSector);
    WriteIoStatus(ioStatusBlock, StatusSuccess, 0x18);
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__NtFlushBuffersFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (file == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    uint32_t status = StatusSuccess;
    if (file->OnHost() && file->writable)
    {
        std::lock_guard lock(file->lock);
        if (!file->stream.flush() || (!file->tempPath.empty() && !savebackup::SyncFile(file->tempPath)))
            status = StatusDiskFull;
    }
    WriteIoStatus(ctx.r4.u32, status, 0);
    ctx.r3.u64 = status;
}

// Disc device names are all resolved by DiscPath and content roots are mounted by XamContentCreateEx, so links need
// no bookkeeping yet.
PPC_FUNC(__imp__ObCreateSymbolicLink)
{
    uint32_t link = ctx.r3.u32;
    uint32_t target = ctx.r4.u32;
    fprintf(stderr, "[file] symbolic link %s -> %s\n", GuestString(link).c_str(), GuestString(target).c_str());
    ctx.r3.u64 = StatusSuccess;
}

PPC_FUNC(__imp__ObDeleteSymbolicLink)
{
    ctx.r3.u64 = StatusSuccess;
}

namespace
{
    bool MatchesMask(const char* name, const char* mask)
    {
        if (*mask == '\0')
            return *name == '\0';
        if (*mask == '*')
            return MatchesMask(name, mask + 1) || (*name != '\0' && MatchesMask(name + 1, mask));
        return *name != '\0' && (*mask == '?' || tolower((unsigned char)*mask) == tolower((unsigned char)*name)) &&
            MatchesMask(name + 1, mask + 1);
    }
}

// NtQueryDirectoryFile(handle, event, apc routine, apc context, io status, information, length, file mask, restart scan
// on the stack): one X_FILE_DIRECTORY_INFORMATION per call, as Xenia answers it: next entry offset, file index, four
// times, end of file, allocation size, attributes, name length, name (not terminated). NO_SUCH_FILE when the
// directory has no match at all, then NO_MORE_FILES.
PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    uint32_t information = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    std::string mask = ctx.r10.u32 != 0 ? GuestString(ctx.r10.u32) : "";
    bool restart = g_memory.Read<uint32_t>(ctx.r1.u32 + 0x54) != 0;
    if (mask.empty())
        mask = "*";

    if (file == nullptr)
    {
        ctx.r3.u64 = StatusInvalidHandle;
        return;
    }

    std::lock_guard lock(file->lock);
    if (!file->listingStarted || restart)
    {
        file->listing.clear();
        file->listed = 0;
        file->listingStarted = true;
        if (!file->OnHost())
        {
            for (const DiscFileSystem::Entry* entry : g_disc.List(file->path))
                file->listing.emplace_back(entry->name, 0);
        }
        else
        {
            std::error_code error;
            for (const auto& item : fs::directory_iterator(file->hostPath, error))
                file->listing.emplace_back(item.path().filename().string(), 0);
            std::sort(file->listing.begin(), file->listing.end());
        }
        std::erase_if(file->listing, [&](const auto& item) { return !MatchesMask(item.first.c_str(), mask.c_str()); });
    }

    uint32_t status = StatusSuccess;
    uint32_t written = 0;
    if (file->listed >= file->listing.size())
    {
        status = file->listing.empty() && file->listed == 0 ? StatusNoSuchFile : StatusNoMoreFiles;
        file->listed = std::max<size_t>(file->listed, 1); // NO_SUCH_FILE once
    }
    else
    {
        const std::string& name = file->listing[file->listed].first;
        Attributes attributes;
        if (!file->OnHost())
        {
            std::string directory = DiscFileSystem::Normalize(file->path);
            std::string child = directory.empty() ? name : directory + "/" + name;
            if (const DiscFileSystem::Entry* entry = g_disc.Find(child))
                attributes = DiscAttributes(*entry);
        }
        else if (auto host = HostAttributes(file->hostPath / name))
        {
            attributes = *host;
        }

        if (0x40 + name.size() > length)
        {
            status = StatusBufferOverflow;
        }
        else
        {
            g_memory.Write<uint32_t>(information + 0x00, 0);
            g_memory.Write<uint32_t>(information + 0x04, uint32_t(file->listed));
            for (uint32_t time = 0x08; time <= 0x20; time += 8)
                g_memory.Write<uint64_t>(information + time, attributes.time);
            g_memory.Write<uint64_t>(information + 0x28, attributes.size);
            g_memory.Write<uint64_t>(information + 0x30, (attributes.size + 0x7FF) & ~uint64_t(0x7FF));
            g_memory.Write<uint32_t>(information + 0x38, attributes.attributes);
            g_memory.Write<uint32_t>(information + 0x3C, uint32_t(name.size()));
            memcpy(g_memory.Translate(information + 0x40), name.data(), name.size());
            written = uint32_t(0x40 + name.size());
            file->listed++;
        }
    }

    if (TraceFiles())
        fprintf(stderr, "[file] list %s (%s) -> 0x%08X\n", file->path.c_str(), mask.c_str(), status);
    CompleteTransfer(ctx, status, written);
}

// Volumes and the file system cache. Mounted content roots are folders on the host, so nothing needs flushing or
// detaching: dismounting succeeds. XAPI calls these when it unmounts a package or the utility drive.

// IoDismountVolume(device object)
PPC_FUNC(__imp__IoDismountVolume)
{
    ctx.r3.u64 = StatusSuccess;
}

// IoDismountVolumeByFileHandle(handle)
PPC_FUNC(__imp__IoDismountVolumeByFileHandle)
{
    ctx.r3.u64 = LookupHandleAs<File>(ctx.r3.u32) != nullptr ? StatusSuccess : StatusInvalidHandle;
}

// FscSetCacheElementCount(0, count): the size of the console's file system cache, in 4 KB elements. The host caches.
PPC_FUNC(__imp__FscSetCacheElementCount)
{
    ctx.r3.u64 = StatusSuccess;
}

// NtDeviceIoControlFile(handle, event, apc routine, apc context, io status, control code, input, input length, output
// and output length on the stack): no device here takes I/O controls. Swap Force's only codes are XAPI's
// XMountUtilityDrive disk queries (IOCTL_DISK_GET_DRIVE_GEOMETRY 0x70000, IOCTL_DISK_GET_PARTITION_INFO 0x74004) on
// the cache partition, a device path the runtime does not open (it resolves to a missing disc file).
PPC_FUNC(__imp__NtDeviceIoControlFile)
{
    static std::atomic_flag reported;
    if (!reported.test_and_set())
        fprintf(stderr, "[file] device I/O control 0x%08X on handle 0x%08X: not supported\n", ctx.r8.u32, ctx.r3.u32);
    WriteIoStatus(ctx.r7.u32, StatusInvalidDeviceRequest, 0);
    ctx.r3.u64 = StatusInvalidDeviceRequest;
}
