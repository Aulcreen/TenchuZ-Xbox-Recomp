# Frame rate (Track F): research

Research for milestones F1 (unlock 60 fps) and F2 (simulation timestep) of
[NATIVE_PORT_PLAN.md](NATIVE_PORT_PLAN.md). It comes from static analysis of `game/default.pe` and two
runs that changed only SDK command-line flags. No code was changed.

Evidence tags: **CONFIRMED** (read in the code, or measured), **LIKELY** (strong evidence, not
proven), **HYPOTHESIS** (needs a test).

## Summary

- **The 30 fps cap is the D3D presentation interval.** The game's graphics init sets
  `D3DRS_PRESENTINTERVAL = 2` at `0x8239FF54`. Before every Swap the GPU waits for two 60 Hz
  vblanks counted by the D3D vblank interrupt. CONFIRMED.
- **Gameplay is built on a measured frame time.** Every main-loop iteration the game reads the
  performance counter and publishes the last frame's length in milliseconds at `0x82B57824`.
  226 functions read it at 334 sites, mostly as `timer += dt` or as
  `per-30-fps-frame rate × dt / 33.3`. CONFIRMED for the code. Behaviour at 60 fps is untested.
- **A few update methods step by a fixed 1/30 s or 1/60 s** instead of the frame time. These
  are the first places to check at 60 fps (F2).
