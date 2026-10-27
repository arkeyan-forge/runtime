#pragma once

#include <cstdint>
#include <vector>
#include <ppc_config.h>
#include <ppc_context.h>

struct LoadedModule
{
    uint32_t handle{}; // guest X_LDR_DATA_TABLE_ENTRY
    uint32_t entryPoint{};
    uint32_t stackSize{};
    uint32_t systemFlags{};

    // Static TLS template (.tls) and dynamic slot count, from the XEX header.
    uint32_t tlsSlotCount{};
    uint32_t tlsRawData{};
    uint32_t tlsDataSize{};
    uint32_t tlsRawDataSize{};
};

// Maps the XEX image into guest memory, fills the function lookup table and resolves imports.
LoadedModule LoadXex(const std::vector<uint8_t>& xex);

extern LoadedModule g_module;

// Gives a host function a guest address, for exports the title reaches through a function pointer
// (XexGetProcedureAddress) instead of an import thunk. Indirect calls to the address run `function`.
uint32_t CreateHostThunk(PPCFunc* function);
