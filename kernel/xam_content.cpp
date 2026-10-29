// XAM storage devices and content packages. The console has one hard drive, backed by a host folder (SKY_SAVE_DIR,
// or SDL's per-user preference folder). A content package (save game...) is a subdirectory, <type>\<file name>, that
// XamContentCreateEx mounts under the title's drive name ("lux:" for this game) for kernel/files.cpp to read and write.
#include "kernel/files.h"
#include "kernel/overlapped.h"
#include "kernel/xam_enumerator.h"
#include "memory.h"
#include "host/collection/save_backup.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ppc_config.h>
#include <ppc_context.h>

namespace fs = std::filesystem;

namespace
{
    constexpr uint32_t ErrorSuccess = 0;
    constexpr uint32_t ErrorFileNotFound = 2;
    constexpr uint32_t ErrorPathNotFound = 3;
    constexpr uint32_t ErrorAccessDenied = 5;
    constexpr uint32_t ErrorInvalidParameter = 87;
    constexpr uint32_t ErrorInvalidName = 123;
    constexpr uint32_t ErrorAlreadyExists = 183;
    constexpr uint32_t ErrorDeviceNotConnected = 1167;
    constexpr uint32_t ErrorNotFound = 1168;
    constexpr uint32_t ErrorCancelled = 1223;
    constexpr uint32_t ErrorNoSuchUser = 0x525;

    constexpr uint32_t HardDriveId = 1;
    constexpr uint32_t DeviceTypeHardDrive = 1;

    // XCONTENT_DATA: device id, content type, WCHAR display name[128], char file name[42], 2 bytes of padding.
    constexpr uint32_t ContentDataType = 0x04;
    constexpr uint32_t ContentDataFileName = 0x108;
    constexpr uint32_t ContentFileNameLength = 42;
    constexpr uint32_t ContentDataDisplayName = 0x08;
    constexpr uint32_t ContentDisplayNameLength = 128;
    constexpr uint32_t ContentDataSize = 0x134;

    // XContentCreate's creation flags (low nibble) are CreateFile's dispositions.
    constexpr uint32_t CreateNew = 1;
    constexpr uint32_t CreateAlways = 2;
    constexpr uint32_t OpenExisting = 3;
    constexpr uint32_t OpenAlways = 4;
    constexpr uint32_t TruncateExisting = 5;

    constexpr uint32_t ContentCreated = 1;
    constexpr uint32_t ContentOpened = 2;

    const fs::path& SaveRoot()
    {
        static const fs::path root = savebackup::Root(SSA_GAME_ID);
        return root;
    }

    // The drive is there while its folder exists or can be created.
    bool HardDrivePresent()
    {
        std::error_code error;
        const fs::path& root = SaveRoot();
        return !root.empty() && (fs::is_directory(root, error) || fs::create_directories(root, error));
    }

    bool IsHardDrive(uint32_t deviceId) { return deviceId == HardDriveId && HardDrivePresent(); }

    std::string GuestCString(uint32_t address, size_t maxLength)
    {
        const char* text = static_cast<const char*>(g_memory.Translate(address));
        return std::string(text, strnlen(text, maxLength));
    }

