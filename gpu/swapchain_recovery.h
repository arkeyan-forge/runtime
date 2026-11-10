#pragma once

// What to do when a swap chain's acquire or present fails on a window that has a size (gpu.cpp). A Linux NVIDIA driver
// on Wayland refused every image after the switch to fullscreen while the window and the swap chain kept the same size,
// so the size check never rebuilt it and the screen stayed black. The first failure of a streak rebuilds the swap chain
// (the surface too when it was lost); the next ones rebuild the surface with it. Five attempts in a row at most, then one
// every 2 s, until a frame gets through. D3D12 reports the present's HRESULT the same way; a removed or reset device is
// not the swap chain's fault and is left to the device-lost path. No plume, Vulkan or DXGI here: tested on every host
// (swapchain_recovery_test.cpp).
#include <cstdint>

namespace gpu::swapchain
{
    // The VkResult values (vulkan_core.h) that an acquire or a present returns; Metal gives 0.
    constexpr int32_t Success = 0;
    constexpr int32_t NotReady = 1;
    constexpr int32_t Timeout = 2;
    constexpr int32_t Suboptimal = 1000001003;
    constexpr int32_t OutOfHostMemory = -1;
    constexpr int32_t OutOfDeviceMemory = -2;
    constexpr int32_t DeviceLost = -4;
    constexpr int32_t SurfaceLost = -1000000000;
    constexpr int32_t NativeWindowInUse = -1000000001;
    constexpr int32_t OutOfDate = -1000001004;
    constexpr int32_t FullScreenExclusiveLost = -1000255000;

    // The HRESULTs (dxgi.h, winerror.h) that D3D12's present returns; they never collide with a VkResult.
    constexpr int32_t DxgiInvalidCall = int32_t(0x887A0001u);
    constexpr int32_t DxgiDeviceRemoved = int32_t(0x887A0005u);
    constexpr int32_t DxgiDeviceHung = int32_t(0x887A0006u);
    constexpr int32_t DxgiDeviceReset = int32_t(0x887A0007u);
    constexpr int32_t DxgiDriverInternalError = int32_t(0x887A0020u);
    constexpr int32_t DxgiNotCurrentlyAvailable = int32_t(0x887A0022u);
    constexpr int32_t OutOfMemoryHr = int32_t(0x8007000Eu);
    constexpr int32_t InvalidArgHr = int32_t(0x80070057u);

    // A failed HRESULT: the lowest VkResult is about -1.0e9, a failed HRESULT is below -2^31 + 2^30.
    inline bool IsHresult(int32_t result) { return result < -1'200'000'000; }

    // The device is gone, not the swap chain: rebuilding it would only fail again (gpu.cpp reports the loss).
    inline bool DeviceGone(int32_t result)
    {
        return result == DxgiDeviceRemoved || result == DxgiDeviceHung || result == DxgiDeviceReset ||
               result == DxgiDriverInternalError;
    }

    inline const char* ResultName(int32_t result)
    {
        switch (result)
        {
        case Success: return "VK_SUCCESS";
        case NotReady: return "VK_NOT_READY";
        case Timeout: return "VK_TIMEOUT";
        case Suboptimal: return "VK_SUBOPTIMAL_KHR";
        case OutOfHostMemory: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case OutOfDeviceMemory: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case DeviceLost: return "VK_ERROR_DEVICE_LOST";
        case SurfaceLost: return "VK_ERROR_SURFACE_LOST_KHR";
        case NativeWindowInUse: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case OutOfDate: return "VK_ERROR_OUT_OF_DATE_KHR";
        case FullScreenExclusiveLost: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
        case DxgiInvalidCall: return "DXGI_ERROR_INVALID_CALL";
        case DxgiDeviceRemoved: return "DXGI_ERROR_DEVICE_REMOVED";
        case DxgiDeviceHung: return "DXGI_ERROR_DEVICE_HUNG";
        case DxgiDeviceReset: return "DXGI_ERROR_DEVICE_RESET";
        case DxgiDriverInternalError: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DxgiNotCurrentlyAvailable: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        case OutOfMemoryHr: return "E_OUTOFMEMORY";
        case InvalidArgHr: return "E_INVALIDARG";
        default: return IsHresult(result) ? "another HRESULT" : "another VkResult";
        }
    }

    enum class Action
    {
        Wait,           // too soon after the last attempt: skip the frame
        Rebuild,        // a new swap chain on the same surface
        RebuildSurface, // the swap chain and its surface, both new
    };

    class Recovery
    {
    public:
        static constexpr uint32_t FastAttempts = 5;
        static constexpr uint64_t SlowIntervalMs = 2000;

        // An acquire or a present failed with `result` at `nowMs`: what to do before the next frame.
        Action Failed(int32_t result, uint64_t nowMs)
        {
            if (m_failures >= FastAttempts && nowMs < m_nextMs)
                return Action::Wait;
            if (m_failures == 0)
                m_streaks++;
            m_failures++;
            if (m_failures > FastAttempts)
                m_nextMs = nowMs + SlowIntervalMs;
            return result == SurfaceLost || m_failures > 1 ? Action::RebuildSurface : Action::Rebuild;
        }

        // A frame got through: returns how many attempts it took (0: there was no failure); the next one starts over.
        uint32_t Presented()
        {
            uint32_t attempts = m_failures;
            m_failures = 0;
            m_nextMs = 0;
            return attempts;
        }

        uint32_t Failures() const { return m_failures; }
        // The log names the first streaks and then every 100th, should a swap chain fail again after each rebuild.
        bool LogStreak() const { return m_streaks <= 5 || m_streaks % 100 == 0; }

    private:
        uint32_t m_failures = 0; // attempts in this streak
        uint32_t m_streaks = 0;
        uint64_t m_nextMs = 0;
    };
}
