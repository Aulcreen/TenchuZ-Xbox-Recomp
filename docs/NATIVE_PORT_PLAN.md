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
- **N4.4 Remaining pipeline state.** ✅ *Done (2026-10-19)*. In A/B screenshots of character
  creation, the Ninja Village and Mission 1, native output matches the emulator: mean colour
  within 2/255, and sharpness within 3% (mean edge strength). Every draw is issued (570 of 570 in
  the mission), with 0 real placeholder textures and no texture hashing.
  - Comparing: start in emulator output, take its screenshot, then F10 to native (the reverse
    order is invalid, see the last item). A same-frame comparison through guest memory isn't
    possible: the emulator keeps its resolves in its own GPU memory.
  - Route into Mission 1 (Path of the Ninja), after the Ninja Village route (~64 s):
    1. Hold W 2.5 s to reach "Accept mission", then press A six times (accept, go on mission,
       mission select, details, yes, depart), 2.6 s apart.
    2. About 11 s later, Start skips the intro text.
    3. About 11 s after that, A then Start: the game ends up paused, and Start resumes.
  - Done (2026-10-19): **colour parity.** In A/B screenshots (emulator, then F10 to native), the
    mean colour matches within 2/255 in character creation and the Ninja Village. Before, the 3D
    scene was red/blue swapped and the image darker. Two causes:
    - **Resolves ignored `RB_COPY_DEST_INFO`.** The value read was always 0. Resolve keeps the
      register in the device (`+0x29A4`) only while it runs and writes 0 there before returning.
      In tiled passes, its packets go to per-tile blocks the observer can't decode yet. It is now
      read from the device at the D3D calls Resolve makes, through a new observer callback. The
      game's colour resolves are `01000302`: red/blue swap, k_8_8_8_8, 8-in-32. The swap, the
      destination format and the exponent bias now apply.
    - **No display gamma ramp.** `sub_8242A4F8` (called by `sub_8241BC70`) writes a D3DGAMMARAMP
      to the 256-entry LUT. It is hooked, and the present blit applies the table like the SDK's
      `apply_gamma_table`. The game's ramp lifts black to 44/1023 and brightens shadows. The
      game sets it at startup, before native decoding starts. The 10-bit PWL ramp
      (`sub_8242A5D8`, for 2_10_10_10 front buffers) isn't handled, and Tenchu doesn't use it.
  - Done: **rasterizer state**, following the SDK pipeline cache's RTV path:
    - cull mode and front face from `PA_SU_SC_MODE_CNTL`, with draws culling both faces dropped;
    - wireframe for line and point polygon modes;
    - depth bias from the polygon offset of the face that isn't culled (integer units for our
      D32_FLOAT depth match unorm24's only for z in [0.5, 1));
    - depth clip from `PA_CL_CLIP_CNTL`.
  - Done: **EDRAM mode**, like the SDK's `IssueDraw`. Draws in copy mode (resolves) aren't
    drawn, and only `kColorDepth` runs the pixel shader (`kDepthOnly` writes depth alone).
  - Done: **multiple render targets and stencil**, following the SDK's RTV path. A draw binds
    each of RT0-3 that both the pixel shader writes and `RB_COLOR_MASK` enables (the SDK's
    normalized colour mask). Unbound targets before the last bound one get a null RTV.
    - Each target has its own blend, write mask, exponent bias and gamma conversion.
    - Depth and stencil come from the SDK's normalized `RB_DEPTHCONTROL`: front and back ops,
      masks and reference from `RB_STENCILREFMASK`, and `_BF` when only back faces are drawn.
    - A Resolve's depth clear keeps the stencil value (`RB_DEPTH_CLEAR` bits 0-7).
    - Draws with no colour target, i.e. depth-only passes, are no longer dropped: 14 per Ninja
      Village frame, now 325 of 325 issued.
    - Character creation and the Ninja Village use neither MRT nor stencil (the frame log now
      counts both), so those paths aren't exercised there yet. The A/B colour match is
      unchanged.
  - Done: **placeholder texture fetches, by reason.** Each logged frame now has a "NATIVE
    placeholders" line (not a texture, no address, unsupported format, load failed, no signed
    view, stacked 2D as 3D, other dimension). In the Ninja Village, every placeholder was the
    signed binding of a fetch with no signed component. The translator declares a signed and an
    unsigned binding per fetch and samples the signed one only for signed components.
    - Those bindings now get the placeholder without a texture lookup or load, as the SDK only
      loads signed views when needed. They're counted apart as "unused signed bindings".
    - Village frame: 0 real placeholders. 215 bindings had been loading textures from stale
      guest memory at resolve addresses (26 bogus cached textures), and texture hashing dropped
      from 14.4 to 13.4 MB per frame. The image is identical.
    - Mission 1: 1,731 unused signed bindings per frame and 0 real placeholders, so the earlier
      1,279 "placeholders" per gameplay frame were these bindings.
  - Done: **MSAA**, like the SDK's RTV path, with D3D12's own 2x/4x sample positions. Render
    targets and depth buffers take the sample count from `RB_SURFACE_INFO`, when the host supports
    it for every target format (`--tenchuz_native_msaa=false` turns MSAA off).
    - Resolves from multisampled targets run in a compute pass. Colour averages or picks samples
      per `RB_COPY_CONTROL.copy_sample_select`, captured during Resolve like `RB_COPY_DEST_INFO`
      (the game averages both samples: `k01`). Depth takes one sample.
    - The system constants gain alpha to coverage and the sample count.
    - The game's 3D passes are 2x MSAA, and their edges now match the emulator's anti-aliasing.
  - Done: **the half-pixel offset.** With Direct3D 9 pixel centres (`PA_SU_VTX_CNTL.pix_center`),
    the viewport offset gains half a pixel, like the SDK's `GetHostViewportInfo`.
    - Without it, the game's full-screen passes sampled exact texels where it means to sample
      between them, so native output was sharper than the emulator's.
    - Character creation, mean edge strength: emulator 5.98; native 9.4-10.0 before, 5.97 after.
      Mission 1: 2.04 against 1.99.
  - Done: **texture write watching** (`--tenchuz_native_texture_watch`, on by default; false
    compares hashes as before). The texture loader registers an SDK physical-memory invalidation
    callback.
    - A guest write to a watched page stamps it with a sequence number. A texture reloads only if
      one of its pages is newer than its last load, and re-watches its pages before copying them.
    - The callback returns the exact page range, so other subscribers (the emulator) can't
      unprotect pages it still watches.
    - Mission 1: texture hashing went from 32.6 MB per frame to 0, with no reloads after the
      first load and an identical image. The intro movie (textures rewritten every frame)
      reloads its 3 textures each frame and plays.
    - The frame log's "written pages" counts every page the SDK reported written, including
      pages only the emulator watches.
  - Found: **the emulator's state degrades while native output is on.** The swap packet is turned
    into NOPs, so the SDK's `IssueSwap` never runs. That function is the only place its frame
    closes. Per-frame state isn't reset, for example `vertex_buffers_in_sync_`. Its upload pools
    are never reclaimed: non-local GPU and private memory grow about 3.6 MB/s. After a native
    stretch, F10 back to the emulator shows stale geometry (title-menu leftovers in character
    creation). For A/B, take the emulator shot first. Fixing this needs an SDK change, or N5.
  - Fixed (2026-10-19): the native flicker. In the Ninja Village, a few frames out of every 16
    turned blue and blurred. The village water is one 32-bit triangle strip (9,417 indices) that
    restarts its rows with index 0xFFFF; Xenos takes the restart value from
    `VGT_MULTI_PRIM_IB_RESET_INDX`, but D3D12 only cuts strips at all-ones. Each restart became
    a real vertex far past the vertex data, and as the waves moved, the triangles to it covered
    the screen up to 47 times. Strip draws with any other restart value now get a copy of their
    indices with that value rewritten to all-ones, like the SDK's primitive processor. Village
    bursts: 11 glitched frames in 140 before, 0 after (the emulator: 0).
  - Diagnostics added on the way (all off by default): `--tenchuz_native_log_draws` (every draw
    and operation of a logged frame, in order), `--tenchuz_native_draw_queries` (per-draw
    occlusion query; samples in the draw list) with `--tenchuz_native_log_coverage=N` (every
    frame, the draws that passed more than N samples), `--tenchuz_native_trace_vs=<hash>` (every
    constant, vertex and index range of one shader's draws), `--tenchuz_native_check_late_writes`
    (guest data changed after we copied it), `--tenchuz_native_recheck_pages` (static pages
    rewritten within a frame), and `--tenchuz_native_dump_frames=N` (F11 and
    `--tenchuz_native_dump_at_frame` dump and log N consecutive frames). Scripted input needs
    `--mnk_mode=true`. For GPU device removals, `--tenchuz_dred=true` adds DRED's unfinished
    command lists and page-fault allocations to the removal report.
- **N4 Draws.** Shaders (SDK translator → DXBC/DXIL, cached by microcode hash), vertex/index
  buffers (byte-swapped), textures (untile + upload, cached), samplers, blend, depth, and cull.
  Exit: menus and the dojo match the Xenos path.
- **N5 Parity + remove the ring.** All missions match. Complete fences on the native GPU timeline,
  turn the ring kick into a no-op, and disable the GPU plugin. **The GPU emulation is gone.**
  ✅ *Done (2026-10-20); the default since 2026-10-20*. `--tenchuz_native_gpu=false` runs the
  emulator instead (with F10 between its output and native output).
  - `src/renderer/native_gpu.cpp` replaces the Xenos plugin as the runtime's graphics system
    (`RuntimeConfig::graphics`, so `rexgpu-xenos` isn't loaded). It provides:
    - the D3D12 device and presenter the native renderer draws through;
    - the GPU's MMIO registers;
    - vblank interrupts at the video mode's refresh rate;
    - a command processor for the ring.
  - The command processor executes only what the game can observe:
    - register writes, memory writes, fences (`EVENT_WRITE_SHD`) and waits;
    - interrupts, the swap counter, the ring read pointer and its write-back.
  - Draws, resolves and swaps in the ring are skipped, because the native renderer records them
    from the D3D calls. Occlusion queries report a fixed 1000 samples (`EVENT_WRITE_ZPD`) or
    "visible" (`VIZ_QUERY`). An opcode it doesn't execute is logged once.
  - Fences complete when the command processor reaches them, not on the native GPU timeline.
    That's safe because the native renderer copies the guest data it draws with while recording,
    and waits for the previous frame before recording the next.
  - In native GPU mode, native output is forced on and F10 can't switch back. The swap packet is
    left alone, since nothing emulates it.
  - Verified: the title, menus, character creation, the Ninja Village and Mission 1 render
    correctly.
    - The Mission 1 frame matches the emulator reference: mean colour 39.6/39.5/39.2 against
      39.5/39.5/39.1, and edge strength 2.00 against 2.04.
    - A 4-minute mission soak held 30 fps, with no errors or unexecuted opcodes, and flat
      non-local GPU memory (645 MB). The never-closed emulator frame and its 3.6 MB/s leak are
      gone.
  - Cost in the Ninja Village, emulator plus native output against native GPU:
    - private memory: 3.5 GB against 1.25-1.3 GB;
    - GPU 3D engine: 35% against 21% busy;
    - CPU: 1.27-1.30 against 1.14-1.25 cores.
  - Other missions (2026-10-20), before making native the default. A test copy of the save has
    every mission marked completed; the real save is untouched (`--user_data_root` on a copy).
    - Save layout (`tenchu.bin`, 5216 bytes, big-endian, no checksum):
      - load and save go through the 5216-byte buffers at `0x82B01D40` (file I/O) and
        `0x82B008D0` (live);
      - the missions are 8 chapter sections of 488 bytes from offset `0x500`: section number,
        count, then 48-byte entries;
      - an entry is id, flags, `0x14`, then three per-difficulty 12-byte records. Each record's
        first word has bit 0 "cleared" (`0x822B76A0`) and bit 1 "completed", the bit the unlock
        rule (`0x822B79D0`) checks on a mission's prerequisite in the same chapter;
      - the 44 12-byte records at `0x1E0` are inventory (count, obtained), not missions.
    - Missions 4 ("Gather Information"), 12, 25, 38 ("Wipe Out the Enemy", kill-counter HUD)
      and 47 ("Hammer of Judgment", in-engine cutscene with subtitles) render correctly
      natively: intro text scenes, cutscenes, gameplay and the pause menu, at 30 fps, with no
      errors and no frame jumps in the per-present check.
  - Not covered:
    - real occlusion query results;
    - CPU reads of resolved images from guest memory: native resolves stay in native textures.
  - For A/B against the emulator, run with `--tenchuz_native_gpu=false --tenchuz_native_output=true`
    and take the emulator shot first.
  - Fixed (2026-10-20): **flicker and glitches** in the menus, loading screens, missions and the
    pause menu. All four came from the live command-stream decoding:
    - **Black frames in menus and loading screens**, about every 0.75 s. The D3D keeps dynamic
      vertices (BeginVertices) at the top of its 4 MB command ring, above its last segment, where
      the write pointer never goes. `InCommandRing` only knew the range the write pointer had
      covered, so those draws took the once-per-frame static upload and read stale vertices.
      The ring's bounds now come from the device (`+0x34C8` start, `+0x34CC` last dword).
    - **Stray geometry in missions** (stretched triangles and lines, one frame at a time).
      - At a segment switch the observer finished the old segment up to its limit plus the
        trailer reserve. But the D3D switches when the next write doesn't fit, so past its last
        write the old segment holds stale packets from the ring's previous lap.
      - Those stale draws were replayed with the current state: an index buffer drawn with
        another mesh's vertices. The old segment's tail is no longer decoded; everything before
        a switch has been decoded at the hooked call that made it.
    - **Pause menu: the scene behind it rendered wrong every 22 frames** (each ring lap).
      - At a kick's end the observer decoded what the kick itself wrote. Near the ring's wrap,
        a kick header parses as a register write: `PA_CL_VTE_CNTL = 0x300`, screen space.
      - The D3D never undoes that write, since it isn't GPU state in its model. So the
        following 3D draws lost their viewport transform and wrote depth everywhere.
      - Nothing a kick writes is decoded any more, and the 512-byte back-scan after kicks is
        gone. That scan had also replayed stale packets.
    - **A crash in pipeline creation**: the driver's DXBC conversion read a null pointer on a
      shader from a mis-decoded load. Pipeline creation now runs under an exception guard and
      fails that pipeline instead.
    - Verified with a per-present brightness check (`--tenchuz_native_check_presents`), from
      the title through Mission 1 and its pause menu, at 16:9 and 21:9: no frame jumps left
      except the intro logo's own animation and the mission-select confirm flash. Holds 30 fps.
      The black frame entering the village was a kick artifact too.
    - Diagnostics added, all off by default:
      - `--tenchuz_native_check_presents`: the brightness of each presented frame and of the
        presenter's output, in four bands;
      - `--tenchuz_native_log_presents`: per frame, the front buffer, its last resolve, and
        the frame's draw, resolve and clear counts;
      - `--tenchuz_native_log_offscreen`: screen-space draws whose vertices reach far off
        screen;
      - `--tenchuz_native_trace_syncs_at` / `_frames`: the observer's syncs, kick ends and draw
        packets for a window of frames;
      - more draw-list fields (`--tenchuz_native_log_draws`): NDC mapping, a hash of the VS
        constants, packet address, and the bound colour and depth surfaces.
- **N6 PC rendering features.** Internal resolution scaling, MSAA/SMAA, widescreen/ultrawide FOV,
  and HUD scaling.
  - **N6.1 Internal resolution scaling.** ✅ *Done (2026-10-20)*.
    `--tenchuz_native_resolution_scale=N` (1-4, changeable while playing) renders natively at N times the
    game's resolution on each axis, like the SDK's draw resolution scale:
    - Render targets, depth buffers and resolve destinations are N times larger. They keep their
      guest size for lookups, so texture fetches still find resolves by address and size.
    - Draws map the guest viewport onto the larger target.
    - The translator is built with the scale. It divides pixel positions by it, and for fetches
      from resolved (scaled) textures it multiplies unnormalized coordinates, sizes and offsets
      by it (the `textures_resolution_scaled` system constant).
    - The slope-scaled depth bias is multiplied by N, like the SDK's ROV path, so the bias per
      guest pixel stays the same.
    - The front buffer goes to the presenter at full size, and the presenter fits it to the
      window.
  - Verified in Mission 1 without GPU emulation, in a 1920×1080 window:
    - 2x: the resolves are 2560×1440, with 578 of 578 draws issued. Mean colour matches 1x
      (39.7 against 39.5). The window image is sharper (edge strength 3.39 against 1.99), and
      the HUD and 3D scene gain detail without moving.
    - Cost on this PC (8 GB GPU): 1x holds 30 fps with the GPU 22% busy. 2x holds 30 fps at
      35%. 4x (5120×2880) drops to about 25 fps at 82%.
    - The title menu and pause screen also draw correctly at 4x.
  - **N6.2 Anti-aliasing.** ✅ *Done (2026-10-20)*. Two options, both off by default:
    - `--tenchuz_native_msaa_samples=4` renders the game's multisampled targets (its 2x 3D
      passes) with 4x MSAA. The pipelines, alpha to coverage and the sample-count system
      constants follow the host count. A resolve that averages every guest sample (the game's
      `k01`) averages all four.
    - `--tenchuz_native_post_effect=fxaa` (or `fxaa_extreme`) runs FXAA on the presented image,
      like the emulator's `swap_post_effect`, which lives in the GPU plugin and is gone without
      it:
      - The present blit writes the image with its luma in alpha, like the SDK's
        `apply_gamma_table_fxaa_luma`, to a 16-bit intermediate.
      - The SDK's FXAA shaders (vendored bytecode) filter that into the presenter's output.
      - It runs at the output's size, so after resolution scaling.
    - Verified in Mission 1 at 1x: both hold 30 fps (GPU 23% busy, against 22%) with no errors.
      - 4x MSAA: resolves read four samples. Mean colour matches (33.5 against 33.6), and
        geometry edges gain intermediate shades, though the change is subtle: the game's
        full-screen passes already soften its image, and resolution scaling does far more.
      - FXAA: same mean colour, with visibly smoother edges. It also softens some texture
        detail (edge strength 1.88 against 1.99).
    - SMAA isn't done: it needs its precomputed area and search textures, which aren't in the
      SDK.
  - **N6.3 Wider aspect ratios (Hor+) and the HUD.** *In progress (2026-10-20)*.
    `--tenchuz_aspect_ratio=21:9` (W:H or a number, 4:3 to 48:9; default 16:9, which changes
    nothing). Code is in `src/widescreen.cpp`.
    - The game's aspect ratio is one read-only constant, 16/9 at `0x8200B73C`.
      - The startup display setup `sub_822A6AE8` copies it to `0x829BF1B4`, which overwrites the
        4:3 default there.
      - Camera init `sub_823E0F90` reads it through `sub_822A6C30` into the camera block at
        `0x8340BD40`: far `+0`, aspect `+8`, FOV `+C`, `+30`.
      - `sub_823E1128` builds the projection with a vertical FOV: y scale cot(fov/2), x scale
        that over the aspect.
      - Four rendering functions also load the constant directly (`0x8237BCE8`, `0x82387264`,
        `0x82388B0C`, `0x82389190`), as does the movie player (`0x82560E54`).
    - `OnPostLoadXexImage` overwrites the constant before any game code runs. The game renders
      Hor+: at 21:9, the Mission 1 frame is the 16:9 frame scaled to the same height with more
      scene on both sides (mean difference 0.58, against 9.11 for a stretch). No pop-in shows at
      the edges.
    - The render targets stay 1280×720, so the image is anamorphic, and the presenter shows it
      at the chosen aspect. At 1x that stretches 1280 columns over the wider screen, so pair it
      with `--tenchuz_native_resolution_scale=2`.
    - HUD (`--tenchuz_widescreen_hud`, on by default): draws on the render target the front
      buffer is resolved from are scaled horizontally to a centred 16:9 area. They must have no
      depth test, sample only game textures, and use a vertex shader without float constants,
      i.e. vertices already in screen pixels.
      - In the logs that's the sprite shader (`64B04B39`: HUD, menus), the menu text quad lists
        (`B393A267`) and the movie (`118071C8`, three textures). Movies keep 16:9.
      - Full-screen passes over resolved images, untextured fades and 3D draws keep the full
        width.
      - In Mission 1 and its pause menu, the HUD and menu keep their proportions, and the pause
        dimming covers the whole screen. The frame log counts "kept at 16:9" draws, and the
        draw list (`--tenchuz_native_log_draws`) shows each draw's target, depth test and
        texture kinds.
    - The one-frame artifact first seen at 21:9 (a white line from a lantern, a wood-textured
      wedge) wasn't widescreen's: it was the stale-draw replay fixed under N5 (2026-10-20), and
      it happened at 16:9 too. Mission 1 at 21:9 is now clean in the per-present check.
    - Open:
      - Particles are drawn by the same quad-list shader as the menu text, but in the 3D scene
        with a depth test, so they aren't squeezed. CPU-projected markers drawn on the final
        target without depth would be.
      - The ultrawide image is anamorphic. Rendering targets wider than the game's 1280 needs
        the game's 2D and post passes to cope, and is not done.

- **GPU cost and tiled passes (2026-10-28).** A one-frame GPU profile (`gpuprofile` script step,
  `DrawPath::RequestProfile`: a timestamp after every draw and operation, logged per render target
  and operation) showed a third of the GPU time at 2x / 4x MSAA in our resolves. The game draws
  1280x720 and its shadow map in 2 tiles and resolves each tile's half (a source rectangle and
  destination point, which we ignored) with a clear between tiles. We render untiled, so every
  resolve copied the whole target, and the clear between the shadow map's halves wiped it before
  the second copy: characters' own shadows were missing. Now:
  - resolves copy the rectangle to the destination point; in a tiled pass a tile's part at its own
    place is the whole target, copied once, and repeats from an unchanged source are skipped
    (`AlreadyResolved`, per-surface write counts);
  - the game's clears between BeginTiling and EndTiling wait for the next draw;
  - depth buffers are D32_FLOAT (the game's depth is 24-bit float) until a draw asks for stencil,
    then D32_FLOAT_S8X24 for the session (`--tenchuz_native_depth32`);
  - MSAA colour resolves that average every sample use `ResolveSubresource`
    (`--tenchuz_native_hardware_resolve`; `ResolveSubresourceRegion` removed the device here).
  - Mission 1 at 2x, 4x MSAA: 10.5-11.7 ms of GPU time a frame before, 8.6-9.5 after, with the
    same image (mean difference 0.44 of 255, run-to-run noise 0.36-0.42) plus the shadows.

- **Fixed (2026-10-21), from the first player reports:**
  - **Bloodied characters rendered black.** Each fighting character's blood map is a 256×256
    target cleared, given blood splats (sprites of `1E260000`) and resolved every frame
    (`1DE82000`, `1DE42000`, `1DE02000`, `1DDC2000`). Once a character is bloodied, its pixel
    shader (`103B8981`) blends the clothes towards the map's colour by the map's alpha. The clear
    passes no colour, and the D3D's `ClearF` and `Resolve` then use `0.0f` from `0x820024DC` for
    all four components (transparent black). We had used `RB_COLOR_CLEAR`, which still held the
    last clear's opaque black, so alpha was 1 everywhere and the whole character turned black.
    The water reflection target (`194A5000`) is cleared the same way, so its alpha depended on
    whatever clear came before.
  - **CPU and GPU now overlap.** Recording a frame no longer waits for the previous one to finish
    on the GPU. The draw path tracks which frame last read each 4 KB page of the guest-memory
    mirror, and waits only before overwriting a page the previous frame read (none in a Mission 1
    run). Uncapped, the village went from about 75 to 185 fps.
  - Diagnostics added: `--tenchuz_native_log_textures` (each distinct texture fetch once, flagging
    guest textures that overlap a resolve destination), bound texture addresses in the draw list,
    the caller and colour source of each logged clear, and the time spent translating shaders and
    creating pipelines in slow frames. Test runs drive the game with `--tenchuz_input_script`
    (`src/input/pc_input.h`) and save their own screenshots, without window focus.

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
