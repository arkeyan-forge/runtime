#include "loader.h"
#include "memory.h"
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <ppc_config.h>
#include <ppc_context.h>

namespace
{
    constexpr uint32_t StatusSuccess = 0;
    constexpr uint32_t StatusNoSuchFile = 0xC000000F;
    constexpr uint32_t StatusOrdinalNotFound = 0xC0000262;

    // System modules are always "loaded" on the console. They get an LDR entry with a distinct
    // fake base so callers can tell them apart; their exports are reached through the import thunks.
    uint32_t SystemModuleHandle(const std::string& name)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, uint32_t> handles;

        if (name != "xam.xex" && name != "xboxkrnl.exe")
            return 0;

        std::lock_guard lock(mutex);
        uint32_t& handle = handles[name];
        if (handle == 0)
        {
            handle = g_memory.SystemAlloc(0x100);
            g_memory.Write<uint32_t>(handle + 0x18, name == "xam.xex" ? 0x81000000 : 0x80040000); // DllBase
        }
        return handle;
    }

    uint32_t ModuleHandleByName(uint32_t namePointer)
    {
        if (namePointer == 0)
            return g_module.handle;

        std::string name = static_cast<const char*>(g_memory.Translate(namePointer));
        for (char& c : name)
            c = char(tolower(c));
        return SystemModuleHandle(name);
    }
}

// Looks up an optional header in a XEX header in guest memory. Keys whose low byte is 0 or 1 store
// their value inline; the others point at their data, returned as a guest address.
PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    uint32_t header = ctx.r3.u32;
    uint32_t key = ctx.r4.u32;

    uint32_t count = g_memory.Read<uint32_t>(header + 0x14);
    for (uint32_t i = 0; i < count; i++)
    {
        if (g_memory.Read<uint32_t>(header + 0x18 + i * 8) == key)
        {
            uint32_t value = g_memory.Read<uint32_t>(header + 0x18 + i * 8 + 4);
            ctx.r3.u64 = (key & 0xFF) <= 1 ? value : header + value;
            return;
        }
    }

    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    ctx.r3.u64 = (g_module.systemFlags >> ctx.r3.u32) & 1;
}

PPC_FUNC(__imp__XexGetModuleHandle)
{
    uint32_t handle = ModuleHandleByName(ctx.r3.u32);
    g_memory.Write<uint32_t>(ctx.r4.u32, handle);
    ctx.r3.u64 = handle != 0 ? StatusSuccess : StatusNoSuchFile;
}

PPC_FUNC(__imp__XexLoadImage)
{
    uint32_t handle = ModuleHandleByName(ctx.r3.u32);
    if (handle == 0)
        fprintf(stderr, "[kernel] XexLoadImage: cannot load %s\n", static_cast<const char*>(g_memory.Translate(ctx.r3.u32)));

    g_memory.Write<uint32_t>(ctx.r6.u32, handle);
    ctx.r3.u64 = handle != 0 ? StatusSuccess : StatusNoSuchFile;
}

PPC_FUNC(__imp__XexUnloadImage)
{
    ctx.r3.u64 = StatusSuccess;
}

PPC_EXTERN_FUNC(__imp__XamInputNonControllerGetRaw);
PPC_EXTERN_FUNC(__imp__XamInputNonControllerSetRaw);

// XexGetProcedureAddress(module, ordinal, PVOID* address). Imports are resolved at load time; the exports
// below are the ones the title looks up at run time and calls through the returned pointer.
PPC_FUNC(__imp__XexGetProcedureAddress)
{
    static const std::pair<uint32_t, PPCFunc*> XamExports[] =
    {
        { 0x4A1, __imp__XamInputNonControllerGetRaw }, // Skylanders portal (kernel/portal.cpp)
        { 0x4A2, __imp__XamInputNonControllerSetRaw },
    };
    static std::mutex mutex;
    static std::unordered_map<uint32_t, uint32_t> thunks; // ordinal -> guest address

    uint32_t ordinal = ctx.r4.u32;
    if (ctx.r3.u32 == SystemModuleHandle("xam.xex"))
    {
        for (const auto& [exportOrdinal, function] : XamExports)
        {
            if (exportOrdinal == ordinal)
            {
                std::lock_guard lock(mutex);
                uint32_t& thunk = thunks[ordinal];
                if (thunk == 0)
                    thunk = CreateHostThunk(function);
                g_memory.Write<uint32_t>(ctx.r5.u32, thunk);
                ctx.r3.u64 = StatusSuccess;
                return;
            }
        }
    }

    fprintf(stderr, "[kernel] XexGetProcedureAddress: ordinal 0x%X not provided\n", ordinal);
    g_memory.Write<uint32_t>(ctx.r5.u32, 0);
    ctx.r3.u64 = StatusOrdinalNotFound;
}
