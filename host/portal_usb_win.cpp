// Windows: the USB portals' calls (host/portal_usb.h) answered by WinUSB, Windows' own generic USB driver and its
// winusb.dll, so a portal on WinUSB needs no libusb. The devices are the present USB device nodes (CfgMgr32) whose driver
// service is WinUSB; each is opened through one of its device interfaces (portal_usb::WinusbInterfaceClasses) with
// CreateFile and WinUsb_Initialize. Transfers are overlapped, so the reader and the writer run at once and each has its
// own timeout: at the timeout the pipe is aborted and what had arrived is returned with a timeout, as libusb does.
#include "host/portal_usb.h"
#include <windows.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <map>
#include <string>
#include <vector>
#ifndef CM_GETIDLIST_FILTER_PRESENT
#define CM_GETIDLIST_FILTER_PRESENT 0x00000100 // Windows 7 and later; older mingw-w64 headers lack it
#endif

namespace portal_usb
{
    static_assert(win32_error::FileNotFound == ERROR_FILE_NOT_FOUND);
    static_assert(win32_error::AccessDenied == ERROR_ACCESS_DENIED);
    static_assert(win32_error::GenFailure == ERROR_GEN_FAILURE);
    static_assert(win32_error::NotSupported == ERROR_NOT_SUPPORTED);
    static_assert(win32_error::SemTimeout == ERROR_SEM_TIMEOUT);
    static_assert(win32_error::OperationAborted == ERROR_OPERATION_ABORTED);
    static_assert(win32_error::DeviceNotConnected == ERROR_DEVICE_NOT_CONNECTED);
    static_assert(win32_error::NoSuchDevice == ERROR_NO_SUCH_DEVICE);
    static_assert(win32_error::BadCommand == ERROR_BAD_COMMAND);

    struct Device
    {
        std::string instance; // "USB\VID_1430&PID_1F17&MI_00\7&..."
        DeviceDescriptor descriptor{};
    };

    struct Claimed
    {
        WINUSB_INTERFACE_HANDLE handle = nullptr;
        bool associated = false; // from WinUsb_GetAssociatedInterface (freed on release)
        std::vector<uint8_t> endpoints; // its pipes' addresses
    };

    struct Handle
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        WINUSB_INTERFACE_HANDLE first = nullptr; // the device's (or the composite function's) first interface
        int firstNumber = 0;
        std::map<int, Claimed> interfaces;
    };
}

namespace
{
    using namespace portal_usb;

    void Log(const std::string& what, DWORD error)
    {
        fprintf(stderr, "[portal] usb (WinUSB): %s: error %lu\n", what.c_str(), error);
    }

