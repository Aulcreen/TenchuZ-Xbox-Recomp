# Native port plan: removing the emulated layers

Goal: Tenchu Z as a native PC game with **no hardware emulation**: no Xenos GPU command
processor, no EDRAM model, no XMA hardware decoder. Higher frame rates and PC features come with it.

## Where we are (2026-10-18)

| Layer | Today | Target |
|---|---|---|
| Game code (+ statically linked XDK libs: D3D, XACT, WMV, CRT) | Native (recompiled to x86-64) | Native |
| Xbox OS (kernel/XAM) | Native C++ reimplementation (ReXGlue HLE) | Native |
| Memory | Guest layout, big-endian load/store byte swaps | Unchanged (inherent to recompilation) |
| **GPU** | **Emulated**: the game's D3D writes Xenos PM4 packets, and `rexgpu-xenos` interprets them and translates shaders to D3D12 at runtime | **Native renderer** behind the game's D3D API |
| **XMA audio decode** | **Emulated** hardware decoder (FFmpeg behind MMIO) | Native decode at the voice/XACT level |

**Baseline frame rate** (`src/frame_stats.cpp`, counted at D3D Swap): **locked at 30.0 fps**,
the console rate (two 60 Hz vblanks per frame). Worst frames are 36–46 ms in steady state
and 61–110 ms at transitions. The user reports gameplay is smooth but wants a higher frame rate.

## Approach: replace the game's D3D library, not the GPU

This is the model Sonic Unleashed Recompiled pioneered and Dante's Inferno's ReXGlue port
(`florinp93/hells-gate-recomp`, `docs/NATIVE_RENDERER_DESIGN.md`) has shipped:

- Xbox 360 D3D is a **static XDK library** inside `default.xex`, so it is already recompiled
  code. Every function can be overridden with `REX_HOOK_RAW` and the original kept as `__imp__sub_*`.
- The D3D device object keeps a **shadow of the GPU register file** (render state, fetch
  constants, shader constants). At each draw our hook reads that state and draws natively.
  The game never knows.
- Shaders are Xenos microcode. The SDK ships `dxbc_translator`/`spirv_translator`
  (`include/rex/graphics/pipeline/shader/`), which we reuse. Translation can move ahead of
  time later: the `.fxo` effect files plus shaders embedded in the XEX.
- The Xenos path stays as a fallback (`--renderer=xenos`) until the native path matches it everywhere.

### Tenchu Z's D3D layer (first map, static analysis)

| Address | What | Evidence |
|---|---|---|
| `0x8241AD48`–`0x824320F0` | XDK D3D core (all PM4 emission found here) | packet headers built in code |
| `sub_8241BF50` | **D3DDevice_Swap** (present) | only caller of `VdSwap`, `VdGetSystemCommandBuffer`; now hooked for FPS |
| `sub_82427128` | Device creation / ring setup | only caller of `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack` |
| `sub_8242F648`, `sub_8242F9B8` | Engine init, interrupt registration | `VdInitializeEngines`, `VdSetGraphicsInterruptCallback` |
| `0x82425D88` | Graphics interrupt (vblank) callback | runtime log `SetInterruptCallback(82425D88, …)` |
| `sub_8241AD48` | Draw path that emits `DRAW_INDX_2` + `IM_LOAD_IMMEDIATE` | packet constants |
| `sub_82426630`, `sub_8242CE80`, `sub_824320F0` | Fence writes (`EVENT_WRITE_SHD`) | packet constants |
| `sub_8241BDE0`, `sub_8241E768`, `sub_82426498`, `sub_8242AC58`, … | GPU waits (`WAIT_REG_MEM`) | packet constants |
| `sub_822A6C48`, `sub_82560DB8` | Video mode queries (`XGetVideoMode`): frame pacing, resolution | import callers |
| `sub_82410500`, `sub_8250FD70` | Timer frequency (`KeQueryPerformanceFrequency`): game timing | import callers |
| `.fxo` ×12 (`K2XMesh`, `K2XCommon`, `Cloth`, `Grass`, `water`, …) | K2 engine effect files (Xenos shaders) | disc |

## Milestones

Each step ends in a build the user plays. The Xenos path stays the default until N5.

