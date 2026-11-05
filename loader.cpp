#include "loader.h"
#include "kernel/objects.h"
#include "memory.h"
#include "platform/platform.h"
#include "thunk_slot.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <image.h>
#include <ppc_config.h>
#include <ppc_context.h>

LoadedModule g_module;

namespace
{
#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { (ORDINAL), #NAME }
    const std::unordered_map<uint32_t, std::string_view> KernelExports = {
#include <xbox/xboxkrnl_table.inc>
    };
#undef XE_EXPORT

    // XEX2 optional header keys.
    constexpr uint32_t XexHeaderImportLibraries = 0x000103FF;
    constexpr uint32_t XexHeaderTlsInfo = 0x00020104;
    constexpr uint32_t XexHeaderDefaultStackSize = 0x00020200;
    constexpr uint32_t XexHeaderSystemFlags = 0x00030000;

    // X_LDR_DATA_TABLE_ENTRY fields.
    constexpr uint32_t LdrDllBase = 0x18;
    constexpr uint32_t LdrImageBase = 0x1C;
    constexpr uint32_t LdrImageSize = 0x20;
    constexpr uint32_t LdrFullImageSize = 0x38;
    constexpr uint32_t LdrEntryPoint = 0x3C;
    constexpr uint32_t LdrXexHeaderBase = 0x58;
    constexpr uint32_t LdrSize = 0x100;

    // Indirect calls to code XenonRecomp didn't treat as a function start land here instead of a null pointer.
    void UnmappedFunction(PPCContext& ctx, uint8_t*)
    {
        fprintf(
            stderr, "Indirect call to unrecompiled guest code (ctr=0x%08X lr=0x%08X)\n", ctx.ctr.u32, uint32_t(ctx.lr));
        abort();
    }

    // The function table is read-only once LoadXex has filled it: a stray guest write there made a later call jump
    // anywhere; now it faults where it happens. In 64 KB granules inside the table (every host page size divides 64 KB).
    constexpr uintptr_t TableGranule = 0x10000;

    uintptr_t TableEntry(uint32_t address)
    {
        return reinterpret_cast<uintptr_t>(&PPC_LOOKUP_FUNC(g_memory.base, address));
    }
    uintptr_t TableBegin() { return (TableEntry(PPC_CODE_BASE) + TableGranule - 1) & ~(TableGranule - 1); }
    uintptr_t TableEnd() { return TableEntry(PPC_CODE_BASE + PPC_CODE_SIZE) & ~(TableGranule - 1); }

    // The granule holding `address`'s entry, if it is in the read-only part.
    void SetEntryReadOnly(uint32_t address, bool readOnly)
    {
        uintptr_t granule = TableEntry(address) & ~(TableGranule - 1);
        if (granule >= TableBegin() && granule < TableEnd())
            PlatformProtectReadOnly(reinterpret_cast<void*>(granule), TableGranule, readOnly);
    }

    uint32_t ReadBE32(const uint8_t* p) { return ByteSwap(*reinterpret_cast<const uint32_t*>(p)); }
    uint16_t ReadBE16(const uint8_t* p) { return ByteSwap(*reinterpret_cast<const uint16_t*>(p)); }

    // Returns the optional header's value: inline data, or the offset of its data in the file.
    bool FindOptionalHeader(const std::vector<uint8_t>& xex, uint32_t key, uint32_t& value)
    {
        uint32_t count = ReadBE32(&xex[0x14]);
        for (uint32_t i = 0; i < count; i++)
        {
            if (ReadBE32(&xex[0x18 + i * 8]) == key)
            {
                value = ReadBE32(&xex[0x18 + i * 8 + 4]);
                return true;
            }
        }
        return false;
    }