    uint32_t CreateContent(const std::string& root, uint32_t contentData, uint32_t creation, uint32_t& disposition)
    {
        if (!IsHardDrive(g_memory.Read<uint32_t>(contentData)))
            return ErrorDeviceNotConnected;

        fs::path name = HostFileName(GuestCString(contentData + ContentDataFileName, ContentFileNameLength));
        if (name.empty())
            return ErrorInvalidName;

        char type[9];
        snprintf(type, sizeof(type), "%08X", g_memory.Read<uint32_t>(contentData + ContentDataType));
        fs::path directory = SaveRoot() / type / name;

        // Reserve the drive name first so that a failure leaves the existing content untouched (the console's error
        // for a drive name in use is unverified).
        if (!MountContentRoot(root, directory))
            return ErrorAlreadyExists;

        std::error_code error;
        bool exists = fs::is_directory(directory, error);
        uint32_t result = ErrorSuccess;
        switch (creation)
        {
        case CreateNew: result = exists ? ErrorAlreadyExists : ErrorSuccess; break;
        case OpenExisting: result = exists ? ErrorSuccess : ErrorPathNotFound; break;
        case OpenAlways: break;
        case TruncateExisting:
        case CreateAlways:
            // Both replace an existing package with an empty one.
            if (!exists)
            {
                result = creation == TruncateExisting ? ErrorPathNotFound : ErrorSuccess;
                break;
            }
            // The old package goes to a backup first; if that fails, it stays.
            if (savebackup::Backup backup = savebackup::BackupFolder(SaveRoot(), directory); !backup.ok)
            {
                fprintf(stderr, "[saves] backup failed: %s\n", backup.error.c_str());
                UnmountContentRoot(root);
                return ErrorAccessDenied;
            }
            else if (backup.copied)
                fprintf(stderr, "[saves] backup %s\n", backup.stamp.c_str());
            fs::remove_all(directory, error);
            result = error ? ErrorAccessDenied : ErrorSuccess;
            exists = false;
            break;
        default: result = ErrorInvalidParameter; break;
        }

        if (result == ErrorSuccess && !exists && !fs::create_directories(directory, error))
            result = ErrorAccessDenied;

        if (result != ErrorSuccess)
        {
            UnmountContentRoot(root);
            return result;
        }
        disposition = exists ? ContentOpened : ContentCreated;
        return ErrorSuccess;
    }
}

// XamShowDeviceSelectorUI(user index, content type, content flags, bytes requested, device id out, overlapped).
// Like the console when a single device can hold the content (and XCONTENTFLAG_FORCE_SHOW_UI is not set), this picks
// the hard drive without showing the selector. With no drive, the player can only back out of the "no storage device"
// dialog, which cancels.
PPC_FUNC(__imp__XamShowDeviceSelectorUI)
{
    uint32_t deviceIdPointer = ctx.r7.u32;
    uint32_t overlapped = ctx.r8.u32;

    bool present = HardDrivePresent();
    g_memory.Write<uint32_t>(deviceIdPointer, present ? HardDriveId : 0);
    fprintf(stderr, "[content] device selector (user %u, type 0x%X, flags 0x%X, %llu bytes) -> device %u\n", ctx.r3.u32,
        ctx.r4.u32, ctx.r5.u32, (unsigned long long)ctx.r6.u64, present ? HardDriveId : 0);

    ctx.r3.u64 = FinishXamCall(overlapped, present ? ErrorSuccess : ErrorCancelled, 0, ctx.r13.u32);
}

// XamContentGetDeviceState(device id, overlapped)
PPC_FUNC(__imp__XamContentGetDeviceState)
{
    uint32_t result = IsHardDrive(ctx.r3.u32) ? ErrorSuccess : ErrorDeviceNotConnected;
    ctx.r3.u64 = FinishXamCall(ctx.r4.u32, result, 0, ctx.r13.u32);
}

// XamContentGetDeviceData(device id, XDEVICE_DATA*): device id, device type, total bytes, free bytes,
// WCHAR friendly name[28].
PPC_FUNC(__imp__XamContentGetDeviceData)
{
    uint32_t data = ctx.r4.u32;
    if (!IsHardDrive(ctx.r3.u32))
    {
        ctx.r3.u64 = ErrorDeviceNotConnected;
        return;
    }

    std::error_code error;
    fs::space_info space = fs::space(SaveRoot(), error);
    if (error)
        space = {};

    memset(g_memory.Translate(data), 0, 0x50);
    g_memory.Write<uint32_t>(data + 0x00, HardDriveId);
    g_memory.Write<uint32_t>(data + 0x04, DeviceTypeHardDrive);
    g_memory.Write<uint64_t>(data + 0x08, space.capacity);
    g_memory.Write<uint64_t>(data + 0x10, space.available);
    const char name[] = "Hard Drive";
    for (size_t i = 0; i < sizeof(name) - 1; i++)
        g_memory.Write<uint16_t>(data + 0x18 + uint32_t(i) * 2, uint16_t(name[i]));
    ctx.r3.u64 = ErrorSuccess;
}

