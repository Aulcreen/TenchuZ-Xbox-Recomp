# Tenchu Z: D3D layer map (native renderer N1)

The XDK Direct3D library (XDK 2.0.2920) is statically linked into `default.xex`, so every
function below is recompiled code and can be replaced with `REX_HOOK_RAW` (see
`src/renderer/`). This map is the interface the native renderer implements.

Sources: runtime traces (`tools/gen_trace_hooks.py` hooks 1,101 functions in
`0x8240C000`–`0x82450000`; F8 or `--tenchuz_d3d_trace_at_frame=N` writes
`user/traces/d3d_trace_f*.tsv`; `tools/analyze_d3d_trace.py` summarizes) plus static analysis
(`tools/d3d_entry_summary.py`: which device offsets each function writes).

Confidence: **CONFIRMED** (code and trace agree), **LIKELY** (strong evidence), **GUESS**.

## Device

| | |
|---|---|
| Device object | `0x40001600` (guest), passed as `r3` to every `D3DDevice_*` call and as `r4` to the interrupt callback |
| Render thread | guest thread 6 (the main game thread, from `xstart`) |
| Interrupt callback | `0x82425D88`, called on host threads 3/4 (vblank / CP interrupts) |

### Device object layout (XDK 2.0.2920; differs from later XDKs)

| Offset | Contents | Evidence |
|---|---|---|
| `+0x2800`, `+0x2808` | surface / depth info registers | written by SetDepthStencilSurface (LIKELY) |
| `+0x285C` | colour-info register for RT0 | written by SetRenderTarget (LIKELY) |
| `+0x2884` | float render state | `82418188` (stfs) |
| `+0x2888`–`+0x2894` | viewport scale/offset floats (`PA_CL_VPORT_*`) | SetViewport (LIKELY) |
| `+0x28B4`–`+0x28E0` | render-state register shadow (depth control, blend control, colour control …) | setters `82417D20`…`82418338` |
| `+0x2A30`–`+0x2A33` | state flag bytes (dirty / tiling) | many functions |
| `+0x2D18` | current vertex declaration | SetVertexDeclaration (CONFIRMED) |
| `+0x2D40`, `+0x2D44` | blend-state bookkeeping | blend setters |
| `+0x2D4C`–`+0x2D58` | current render targets RT0–3 | SetRenderTarget (LIKELY) |
| `+0x2FA0` | current depth-stencil surface | SetDepthStencilSurface (LIKELY) |
| `+0x3060`–`+0x3074` | D3DVIEWPORT9 copy (X, Y, W, H, MinZ, MaxZ) | SetViewport (CONFIRMED by 24-byte size) |
| `+0x3088` | current pixel shader | SetPixelShader (CONFIRMED) |
| `+0x308C` | current vertex shader | SetVertexShader (CONFIRMED) |
| `+0x3098`–`+0x30A0` | predication (tile mask) state | SetPredication |
| `+0x30C0` | tiling flags | BeginTiling |
| `+0x3268`–`+0x3294` | tiling: tile rects, clear colour, clear Z | BeginTiling (LIKELY) |
| `+0x3364` | pending vertex shader | SetVertexShader wrapper |
| `+0x3378` | texture stage slots | SetTexture (LIKELY: `+0x3378 + 4*stage`) |

## Functions

### Frame structure
| Address | Name | Args (r4…) | Notes | Conf. |
|---|---|---|---|---|
| `0x8241BF50` | **D3DDevice_Swap** | | only caller of `VdSwap`; hooked in `frame_stats.cpp` | CONFIRMED |
| `0x8241BDE0` | pre-present sync | | `WAIT_REG_MEM`; called right before Swap from `823B8DF8`. Probably the 30 fps (two vblanks) wait, for track F | LIKELY |
| `0x8241C790` | **D3DDevice_BeginTiling** | flags=1, **count=2**, rects, … | Tenchu renders each frame in **2 EDRAM tiles** | LIKELY |
| `0x8241C630` | **D3DDevice_SetPredication** | tile mask (3, 0xC, 0) | | LIKELY |
| `0x8241CC58` | **D3DDevice_EndTiling** | | per-tile SetPredication + Resolve | LIKELY |
| `0x8241B308` | **D3DDevice_Clear(F)** | flags `0x11` = TARGET0 + ZBUFFER, … | issues clear draws | LIKELY |
| `0x8241D770` | **D3DDevice_Resolve** | flags `0x14`, src rect, dest texture … | ~25 callers in `0x8239A000`–`0x823C3000` (K2 post-processing passes) | LIKELY |
| `0x82425790` | GPU wait spin | | 32× `mr r31,r31` yield, then polls `device+0x2A31`. Called from the waits `82425B10`, `82425BA8`, `824267D0`, `82431820`; ~500k calls/frame (excluded from tracing) | CONFIRMED |

