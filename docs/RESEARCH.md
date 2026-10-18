# Research notes

Findings gathered on 2026-10-18 while bootstrapping the project.

## The game

| | |
|---|---|
| Title | Tenchu Z (USA, Europe) (En,Fr,De,Es,It), Redump XGD2 image ([redump #11829](http://redump.org/disc/11829/)) |
| ISO | 7,835,492,352 bytes, SHA-1 `621cc9958945d1572fd74ad47141549ead985d8b`, CRC32 `b26af6db` |
| `default.xex` | SHA-1 `dde36f938ffb0b3226af32d4d9ff10e6750c5a0b` (decrypted PE: `82a70258…48b8`) |
| Title ID | `465307D7` |
| Media ID | `7A0ADB06` |
| XEX build time | 2007-04-28 03:32:30 UTC, version 0.0.0.1 |
| Developer | K2 LLC (FromSoftware JP / Microsoft Game Studios WW) |
| XDK | 2.0.2920 (static libs: XAPILIB, D3D9, D3DX9, XGRAPHC, XNET, XAUD, XACT, X3DAUD, XMP, LIBCPMT, XHV) |
| Entry point | `0x82410E68`, image base `0x82000000`, stack 256 KiB |
| Imports | 87 from `xam.xex`, 128 from `xboxkrnl.exe` (full list: [xenia/imports.txt](xenia/imports.txt)) |
| XEX format | Retail AES encryption, *basic* (zero-block) compression, not LZX |
| Title update | none on disc; `$SystemUpdate/su20076000_00000000` is a dashboard update |

### Image layout (from the extracted PE)

| Section | VA | Size |
|---|---|---|
| `.rdata` | `0x82000600` | 2.4 MB |
| `.pdata` | `0x8224DE00` | 77 KB (function table, which makes boundary detection reliable) |
| `.text` | `0x82270000` | 5.6 MB |
| `.embsec_*` ×7 | `0x82809E00` | precompiled D3D/XGraphics data |
| `.data` | `0x82830000` | 12 MB |
| `.reloc` | `0x83430200` | 350 KB |

### Disc contents (2,131 files, ~4.5 GB), full listing in [disc_listing.txt](disc_listing.txt)

| Ext | Count | Size | What it is |
|---|---|---|---|
| `.bin` | 54 | 2.2 GB | K2 engine stage/model packs (`DATA/MODEL/STG/NN/STNN.bin`, `CHAR.bin`, `WEAPON.bin`) |
| `.wmv` | 41 | 1.1 GB | FMV cutscenes (WMV, decoded in-game by XMV/WMV code in the XEX) |
| `.xpr` | 1188 | 900 MB | Xbox Packed Resource, standard XDK texture container |
| `.xwb/.xsb/.xgs` | 94 | 142 MB | XACT wave banks / sound banks / global settings, XMA audio |
| `.x` | 292 | 16 MB | DirectX `.x` meshes |
| `.m`, `.khd`, `.k2m`, `.k2ec`, `.k2es`, `.sad`, `.edg`, `.squ`, `.set`, `.dor`, `.bgo` | ~400 | ~40 MB | K2 proprietary formats (motion, scripts, effects, stage layout) |
| `.fxo` | 12 | 0.3 MB | Compiled Xenos shaders (effect objects) |

No public reverse-engineering documentation for Tenchu Z's own formats was found;
those formats are original work for this project.

### Xenia behaviour (from `H:\xenia_canary\xenia.log`, canary build dc29307a5, Aug 2023)

- Boots and runs. Xenia Canary netplay notes list Tenchu Z as "buggy but works".
- 82/87 xam and 124/128 kernel imports were implemented; the 9 unimplemented ones are candidates for stubs.
- Repeated `PM4_DRAW_INDX_2(3, 8, 2): Failed in backend` / `Resolve region is empty`:
  draws with an empty resolve rectangle. Expect the same in ReXGlue's Xenos backend (Xenia-derived).

## Tooling landscape

### ReXGlue SDK (chosen)
- https://github.com/rexglue/rexglue-sdk, wiki https://github.com/rexglue/rexglue-sdk/wiki
- Static recompiler **plus a full runtime**: kernel/XAM HLE, Xenos GPU (D3D12/Vulkan),
  XMA audio, XInput, VFS. It grew out of Xenia and XenonRecomp.
- Very active: ~1.1k stars, nightly builds (we use `nightly-20261002`, 0.10.0.24).
- Needs Clang (20+ recommended; we test with VS Build Tools' clang 19.1.5), CMake 3.25+, Ninja.
- Note: the wiki lags the nightly CLI. The current `init` takes `--project-name/--xex-path/--game-root`
  and uses a `*_manifest.toml` with `[entrypoint].includes` for extra config files.

### XenonRecomp (background)
- https://github.com/hedge-dev/XenonRecomp: the recompiler behind Unleashed Recompiled.
  It only generates code and has no runtime. ReXGlue supersedes it for our purposes.
- Its README documents the techniques we rely on: save/restore helper detection,
  jump tables, mid-asm hooks, and setjmp/longjmp redirection.

### Reference ports built on ReXGlue (study these for patterns)
- `sal063/AC6_recomp` (Ace Combat 6, 2007 Japanese title): widescreen, FPS/physics fix, KB+M, texture overrides
- `florinp93/hells-gate-recomp` (Dante's Inferno): native D3D renderer design docs, launcher, VP6 FMV fix
- `psxrestore/Condemned2Recomp`: clean hooks/input/settings split, in-app XDVDFS extractor
- `birabittoh/NocturneRecomp`, `birabittoh/KameoRePowered`: Windows and Linux builds
- `SunJaycy/GoldenEye-Recomp-rexglue`, `masterspike52/reNut`, `SkiddyToast/Crackdown`
- `birabittoh/GoopieLauncher`: generic launcher for ReXGlue ports
- Community thread: https://evoweb.uk/threads/xbox-360-recompilations.106908/

## First codegen result

- Pass 1: 4 `UnresolvedCall` errors, all branch targets missing `.pdata` entries:
  C++ this-adjustor thunks and frameless leaf functions reached only by tail branch.
  Fixed with explicit sizes in [`config/default.toml`](../config/default.toml).
- Pass 2: **clean**. 102 TUs, about 105 MB of C++. `__savegprlr_*`/`__restgprlr_*`/`fpr`/`vmx`
  helpers were found automatically.
- `setjmp` = `0x82657B00` (found by searching for `stfd f14,0(r3); stfd f15,8(r3)`) and
  `longjmp` = `0x826578C0` (one of two `RtlUnwind` callers; the other, `0x826662B0`, is SEH
  dispatch). These map to 7 `ppc_setjmp` and 21 `ppc_longjmp` call sites.
- Of the 9 imports Xenia marked unimplemented, ReXGlue only really implements `ExThreadObjectType`.
  `RtlCaptureContext`, `RtlUnwind` and `__C_specific_handler` are logging stubs
  (`kernel/xboxkrnl/xboxkrnl_rtl.cpp`). This is fine as long as `longjmp` goes through the native
  `ppc_longjmp` and the game doesn't rely on SEH. The rest (Xbox Live, QoS, gamer card, stats
  enumeration) are only in the export table and may need stubs.
- **No RTTI.** ReXGlue's vtable scanner finds vtables through MSVC RTTI Complete Object Locators,
  and it found 0 here. Frameless leaf functions without `.pdata` that are referenced only from
  vtables (virtual-call thunks, this-adjustors, getters) were never generated, so the first boot
  died with `Call to invalid or unregistered function at 0x82510138`.
  `tools/find_data_funcs.py` scans `.rdata`/`.data` for pointers into `.text` that fall outside
  every known function and sizes each one by control-flow walk. It found 87, written to
  `config/data_funcs.toml`. Note: for functions without `.pdata`, the trace's `funcEnd` is the
  search-region end, not the function end, so the tool uses the instruction count instead.
  Rerun it after any change that affects discovery.
- ReXGlue's own `lis/addi` function-pointer scan is disabled upstream (`analyze.cpp`, "too many
  false positives"). Our scanner accepts a code-built pointer only if it starts right after a
  function end (the previous word is `blr`, `bctr`, `b`, or padding). It must also skip unpatched
  XEX import thunks (`0x01xxxxxx`/`0x02xxxxxx` records), which otherwise cause undefined
  `sub_82809Dxx` link errors.
- `sub_8248D898` is a genuine 285 KB function (unoptimized, 3.6 KB stack frame), probably a
  table or menu initializer. It produces 5.3 MB of C++ and will dominate compile time.

## First full boot (2026-10-18)

- Sequence: K2 LLC splash, then the intro FMV (WMV decoded by game code), then the title screen
  "PRESS START BUTTON". No visible rendering problems on an AMD RX 6600 (D3D12).
- Pressing Start crashed with a null `memcpy` (guest backtrace `sub_82654070` ← `sub_82282F20`
  ← `sub_8229D508` ← `sub_822A5CB8` ← `sub_822A1E30` ← `sub_8228E868` ← `xstart`).
  - `sub_8255EAB0` is the XAPI `XUserReadProfileSettings` wrapper. The game reads 22 settings
    (table at `0x82002898`), including `XPROFILE_TITLE_SPECIFIC1..3` (`0x63E83FFF/FE/FD`, 1000-byte binary).
  - For `TITLE_SPECIFIC1` the game checks `source == XSOURCE_DEFAULT (1)` and otherwise copies
    20 bytes from `pbData`. ReXGlue (`xam_user.cpp`) reports never-written settings as
    `XSOURCE_NO_VALUE (0)` with no data, so `pbData` was NULL.
  - Fix: `src/profile_fix.cpp` hooks the wrapper and rewrites source 0 to 1 after the call.
- Storage: user data and the shader cache go to `<root>/user` (`TenchuzApp::OnConfigurePaths`).
  Game data and the ISO use NTFS LZX (`compact /exe:lzx`): `game/` takes 2.2 GB on disk instead
  of 4.5 GB, and the ISO 4.7 GB instead of 7.5 GB. `.wmv`/`.xwb` are left alone (already compressed).

## First mission (2026-10-18)

- With the Start crash fixed, an 8-minute session went title screen → opening cinematic →
  character/partner creation → Game Settings → mission load screen → **gameplay** (training dojo:
  HUD, tutorial prompts, player model, particle effects). The process stayed stable at ~850 MB RAM.
- The first attempt went black when a mission started. The log was flooded with
  `Texture fetch constant (0000D800 00000000 00000000 01280000 00000003 00000000) has "invalid" type`
  (3,422 times). `gpu_allow_invalid_fetch_constants=true` (also a Xenia flag) fixes it, and it is now
  the default (`TenchuzApp::OnPostSetup`; the cvar belongs to the GPU plugin, so it can only be set
  after the plugin loads).
- The remaining GPU errors are `Resolve region is empty` + `PM4_DRAW_INDX_2(3, 8, 2): Failed in backend`
  (edram_mode=6, i.e. a resolve/copy). This is the same pair the user's 2023 Xenia log shows.
- Tooling: `tools/run_capture.ps1` (launch, window screenshots at set times, stop),
  `tools/contact_sheet.ps1` (grid of a run's screenshots), and `tools/disk_usage.ps1` (real on-disk sizes).
  Screenshots copy the screen region, so they show whatever window is on top.
