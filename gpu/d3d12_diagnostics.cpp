// D3D12 device removal diagnostics (Windows and Xbox): DRED (Device Removed Extended Data) auto-breadcrumbs and page
// faults, turned on before the device is created, and a report in the log the first time the device is found removed.
#include "gpu/context.h"
#include <plume_d3d12.h>
#include <cstdio>
#include <iterator>

namespace
{
    // D3D12_AUTO_BREADCRUMB_OP, in enum order.
    const char* const OpNames[] = { "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
        "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles",
        "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView",
        "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission",
        "DecodeFrame", "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
        "BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand",
        "EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
        "ExecuteExtensionCommand", "DispatchMesh", "EncodeFrame", "ResolveEncoderOutputMetadata" };

    const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) { return uint32_t(op) < std::size(OpNames) ? OpNames[op] : "?"; }

    const char* ReasonName(HRESULT reason)
    {
        switch (uint32_t(reason))
        {
        case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
        case 0x887A0006: return "DXGI_ERROR_DEVICE_HUNG (a command took too long or the GPU faulted)";
        case 0x887A0007: return "DXGI_ERROR_DEVICE_RESET (badly formed commands)";
        case 0x887A0020: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
        case 0x8007000E: return "E_OUTOFMEMORY";
        default: return "?";
        }
    }

    void PrintAllocations(const char* what, const D3D12_DRED_ALLOCATION_NODE1* node)
    {
        for (uint32_t count = 0; node != nullptr && count < 16; node = node->pNext, count++)
            fprintf(stderr, "[gpu]   %s: %s (allocation type %d)\n", what,
                node->ObjectNameA != nullptr ? node->ObjectNameA : "(unnamed)", int(node->AllocationType));
    }
}

namespace gpu::detail
{
    void D3D12EnableDred()
    {
        ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&settings))))
        {
            settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            settings->Release();
            fprintf(stderr, "[gpu] D3D12 DRED on (breadcrumbs, page faults)\n");
        }
        else
        {
            fprintf(stderr, "[gpu] D3D12 DRED unavailable\n");
        }
    }

    bool D3D12DeviceRemoved(plume::RenderDevice& device)
    {
        ID3D12Device* d3d = static_cast<plume::D3D12Device&>(device).d3d;
        HRESULT reason = d3d->GetDeviceRemovedReason();
        if (reason == S_OK)
            return false;
        fprintf(stderr, "[gpu] D3D12 device removed: reason 0x%08lX %s\n", static_cast<unsigned long>(reason),
            ReasonName(reason));

        ID3D12DeviceRemovedExtendedData1* dred = nullptr;
        if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dred))))
        {
            fprintf(stderr, "[gpu] no DRED data\n");
            return true;
        }
        // Breadcrumbs: per command list, the operations recorded; the GPU completed the first `*pLastBreadcrumbValue`.
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            uint32_t lists = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode;
                node != nullptr && lists < 8; node = node->pNext)
            {
                uint32_t completed = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
                if (completed >= node->BreadcrumbCount)
                    continue; // finished: not where the GPU stopped
                lists++;
                fprintf(stderr, "[gpu] DRED: command list %p, %u of %u operations completed\n",
                    static_cast<void*>(node->pCommandList), completed, node->BreadcrumbCount);
                uint32_t first = completed > 4 ? completed - 4 : 0;
                for (uint32_t i = first; i < node->BreadcrumbCount && i < completed + 8; i++)
                    fprintf(stderr, "[gpu]   %s%u %s\n", i == completed ? "-> " : "   ", i,
                        OpName(node->pCommandHistory[i]));
            }
            if (lists == 0)
                fprintf(stderr, "[gpu] DRED: every command list recorded completed\n");
        }
        else
        {
            fprintf(stderr, "[gpu] DRED: no breadcrumbs\n");
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault = {};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)) && pageFault.PageFaultVA != 0)
        {
            fprintf(stderr, "[gpu] DRED: page fault at GPU address 0x%llX\n",
                static_cast<unsigned long long>(pageFault.PageFaultVA));
            PrintAllocations("existing allocation", pageFault.pHeadExistingAllocationNode);
            PrintAllocations("recently freed allocation", pageFault.pHeadRecentFreedAllocationNode);
        }
        else
        {
            fprintf(stderr, "[gpu] DRED: no page fault\n");
        }
        dred->Release();
        return true;
    }
}

namespace gpu::detail
{
    void D3D12LogMemory(plume::RenderDevice& device, const char* when)
    {
        IDXGIAdapter3* adapter = nullptr;
        IDXGIAdapter1* adapter1 = static_cast<plume::D3D12Device&>(device).adapter;
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (adapter1 != nullptr && SUCCEEDED(adapter1->QueryInterface(IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            fprintf(stderr, "[gpu] video memory %s: %.0f MB used of a %.0f MB budget\n", when,
                double(info.CurrentUsage) / (1 << 20), double(info.Budget) / (1 << 20));
        else
            fprintf(stderr, "[gpu] video memory %s: unknown\n", when);
        if (adapter != nullptr)
            adapter->Release();
    }
}

namespace gpu::detail
{
    long D3D12RemovedReason(plume::RenderDevice& device)
    {
        return static_cast<plume::D3D12Device&>(device).d3d->GetDeviceRemovedReason();
    }
}
