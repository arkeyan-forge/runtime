# runtime/: the shared runtime

> **Purpose:** what is in the `runtime/` code folder and where is it documented? · **Status:** current (2026-10-26) · **Tags:** [Xbox] [XDK]

## What it is

What replaces the Xbox 360 for every game: the kernel and XAM, the D3D layer and the host GPU, audio, input, the
portal, the Toy Box and the collection. Nothing here names a game: addresses come from `games/<id>/addresses.h`
(`game_hooks.h`), per-game questions are answered by the game's adapter (`game.h`), and engine behaviour lives in
`engines/`. The documentation: [docs/runtime/](https://github.com/arkeyan-forge/forge/blob/main/docs/runtime/README.md).

## Where the code is

| Folder / file | What |
|---|---|
| `main.cpp`, `loader.*`, `memory.*`, `vfs.*`, `thread.*` | start-up, XEX loading, the 4 GB guest address space, the disc, guest threads |
| `game.h`, `game_hooks.h` | the adapter a game implements; the `GAME_HOOK` / `GAME_IMP` macros hooks are written with |
| `kernel/` | the kernel and XAM imports ([xbox360/](https://github.com/arkeyan-forge/forge/blob/main/docs/runtime/xbox360/README.md)) |
| `d3d/`, `gpu/` | the D3D layer and the host GPU through plume ([gpu/](https://github.com/arkeyan-forge/forge/blob/main/docs/runtime/gpu/README.md)) |
| `apu/` | XMA decoding with FFmpeg |
| `host/` | window, input, audio out, settings, portal, figures, collection, Toy Box, art, the `--launcher` window ([host/](https://github.com/arkeyan-forge/forge/blob/main/docs/runtime/host/README.md)) |
| `platform/`, `host/uwp/` | OS-specific code, and the Xbox UWP host |
| `inflate.cpp`, `indirect_call_guard.cpp` | the zlib hook, the guard for bad indirect calls |

One line per file: [docs/architecture/layout.md](https://github.com/arkeyan-forge/forge/blob/main/docs/architecture/layout.md). Per subsystem (files, invariants):
[docs/architecture/subsystems.md](https://github.com/arkeyan-forge/forge/blob/main/docs/architecture/subsystems.md). Every hook:
[docs/architecture/hooks.md](https://github.com/arkeyan-forge/forge/blob/main/docs/architecture/hooks.md).

## How to test it

`tools/test_all.sh` (both games, every unit test): [docs/getting-started/testing.md](https://github.com/arkeyan-forge/forge/blob/main/docs/getting-started/testing.md).

## Gotchas

Every game run isolates the user's files (`SKY_SETTINGS`, `SKY_LAUNCHER_INI`, `SKY_COLLECTION`, `SKY_SAVE_DIR`,
`SKYLANDERS_DATA`, `SKY_LOG`), see testing.md. Settings and environment variables:
[settings.md](https://github.com/arkeyan-forge/forge/blob/main/docs/getting-started/settings.md), [debugging.md](https://github.com/arkeyan-forge/forge/blob/main/docs/porting/debugging.md).