### Render targets and viewport
| Address | Name | Args | Conf. |
|---|---|---|---|
| `0x8241AD40` → `0x8241A780` | **D3DDevice_SetRenderTarget** | index, surface | LIKELY |
| `0x8241AA88` | **D3DDevice_SetDepthStencilSurface** | surface | LIKELY |
| `0x82419F78` | **D3DDevice_SetViewport** | `D3DVIEWPORT9*` (24 bytes) | CONFIRMED |
| `0x8241A1A0` | D3DDevice_GetViewport | `D3DVIEWPORT9*` (memcpy out) | LIKELY |

### Shaders, vertex input, textures
| Address | Name | Args | Conf. |
|---|---|---|---|
| `0x82420D40` | **D3DDevice_SetVertexDeclaration** | decl | CONFIRMED |
| `0x82420CF0` → `0x82420BA0` | **D3DDevice_SetVertexShader** | shader | CONFIRMED |
| `0x82420968` | **D3DDevice_SetPixelShader** | shader | CONFIRMED |
| `0x82417868` | **D3DDevice_SetTexture** | stage, texture | LIKELY |
| `0x824194D0`, `0x82419658` | sampler-state setters | sampler, value | LIKELY |

### Render-state setters (one per state, single value in r4)
| Address | Writes | Seen values | Conf. |
|---|---|---|---|
| `0x82417D20` | bitfield in `+0x28BC` | 0, 1 | LIKELY (depth enable) |
| `0x824181F0` | bitfield in `+0x28BC` | 6 | LIKELY (depth func) |
| `0x82418188` | float `+0x2884` | 0 | GUESS (alpha ref) |
| `0x82417D60` | `+0x2D44`, `+0x28B8`, `+0x28D8`–`+0x28E0` | 0, 1 | LIKELY (alpha blend enable) |
| `0x82417E80`, `0x82417F10` | `+0x2D40`, blend control | 6, 7 | LIKELY (src/dest blend) |
| `0x82418338` | `+0x2D5C`, `+0x28B4` | 0 | GUESS |
| `0x82417CC0`, `0x82417CF0`, `0x824188C0` | `+0x28C8` | 0, 6, … | GUESS |
| `0x82418E90` | `+0x28CC`, `+0x28C4` | 0, 1 | GUESS |
| `0x82418ED0`, `0x82417D10` | read `+0x28CC` / `+0x28C8` | | getters |

### Vertex/index input and shader constants
| Address | Name | Args | Conf. |
|---|---|---|---|
| `0x8241A1B8` | **D3DDevice_SetStreamSource** | stream, vertex buffer, offset, stride | LIKELY |
| `0x8241A368` | **D3DDevice_SetIndices** | index buffer | LIKELY |
| `0x824207C8` | **SetVertexShaderConstantF** | start, data, count | LIKELY (paired with PS version, called from K2 helper `0x824FA768`) |
| `0x82420820` | **SetPixelShaderConstantF** | start, data, count | LIKELY (`0x824FA840`) |
| `0x82419C00`, `0x82419C50`, `0x824197E0` | sampler-state setters (address U/V, filter) | sampler, value | LIKELY |

### Draws
| Address | Name | Args | Conf. |
|---|---|---|---|
| `0x82425168` | **D3DDevice_DrawIndexedVertices**: the main 3D draw (~470/frame in gameplay) | primitive type (4 = triangle list), base vertex, start index, count | LIKELY |
| `0x82424238` | **D3DDevice_BeginVertices** | primitive type (6 = strip, 8 = rect list), vertex count, stride; returns a pointer the game fills | CONFIRMED (N2: no draw packet until EndVertices) |
| `0x824246E8` | **D3DDevice_EndVertices** | | CONFIRMED (N2: emits the `DRAW_INDX` packet) |
| `0x8241AD48` | internal draw-packet builder (`DRAW_INDX_2`) | | CONFIRMED |
| `0x82420E40` | internal: writes `WAIT_UNTIL` + 8× `SQ_GPR_MANAGEMENT` (`0x0D00`) and a 1-point helper draw with immediate shaders. ~48 per frame; not game geometry | CONFIRMED (N2) |