### Track F: frame rate (independent of the renderer, quick win)
Research (2026-10-19) is in **`docs/FRAME_RATE.md`**. The cap is `D3DRS_PRESENTINTERVAL = 2`, set
at `0x8239FF54`. Gameplay mostly scales by a measured frame time (`0x82B57824`, 334 readers),
with a few fixed-step candidates to test at 60 fps.
- **F1** Find the frame cap: the D3D presentation interval (Swap waits for 2 vblanks) and the
  game's own pacing (`XGetVideoMode` / timer callers). Unlock 60 fps.
- **F2** Find the simulation timestep. If gameplay assumes 1/30 s per frame, add delta-time
  scaling or a fixed-step tick with interpolation, and check movement, physics, animation, AI,
  timers, UI, and FMV sync at 60.
- **F3** Higher refresh rates (120/144) and frame-pacing smoothing. Look at the 46–110 ms spikes
  (shader translation stalls and loads).

### Track N: native renderer
- **N0 Map the D3D layer.** ✅ *Started* (table above).
- **N1 Trace.** ✅ *Mostly done (2026-10-18)*. 1,101 generated trace hooks, F8 /
  `--tenchuz_d3d_trace_at_frame` captures, and 23 traces (title, menus, dojo gameplay). The draw,
  state, texture, shader, RT, clear, resolve, tiling and swap entry points are identified, plus
  the per-draw state flush and the D3DX mesh path. See **`docs/D3D_MAP.md`**. Left: confirm the
  LIKELY signatures (argument order of DrawIndexedVertices, Resolve, Clear) and name the
  render-state setters by register.
- **N2 Observer.** ✅ *Done (2026-10-19)*. Our own PM4 decoder (no dependency on the emulator)
  reconstructs the GPU registers at every draw, and `tools/analyze_gpu_observer.py` learned the
  device layout from 5,464 gameplay draws: fetch constants at `+0x400`, VS/PS constants at
  `+0x700`/`+0x1700`, bool/loop at `+0x2700`, and register groups at `+0x2800`–`+0x29EF`. Game
  draws match; the differences are D3D-internal clear/resolve draws (which we'll implement
  natively), the `c255` helper constant, and tiled-pass viewport/surface values that are settled
  visually in N3/N4. See `docs/D3D_MAP.md`. Triggers: F9,
  `--tenchuz_gpu_observe_at_frame=N`, `--tenchuz_gpu_observe_every=N`.
- **N3 Native frame skeleton.** ✅ *Done (2026-10-19)*, in `src/renderer/native_renderer.cpp`:
  - Presents through the SDK's D3D12 presenter (`RefreshGuestOutput`, same device and queue, so
    the overlays keep working). A compute-shader blit copies our front buffer into its output.
  - Mid-asm hook `TenchuzAfterVdSwap` (`0x8241C0E0`) reads the front-buffer address from the
    swap packet and turns the emulator's `XE_SWAP` into NOPs, so the emulator keeps running
    fences, interrupts and pacing but stops presenting. No SDK patch is needed.
  - Native render targets keyed by EDRAM base/format and sized from `BeginTiling` /
    `SetRenderTarget` (untiled, full 1280×720). Hooks on `ClearF`, `Resolve` (into native
    textures keyed by physical address), `SetRenderTarget` and `BeginTiling`.
  - **F10** toggles native output. `--tenchuz_native_output` starts native.
    `--tenchuz_native_shadow` runs it in the background while the emulator presents.
    `--tenchuz_native_log_every=N` / `_log_frames` log the operations
    (`tools/summarize_native_log.py`).
  - Verified: title, menus and FMV frames reproduce the D3D frame structure (tiling, clears,
    per-tile resolves, front buffer) at a steady 30 fps. Draws aren't issued yet, so native
    output shows clear colours. Open: depth resolves are skipped, MSAA isn't modelled, and the
    gameplay pass log will be checked during N4 sessions.
- **N4.1 Shader translation.** ✅ *Done (2026-10-19)*. The SDK's Xenos→DXBC translator is
  vendored in `external/xenos_shader/` and built into the exe. `src/renderer/shader_cache.cpp`
  takes each draw's microcode from the live-decoded command stream (`IM_LOAD` /
  `IM_LOAD_IMMEDIATE`), picks the translation modification like the SDK's pipeline cache, and
  caches by hash. On the title, menus and FMV: 0 failures. Gameplay (N4.2 session): 83 shaders,
  99 translations, 0 failures.
