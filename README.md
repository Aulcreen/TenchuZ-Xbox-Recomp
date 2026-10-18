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
| `tools/` | `xiso.py` (disc extractor), `xex_extract.py` (XEX→PE), `dis.sh` (PPC disassembly), `find_data_funcs.py` (pointer-only function recovery), `salvage_copy.py` (resumable copy plus SHA-1 check), `run_capture.ps1` / `contact_sheet.ps1` (test runs with screenshots), `disk_usage.ps1` |
| `user/` | Saves and the shader cache, created at runtime (git-ignored) |
| `docs/` | [RESEARCH.md](docs/RESEARCH.md), [ROADMAP.md](docs/ROADMAP.md), disc listing, Xenia import list |
| `third_party/` | ReXGlue SDK source, prebuilt binaries, and wiki (git-ignored) |

## Requirements (Windows)

- Visual Studio 2022 Build Tools with **C++** and the **Clang** component (clang 19 builds; ReXGlue recommends 20+)
- CMake 3.25+, Ninja (both ship with VS)
- Python 3 with `cryptography` (only for the tools)
- ReXGlue SDK in `third_party/rexglue-bin/win-amd64` (from the SDK's GitHub releases)

## Setup

```bash
python tools/xiso.py extract "path/to/Tenchu Z.iso" game
python tools/xex_extract.py game/default.xex game/default.pe
third_party/rexglue-bin/win-amd64/bin/rexglue.exe codegen tenchuz_manifest.toml
build.bat
out/build/win-amd64-relwithdebinfo/tenchuz.exe --game_data_root=game --log_file=logs/boot.log
```

`build.bat` also re-runs codegen automatically when the manifest or `config/*.toml` changes.
To refresh `config/data_funcs.toml` after discovery changes:

```bash
rexglue.exe codegen tenchuz_manifest.toml --ignore-stamp -v --log-file logs/trace.log
python tools/find_data_funcs.py game/default.pe logs/trace.log config/data_funcs.toml
```

See [docs/ROADMAP.md](docs/ROADMAP.md) for status.
