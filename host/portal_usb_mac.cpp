// macOS: the USB portals' calls (host/portal_usb.h) answered by IOKit's USB user client, so an Xbox 360 or Xbox One
// portal needs nothing installed. The device is opened for its configuration, the interface for its pipes.
// IOKit has no timeout on interrupt pipes (ReadPipeTO is for bulk pipes only): each claimed interface gets a thread
// that keeps a ReadPipe pending on its IN pipe and queues what arrives, and an IN transfer waits on that queue for
// its timeout. Closing aborts the pipes, which ends the pending ReadPipe.
#include "host/portal_usb.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBHostFamilyDefinitions.h>
#include <IOKit/usb/IOUSBLib.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace portal_usb
{
    static_assert(io_return::NoDevice == uint32_t(kIOReturnNoDevice));
    static_assert(io_return::NotPrivileged == uint32_t(kIOReturnNotPrivileged));
    static_assert(io_return::ExclusiveAccess == uint32_t(kIOReturnExclusiveAccess));
    static_assert(io_return::Unsupported == uint32_t(kIOReturnUnsupported));
    static_assert(io_return::Timeout == uint32_t(kIOReturnTimeout));
    static_assert(io_return::NotPermitted == uint32_t(kIOReturnNotPermitted));
    static_assert(io_return::Aborted == uint32_t(kIOReturnAborted));
    static_assert(io_return::NotResponding == uint32_t(kIOReturnNotResponding));
    static_assert(io_return::PipeStalled == uint32_t(kIOUSBPipeStalled));
    static_assert(io_return::TransactionTimeout == uint32_t(kIOUSBTransactionTimeout));
    static_assert(kUSBIn == 1);

    using DeviceInterface = IOUSBDeviceInterface182;
    using InterfaceInterface = IOUSBInterfaceInterface190;

    struct Device
    {
        io_service_t service = 0;
        DeviceDescriptor descriptor{};
    };

    // The IN pipe's reader: packets as they arrive (the newest 64), or the error that ended it.
    struct Reader
    {
        std::mutex mutex;
        std::condition_variable arrived;
        std::deque<std::vector<uint8_t>> packets;
        int error = 0;
        std::thread thread;
    };

    struct Claimed
    {
        InterfaceInterface** interface = nullptr;
        std::map<uint8_t, uint8_t> pipes; // endpoint address -> pipe reference
        std::map<uint8_t, uint16_t> packetSizes; // endpoint address -> wMaxPacketSize
        std::map<uint8_t, std::unique_ptr<Reader>> readers;
    };

    struct Handle
    {
        DeviceInterface** device = nullptr;
        bool opened = false; // USBDeviceOpen succeeded (needed to set a configuration)
        io_service_t service = 0;
        std::map<int, Claimed> interfaces;
    };
}

namespace
{
    using namespace portal_usb;
    using Handle = portal_usb::Handle; // not MacTypes' Handle

    void Log(const char* what, IOReturn result)
    {
        fprintf(stderr, "[portal] usb (IOKit): %s: 0x%08X\n", what, static_cast<unsigned>(result));
    }

