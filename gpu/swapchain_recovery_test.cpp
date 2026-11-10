// Checks what the runtime does when a swap chain's acquire or present fails (gpu/swapchain_recovery.h).
// usage: ArkeyanSwapChainRecoveryTest
#include "gpu/swapchain_recovery.h"
#include <cstdio>
#include <cstring>

int main()
{
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* what) {
        checks++;
        if (!ok)
        {
            fprintf(stderr, "FAIL %s\n", what);
            failures++;
        }
    };
    using namespace gpu::swapchain;

    // Out of date: the swap chain first, then the surface with it; five in a row, then one every 2 s.
    Recovery recovery;
    check(recovery.Presented() == 0, "no failure yet");
    check(recovery.Failed(OutOfDate, 1000) == Action::Rebuild, "first failure: rebuild the swap chain");
    check(recovery.Failed(OutOfDate, 1016) == Action::RebuildSurface, "second failure: the surface too");
    for (uint64_t now = 1032; recovery.Failures() < Recovery::FastAttempts; now += 16)
        check(recovery.Failed(OutOfDate, now) == Action::RebuildSurface, "fast attempts rebuild the surface");
    check(recovery.Failed(OutOfDate, 1100) == Action::RebuildSurface, "the sixth attempt still goes");
    check(recovery.Failures() == Recovery::FastAttempts + 1, "six attempts counted");
    check(recovery.Failed(OutOfDate, 1116) == Action::Wait, "then it waits");
    check(recovery.Failed(OutOfDate, 1100 + Recovery::SlowIntervalMs - 1) == Action::Wait, "for 2 s");
    check(recovery.Failures() == Recovery::FastAttempts + 1, "waits are not attempts");
    check(recovery.Failed(OutOfDate, 1100 + Recovery::SlowIntervalMs) == Action::RebuildSurface, "2 s later, again");
    check(recovery.Failed(OutOfDate, 1200 + Recovery::SlowIntervalMs) == Action::Wait, "and waits again");
    check(recovery.Presented() == Recovery::FastAttempts + 2, "a frame through: the attempts it took");
    check(recovery.Failures() == 0, "starts over");
    check(recovery.Failed(OutOfDate, 9000) == Action::Rebuild, "a new streak starts with the swap chain");

    // A lost surface is rebuilt at once.
    Recovery lost;
    check(lost.Failed(SurfaceLost, 0) == Action::RebuildSurface, "surface lost: the surface at once");

    // The log names the first streaks only, then every 100th.
    Recovery streaks;
    for (int i = 1; i <= 5; i++)
    {
        streaks.Failed(OutOfDate, 0);
        streaks.Presented();
    }
    check(streaks.LogStreak(), "the fifth streak is logged");
    streaks.Failed(OutOfDate, 0);
    check(!streaks.LogStreak(), "the sixth is not");

    check(strcmp(ResultName(OutOfDate), "VK_ERROR_OUT_OF_DATE_KHR") == 0, "out of date's name");
    check(strcmp(ResultName(SurfaceLost), "VK_ERROR_SURFACE_LOST_KHR") == 0, "surface lost's name");
    check(strcmp(ResultName(12345), "another VkResult") == 0, "an unknown code");

    // D3D12: DXGI's HRESULTs. A swap chain error is rebuilt as on Vulkan; a gone device is not the swap chain's.
    check(IsHresult(DxgiInvalidCall) && IsHresult(DxgiDeviceRemoved) && IsHresult(OutOfMemoryHr), "HRESULTs");
    check(!IsHresult(OutOfDate) && !IsHresult(SurfaceLost) && !IsHresult(FullScreenExclusiveLost) && !IsHresult(0),
        "VkResults are not");
    check(DeviceGone(DxgiDeviceRemoved) && DeviceGone(DxgiDeviceReset) && DeviceGone(DxgiDeviceHung) &&
              DeviceGone(DxgiDriverInternalError),
        "removed, reset, hung, driver error: the device");
    check(!DeviceGone(DxgiInvalidCall) && !DeviceGone(OutOfDate) && !DeviceGone(DeviceLost) && !DeviceGone(0),
        "the rest: the swap chain");
    Recovery dxgi;
    check(dxgi.Failed(DxgiInvalidCall, 0) == Action::Rebuild, "DXGI invalid call: ResizeBuffers first");
    check(dxgi.Failed(DxgiInvalidCall, 16) == Action::RebuildSurface, "then a new swap chain on the window");
    check(strcmp(ResultName(DxgiDeviceRemoved), "DXGI_ERROR_DEVICE_REMOVED") == 0, "device removed's name");
    check(strcmp(ResultName(int32_t(0x80004005u)), "another HRESULT") == 0, "an unknown HRESULT");

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