- **N4.2 Native draws.** ✅ *Done (2026-10-19)*. `src/renderer/draw_path.cpp` issues each
  live-decoded draw on the native command list into the bound native RT0 and depth surface. It
  follows the translator's bindful contract: root CBVs for fetch, VS/PS float (packed by the
  shader's constant bitmap), system and bool/loop constants, plus a 512 MB "shared memory" SRV.
  That SRV is an upload-heap mirror of guest physical memory, holding the vertex and index pages
  each draw references (copied once per frame per 4 KB page). Indices are read in place and
  byte-swapped by the VS. Host NDC comes from the device's untiled viewport registers. Draws
  emitted inside `ClearF` / `Resolve` / `EndTiling` are skipped, because the native renderer does
  those itself. The observer now seeds its decoded registers from the device's register shadow
  at start and on every resync. Textures are 1×1 white placeholders (N4.3). Fans, rect lists
  and quads are skipped (N4.4), and culling is off. `--tenchuz_native_debug_textures` swaps the
  white for a UV-gradient checker to check geometry. Title screen: 54 of 55 draws issued
  (the skipped one is a rectangle list), 0 translation or pipeline failures, steady 30 fps. With the
  checker, the background, "Z" logo, "PRESS START BUTTON" bar and copyright line sit exactly
  where the emulator draws them, with correct UV orientation and blending.
  - Gameplay (a 6-minute mission session in shadow mode): 425 draws issued per frame on average,
    750 at most, about 32 per frame skipped for their primitive type, and 0 pipeline failures.
    It stayed at 30 fps outside loading. In the native view the screen is a single checker,
    because the game's last pass is a full-screen quad sampling its resolved scene texture. That
    texture is a placeholder until resolved render targets are bound as textures (first step of
    N4.3).
  - Fixed on the way: a GPU hang (DEVICE_HUNG) on entering a mission. A resolve destination
    (`194A5000`) is resolved at 512×256 and 512×512 every frame, and recreating its texture
    freed one still used earlier in the same command list. Resolve textures are now keyed by
    address and size, and replaced resources are released only after their frame's fence passes.
  - Diagnostics: every native draw, clear and resolve copy is wrapped in
    `WriteBufferImmediate` breadcrumbs. On device removal (fence watcher, or the SDK's abort),
    the log names the unfinished operation with its shaders and buffers.
    `--tenchuz_native_dump_shaders` writes each shader's microcode disassembly to
    `logs/shaders/`, and `--d3d12_debug=true` turns on DRED. F10 ignores keyboard auto-repeat.
- **N4.3 Textures.** ✅ *Done (2026-10-19)*. Native draws sample the game's real textures.
  - Resolved render targets: a fetch whose address and size match a native resolve destination
    binds that texture (else the latest resolve to that address). Resolves now convert to the
    destination format and apply `copy_dest_exp_bias` in a compute pass, and keep the red/blue
    swap. Depth resolves (skipped in N3) copy the depth plane into an R32 texture. A Resolve's
    clear flags (`0x100` / `0x200`) run before the next draw or clear, because tiled passes
    resolve once per tile.
  - Guest textures: `src/renderer/texture_loader.cpp` ports the SDK's D3D12 texture cache load
    path. That covers the format table, the SDK's untile and byte-swap compute shaders (27, vendored as
    DXBC in `external/xenos_shader/shaders/`, with the layout code in
    `external/xenos_shader/texture/`), 2D, array, 3D and cube textures with mips, and signed
    views. Textures are cached by fetch key and reloaded when a hash of their guest memory changes
    (no write watching yet). Not ported: resolution scaling, separate signed loads, stacked 2D
    textures sampled as 3D, and memory budgets.
  - Samplers come from the fetch constants (SDK `GetSamplerParameters`), with descriptor sets
    cached. Swizzles follow the SDK's `GuestToHostSwizzle`. A texture that can't be bound now
    reads as zero, like the SDK's null descriptors, instead of white.
  - Primitive types, pulled forward from N4.4: rectangle lists go through the SDK's geometry
    shader (`external/xenos_shader/geometry_shader.cpp`), and fans and quad lists are drawn as
    triangle lists with generated indices.
  - Dynamic geometry: vertices and indices in the D3D's command ring (BeginVertices/EndVertices:
    2D, HUD and full-screen passes) are rewritten within a frame, so each draw copies them into
    a per-frame arena. Recording a frame now waits for the previous one, because the
    guest-memory mirror is updated in place. `--tenchuz_native_ring_flush` is an alternative
    that submits mid-frame when a ring page changes.
  - Decoding: the observer hooks the D3D's kick and segment switch (`sub_82426DF0`,
    `sub_8242A8B8`), decodes only plausible packets, and resumes past the per-tile blocks the D3D
    reserves, so draws stay in order with the clears and resolves around them. Shader analysis
    runs under an exception handler and rejects mis-decoded loads.
  - Tools: **F11** / `--tenchuz_native_dump_at_frame=N` write every resolved image of a frame
    to `logs/frame_dumps/` (PPM, depth as PGM). The frame log adds texture, decode and
    draw-kind counters. `tools/send_keys_test.ps1` gains `BURST` and `HOLDxx`, and both capture
    scripts use `PrintWindow`.
  - Verified with this build: menus, loading screens and a mission in native output at 29.7 fps.
    A gameplay frame: 584 draws, 558 issued, 0 skipped for primitive type, 0 pipeline failures.
    Shaders: 75 unique, 102 translations, 0 failures. Texture fetches: 462 resolved targets,
    1,745 guest textures, 1,279 placeholders, 0 unsupported formats. The intro movie plays
    natively (6 guest textures a frame, 30 fps). In A/B captures native output looks darker
    than the emulator's; that's not explained yet.