    uint32_t CreateModuleHandle(const std::vector<uint8_t>& xex, const Image& image)
    {
        // Some XAM/kernel callers inspect the XEX header through the module, so keep a copy in guest memory.
        uint32_t headerSize = ReadBE32(&xex[0x8]);
        uint32_t header = g_memory.SystemAlloc(headerSize);
        memcpy(g_memory.Translate(header), xex.data(), headerSize);

        uint32_t ldr = g_memory.SystemAlloc(LdrSize);
        g_memory.Write<uint32_t>(ldr + LdrDllBase, image.base);
        g_memory.Write<uint32_t>(ldr + LdrImageBase, image.base);
        g_memory.Write<uint32_t>(ldr + LdrImageSize, image.size);
        g_memory.Write<uint32_t>(ldr + LdrFullImageSize, image.size);
        g_memory.Write<uint32_t>(ldr + LdrEntryPoint, image.entry_point);
        g_memory.Write<uint32_t>(ldr + LdrXexHeaderBase, header);
        return ldr;
    }

    uint32_t CreateKernelVariable(std::string_view name, uint32_t moduleHandle)
    {
        if (name == "XboxKrnlVersion")
        {
            uint32_t version = g_memory.SystemAlloc(8);
            g_memory.Write<uint16_t>(version + 0, 2);
            g_memory.Write<uint16_t>(version + 2, 0);
            g_memory.Write<uint16_t>(version + 4, 17559);
            return version;
        }
        if (name == "XboxHardwareInfo")
        {
            uint32_t info = g_memory.SystemAlloc(0x10);
            g_memory.Write<uint8_t>(info + 4, 6); // NumberOfProcessors
            return info;
        }
        if (name == "XexExecutableModuleHandle")
        {
            uint32_t handle = g_memory.SystemAlloc(4);
            g_memory.Write<uint32_t>(handle, moduleHandle);
            return handle;
        }
        if (name == "ExLoadedCommandLine")
        {
            uint32_t commandLine = g_memory.SystemAlloc(0x200);
            strcpy(static_cast<char*>(g_memory.Translate(commandLine)), "\"default.xex\"");
            return commandLine;
        }
        if (name == "KeTimeStampBundle")
        {
            uint32_t bundle = g_memory.SystemAlloc(0x18);
            StartTimeStampBundle(bundle);
            return bundle;
        }
        if (name == "VdGpuClockInMHz")
        {
            uint32_t clock = g_memory.SystemAlloc(4);
            g_memory.Write<uint32_t>(clock, 500);
            return clock;
        }

        // Monitor data, object types, GPU globals: zeroed until something needs more.
        return g_memory.SystemAlloc(0x100);
    }

    void ResolveImports(const std::vector<uint8_t>& xex, uint32_t moduleHandle)
    {
        uint32_t offset;
        if (!FindOptionalHeader(xex, XexHeaderImportLibraries, offset))
            return;

        const uint8_t* header = &xex[offset];
        uint32_t stringTableSize = ReadBE32(header + 4);
        uint32_t libraryCount = ReadBE32(header + 8);

        std::vector<std::string_view> libraryNames;
        for (const char* name = reinterpret_cast<const char*>(header + 12); libraryNames.size() < libraryCount;)
        {
            libraryNames.emplace_back(name);
            name += (libraryNames.back().size() + 4) & ~3;
        }

        const uint8_t* library = header + 12 + stringTableSize;
        for (uint32_t i = 0; i < libraryCount; i++)
        {
            std::string_view libraryName = libraryNames[ReadBE16(library + 0x24)];
            uint16_t recordCount = ReadBE16(library + 0x26);

            // Each function import is an IAT slot followed by its thunk; a slot with no thunk is a variable.
            // XenonUtils already turned thunks into `nop; nop; nop; blr` and byteswapped the slots in place.
            uint32_t pendingSlot = 0;
            auto resolveVariable = [&](uint32_t slot) {
                uint32_t ordinal = *static_cast<uint32_t*>(g_memory.Translate(slot)) & 0xFFFF;
                auto name =
                    libraryName == "xboxkrnl.exe" && KernelExports.contains(ordinal) ? KernelExports.at(ordinal) : "?";
                uint32_t variable = CreateKernelVariable(name, moduleHandle);
                g_memory.Write<uint32_t>(slot, variable);
                fprintf(stderr, "[loader] %.*s!%.*s (ordinal %u) -> 0x%08X\n", int(libraryName.size()),
                    libraryName.data(), int(name.size()), name.data(), ordinal, variable);
            };

            for (uint32_t j = 0; j < recordCount; j++)
            {
                uint32_t record = ReadBE32(library + 0x28 + j * 4);
                if (*static_cast<uint32_t*>(g_memory.Translate(record)) == 0x00000060)
                {
                    // Calls through the IAT land on the thunk, which the lookup table maps to the import.
                    if (pendingSlot != 0)
                        g_memory.Write<uint32_t>(pendingSlot, record);
                    pendingSlot = 0;
                }
                else
                {
                    if (pendingSlot != 0)
                        resolveVariable(pendingSlot);
                    pendingSlot = record;
                }
            }
            if (pendingSlot != 0)
                resolveVariable(pendingSlot);

            library += ReadBE32(library);
        }
    }
}

