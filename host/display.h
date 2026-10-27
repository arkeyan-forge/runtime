#pragma once

#include <cstdint>
#include <vector>

struct SDL_Window;

// Hands a finished RGBA8 frame from a game thread to the window (until the real renderer presents).
void SubmitDisplayFrame(std::vector<uint32_t> rgba, int width, int height);

// On the host's main thread: creates the window (desktop fullscreen: no display mode change) and the GPU output
// presenting into it.
SDL_Window* CreateDisplayWindow(const char* title, int width, int height, bool fullscreen = false);

// On the host's main thread: uploads the latest submitted frame, if any, and presents it (paced by vsync). Does nothing
// once the game presents its own frames.
void PresentDisplayFrame();

// UWP, on the UI thread while the game renders: presents the game's frames here (gpu::ServicePresents), waiting up to
// `timeoutMs` for one.
void ServiceGamePresents(uint32_t timeoutMs);

// After presenting a frame: SSA_GPU_SNAPSHOT=<prefix> saves every 100th one (offscreen output only).
void SnapshotPresentedFrame();

// The game's Direct3D frames are presented by the renderer (d3d/render.cpp) from now on.
void SetGameRendering(bool active);
bool IsGameRendering();