// XamContentCreateEx(user index, root name, XCONTENT_DATA*, flags, disposition out, license mask out, cache size,
// content size, overlapped). Save games belong to the one signed-in profile, so the user index plays no part.
PPC_FUNC(__imp__XamContentCreateEx)
{
    std::string root = GuestCString(ctx.r4.u32, 64);
    uint32_t contentData = ctx.r5.u32;
    uint32_t flags = ctx.r6.u32;
    uint32_t dispositionPointer = ctx.r7.u32;
    uint32_t licenseMaskPointer = ctx.r8.u32;
    uint32_t overlapped = g_memory.Read<uint32_t>(ctx.r1.u32 + 0x54); // ninth argument, on the stack

    uint32_t disposition = 0;
    uint32_t result = CreateContent(root, contentData, flags & 0xF, disposition);
    fprintf(stderr, "[content] create %s: \"%s\" type 0x%X flags 0x%X -> %u (disposition %u)\n", root.c_str(),
        GuestCString(contentData + ContentDataFileName, ContentFileNameLength).c_str(),
        g_memory.Read<uint32_t>(contentData + ContentDataType), flags, result, disposition);

    if (result == ErrorSuccess)
    {
        if (dispositionPointer != 0)
            g_memory.Write<uint32_t>(dispositionPointer, disposition);
        if (licenseMaskPointer != 0)
            g_memory.Write<uint32_t>(licenseMaskPointer, 0);
    }
    ctx.r3.u64 = FinishXamCall(overlapped, result, 0, ctx.r13.u32);
}

// XamContentClose(root name, overlapped). A root that is not open gives ERROR_FILE_NOT_FOUND, as XAM (and Xenia) do:
// Giants closes its save root after a failed OPEN_EXISTING and reads ERROR_NOT_FOUND as "save content is damaged".
PPC_FUNC(__imp__XamContentClose)
{
    std::string root = GuestCString(ctx.r3.u32, 64);
    uint32_t result = UnmountContentRoot(root) ? ErrorSuccess : ErrorFileNotFound;
    fprintf(stderr, "[content] close %s -> %u\n", root.c_str(), result);
    ctx.r3.u64 = FinishXamCall(ctx.r4.u32, result, 0, ctx.r13.u32);
}

// XamContentDelete(user index, XCONTENT_DATA*, overlapped): removes the package's folder from the hard drive.
PPC_FUNC(__imp__XamContentDelete)
{
    uint32_t contentData = ctx.r4.u32;
    uint32_t result = ErrorSuccess;
    if (!IsHardDrive(g_memory.Read<uint32_t>(contentData)))
    {
        result = ErrorDeviceNotConnected;
    }
    else
    {
        fs::path name = HostFileName(GuestCString(contentData + ContentDataFileName, ContentFileNameLength));
        char type[9];
        snprintf(type, sizeof(type), "%08X", g_memory.Read<uint32_t>(contentData + ContentDataType));
        std::error_code error;
        if (name.empty())
            result = ErrorInvalidName;
        else if (!fs::is_directory(SaveRoot() / type / name, error))
            result = ErrorPathNotFound;
        else if (fs::remove_all(SaveRoot() / type / name, error); error)
            result = ErrorAccessDenied;
        fprintf(stderr, "[content] delete \"%s\" type %s -> %u\n", name.string().c_str(), type, result);
    }
    ctx.r3.u64 = FinishXamCall(ctx.r5.u32, result, 0, ctx.r13.u32);
}