LoadedModule LoadXex(const std::vector<uint8_t>& xex)
{
    const auto image = Image::ParseImage(xex.data(), xex.size());
    if (image.data == nullptr || image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
    {
        fprintf(stderr, "default.xex is not the executable this build was recompiled from\n");
        exit(1);
    }

    memcpy(g_memory.Translate(image.base), image.data.get(), image.size);

    for (uint32_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
        PPC_LOOKUP_FUNC(g_memory.base, address) = UnmappedFunction;
    for (auto* mapping = PPCFuncMappings; mapping->host != nullptr; mapping++)
        PPC_LOOKUP_FUNC(g_memory.base, mapping->guest) = mapping->host;
    if (TableEnd() > TableBegin() &&
        !PlatformProtectReadOnly(reinterpret_cast<void*>(TableBegin()), TableEnd() - TableBegin(), true))
        fprintf(stderr, "[loader] the function table stays writable\n");

    LoadedModule module;
    module.handle = CreateModuleHandle(xex, image);
    module.entryPoint = image.entry_point;
    FindOptionalHeader(xex, XexHeaderSystemFlags, module.systemFlags);

    ResolveImports(xex, module.handle);

    module.stackSize = 0x40000;
    FindOptionalHeader(xex, XexHeaderDefaultStackSize, module.stackSize);

    uint32_t tlsOffset;
    if (FindOptionalHeader(xex, XexHeaderTlsInfo, tlsOffset))
    {
        module.tlsSlotCount = ReadBE32(&xex[tlsOffset + 0x0]);
        module.tlsRawData = ReadBE32(&xex[tlsOffset + 0x4]);
        module.tlsDataSize = ReadBE32(&xex[tlsOffset + 0x8]);
        module.tlsRawDataSize = ReadBE32(&xex[tlsOffset + 0xC]);
    }

    return module;
}

uint32_t CreateHostThunk(PPCFunc* function)
{
    // Thunks take words of the zero padding between code sections, from the end of the code range down: no
    // translated function starts there, and a run of 16 zero words is never code. A title whose code is one section
    // has no such run (only lone zero words between functions): then any zero word no function starts at.
    static std::mutex mutex;
    static uint32_t next = PPC_CODE_BASE + PPC_CODE_SIZE;

    auto isPadding = [](uint32_t address) {
        return PPC_LOOKUP_FUNC(g_memory.base, address) == UnmappedFunction && g_memory.Read<uint32_t>(address) == 0;
    };

    std::lock_guard lock(mutex);
    uint32_t thunk = FindThunkSlot(next, PPC_CODE_BASE, 16, isPadding);
    if (thunk == 0)
        thunk = FindThunkSlot(next, PPC_CODE_BASE, 1, isPadding);
    if (thunk != 0)
    {
        SetEntryReadOnly(thunk, false);
        PPC_LOOKUP_FUNC(g_memory.base, thunk) = function;
        SetEntryReadOnly(thunk, true);
        next = thunk;
        return thunk;
    }

    fprintf(stderr, "No code padding left for a host thunk\n");
    abort();
}