- **N4.4 Remaining pipeline state.** *Next.* Culling (draws use `CullMode NONE`), multiple render
  targets (only RT0 is bound), stencil, MSAA, and the darker output (the native present has no
  gamma-ramp step; check that first). The 1,279 placeholder fetches per gameplay frame need a
  count by reason: non-texture fetch constants, stacked 2D sampled as 3D, signed resolved
  targets. Texture invalidation should move from hashing (about 33 MB per gameplay frame) to
  write watching. A same-frame A/B dump in shadow mode (emulator front buffer next to the native
  one) would make parity measurable.
- **N4 Draws.** Shaders (SDK translator → DXBC/DXIL, cached by microcode hash), vertex/index
  buffers (byte-swapped), textures (untile + upload, cached), samplers, blend, depth, and cull.
  Exit: menus and the dojo match the Xenos path.
- **N5 Parity + remove the ring.** All missions match. Complete fences on the native GPU timeline,
  turn the ring kick into a no-op, and disable the GPU plugin. **The GPU emulation is gone.**
- **N6 PC rendering features.** Internal resolution scaling, MSAA/SMAA, widescreen/ultrawide FOV,
  and HUD scaling.

### Track A: native audio (low priority: audio already works)
- **A1** Map XACT/XAudio usage: which voices take XMA from the `.xwb` banks.
- **A2** Decode XMA at the voice level (or pre-convert banks to PCM/Opus at install) and drop the
  XMA hardware model.

### Not changing
- Big-endian guest memory and byte-swapped access: part of what recompiled code is.
- Kernel/XAM HLE: already native C++. We extend it as needed (as with `profile_fix.cpp`).

## Decisions for the user

1. **Order:** frame rate (Track F) first for a quick visible win, or the renderer (Track N) first?
2. **Backend:** D3D12 only (Windows, simplest, the RX 6600 is fine), or Vulkan for Linux/Steam Deck too?
3. **Fallback:** keep `--renderer=xenos` available during development (recommended).

## References
- Dante's Inferno ReXGlue port: `docs/NATIVE_RENDERER_DESIGN.md`, `docs/NATIVE_D3D_MAP.md`,
  `docs/frame_rate.md` (github.com/florinp93/hells-gate-recomp)
- Burnout Revenge "Xerenge": ReXGlue plus a standalone renderer (plume) and XenosRecomp
  ahead-of-time shaders (github.com/shipa-2/Xerenge)
- Ace Combat 6 recomp: FPS/physics fix, widescreen, KB+M (github.com/sal063/AC6_recomp)
- XenonRecomp / XenosRecomp / plume (hedge-dev, Sonic Unleashed Recompiled)
