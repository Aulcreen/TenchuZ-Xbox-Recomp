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
