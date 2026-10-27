#pragma once

#include <cstdint>
#include <string>

struct IUnknown;

// Glue between the UWP host's entry point (host/uwp/main.cpp) and its SDL subset (host/uwp/sdl_subset.cpp).

// LocalState, as UTF-8 with a trailing backslash: what SDL_GetPrefPath returns, so saves and figure dumps go there.
void UwpSetPreferencePath(std::string path);

// The view's CoreWindow and the swap chain size in pixels: SDL_CreateWindow returns a window that stands for it.
void UwpSetCoreWindow(IUnknown* window, uint32_t width, uint32_t height);

// The SDL property of that window holding the CoreWindow (the name SDL 3 previews used before UWP support went).
constexpr const char* UwpCoreWindowProperty = "SDL.window.winrt.window";

// Reads LocalState\launcher.ini (`localState` with its trailing backslash; written with the console defaults when
// missing) and applies it: resolution scale, anisotropy, vsync, languages (host/uwp/settings_uwp.cpp).
void UwpApplySettings(const std::string& localState, uint32_t defaultScale);
