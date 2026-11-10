#pragma once

// Desktop Windows' two graphics APIs (docs/runtime/gpu/plume_backends.md): [<game>.video] gpu_backend = d3d12 | vulkan,
// the launcher's per-game choice, Direct3D 12 when not set. The chosen one starts first and the other is the fallback
// when it has no device. SKY_GPU_BACKEND (d3d12, vulkan) names one and allows no other. macOS (Metal), Linux (Vulkan)
// and UWP (D3D12) have one backend each and ignore the setting.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>

namespace gpubackend
{
    enum class Backend : uint8_t
    {
        D3D12,
        Vulkan,
    };

    inline bool Parse(std::string_view text, Backend& out)
    {
        auto same = [&](std::string_view name) {
            if (text.size() != name.size())
                return false;
            for (size_t i = 0; i < text.size(); i++)
                if ((text[i] >= 'A' && text[i] <= 'Z' ? char(text[i] - 'A' + 'a') : text[i]) != name[i])
                    return false;
            return true;
        };
        if (same("d3d12"))
            out = Backend::D3D12;
        else if (same("vulkan"))
            out = Backend::Vulkan;
        else
            return false;
        return true;
    }

    // Not set: Direct3D 12 (the owner's choice, 2026-11-10; Vulkan came first before 0.9.5).
    constexpr Backend RuntimeDefault = Backend::D3D12;

    // The settings file's word.
    inline const char* Name(Backend backend) { return backend == Backend::Vulkan ? "vulkan" : "d3d12"; }
    // The log's and the debug screen's.
    inline const char* Label(Backend backend) { return backend == Backend::Vulkan ? "Vulkan" : "D3D12"; }
    inline Backend Other(Backend backend) { return backend == Backend::Vulkan ? Backend::D3D12 : Backend::Vulkan; }

    // What gpu::Init tries, in order, and why the first.
    struct Plan
    {
        Backend first = Backend::D3D12;
        bool fallback = true; // the other one when the first has no device
        const char* why = "the setting";
    };

    // `setting`: gpu_backend; `env`: SKY_GPU_BACKEND (nullptr when not set; a word other than d3d12 or vulkan is ignored).
    inline Plan Choose(Backend setting, const char* env)
    {
        Backend named;
        if (env != nullptr && Parse(env, named))
            return { named, false, "SKY_GPU_BACKEND" };
        return { setting, true, "the setting" };
    }

    // Starts the plan's first backend, then the other when it has not started and the plan allows it. `start(backend)`
    // returns nullptr once it runs, else why it did not; `log(line)` gets the log's lines. The one that runs, if any.
    template <class Start, class Log>
    std::optional<Backend> Run(const Plan& plan, Start&& start, Log&& log)
    {
        char line[160];
        snprintf(line, sizeof(line), "[gpu] backend %s first (%s)%s", Label(plan.first), plan.why,
            plan.fallback ? "" : ", no other");
        log(line);
        const char* why = start(plan.first);
        if (why == nullptr)
            return plan.first;
        if (!plan.fallback)
            return std::nullopt;
        const Backend other = Other(plan.first);
        snprintf(line, sizeof(line), "[gpu] %s did not start (%s): trying %s", Label(plan.first), why, Label(other));
        log(line);
        if (start(other) == nullptr)
            return other;
        return std::nullopt;
    }
}