// XamContentGetCreator(user index, XCONTENT_DATA*, is creator out, creator XUID out, overlapped): the one local profile
// made every save on the drive; a package that is not there gives ERROR_PATH_NOT_FOUND (as Xenia answers). Giants asks
// before opening its save: "yes" for a missing save made it open it, fail, and call the save damaged.
PPC_FUNC(__imp__XamContentGetCreator)
{
    constexpr uint64_t LocalXuid = 0xE000000000000001ull; // kernel/xam_user.cpp
    uint32_t contentData = ctx.r4.u32;
    uint32_t result = ErrorSuccess;
    fs::path name = HostFileName(GuestCString(contentData + ContentDataFileName, ContentFileNameLength));
    char type[9];
    snprintf(type, sizeof(type), "%08X", g_memory.Read<uint32_t>(contentData + ContentDataType));
    std::error_code error;
    if (!IsHardDrive(g_memory.Read<uint32_t>(contentData)))
        result = ErrorDeviceNotConnected;
    else if (name.empty() || !fs::is_directory(SaveRoot() / type / name, error))
        result = ErrorPathNotFound;
    else
    {
        if (ctx.r5.u32 != 0)
            g_memory.Write<uint32_t>(ctx.r5.u32, 1);
        if (ctx.r6.u32 != 0)
            g_memory.Write<uint64_t>(ctx.r6.u32, LocalXuid);
    }
    fprintf(stderr, "[content] creator of \"%s\" type %s -> %u\n", name.string().c_str(), type, result);
    ctx.r3.u64 = FinishXamCall(ctx.r7.u32, result, 0, ctx.r13.u32);
}

// XamContentCreateEnumerator(user index, device id, content type, content flags, items per enumerate, buffer size out,
// handle out), signature as in Xenia (BSD). The enumerator lists the packages of that type on the hard drive (device 0
// means any device), one XCONTENT_DATA each, sorted by name; the display name is the file name, as the host keeps no
// other. Swap Force enumerates its saved games (type 1) this way, 4 at a time, before it opens one.
PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    uint32_t userIndex = ctx.r3.u32;
    uint32_t deviceId = ctx.r4.u32;
    uint32_t contentType = ctx.r5.u32;
    uint32_t itemsPerEnumerate = ctx.r7.u32;
    uint32_t bufferSizePointer = ctx.r8.u32;
    uint32_t handlePointer = ctx.r9.u32;

    if (userIndex != 0 && userIndex < 4) // one profile is signed in; 0xFE/0xFF mean no or any user
    {
        ctx.r3.u64 = ErrorNoSuchUser;
        return;
    }

    std::vector<std::string> names;
    char type[9];
    snprintf(type, sizeof(type), "%08X", contentType);
    if ((deviceId == 0 || deviceId == HardDriveId) && HardDrivePresent())
    {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(SaveRoot() / type, error))
        {
            std::string name = entry.path().filename().string();
            if (entry.is_directory(error) && !name.empty() && name.size() < ContentFileNameLength)
                names.push_back(name);
        }
        std::sort(names.begin(), names.end());
    }

    auto enumerator = std::make_shared<XamEnumerator>();
    enumerator->itemSize = ContentDataSize;
    enumerator->itemsPerEnumerate = itemsPerEnumerate;
    enumerator->items.assign(names.size() * ContentDataSize, 0);
    for (size_t i = 0; i < names.size(); i++)
    {
        uint8_t* item = enumerator->items.data() + i * ContentDataSize;
        auto put32 = [](uint8_t* at, uint32_t value) {
            at[0] = uint8_t(value >> 24);
            at[1] = uint8_t(value >> 16);
            at[2] = uint8_t(value >> 8);
            at[3] = uint8_t(value);
        };
        put32(item, HardDriveId);
        put32(item + ContentDataType, contentType);
        for (size_t c = 0; c < names[i].size() && c < ContentDisplayNameLength - 1; c++)
            item[ContentDataDisplayName + c * 2 + 1] = uint8_t(names[i][c]); // big-endian UTF-16
        memcpy(item + ContentDataFileName, names[i].data(), names[i].size());
    }
    fprintf(
        stderr, "[content] enumerate type 0x%X on device %u: %zu package(s)\n", contentType, deviceId, names.size());

    if (bufferSizePointer != 0)
        g_memory.Write<uint32_t>(bufferSizePointer, itemsPerEnumerate * ContentDataSize);
    g_memory.Write<uint32_t>(handlePointer, CreateHandle(enumerator));
    ctx.r3.u64 = ErrorSuccess;
}
