#include "host/windows_devices.h"
#ifdef _WIN32
#include <windows.h>
#include <cfgmgr32.h>
#include <cstring>
#ifndef CM_GETIDLIST_FILTER_PRESENT
#define CM_GETIDLIST_FILTER_PRESENT 0x00000100 // Windows 7 and later; older mingw-w64 headers lack it
#endif

namespace
{
    std::string Property(DEVINST instance, ULONG property)
    {
        char value[1024] = "";
        ULONG length = sizeof(value) - 2;
        if (CM_Get_DevNode_Registry_PropertyA(instance, property, nullptr, value, &length, 0) != CR_SUCCESS)
            return {};
        value[sizeof(value) - 2] = value[sizeof(value) - 1] = '\0';
        if (property != CM_DRP_HARDWAREID)
            return value;
        std::string list; // a REG_MULTI_SZ
        for (const char* id = value; *id != '\0' && id < value + sizeof(value); id += strlen(id) + 1)
            list += (list.empty() ? "" : ", ") + std::string(id);
        return list;
    }

    std::string InstanceId(DEVINST instance)
    {
        char id[MAX_DEVICE_ID_LEN + 1] = "";
        return CM_Get_Device_IDA(instance, id, sizeof(id), 0) == CR_SUCCESS ? id : "";
    }
}

std::vector<windows_devices::Device> windows_devices::PortalLikeDevices()
{
    std::vector<Device> devices;
    std::vector<char> list;
    for (int attempt = 0; attempt < 3; attempt++) // a device plugged in between the two calls makes the list longer
    {
        ULONG size = 0;
        if (CM_Get_Device_ID_List_SizeA(&size, nullptr, CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS || size == 0)
            return devices;
        list.assign(size, '\0');
        const CONFIGRET result = CM_Get_Device_ID_ListA(nullptr, list.data(), size, CM_GETIDLIST_FILTER_PRESENT);
        if (result == CR_SUCCESS)
            break;
        if (result != CR_BUFFER_SMALL || attempt == 2)
            return devices;
    }
    for (const char* id = list.data(); id < list.data() + list.size() && *id != '\0'; id += strlen(id) + 1)
    {
        DEVINST instance = 0;
        if (CM_Locate_DevNodeA(&instance, const_cast<char*>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;
        Device device;
        device.instanceId = id;
        device.name = Property(instance, CM_DRP_FRIENDLYNAME);
        device.service = Property(instance, CM_DRP_SERVICE);
        device.hardwareIds = Property(instance, CM_DRP_HARDWAREID);
        if (device.name.empty())
            device.name = Property(instance, CM_DRP_DEVICEDESC);
        Portal portal = LooksLikePortal(device.instanceId, device.hardwareIds, device.name);
        if (portal != Portal::No)
            device.portalNode = device.instanceId;
        // Or under one: a portal's audio output, its HID or GIP child (a few levels at most).
        DEVINST node = instance, parent = 0;
        for (int depth = 0; device.portalNode.empty() && depth < 8 && CM_Get_Parent(&parent, node, 0) == CR_SUCCESS;
            depth++, node = parent)
        {
            const std::string parentId = InstanceId(parent);
            std::string parentName = Property(parent, CM_DRP_FRIENDLYNAME);
            if (parentName.empty())
                parentName = Property(parent, CM_DRP_DEVICEDESC);
            if (parentId.empty() || parentId.rfind("ROOT\\", 0) == 0 || parentId.rfind("HTREE\\", 0) == 0)
                break;
            portal = LooksLikePortal(parentId, Property(parent, CM_DRP_HARDWAREID), parentName);
            if (portal != Portal::No)
                device.portalNode = parentId;
        }
        device.sure = portal == Portal::Sure;
        if (!device.portalNode.empty())
            devices.push_back(device);
    }
    return devices;
}

std::vector<std::string> windows_devices::PortalAudioOutputNames()
{
    std::vector<std::string> names;
    for (const Device& device : PortalLikeDevices())
    {
        if (device.sure && IsAudioOutput(device) && !device.name.empty())
            names.push_back(device.name);
    }
    return names;
}
#else
std::vector<windows_devices::Device> windows_devices::PortalLikeDevices() { return {}; }

std::vector<std::string> windows_devices::PortalAudioOutputNames() { return {}; }
#endif