    std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); } // ASCII ids

    int Init(Context** context)
    {
        *context = nullptr;
        return 0;
    }

    // The present USB nodes with WinUSB as their driver.
    ptrdiff_t GetDeviceList(Context*, Device*** list)
    {
        *list = nullptr;
        const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
        std::vector<char> ids;
        for (int attempt = 0; attempt < 3; attempt++) // a device plugged in between the two calls makes it longer
        {
            ULONG size = 0;
            if (CM_Get_Device_ID_List_SizeA(&size, "USB", flags) != CR_SUCCESS || size == 0)
                return ErrorIo;
            ids.assign(size, '\0');
            const CONFIGRET result = CM_Get_Device_ID_ListA("USB", ids.data(), size, flags);
            if (result == CR_SUCCESS)
                break;
            if (result != CR_BUFFER_SMALL || attempt == 2)
                return ErrorIo;
        }
        std::vector<Device*> found;
        for (const char* id = ids.data(); id < ids.data() + ids.size() && *id != '\0'; id += strlen(id) + 1)
        {
            WindowsUsbNode node;
            DEVINST instance = 0;
            if (!ParseWindowsUsbInstance(id, node) ||
                CM_Locate_DevNodeA(&instance, const_cast<char*>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
                continue;
            char service[128] = "";
            ULONG length = sizeof(service);
            if (CM_Get_DevNode_Registry_PropertyA(instance, CM_DRP_SERVICE, nullptr, service, &length, 0) !=
                    CR_SUCCESS ||
                _stricmp(service, "WinUSB") != 0)
                continue;
            char hardware[512] = ""; // REG_MULTI_SZ: the first id carries the revision
            length = sizeof(hardware) - 1;
            CM_Get_DevNode_Registry_PropertyA(instance, CM_DRP_HARDWAREID, nullptr, hardware, &length, 0);
            auto* device = new Device;
            device->instance = id;
            device->descriptor.bLength = 18;
            device->descriptor.bDescriptorType = 1;
            device->descriptor.idVendor = node.vendor;
            device->descriptor.idProduct = node.product;
            device->descriptor.bcdDevice = WindowsRevision(hardware);
            device->descriptor.bNumConfigurations = 1;
            found.push_back(device);
        }
        *list = new Device*[found.size() + 1];
        std::copy(found.begin(), found.end(), *list);
        (*list)[found.size()] = nullptr;
        return ptrdiff_t(found.size());
    }

    void FreeDeviceList(Device** list, int)
    {
        if (list == nullptr)
            return;
        for (Device** device = list; *device != nullptr; device++)
            delete *device;
        delete[] list;
    }

    int GetDeviceDescriptor(Device* device, DeviceDescriptor* descriptor)
    {
        *descriptor = device->descriptor;
        return 0;
    }

    // The registry's interface classes for the node (DeviceInterfaceGUIDs, DeviceInterfaceGUID), as one list.
    std::string RegistryInterfaceClasses(DEVINST instance)
    {
        HKEY key = nullptr;
        if (CM_Open_DevNode_Key(instance, KEY_READ, 0, RegDisposition_OpenExisting, &key, CM_REGISTRY_HARDWARE) !=
            CR_SUCCESS)
            return {};
        std::string list;
        for (const char* name : { "DeviceInterfaceGUIDs", "DeviceInterfaceGUID" })
        {
            char value[1024] = "";
            DWORD size = sizeof(value) - 2, type = 0;
            if (RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS &&
                (type == REG_MULTI_SZ || type == REG_SZ))
                list.append(value, std::min<size_t>(size, sizeof(value) - 2)).push_back('\0');
        }
        RegCloseKey(key);
        return list;
    }

    // The node's device interface paths of one class.
    std::vector<std::wstring> InterfacePaths(const std::string& guidText, const std::string& instance)
    {
        GUID guid{};
        std::vector<std::wstring> paths;
        const std::wstring wideGuid = Wide(guidText);
        if (CLSIDFromString(wideGuid.c_str(), &guid) != S_OK)
            return paths;
        std::wstring id = Wide(instance);
        ULONG size = 0;
        if (CM_Get_Device_Interface_List_SizeW(&size, &guid, id.data(), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
                CR_SUCCESS ||
            size <= 1)
            return paths;
        std::vector<wchar_t> list(size, L'\0');
        if (CM_Get_Device_Interface_ListW(&guid, id.data(), list.data(), size, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
            CR_SUCCESS)
            return paths;
        for (const wchar_t* path = list.data(); path < list.data() + list.size() && *path != L'\0';
            path += wcslen(path) + 1)
            paths.emplace_back(path);
        return paths;
    }

    int Open(Device* device, Handle** out)
    {
        *out = nullptr;
        DEVINST instance = 0;
        if (CM_Locate_DevNodeA(&instance, const_cast<char*>(device->instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) !=
            CR_SUCCESS)
            return ErrorNoDevice;
        int failure = ErrorNotFound;
        for (const std::string& guid : WinusbInterfaceClasses(RegistryInterfaceClasses(instance)))
        {
            for (const std::wstring& path : InterfacePaths(guid, device->instance))
            {
                HANDLE file =
                    CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                {
                    const DWORD error = GetLastError();
                    Log("cannot open " + device->instance + " through " + guid, error);
                    failure = ErrorFromWin32(error);
                    continue;
                }
                WINUSB_INTERFACE_HANDLE first = nullptr;
                if (!WinUsb_Initialize(file, &first))
                {
                    const DWORD error = GetLastError();
                    Log("WinUsb_Initialize on " + device->instance + " through " + guid, error);
                    CloseHandle(file);
                    failure = ErrorFromWin32(error);
                    continue;
                }
                auto* handle = new Handle;
                handle->file = file;
                handle->first = first;
                USB_INTERFACE_DESCRIPTOR descriptor{};
                if (WinUsb_QueryInterfaceSettings(first, 0, &descriptor))
                    handle->firstNumber = descriptor.bInterfaceNumber;
                *out = handle;
                return 0;
            }
        }
        return failure;
    }

    int ReleaseInterface(Handle* handle, int number);

    void Close(Handle* handle)
    {
        if (handle == nullptr)
            return;
        while (!handle->interfaces.empty())
            ReleaseInterface(handle, handle->interfaces.begin()->first);
        if (handle->first != nullptr)
            WinUsb_Free(handle->first);
        if (handle->file != INVALID_HANDLE_VALUE)
            CloseHandle(handle->file);
        delete handle;
    }

    int SetAutoDetachKernelDriver(Handle*, int) { return 0; } // no kernel driver to detach: WinUSB is the driver

    // The first interface, or one associated with it (the next ones of the device or of the composite function).
    int ClaimInterface(Handle* handle, int number)
    {
        if (handle->interfaces.count(number) != 0)
            return 0;
        Claimed claimed;
        if (number == handle->firstNumber)
            claimed.handle = handle->first;
        else
        {
            for (UCHAR index = 0; index < 16 && claimed.handle == nullptr; index++)
            {
                WINUSB_INTERFACE_HANDLE other = nullptr;
                if (!WinUsb_GetAssociatedInterface(handle->first, index, &other))
                    break;
                USB_INTERFACE_DESCRIPTOR descriptor{};
                if (WinUsb_QueryInterfaceSettings(other, 0, &descriptor) && descriptor.bInterfaceNumber == number)
                {
                    claimed.handle = other;
                    claimed.associated = true;
                }
                else
                    WinUsb_Free(other);
            }
            if (claimed.handle == nullptr)
                return ErrorNotFound;
        }
        USB_INTERFACE_DESCRIPTOR descriptor{};
        WinUsb_QueryInterfaceSettings(claimed.handle, 0, &descriptor);
        for (UCHAR index = 0; index < descriptor.bNumEndpoints; index++)
        {
            WINUSB_PIPE_INFORMATION pipe{};
            if (!WinUsb_QueryPipe(claimed.handle, 0, index, &pipe))
                continue;
            claimed.endpoints.push_back(pipe.PipeId);
            if (pipe.PipeId & 0x80)
            {
                UCHAR on = TRUE; // a stalled IN pipe recovers by itself
                WinUsb_SetPipePolicy(claimed.handle, pipe.PipeId, AUTO_CLEAR_STALL, sizeof(on), &on);
            }
        }
        handle->interfaces[number] = claimed;
        return 0;
    }

    int ReleaseInterface(Handle* handle, int number)
    {
        auto claimed = handle->interfaces.find(number);
        if (claimed == handle->interfaces.end())
            return ErrorNotFound;
        for (uint8_t endpoint : claimed->second.endpoints)
            WinUsb_AbortPipe(claimed->second.handle, endpoint);
        if (claimed->second.associated)
            WinUsb_Free(claimed->second.handle);
        handle->interfaces.erase(claimed);
        return 0;
    }

    int ControlTransfer(Handle* handle, uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
        unsigned char* data, uint16_t length, unsigned int timeout)
    {
        ULONG milliseconds = timeout;
        WinUsb_SetPipePolicy(handle->first, 0, PIPE_TRANSFER_TIMEOUT, sizeof(milliseconds), &milliseconds);
        ULONG transferred = 0;
        if (requestType == 0x80 && request == 0x06) // GET_DESCRIPTOR: WinUSB's own call for it
        {
            if (!WinUsb_GetDescriptor(
                    handle->first, UCHAR(value >> 8), UCHAR(value & 0xFF), index, data, length, &transferred))
                return ErrorFromWin32(GetLastError());
            return int(transferred);
        }
        WINUSB_SETUP_PACKET setup{};
        setup.RequestType = requestType;
        setup.Request = request;
        setup.Value = value;
        setup.Index = index;
        setup.Length = length;
        if (!WinUsb_ControlTransfer(handle->first, setup, data, length, &transferred, nullptr))
            return ErrorFromWin32(GetLastError());
        return int(transferred);
    }

    // One overlapped transfer, waited for up to `timeout` (0: forever, as libusb); at the timeout the pipe is aborted
    // and what had arrived is kept.
    int InterruptTransfer(
        Handle* handle, unsigned char endpoint, unsigned char* data, int length, int* transferred, unsigned int timeout)
    {
        *transferred = 0;
        WINUSB_INTERFACE_HANDLE pipeOwner = nullptr;
        for (auto& [number, claimed] : handle->interfaces)
            if (std::find(claimed.endpoints.begin(), claimed.endpoints.end(), endpoint) != claimed.endpoints.end())
                pipeOwner = claimed.handle;
        if (pipeOwner == nullptr)
            return ErrorNotFound;

        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (overlapped.hEvent == nullptr)
            return ErrorIo;
        ULONG done = 0;
        BOOL started = (endpoint & 0x80)
                           ? WinUsb_ReadPipe(pipeOwner, endpoint, data, ULONG(length), &done, &overlapped)
                           : WinUsb_WritePipe(pipeOwner, endpoint, data, ULONG(length), &done, &overlapped);
        DWORD error = started ? ERROR_SUCCESS : GetLastError();
        bool timedOut = false;
        if (!started && error == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(overlapped.hEvent, timeout != 0 ? timeout : INFINITE) == WAIT_TIMEOUT)
            {
                timedOut = true;
                WinUsb_AbortPipe(pipeOwner, endpoint);
            }
            error = WinUsb_GetOverlappedResult(pipeOwner, &overlapped, &done, TRUE) ? ERROR_SUCCESS : GetLastError();
        }
        CloseHandle(overlapped.hEvent);
        *transferred = int(done);
        if (timedOut)
            return ErrorTimeout;
        return ErrorFromWin32(error);
    }

    const char* ErrorNameOf(int error) { return ErrorName(error); }
}

namespace portal_usb
{
    bool LoadWinusb(Api& usb)
    {
        usb.init = Init;
        usb.get_device_list = GetDeviceList;
        usb.free_device_list = FreeDeviceList;
        usb.get_device_descriptor = GetDeviceDescriptor;
        usb.open = Open;
        usb.close = Close;
        usb.set_auto_detach_kernel_driver = SetAutoDetachKernelDriver;
        usb.claim_interface = ClaimInterface;
        usb.release_interface = ReleaseInterface;
        usb.control_transfer = ControlTransfer;
        usb.interrupt_transfer = InterruptTransfer;
        usb.error_name = ErrorNameOf;
        usb.from = "WinUSB (Windows' own driver, nothing to install)";
        return usb.init(&usb.context) == 0;
    }
}