### State flush (called inside draws; where the native renderer reads the full GPU state)
| Address | Flushes |
|---|---|
| `0x8242AF38` | ALU constants: VS (reg `0x4000`), PS (reg `0x4400`) |
| `0x8242BA80` (→ `0x8242A800`) | fetch constants (reg `0x4800`): textures and vertex streams |
| `0x8242A970` | type-0 register run (e.g. `0x1108`) |
| `0x8242AB98`, `0x8242ADD8` | register groups `0x2200`, `0x2209`, `0x2180`, bool/loop constants `0x4900` |

### D3DX9 (static, `0x8244xxxx`)
Tenchu Z draws its `.x` meshes through D3DX.
| Address | Name | Notes |
|---|---|---|
| `0x82441DB8` | **ID3DXMesh::DrawSubset**(mesh, attrib) | SetVertexDeclaration → SetStreamSource → SetIndices → DrawIndexedVertices |
| `0x82441560` | second mesh draw path (skinned/other) | same calls |

### Not D3D (in the traced range)
`0x824104E8`, `0x824103F8` (→ `XamInputGetState`), `0x8240FD40`, `0x8240F6E8`, `0x824111F0`,
CRT/XAPI or game helpers. Also
`0x8240F890` (~3,000 calls/frame, varying r3) and `0x824104E0`/`0x82411160`/`0x824111E8`/
`0x82411110` (~960 each, from the CRT at `0x82656Fxx`; probably critical sections).

## Gameplay frame (dojo, trace f10787: 35,250 calls, 97 entry points)

| # | Pass | Calls site | Work |
|---|---|---|---|
| 1 | 3× small depth/shadow passes (RT `4460E610`, depth `4474EA70`) | `0x823BBCCC` | clear + resolve each |
| 2 | Depth pre-pass, 2 tiles | `0x823B9898` | 22 draws, Z-only clear, per-tile resolve |
| 3 | **Main scene**, 2 tiles | `0x8239F2F0` | **452 indexed draws**, per-tile resolves, EndTiling |
| 4 | 4× small offscreen passes | `0x8237C2D4` | clear (`0x1F`) + resolve each |
| 5 | **Post-processing chain** | `0x8239A738`–`0x8239E53C` | ~20 single-draw passes (downsample / blur iterations / bloom) |
| 6 | HUD, then present | `0x823B8C78`, `0x823B8DFC` | 20 draws, resolve `0x100` to front buffer `0x40084160`, Swap |

Totals per frame: **474 DrawIndexedVertices + 50 DrawVerticesUP, 42 SetRenderTarget, 41 Resolve,
13 Clear, 2 BeginTiling/EndTiling**. Front buffer: `0x40084160`. Main colour/depth surfaces:
`0x40084250`/`0x40084290` (title) and `0x400842D0`/`0x40084310`.

## Title screen frame (trace f4000, 2 frames, 1,240 calls)
Per frame: SetRenderTarget/SetDepthStencilSurface → BeginTiling(2 tiles) → viewport →
Clear → 7 × [render states → sampler states → VertexDeclaration → VertexShader → PixelShader
→ SetTexture → BeginVertices → (game writes vertices) → EndVertices] → EndTiling (per-tile Resolve) → pre-present sync → Swap.

## Device register shadow (N2, confirmed over 5,464 gameplay draws)

`src/renderer/gpu_observer.cpp` decodes the command packets each D3D call emits
(`src/renderer/pm4_decoder.cpp`) and, at every draw, records the resulting GPU registers next to a
raw copy of the device object. `tools/analyze_gpu_observer.py` learned and then checked this
layout (11 auto-recorded frames: title, menus, dojo gameplay):

| Device | GPU registers | Contents | Check |
|---|---|---|---|
| `+0x0400`–`+0x06FF` | `0x4800`–`0x48BF` | 32 fetch constants × 24 bytes (textures, vertex streams) | 172/192 exact |
| `+0x0700`–`+0x16FF` | `0x4000`–`0x43FF` | 256 VS float constants | game draws match; `c255` is a D3D helper (see below) |
| `+0x1700`–`+0x26FF` | `0x4400`–`0x47FF` | 256 PS float constants | 99.9%+ |
| `+0x2700`–`+0x279F` | `0x4900`–`0x4927` | bool (8, VS/PS halves stored swapped) + loop (32) constants | exact |
| `+0x2800`–`+0x283F` | `0x2000`–`0x200F` | surface/colour/depth info, scissors | |
| `+0x284C`–`+0x289F` | `0x2100`–`0x2114` | `VGT_MAX_VTX_INDX`, colour mask, stencil, alpha ref, blend colour, viewport | |
| `+0x28A0`–`+0x28B3` | `0x2180`–`0x2184` | `SQ_PROGRAM_CNTL`, `SQ_CONTEXT_MISC`, interpolators, wrapping | |
| `+0x28B4`–`+0x28E3` | `0x2200`–`0x220B` | depth, blend 0–3, colour, HiZ, clip, cull/fill, VTE, mode control | |
| `+0x28E4`–`+0x2937` | `0x2280`–`0x2294` | point size, line width, … | exact where known |
| `+0x2938`–`+0x29CF` | `0x2300`–`0x2325` | line/scissor/AA config, `RB_COPY_*` (resolve), clear values | |
| `+0x29D0`–`+0x29EF` | `0x2380`–`0x2387` | polygon offset, point cull | exact |