    int Number(io_service_t service, CFStringRef key)
    {
        int value = -1;
        if (CFTypeRef number = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0))
        {
            if (CFGetTypeID(number) == CFNumberGetTypeID())
                CFNumberGetValue(static_cast<CFNumberRef>(number), kCFNumberIntType, &value);
            CFRelease(number);
        }
        return value;
    }

    // The user client of a device or interface service, through its CFPlugIn.
    template <typename T>
    T** Plugin(io_service_t service, CFUUIDRef type, CFUUIDRef interface, IOReturn& result)
    {
        IOCFPlugInInterface** plugin = nullptr;
        SInt32 score = 0;
        result = IOCreatePlugInInterfaceForService(service, type, kIOCFPlugInInterfaceID, &plugin, &score);
        if (result != kIOReturnSuccess || plugin == nullptr)
            return nullptr;
        T** object = nullptr;
        HRESULT query =
            (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(interface), reinterpret_cast<LPVOID*>(&object));
        IODestroyPlugInInterface(plugin);
        if (query != S_OK)
        {
            result = kIOReturnUnsupported;
            return nullptr;
        }
        return object;
    }

    int Init(Context** context)
    {
        *context = nullptr;
        return 0;
    }

    ptrdiff_t GetDeviceList(Context*, Device*** list)
    {
        *list = nullptr;
        io_iterator_t iterator = 0;
        if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching(kIOUSBHostDeviceClassName), &iterator) !=
            KERN_SUCCESS)
            return ErrorIo;
        std::vector<Device*> devices;
        while (io_service_t service = IOIteratorNext(iterator))
        {
            auto* device = new Device;
            device->service = service;
            device->descriptor.bLength = sizeof(DeviceDescriptor);
            device->descriptor.bDescriptorType = 1;
            device->descriptor.idVendor = uint16_t(Number(service, CFSTR(kUSBVendorID)));
            device->descriptor.idProduct = uint16_t(Number(service, CFSTR(kUSBProductID)));
            devices.push_back(device);
        }
        IOObjectRelease(iterator);
        *list = new Device*[devices.size() + 1];
        std::copy(devices.begin(), devices.end(), *list);
        (*list)[devices.size()] = nullptr;
        return ptrdiff_t(devices.size());
    }

    void FreeDeviceList(Device** list, int)
    {
        if (list == nullptr)
            return;
        for (Device** device = list; *device != nullptr; device++)
        {
            IOObjectRelease((*device)->service);
            delete *device;
        }
        delete[] list;
    }

    int GetDeviceDescriptor(Device* device, DeviceDescriptor* descriptor)
    {
        *descriptor = device->descriptor;
        return 0;
    }

    int Open(Device* device, Handle** handle)
    {
        IOReturn result;
        auto** interface =
            Plugin<DeviceInterface>(device->service, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID182, result);
        if (interface == nullptr)
            return ErrorFromIoReturn(result);
        auto* opened = new Handle;
        opened->device = interface;
        opened->service = device->service;
        IOObjectRetain(opened->service);
        // Another client holding the device (exclusive) leaves its configuration as it is: the interfaces may still open.
        result = (*interface)->USBDeviceOpen(interface);
        opened->opened = result == kIOReturnSuccess;
        if (!opened->opened && result != kIOReturnExclusiveAccess)
            Log("USBDeviceOpen", result);
        *handle = opened;
        return 0;
    }

    void StopReaders(Claimed& claimed)
    {
        for (auto& [endpoint, reader] : claimed.readers)
        {
            (*claimed.interface)->AbortPipe(claimed.interface, claimed.pipes[endpoint]);
            if (reader->thread.joinable())
                reader->thread.join();
        }
        claimed.readers.clear();
    }

    int ReleaseInterface(Handle* handle, int number)
    {
        auto found = handle->interfaces.find(number);
        if (found == handle->interfaces.end())
            return ErrorNotFound;
        Claimed& claimed = found->second;
        StopReaders(claimed);
        for (auto& [endpoint, pipe] : claimed.pipes)
            (*claimed.interface)->AbortPipe(claimed.interface, pipe);
        (*claimed.interface)->USBInterfaceClose(claimed.interface);
        (*claimed.interface)->Release(claimed.interface);
        handle->interfaces.erase(found);
        return 0;
    }

    void Close(Handle* handle)
    {
        while (!handle->interfaces.empty())
            ReleaseInterface(handle, handle->interfaces.begin()->first);
        if (handle->opened)
            (*handle->device)->USBDeviceClose(handle->device);
        (*handle->device)->Release(handle->device);
        IOObjectRelease(handle->service);
        delete handle;
    }

    int SetAutoDetachKernelDriver(Handle*, int) { return 0; } // no kernel driver to detach on macOS

    // An unconfigured device (a vendor-specific one no driver took) gets its first configuration.
    int Configure(Handle* handle)
    {
        UInt8 current = 0;
        if ((*handle->device)->GetConfiguration(handle->device, &current) == kIOReturnSuccess && current != 0)
            return 0;
        IOUSBConfigurationDescriptorPtr configuration = nullptr;
        IOReturn result = (*handle->device)->GetConfigurationDescriptorPtr(handle->device, 0, &configuration);
        if (result != kIOReturnSuccess || configuration == nullptr)
            return Log("GetConfigurationDescriptorPtr", result), ErrorFromIoReturn(result);
        if (!handle->opened)
            return ErrorBusy;
        result = (*handle->device)->SetConfiguration(handle->device, configuration->bConfigurationValue);
        if (result != kIOReturnSuccess)
            return Log("SetConfiguration", result), ErrorFromIoReturn(result);
        return 0;
    }

    int ClaimInterface(Handle* handle, int number)
    {
        if (handle->interfaces.count(number) != 0)
            return 0;
        if (int error = Configure(handle); error != 0)
            return error;
        IOUSBFindInterfaceRequest request{ kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
            kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
        io_iterator_t iterator = 0;
        IOReturn result = (*handle->device)->CreateInterfaceIterator(handle->device, &request, &iterator);
        if (result != kIOReturnSuccess)
            return Log("CreateInterfaceIterator", result), ErrorFromIoReturn(result);
        InterfaceInterface** interface = nullptr;
        result = kIOReturnNotFound;
        while (io_service_t service = IOIteratorNext(iterator))
        {
            if (interface == nullptr && Number(service, CFSTR(kUSBInterfaceNumber)) == number)
                interface = Plugin<InterfaceInterface>(
                    service, kIOUSBInterfaceUserClientTypeID, kIOUSBInterfaceInterfaceID190, result);
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
        if (interface == nullptr)
            return result == kIOReturnNotFound ? ErrorNotFound : ErrorFromIoReturn(result);
        result = (*interface)->USBInterfaceOpen(interface);
        if (result != kIOReturnSuccess)
        {
            (*interface)->Release(interface);
            return ErrorFromIoReturn(result); // busy: a driver or another program has it
        }
        Claimed& claimed = handle->interfaces[number];
        claimed.interface = interface;
        UInt8 endpoints = 0;
        (*interface)->GetNumEndpoints(interface, &endpoints);
        for (UInt8 pipe = 1; pipe <= endpoints; pipe++)
        {
            UInt8 direction = 0, endpoint = 0, type = 0, interval = 0;
            UInt16 packetSize = 0;
            if ((*interface)
                    ->GetPipeProperties(interface, pipe, &direction, &endpoint, &type, &packetSize, &interval) ==
                kIOReturnSuccess)
            {
                claimed.pipes[EndpointAddress(direction, endpoint)] = pipe;
                claimed.packetSizes[EndpointAddress(direction, endpoint)] = packetSize;
            }
        }
        return 0;
    }

    int ControlTransfer(Handle* handle, uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
        unsigned char* data, uint16_t length, unsigned int timeout)
    {
        IOUSBDevRequestTO setup{};
        setup.bmRequestType = requestType;
        setup.bRequest = request;
        setup.wValue = value;
        setup.wIndex = index;
        setup.wLength = length;
        setup.pData = data;
        setup.noDataTimeout = setup.completionTimeout = timeout;
        IOReturn result = (*handle->device)->DeviceRequestTO(handle->device, &setup);
        return result == kIOReturnSuccess ? int(setup.wLenDone) : ErrorFromIoReturn(result);
    }

    // One packet per ReadPipe (`packetSize`, the pipe's wMaxPacketSize): a larger read of 32-byte packets would wait
    // for a second packet and join the two (portal_usb::ReadLength).
    void ReadLoop(InterfaceInterface** interface, uint8_t pipe, uint16_t packetSize, Reader* reader)
    {
        for (int stalls = 0;;)
        {
            uint8_t packet[1024];
            UInt32 size = packetSize != 0 && packetSize < sizeof(packet) ? packetSize : 64;
            IOReturn result = (*interface)->ReadPipe(interface, pipe, packet, &size);
            if (result == kIOUSBPipeStalled && ++stalls < 3) // a stall cleared once or twice; then it is an error
            {
                (*interface)->ClearPipeStallBothEnds(interface, pipe);
                continue;
            }
            stalls = 0;
            std::lock_guard lock(reader->mutex);
            if (result != kIOReturnSuccess)
            {
                reader->error = result == kIOReturnAborted ? ErrorNoDevice : ErrorFromIoReturn(result);
                reader->arrived.notify_all();
                return;
            }
            if (reader->packets.size() >= 64)
                reader->packets.pop_front();
            reader->packets.emplace_back(packet, packet + size);
            reader->arrived.notify_all();
        }
    }

    int InterruptTransfer(
        Handle* handle, unsigned char endpoint, unsigned char* data, int length, int* transferred, unsigned int timeout)
    {
        *transferred = 0;
        for (auto& [number, claimed] : handle->interfaces)
        {
            auto pipe = claimed.pipes.find(endpoint);
            if (pipe == claimed.pipes.end())
                continue;
            if ((endpoint & 0x80) == 0)
            {
                // Interrupt OUT: no timeout either; a portal takes its 32 bytes at once, a gone one fails.
                IOReturn result =
                    (*claimed.interface)->WritePipe(claimed.interface, pipe->second, data, UInt32(length));
                if (result == kIOUSBPipeStalled)
                    (*claimed.interface)->ClearPipeStallBothEnds(claimed.interface, pipe->second);
                if (result != kIOReturnSuccess)
                    return ErrorFromIoReturn(result);
                *transferred = length;
                return 0;
            }
            std::unique_ptr<Reader>& reader = claimed.readers[endpoint];
            if (reader == nullptr)
            {
                reader = std::make_unique<Reader>();
                reader->thread =
                    std::thread(ReadLoop, claimed.interface, pipe->second, claimed.packetSizes[endpoint], reader.get());
            }
            std::unique_lock lock(reader->mutex);
            if (!reader->arrived.wait_for(lock, std::chrono::milliseconds(timeout),
                    [&] { return !reader->packets.empty() || reader->error != 0; }))
                return ErrorTimeout;
            if (reader->packets.empty())
                return reader->error;
            std::vector<uint8_t>& packet = reader->packets.front();
            *transferred = int(std::min(packet.size(), size_t(length)));
            memcpy(data, packet.data(), size_t(*transferred));
            reader->packets.pop_front();
            return 0;
        }
        return ErrorNotFound;
    }
}

namespace portal_usb
{
    bool LoadIokit(Api& usb)
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
        usb.error_name = ErrorName;
        usb.from = "IOKit (macOS, nothing to install)";
        return usb.init(&usb.context) == 0;
    }
}
