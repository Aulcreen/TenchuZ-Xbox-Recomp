# Roadmap: native PC Tenchu Z

The goal is a native Windows (later Linux) executable built by statically recompiling
`default.xex` with the ReXGlue SDK. Players supply their own disc image.

## Phase 0: Bootstrap ✅
- [x] Pull `default.xex` from the Redump ISO (`tools/xiso.py`)
- [x] Decrypt the XEX to a raw PE for disassembly (`tools/xex_extract.py`, `tools/dis.sh`)
- [x] `rexglue init` project scaffold, manifest, and `config/default.toml` overrides
- [x] Codegen passes validation (4 missing-`.pdata` functions fixed)
- [x] Mine the old Xenia log: imports, XDK libs, and GPU warnings

## Phase 1: First boot
- [x] Get a full compile and link (`build.bat`): clang 19.1.5, ~8 min cold on HDD, 32 MB `tenchuz.exe`
- [x] Find `setjmp` (`0x82657B00`) and `longjmp` (`0x826578C0`): 7 and 21 call sites, now native
- [x] Recover pointer-only leaf functions codegen misses: `tools/find_data_funcs.py` → `config/data_funcs.toml`
      (87 from vtables in data + 298 built by `lis/addi` in code, import thunks excluded = 385)
- [x] Enable the Xenos GPU plugin (`GPU_PLUGINS xenos` and `OnPreSetup`)
- [x] Smoke boot with only `default.xex`: GPU, audio, and input come up, and the game runs until it opens `game:\DATA\xten.xgs` (Dirty Disc screen, because the data isn't there)
- [x] Extract the full disc into `game/` from the Redump-verified ISO (`I:\Tenchu Z\Tenchu Z.iso`)
- [x] Boot with full data: K2 splash, the intro FMV, then the **title screen ("PRESS START BUTTON")** all render correctly
- [x] Crash reporter (`src/crash_report.cpp`): guest registers and a symbolized `sub_XXXXXXXX` backtrace in the log
- [x] Fix the crash when Start is pressed: unset profile settings now report `XSOURCE_DEFAULT` (`src/profile_fix.cpp`)
- [x] Keep user data and the shader cache in `<root>/user` instead of `Documents` on C:
- [ ] Map CRT functions into `[rexcrt]` (heap, memcpy/memset, and str*) for speed and correctness
- [ ] Stub the online-only imports ReXGlue lacks (`NetDll_WSAEventSelect`, `NetDll_XNetQosLookup`,
      `XamShowGamerCardUIForXUID`, `XamUserCreateStatsEnumerator`, `Refresh`) if the game calls them
- [x] Get from the title screen to the menus, then load a mission. Character and partner creation,
      game settings, the opening cinematic, the mission load screen, and **gameplay in the training
      dojo** all work (2026-10-18)
- [x] Default `gpu_allow_invalid_fetch_constants=true` (set in `OnPostSetup`); without it, starting a mission went black

## Phase 2: Playable
- [ ] Audio: XACT banks play (XMA decode through the SDK)
- [ ] FMV: the `.wmv` cutscenes play correctly (the software decoder lives in the XEX). The intro movie already renders.
- [ ] Saves: XContent / save device redirected to a host folder
- [ ] Input: XInput gamepad, then keyboard and mouse (see AC6_recomp's `ac6_kbm_input.cpp`)
- [ ] Fix the GPU `Resolve region is empty` / `PM4_DRAW_INDX_2 Failed in backend` pair. It's identical
      to the 2023 Xenia log, about 3,000 per 8 minutes in menus and missions, and has no obvious visual impact so far
- [ ] Play through all stages and log every issue in `docs/ISSUES.md`

## Phase 3: PC enhancements
- [ ] Resolution and aspect ratio: widescreen FOV and HUD hooks
- [ ] Frame rate: find the game-tick and delta-time logic and decouple it from 30 fps
- [ ] Settings UI (ImGui overlay from the SDK), config file, and launcher
- [ ] Optimization flags (`cr_as_local`, `non_volatile_as_local`, …) once the build is stable
- [ ] Linux (Vulkan) build
- [ ] Multiplayer: System Link over a virtual LAN, over Steam lobbies and P2P (App ID 480) or
      loopback between local instances (`docs/MULTIPLAYER.md`). Local co-op works up to a
      shared mission. Steam builds, opens a lobby and hosts; a second player joining over Steam
      is untested (needs two PCs). Xbox LIVE modes are not covered.

## Phase 4: Distribution
- [ ] Installer flow: user points at their ISO, then we extract it and verify the XEX hash
- [ ] Release CI that ships the EXE only, never game data

## Reverse-engineering backlog
- K2 formats: `.bin` stage packs, `.k2m`, `.k2ec`/`.k2es`, `.m` motion, `.khd`, `.set`, `.squ`
- Name key functions in `config/default.toml` (`[functions] 0x... = { name = "..." }`)
  as they are identified, so the generated code stays navigable.
- Set up Ghidra (`F:\etc\ghidra_12.1.2_PUBLIC`) with an XEX loader extension, or load
  `game/default.pe` as a raw big-endian 64-bit PowerPC image (32-bit addressing) at `0x82000000`.
