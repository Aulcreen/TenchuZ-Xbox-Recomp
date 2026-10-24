# Tenchu Z Recompiled

A native PC port of **Tenchu Z** (Xbox 360, 2006/2007), built by statically recompiling the
original `default.xex` to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

> No game data is included or will ever be distributed. You need your own copy of the game.

## Layout

| Path | Contents |
|---|---|
| `tenchuz_manifest.toml` | ReXGlue project manifest |
| `config/default.toml` | Codegen overrides for `default.xex` (function boundaries, hooks, names) |
| `src/` | Host-side app code (`TenchuzApp`, hooks, and PC enhancements) |
| `generated/` | Output of `rexglue codegen` (git-ignored, regenerate it) |
| `game/` | Your extracted game files (git-ignored) |
| `tools/` | `macos/build.sh` (macOS build and disk image), `xiso.py` (disc extractor), `xex_extract.py` (XEX→PE), `dis.sh` (PPC disassembly), `find_data_funcs.py` (pointer-only function recovery), `salvage_copy.py` (resumable copy plus SHA-1 check), `run_capture.ps1` / `contact_sheet.ps1` (test runs with screenshots), `disk_usage.ps1` |
| `user/` | Saves and the shader cache, created at runtime (git-ignored) |
| `docs/` | [RESEARCH.md](docs/RESEARCH.md), [ROADMAP.md](docs/ROADMAP.md), disc listing, Xenia import list |
| `external/xenos_shader/` | Xenos→DXBC shader translator, geometry shaders, texture layout code and texture load shaders, vendored from the ReXGlue SDK (BSD-3) for the native renderer |
| `third_party/` | `steamworks/sdk`: the Steamworks SDK headers and Steam API libraries (committed). The ReXGlue SDK binaries (downloaded by the build scripts), its source and wiki (git-ignored) |

## Requirements (Windows)

- Visual Studio 2022 Build Tools with **C++** and the **Clang** component (clang 19 builds; ReXGlue recommends 20+)
- CMake 3.25+, Ninja (both ship with VS)
- Python 3 with `cryptography` (only for the tools)
- The ReXGlue SDK: `build.bat` downloads it to `third_party/rexglue-bin/win-amd64` (nightly bd833a2)
- Your own Tenchu Z disc image (never in the repository)

## Setup

```bash
python tools/xiso.py extract "path/to/Tenchu Z.iso" game
build.bat
out/build/win-amd64-relwithdebinfo/tenchuz.exe --game_data_root=game --log_file=logs/boot.log
```

`build.bat` also re-runs codegen automatically when the manifest or `config/*.toml` changes.
To refresh `config/data_funcs.toml` after discovery changes:

```bash
rexglue.exe codegen tenchuz_manifest.toml --ignore-stamp -v --log-file logs/trace.log
python tools/find_data_funcs.py game/default.pe logs/trace.log config/data_funcs.toml
```

## macOS (Apple Silicon)

On the Mac, with the Xcode Command Line Tools:

```bash
tools/macos/build.sh "path/to/Tenchu Z.iso"
```

It installs cmake, ninja, vulkan-loader and molten-vk with [Homebrew](https://brew.sh) when it's
installed, or otherwise from conda-forge into `third_party/mac-deps` (with micromamba, which it
downloads if needed; no password or Homebrew required). It downloads the macOS build
of the same ReXGlue SDK (nightly bd833a2), extracts the disc to `game/`, builds `tenchuz.app`
and packages `dist/TenchuZ-mac.dmg`: **Tenchu Z.app** and **Set Up Game Files**, which extracts
a player's own disc image to `~/Library/Application Support/TenchuZ/game`. Saves, settings
(`tenchuz.toml`) and logs are in that folder too. System Link over Steam is built in (the
repository carries the Steamworks files). `JOBS=<n>` sets the compile jobs (default 4).

The Mac build draws with the SDK's Xenos GPU emulation (Vulkan on MoltenVK). The native D3D12
renderer, its display options and the volume slider are Windows only. The app is signed
ad hoc: on another Mac, right-click it and choose Open the first time (or run
`xattr -dr com.apple.quarantine "/Applications/Tenchu Z.app"`).

The **macOS compile check** workflow (run it from the Actions tab) compiles the game's own
sources for macOS without the game files.

See [docs/ROADMAP.md](docs/ROADMAP.md) for status.