Same group structure as later XDKs (Dante's Inferno), at Tenchu-specific offsets. In gameplay,
draws come from `0x82441560`/`0x82441DB8` (D3DX DrawSubset, ~78%), `0x824246E8` (EndVertices),
`0x82425168` (DrawIndexedVertices) and internal Clear/Resolve/EndTiling draws.

### Where device state and the decoded GPU state differ
1. **D3D-internal draws** (`Resolve 8241D770`, `Clear 8241B308`, `EndTiling 8241CC58`, `8241BBC0`):
   100% differ on depth control, shader program control, fetch slot 0, etc. They run with the D3D's
   own state. The native renderer implements clears and resolves directly, so no rule is needed.
2. **VS `c255`** on EndVertices draws (51%): a helper constant the D3D writes for 2D draws. Rule TBD (N4).
3. **Viewport / surface info / AA config in tiled passes**: the device holds the game's intent
   (1280×720, reversed depth: Z scale −1, offset 1). The decoded GPU value is the 640×736
   tile-resolve viewport, which is stale: per-tile setup is written into a reserved block at
   EndTiling, after the draws, and the linear decode does not see it in time. Settle visually in N3/N4.

**Conclusion:** the native renderer reads its per-draw state straight from the device object at
these offsets, and implements Clear/Resolve/tiling itself.

## Objects and bound state (N3, confirmed at runtime)

| Device offset | Contents |
|---|---|
| `+0x28` / `+0x30` | command write pointer (address of the last written dword) / end of the current ring segment |
| `+0x2F90`–`+0x2F9C` | bound render targets RT0–RT3 (`D3DSurface*`) |
| `+0x2FA0` | bound depth-stencil surface |
| `+0x3060`–`+0x306C` | `D3DVIEWPORT9` X, Y, Width, Height (dwords; MinZ/MaxZ floats follow) |

**D3DSurface** (64 bytes, XDK 2.0.2920): `+0x00` common flags (`0x00100004`), `+0x04` refcount,
`+0x08`/`+0x0C` fences, `+0x14` `0xFFFF0000`, **`+0x18` `RB_SURFACE_INFO`** (EDRAM pitch per tile,
MSAA), **`+0x1C` `RB_COLOR_INFO` / `RB_DEPTH_INFO`** (EDRAM base, format), **`+0x24` size**:
`(width-1) << 18 | (height-1) << 3` (the *tile* size for tiled targets: 640×736), **`+0x2C` size in bytes** (1280×736×4 for the main targets).

- The main targets are 1280×736 with **2× MSAA**, rendered as **two 640-wide EDRAM tiles**:
  `RB_SURFACE_INFO` holds the 640 tile pitch, and the screen scissor stays at 8192.
- `SetRenderTarget(0, …)` (`0x8241A780`) resets the viewport to the (tile) surface size.
  **`BeginTiling(flags, count, D3DRECT* tiles, …)`** (`0x8241C790`) gives the full frame: the
  union of the tile rects (1280×720).
- **`Clear` = `D3DDevice_ClearF(flags, D3DRECT*, D3DVECTOR4* colour, float z (f1), stencil)`**.
  Flags: bits 0–3 colour RT0–3, `0x10` Z, `0x20` stencil.
- **Resolve** (`0x8241D770`, flags in r4): after it returns, the device holds `RB_COPY_CONTROL`
  (`+0x2998`: source select, `0x100` clear colour, `0x200` clear depth), `RB_COPY_DEST_BASE`
  (`+0x299C`, physical), `RB_COPY_DEST_PITCH` (`+0x29A0`, width/height) and the clear values
  (`+0x29AC` depth, `+0x29B0` colour). It is called once per tile from EndTiling.
- **Swap**: `VdSwap` is called at `0x8241C0DC`. At `0x8241C0E0`, `r29 + 4` is the 64-dword block
  it filled: a type-0 write of the front-buffer fetch constant, then the SDK's `XE_SWAP` packet
  (`0x64`: signature, front buffer physical address, width, height).
