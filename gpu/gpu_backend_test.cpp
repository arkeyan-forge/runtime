// Checks desktop Windows' choice of graphics API and its fallback (gpu/gpu_backend.h), with fake backends.
// usage: ArkeyanGpuBackendTest
#include "gpu/gpu_backend.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
    using namespace gpubackend;

    // The settings file's words, either case; nothing else.
    Backend b = Backend::Vulkan;
    check(Parse("d3d12", b) && b == Backend::D3D12, "d3d12");
    check(Parse("Vulkan", b) && b == Backend::Vulkan, "Vulkan");
    check(Parse("D3D12", b) && b == Backend::D3D12, "D3D12");
    check(!Parse("dx12", b) && !Parse("", b) && !Parse("vulkan ", b) && b == Backend::D3D12, "anything else");
    check(strcmp(Name(Backend::D3D12), "d3d12") == 0 && strcmp(Name(Backend::Vulkan), "vulkan") == 0, "names");
    check(Other(Backend::D3D12) == Backend::Vulkan && Other(Backend::Vulkan) == Backend::D3D12, "the other");
    check(RuntimeDefault == Backend::D3D12, "D3D12 by default");

    // The setting first with the other as fallback; SKY_GPU_BACKEND names one alone; an unknown word is no override.
    Plan plan = Choose(Backend::D3D12, nullptr);
    check(plan.first == Backend::D3D12 && plan.fallback, "the setting: D3D12, then Vulkan");
    plan = Choose(Backend::Vulkan, nullptr);
    check(plan.first == Backend::Vulkan && plan.fallback, "the setting: Vulkan, then D3D12");
    plan = Choose(Backend::D3D12, "vulkan");
    check(plan.first == Backend::Vulkan && !plan.fallback, "SKY_GPU_BACKEND=vulkan over the setting, alone");
    plan = Choose(Backend::Vulkan, "D3D12");
    check(plan.first == Backend::D3D12 && !plan.fallback, "SKY_GPU_BACKEND=D3D12 over the setting, alone");
    plan = Choose(Backend::Vulkan, "auto");
    check(plan.first == Backend::Vulkan && plan.fallback, "an unknown SKY_GPU_BACKEND: the setting");

    // Run with fake backends: which start, in which order, what the log says.
    auto run = [](const Plan& p, bool d3d12Starts, bool vulkanStarts, std::vector<Backend>& tried,
                   std::vector<std::string>& lines) {
        return Run(
            p,
            [&](Backend backend) -> const char* {
                tried.push_back(backend);
                const bool starts = backend == Backend::D3D12 ? d3d12Starts : vulkanStarts;
                return starts ? nullptr : "no device was created";
            },
            [&](const char* line) { lines.emplace_back(line); });
    };
    std::vector<Backend> tried;
    std::vector<std::string> lines;
    auto ran = run(Choose(Backend::D3D12, nullptr), true, true, tried, lines);
    check(ran == Backend::D3D12 && tried.size() == 1, "D3D12 starts: no other tried");
    check(lines.size() == 1 && lines[0] == "[gpu] backend D3D12 first (the setting)", "its one log line");

    tried.clear(), lines.clear();
    ran = run(Choose(Backend::D3D12, nullptr), false, true, tried, lines);
    check(ran == Backend::Vulkan && tried.size() == 2 && tried[1] == Backend::Vulkan, "no D3D12 device: Vulkan runs");
    check(lines.size() == 2 && lines[1] == "[gpu] D3D12 did not start (no device was created): trying Vulkan",
        "the log says why");

    tried.clear(), lines.clear();
    ran = run(Choose(Backend::Vulkan, nullptr), true, false, tried, lines);
    check(ran == Backend::D3D12 && tried.size() == 2 && tried[0] == Backend::Vulkan, "no Vulkan device: D3D12 runs");

    tried.clear(), lines.clear();
    ran = run(Choose(Backend::D3D12, "vulkan"), true, false, tried, lines);
    check(!ran && tried.size() == 1 && tried[0] == Backend::Vulkan, "SKY_GPU_BACKEND=vulkan without Vulkan: none");
    check(lines.size() == 1 && lines[0] == "[gpu] backend Vulkan first (SKY_GPU_BACKEND), no other", "said so");

    tried.clear(), lines.clear();
    ran = run(Choose(Backend::D3D12, nullptr), false, false, tried, lines);
    check(!ran && tried.size() == 2, "neither starts: none, both tried");

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