- **Removing the vblank wait with SDK flags alone does not reach 60 fps on the intro movie.**
  It runs at 34–47 fps there, about 27 ms per frame. Gameplay headroom hasn't been measured yet
  (see [Runtime checks](#5-runtime-checks)).

## 1. The presentation interval (F1)

| Address | What | Evidence |
|---|---|---|
| `0x8239FF54` | In `sub_8239FB70` (graphics init, called from the game's main `sub_8228E868` and from `sub_823A00E8`): `li r4,2` / `lwz r3,9912(r31)` / `bl 0x824194A0`, which is `SetRenderState(D3DRS_PRESENTINTERVAL, D3DPRESENT_INTERVAL_TWO)` | CONFIRMED |
| `sub_824194A0` / `sub_824194A8` | Setter / getter of `D3DRS_PRESENTINTERVAL`: `stw r4,0x33C0(r3)` / `lwz r3,0x33C0(r3)` | CONFIRMED |
| `0x82AD41A0` | D3D render-state table, 12-byte entries `{getter, setter, default}`. Entry 94 (`0x82AD4608`) is `PRESENTINTERVAL` (state value `0x178`, device setter slot `+0x1B0`), default 0 | CONFIRMED |
| `sub_8242F078` | Device state reset: applies every default, then restores the saved interval, so the value survives resets | CONFIRMED |
| device `+0x33C0` | Present interval. Device object at `0x40001600` ([D3D_MAP.md](D3D_MAP.md)) | CONFIRMED |
| device `+0x2D44` bits 2–8 | `D3DRS_PRESENTIMMEDIATETHRESHOLD` (setter `sub_824194B0`): percent of the frame; a late frame within it presents at once | LIKELY |
| `sub_8241BDE0` | `SynchronizeToPresentationInterval`, called right before each Swap (`sub_8241BF50`) at 9 sites: the main present `0x823B8DF8`, other game present paths (`0x82297200`/`0x8229738C`/`0x822975E4`, `0x8239F1C4`, `0x8239F45C`, `0x823A0080`; probably loading and transition screens, not checked), and D3D's own `0x8241C560`/`0x82431D90` | CONFIRMED |
| `sub_8241BD50` | CP-interrupt callback queued by the sync: computes the vblanks still to wait, or presents now if late and within the threshold | CONFIRMED |
| `0x82425D88` | Graphics (vblank) interrupt: increments the vblank count at `+0x3B14`, decrements the pending wait at `+0x3B1C`, and at zero writes the release value the GPU's `WAIT_REG_MEM` polls (`[device+0x2A14]+4`) | CONFIRMED |

How a frame is paced:

1. The game calls `SynchronizeToPresentationInterval`, which writes a `WAIT_REG_MEM` into the
   ring. The GPU stalls until a memory word is released. It also queues a CP interrupt that runs
   `sub_8241BD50`.
2. `sub_8241BD50` sets the pending wait to `interval − (vblank_now − vblank_at_last_swap)`.
   Interval values map to vblanks as 0 or 1 → 1, 2 → 2, 4 → 3. `0x80000000`
   (`D3DPRESENT_INTERVAL_IMMEDIATE`) skips the wait entirely (checked at the top of
   `sub_8241BDE0`).
3. Each vblank interrupt counts the wait down and releases the GPU at zero.
4. The CPU doesn't block in Swap. It's held back when it next waits on the GPU, so the whole
   frame runs at 60 / interval Hz.

The SDK raises guest vblanks on its "GPU VSync" thread at `video_mode_refresh_rate` (default 60 Hz),
or at 1 kHz with `--vsync=false` (`third_party/rexglue-sdk/src/graphics/graphics_system.cpp`).

The game calls `XGetVideoMode` twice. `sub_822A6C48` (startup) keeps the mode at `0x82B62B78` for
device creation. The FMV player `sub_82560DB8` uses only `is_widescreen` and the size, for its
aspect ratio. Neither reads `refresh_rate`, so the game's timing doesn't depend on the reported
refresh rate. CONFIRMED.

## 2. The game's clock

Main loop: `sub_8228E868` (the title's entry, called from the startup code `sub_82410E68` at `0x82410FFC`). After init it
loops at `0x8228E940` while `byte [0x82B577F9]` is set:

```
0x8228E940  bl sub_8228C0B8
0x8228E944  bl sub_8228E460   ; frame timer: QPC delta, FPS window
0x8228E948  bl sub_8228C7D8
0x8228E94C  bl sub_8228A780
0x8228E950  bl sub_822A1E30
0x8228E954  bl sub_8228E5C8   ; frame time for the next frame
0x8228E960  bl sub_8240FD40   ; SleepEx(0, TRUE): a yield, not a limiter
```

Timer state (all CONFIRMED from `sub_8228E3B8` init, `sub_8228E460`, `sub_8228E5C8`, `sub_8228E6F0`):

| Address | Type | Meaning |
|---|---|---|
| `0x82B577C0` | i64 | Performance-counter frequency (`sub_82410500`, the XDK `QueryPerformanceFrequency`) |
| `0x82B577F0` | f64 | Ticks per millisecond (frequency × 0.001, constant at `0x82002FE0`) |
| `0x82B577D0` / `0x82B577B0` | i64 | Counter at the last `sub_8228E460` / the delta since then |
| `0x82B577D8` | f32 | That delta in seconds (written, no direct readers found) |
| `0x82B577E0` | i32 | That delta in whole ms (read once, by `sub_823B8A58` just before the present) |
| `0x82B577DC` | f32 | Seconds since the timer started |
| `0x82B577C8` | i32 | Frame counter (one reader, `0x82291868`) |
| `0x82B577B8` | f32 | Frames per second, recomputed each second (read by `sub_8228A780`, probably a debug display) |
| `0x82B577BC` | f32 | **Frame time in seconds**: counter delta between consecutive `sub_8228E5C8` calls, so one full loop iteration. Getter `sub_8228E5B8`, 13 callers |
| `0x82B57824` | f32 | **Frame time in ms** = `0x82B577BC` × 1000, set at the top of the game update `sub_822929D0` (`0x82292A7C`). Zeroed at init (`0x822902C8`). Not clamped |

## 3. Who reads the frame time

Readers of `0x82B57824` (a `lis` with `0x82B5` plus `lfs …,30756`): **334 sites in 226
functions, `0x822812B8`–`0x823E3D58`**, which is the bulk of the game code. Grouped by the
first operation that uses the value:

| Use | Sites | Meaning |
|---|---|---|
| `x ± dt_ms` | 212 | Millisecond timers counting up or down: real time, frame-rate independent |
| `× 0.03` (30/1000) | 9 | ms → "30 fps frames" |
| `× 1/33.3` scaled constants (`0.030303`, `0.30303`, `0.45045`, `0.900901`, `1.351351`, `1.515152`, `0.39039`, `0.18018`, …) | ~30 | Per-30-fps-frame amounts (1, 10, 15, 30, 45, 50, 13, 6 per frame) scaled by the elapsed time |
| `× 0.000505`, `× 0.01`, `× 0.005`, … | ~35 | Rates per millisecond |
| Other `fmuls` / `fmadds` with a variable | ~25 | Variable rate × dt |

The 13 callers of the seconds getter `sub_8228E5B8` copy it into object fields `+500` / `+504`
(effects or shader time, `0x823E7C08`, `0x823EED50`, `0x824062B0`), accumulate it into timers
(`0x823C1A24`, `0x823AB2EC`), or pass it to virtual update methods (`0x822F0418`,
`0x8233A97C`, …).

So the game was designed for a variable frame time: most motion and timers express 30 fps
quantities as "per 33.3 ms" and scale by the measured delta. LIKELY. That's typical of an
engine shipped at 30 fps with drops.

## 4. Fixed-step candidates (F2 risks)

Code that assumes a frame length instead of measuring it. It's found by searching for 1/30 and
1/60 constants, so per-frame multiplications (`x *= 0.9`) and counters (`n -= 1`) are not
covered. Those can only be found by testing.

| Where | What | Notes |
|---|---|---|
| `sub_823EBEC0`, `sub_823EC750`, `sub_823F09F8`, `sub_823F1D08`, `sub_823F2100`, `sub_823F27D8`, `sub_823F51B0`, `sub_823FC9C0` | `field ±= 1/30` (constant `0x821DBCA8`) on fields `+5452`/`+5456`/`+13532` | Mostly virtual methods with no direct callers. The same region stores the frame time into `+500`/`+504` fields, so this may be UI or effects. HYPOTHESIS: would run 2× fast at 60 fps |
| `sub_8238E8A0` (from `sub_82394800`) | `x += global × 1/60` (constant `0x8202B25C`) into `+2936` | HYPOTHESIS: a per-frame accumulator |
| `sub_822E7038` (`0x822E73C4`) | Uses 1/30 (constant `0x82012520`) | Not inspected yet |
| `0x8200D868` (30.0, 69 sites) and `0x8200E910` (60.0, 102 sites) in `0x822BC4F0`–`0x822CF050` | Constants combined with positions and angles | Probably angles in degrees or distances, not time. Check if anything there misbehaves |
| `0x821F9488` (30.0, 16 sites in `0x825209EC`–`0x82522294`) | Media library | Probably the movie frame rate. Leave alone |
| Frame counter `0x82B577C8` | Read at `0x82291868` | Check what counts frames |

Not checked statically: animation playback (`.m` motion files; does it advance by dt or by
frame?), cloth (`Cloth.fxo` suggests a simulation), physics, and AI reaction timers.

## 5. Runtime checks

Both runs used the current build (with the uncommitted N4.3 work), the Xenos renderer, and only SDK
flags. FPS comes from `src/frame_stats.cpp` (counted at Swap, logged every 5 s).

| Run | Flag | Emulated vblank | Effective cap | Measured (intro movie) |
|---|---|---|---|---|
| baseline (earlier logs, e.g. `n43_play_05`) | none | 60 Hz | 30 fps | 30.0 fps, worst frames 36–46 ms |
| `logs/f1_vblank120_01.log` | `--video_mode_refresh_rate=120` | 120 Hz | 60 fps | **33.4–35.2 fps**, worst 37–48 ms |
| `logs/f1_novsync_01.log` | `--vsync=false` | 1 kHz | ~500 fps | **36.2–46.9 fps**, worst ~36 ms |

- With the vblank wait effectively removed, the intro movie still takes about 27 ms per frame.
  Something else limits it there: software WMV decoding in the recompiled XEX, the movie
  player pacing itself, or emulated-GPU cost. HYPOTHESIS: start by profiling the movie
  decoder thread.
- Both runs stayed on the intro movie. The Enter presses from `tools/send_keys_test.ps1` didn't
  reach the game. Most likely its window didn't have focus (`AppActivate` can't take the
  foreground from the active window), so the keys went to whatever window did. **Don't use
  `send_keys_test.ps1` while someone is using the PC.** Gameplay frame rates are still
  unmeasured.
- Every vblank-driven thing changes with these flags. That's harmless for this test, but not a
  shipping solution.

## 6. Proposed implementation

### F1: unlock 60 fps
1. **Hook the setter, not the call site.** Override `sub_824194A0` with `REX_HOOK_RAW` (a
   two-instruction leaf) and rewrite the value the game asks for:
   `--tenchuz_present_interval` (default 2 = original 30 fps, 1 = 60 fps, 0 = uncapped as
   `D3DPRESENT_INTERVAL_IMMEDIATE`). This covers every present path, including loading screens,
   and survives `sub_8242F078` state resets.
2. **First test:** interval 1 at the default 60 Hz vblank, in the training dojo and a mission.
   Log the frame rate and a histogram of `0x82B57824`.
3. **If it can't hold 60**, profile before changing anything else. A frame that misses its
   vblank waits for the next, so a 20 ms frame shows as 30 fps under interval 1.
   `PRESENTIMMEDIATETHRESHOLD` (device `+0x2D44`) can trade that for tearing.
4. **120/144 Hz (F3):** interval 1 plus `--video_mode_refresh_rate=120|144` (the game ignores
   the reported rate), or interval 0 with a host frame limiter.

### F2: check what assumes 30 fps
At 60 fps, compare against 30 fps in the same scenes:
- The player ninja's run, jump and grapple distances, and attack and stealth-kill animation lengths
- Enemy patrol and alert timing, the ki meter, and mission and alert countdowns (they should
  follow wall time: they're the `dt_ms` timers)
- Menu, map and item-wheel animations: the fixed 1/30 methods in section 4 are the first suspects
- Cloth, grass and water motion; particles and effects (`+500`/`+504` time fields)
- Real-time cutscenes against their audio; FMV against audio
- Dt-based logic often still breaks at very small steps (float precision, thresholds like
  `if dt_ms > 0`). Also test uncapped once 60 works.

Fixes follow what's found: scale a fixed step by `dt_ms / 33.33`, or call a 30 fps-only routine
every other frame.

### Interaction with the native renderer
Pacing lives in the GPU ring today: a `WAIT_REG_MEM` released by the vblank interrupt. N5
removes the ring, so the native renderer must then do this pacing itself: wait for
`interval` host vblanks (or a frame limiter) before presenting, and keep raising the guest
vblank interrupt so the D3D counters still advance. Until N5, F1 works with either renderer,
because both keep the emulator's ring and interrupts.

## Open questions
- Gameplay frame cost with the cap removed. Measure in a mission with `--vsync=false` and
  someone at the controls.
- What limits the intro movie to ~37 fps.
- What the fixed-step methods in section 4 belong to. Name them in `config/default.toml` once
  identified.
- Whether animation and physics advance by `dt_ms` or by frames.
