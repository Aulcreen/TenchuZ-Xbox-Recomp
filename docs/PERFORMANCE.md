# Performance

## Measured (2026-10-29)

Mission 1, bridge, camera turning, 60 fps. Ryzen 5 5600G, Radeon RX 6600, 1440p display at 180 Hz.
CPU per thread from Windows' process counters; GPU from the "GPU Engine 3D" utilisation counter.

| Settings | GPU 3D | CPU (cores) |
|---|---|---|
| 1440p (2x), 4x MSAA, FXAA, 16x filtering | 66-72% | 1.25-1.4 |
| same, texture filtering off | 68% | 1.4 |
| same, the game's 2x MSAA | 65% | 1.4 |
| same, no FXAA | 66% | 1.5 |
| 720p (1x), 4x MSAA, FXAA | 37% | 0.8 |

- **Resolution is what costs:** MSAA, filtering and FXAA are close to free. Most of a pixel's cost is the
  game's own shaders as translated (the world shader is about 1,850 instructions, 45 texture reads).
- **CPU:**
  - The game thread uses about half a core.
  - The rest goes to the UI/present thread, most of it waiting inside the graphics driver, which grows
    with the GPU load.
  - Presenting from a non-UI thread (`--host_present_from_non_ui_thread`) made no difference.
- **GPU profile** (`gpuprofile`, one frame at 1440p): main scene 5 ms, two depth resolves 1.3 ms (the MSAA
  depth is decompressed to be read), about 37 resolve copies 0.7-1.1 ms, and the post passes about 1.5 ms.
- **Early depth (`--tenchuz_native_early_z`, 2026-10-29):**
  - Every translated pixel shader has the alpha test as a `discard`, so the GPU shaded pixels before
    testing their depth.
  - Shaders that can't change coverage (no alpha test or alpha to coverage, no kill, no depth write) now
    get `[earlydepthstencil]`, as in the SDK's D3D12 pipeline cache. That is 18 of 43 variants on the bridge.
  - No change on the bridge (little overdraw); it renders identically.

- **Shadow map without MSAA (`--tenchuz_native_single_sample_shadows`, 2026-10-29):**
  - The shadow map is a tiled pass that clears only depth, 1024x1024 at 4x MSAA. The game resolves one
    sample of it.
  - Drawn single-sampled, its draws went from 0.29 to 0.06 ms and the two depth resolves from 1.27 to
    0.77 ms (no decompression of a multisampled shadow map). About 0.7 ms a frame at 1440p.
  - Screenshots match within run-to-run noise.

- **Measuring GPU work (2026-10-30):** at 60 fps the RX 6600 lowers its clocks, so its
  utilisation and per-draw times say little; 720p and 1440p frames took the same GPU time. A
  GPU-bound benchmark does: the bridge at 4x (5120x2880, 4x MSAA, FXAA) with the camera turning
  holds 34-35 fps within about 0.5. Against it:
  - texture filtering is free;
  - 4x MSAA costs 11% against the game's 2x;
  - early depth is worth 2%.
  - A hand-written world shader (about 90% fewer instructions than its translation, same image)
    gained nothing: the GPU waits on memory, not on shader arithmetic. Not kept.
  - At 4x a frame is 27 ms: the 3D scene 44%, the post passes 21%, resolves 23%, present 8%.
- **Resolves (2026-10-30), 34.3 -> 38.3 fps at 4x (+12%):**
  - **Tiled resolves in EndTiling:** EndTiling resolves (and clears) the tiles itself, but the
    renderer ended the tiled pass when EndTiling began. The main scene's resolve
    (`1F160000`, copy control `0x340`) and a 1024x1024 one were then done as compute copies per
    tile instead of one hardware resolve. The pass now ends at the next call outside EndTiling:
    +5.5%.
  - **Depth read without decompressing (`--tenchuz_native_depth_read_resolve`):** the
    multisampled depth was moved to the shader-resource state for its resolve, which
    decompresses it (about 1.6 ms of the 2.6 the resolve cost at 4x). It now moves to
    depth-read plus shader-resource, which lets the driver read it compressed: +9%. Valid D3D12
    on every vendor; a driver that can't keep it compressed decompresses as before.
  - Screenshots match within run-to-run noise (HUD state, animated water and trees).
- **Where a 4x frame goes (2026-10-31, `--tenchuz_native_profile_timeline` with `gpuprofile`):**
  every draw and operation's GPU time in order. About 24 ms:
  - The main scene is 10-12 ms, most of it the world shader (`BDD29634`: 3 material textures and
    13 point-sampled shadow-map taps).
  - The final composite (`DB1B0ACD`, depth of field over 6 textures) takes 2 ms.
  - A quarter-resolution blur chain takes 2 ms: 12 passes of `27A33888` (8 taps each), each pass
    resolved.
  - The rest is present (blit 0.8-1.0 ms, FXAA 1 ms), the depth resolve for the composite
    (0.7 ms), the MSAA resolves (1 ms) and a full-screen copy of the scene (0.65 ms).
  - Every render target is RGBA8; nothing is wider than it needs to be.
- **Shadow-map clear (2026-10-31):** reading the single-sampled shadow depth in the depth-read
  state (as the multisampled depth is) made the clear right after its resolve cost 0.48 ms at 4x,
  against 0.01 ms from the plain shader-resource state. Depth-read is now for multisampled depth
  only. The resolve costs the same either way.
- **Lent resolves (`--tenchuz_native_lend_resolves`, 2026-10-31), +2.3% at 4x:**
  - A whole single-sampled target resolved to a texture now hands the texture its resource and
    takes another one, instead of copying. The target only gets its content back if it's drawn on
    before a clear. A texture whose loan had to be returned gets copies from then on: the blur
    chain draws on again right after each resolve.
  - This removes the scene copy before post-processing (`1C3E9000`) and the front buffer's copy
    (`1F4F8000`), 0.3 ms each at 4x. Blood maps aren't lent: their dried layer is written by
    compute.
  - The bridge benchmark: 39.8 / 39.4 fps without, 40.3 / 40.8 with (runs alternated). Screenshots
    match within run-to-run noise, at 2x through the menus, loading, a fight with blood and the
    pause menu, and at 1x across live 1x/2x switches.
- **Tried and dropped (2026-10-31):**
  - Resolve destinations flagged as render targets, so drivers might keep them compressed: no
    change (40.4 against 40.6 fps).
  - Lending the shadow map's depth too: its texture's loan gets returned, so it never paid off.

## Automatic resolution

`--tenchuz_native_resolution_scale=0` (the default; Esc menu > Display > Resolution > Auto),
`src/renderer/auto_resolution.cpp`.

- **What it measures:** timestamps at the start and end of each frame's command list. They are read once
  the frame's fence passes, and only 3D frames count.
- **Where it starts:** at the display's scale: the game window's monitor height / 720, rounded (1080p and
  1440p: 2x, 2160p: 3x). That is also the highest it goes.
- **Stepping down:** one scale down when the smoothed GPU time is over 85% of a frame (16.7 ms at 60 fps)
  for 2 s.
- **Stepping up:**
  - One scale up when the next scale's estimated time stays under 60% for 5 s.
  - The estimate uses the cost ratio between the two scales, learnt when it last stepped down. Before
    that it uses their pixel counts.
  - It doesn't go back up to a scale it left for being slow for a minute.
- **Pacing:** 10 s at least between changes (each makes the render targets again, a short hitch).
- **Tested:** 8 ms of pretend GPU load added for 40 s. It went 2x -> 1x in 2 s, then back to 2x once the
  minute had passed, and held there.
