// tenchuz - frame rate (docs/FRAME_RATE.md).
//
// The game caps itself at 30 fps with D3DRS_PRESENTINTERVAL = 2: before each
// Swap the GPU waits for two 60 Hz vblanks. Graphics init sets it once
// (0x8239FF54), and the device's state reset restores it. Gameplay, cameras
// and cutscenes run on a measured frame time (0x82B57824, in ms), and play the
// same at 60 fps.
//
// The 2D screens don't: the title, menus, mission briefings and loading
// screens animate with fixed steps per frame (e.g. sub_822A29B8: alpha +8 and
// -12 a frame), written screen by screen, so at 60 fps their fades and
// transitions run twice as fast. With --tenchuz_menus_at_30 (default), frames
// that draw no 3D scene (no depth-tested draw, from the native renderer) and
// frames with the pause menu open keep the original two-vblank interval, and
// 3D frames get one. Above 60 fps the native GPU raises vblanks at the chosen
// rate during 3D scenes (native_gpu.h).
//
// Some effect objects step timers by a fixed 1/30 s a frame (constant
// 0x821DBCA8, read by 8 update methods at 0x823EBxxx-0x8240Dxxx). At 60 fps
// the constant follows the measured frame time.

#include "frame_rate.h"

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <cstring>

#include <windows.h>
#include <timeapi.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "debug_tools.h"
#include "input/pc_input.h"
#include "ui/game_menu.h"
#include "profiler.h"
#include "renderer/native_gpu.h"
#include "renderer/native_renderer.h"

REXCVAR_DEFINE_INT32(tenchuz_frame_rate, 60, "tenchuz",
                     "Frame rate cap in 3D scenes: 30 (the original), 60, 120, 144, 165, 180 or 240");
REXCVAR_DEFINE_BOOL(tenchuz_menus_at_30, true, "tenchuz",
                    "Keep screens without a 3D scene (title, menus, briefings, loading) and the pause menu at "
                    "30 fps, where the game animates by frames");

namespace {

constexpr uint32_t kIntervalTwo = 2;  // D3DPRESENT_INTERVAL_TWO
constexpr uint32_t kIntervalOne = 1;
constexpr uint32_t kDeviceInterval = 0x33C0;  // device field the setter writes
constexpr uint32_t kStep30 = 0x821DBCA8;      // 1/30 s
constexpr uint32_t kFrameSeconds = 0x82B577BC;
// The UI task list's pause mode (sub_822A1E30 reads it at 0x82B5A650): 1
// while the pause menu is open, when only pause-level tasks run.
constexpr uint32_t kPauseMode = 0x82B5A650;

// Frames of a kind in a row before switching (3D: full rate, 2D: 30 fps). A
// few 2D frames inside a 3D scene (a black frame between cuts) don't count.
constexpr int kToFullAfter = 2;
constexpr int kTo30After = 8;

int TargetRate() { return std::clamp(REXCVAR_GET(tenchuz_frame_rate), 30, 240); }
bool Want60() { return TargetRate() > 30; }

}  // namespace

namespace tenchuz::frame_rate {

void Initialize() { timeBeginPeriod(1); }

// Frames are paced by the D3D in guest vblanks: interval 1 waits one, 2 waits
// two. Full rate is interval 1 at that many vblanks a second; 30 fps is
// interval 2 at 60.
void OnSwap(uint8_t* base, uint32_t device) {
  if (!Want60() || !device) return;
  static bool scene_3d = true;
  static int run_3d = 0, run_2d = 0;
  const int depth_draws = native::LastFrameDepthDraws();
  const bool paused = REX_LOAD_U32(kPauseMode) != 0;
  if (depth_draws < 0 || !REXCVAR_GET(tenchuz_menus_at_30)) {
    scene_3d = true;
  } else if (depth_draws > 0 && !paused) {
    run_2d = 0;
    if (++run_3d >= kToFullAfter) scene_3d = true;
  } else {
    run_3d = 0;
    if (++run_2d >= (paused ? 1 : kTo30After)) scene_3d = false;
  }
  const uint32_t want = scene_3d ? kIntervalOne : kIntervalTwo;
  const int rate = TargetRate();
  native::SetVblankRate(scene_3d && rate > 60 ? double(rate) : 0.0);
  const uint32_t current = REX_LOAD_U32(device + kDeviceInterval);
  if ((current == kIntervalOne || current == kIntervalTwo) && current != want) {
    REX_STORE_U32(device + kDeviceInterval, want);
    static int logged = 0;
    if (logged++ < 40) {
      REXLOG_INFO("tenchuz: frame rate {} ({})", want == kIntervalOne ? rate : 30,
                  scene_3d ? "3D scene" : (paused ? "paused" : "2D screen"));
    }
  }
}

}  // namespace tenchuz::frame_rate

REX_EXTERN(__imp__sub_8228E5C8);
REX_HOOK_RAW(sub_8228E5C8) {  // the main loop's frame timer: frame time in s at 0x82B577BC
  // The game menu pauses here. The frame after it keeps the last frame time
  // instead of the whole pause.
  const uint32_t last_seconds = REX_LOAD_U32(kFrameSeconds);
  const bool paused = tenchuz::input::WaitWhileMenuOpen();
  __imp__sub_8228E5C8(ctx, base);
  if (paused) REX_STORE_U32(kFrameSeconds, last_seconds);
  static int frames = 0;
  if (++frames == 120) tenchuz::game_menu::ApplyStartupSettings();  // once audio is running
  tenchuz::profiler::OnMainLoopFrame();
  tenchuz::debug_tools::OnMainLoopFrame(base);
  if (!Want60()) return;
  static bool writable = false;
  if (!writable) {
    DWORD old_protect = 0;
    writable = VirtualProtect(REX_RAW_ADDR(kStep30), 4, PAGE_READWRITE, &old_protect);
    if (!writable) return;
  }
  uint32_t bits = REX_LOAD_U32(kFrameSeconds);
  float seconds;
  std::memcpy(&seconds, &bits, 4);
  seconds = std::clamp(seconds, 1.0f / 240.0f, 1.0f / 15.0f);
  std::memcpy(&bits, &seconds, 4);
  REX_STORE_U32(kStep30, bits);
}

REX_EXTERN(__imp__sub_824194A0);
REX_HOOK_RAW(sub_824194A0) {  // SetRenderState(D3DRS_PRESENTINTERVAL): device r3, value r4
  // Without native renderer statistics (emulated GPU), this sets 60 fps for
  // good; otherwise OnSwap picks the interval frame by frame.
  if (ctx.r4.u32 == kIntervalTwo && Want60()) ctx.r4.u64 = kIntervalOne;
  __imp__sub_824194A0(ctx, base);
}
