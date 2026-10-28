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

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "debug_tools.h"
#include "input/pc_input.h"
#include "ui/game_menu.h"
#include "profiler.h"
#include "renderer/native_gpu.h"
#include "renderer/native_renderer.h"
#include "widescreen.h"

REXCVAR_DEFINE_INT32(tenchuz_frame_rate, 60, "tenchuz",
                     "Frame rate cap in 3D scenes: 30 (the original) or 60. Above 60 some effects step per "
                     "frame (blood hangs in the air), so higher values mean 60");
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

// 30 or 60: settings from builds that offered 120-240 run at 60.
int TargetRate() { return REXCVAR_GET(tenchuz_frame_rate) > 30 ? 60 : 30; }
bool Want60() { return TargetRate() > 30; }

constexpr uint32_t kFrameMs = 0x82B57824;     // the frame time in ms (most game code reads this)
constexpr uint32_t kFrameCount = 0x82B577C8;  // main-loop frames
bool g_full_rate = false;                     // this frame runs at 60 (interval 1)

// Updates that count frames rather than time, run at their original 30 Hz
// while the game runs at 60: every other main-loop frame, with the frame
// time doubled for the call (the 1/30 s step too), as a 30 fps frame would
// see it. Found by dumping consecutive frames at 30 and 60 fps
// (tools/find_frame_steps.py) and watching the values that stepped the same.
// Returns false on the frames they skip.
class ThirtyHzUpdate {
 public:
  explicit ThirtyHzUpdate(uint8_t* base) : base_(base) {  // REX_LOAD/STORE use `base`
    run_ = !g_full_rate || (REX_LOAD_U32(kFrameCount) & 1) == 0;
    if (!run_ || !g_full_rate) return;
    for (int i = 0; i < 3; ++i) {
      saved_[i] = REX_LOAD_U32(kTimes[i]);
      float value;
      std::memcpy(&value, &saved_[i], 4);
      value *= 2.0f;
      uint32_t bits;
      std::memcpy(&bits, &value, 4);
      REX_STORE_U32(kTimes[i], bits);
    }
    doubled_ = true;
  }
  ~ThirtyHzUpdate() {
    if (!doubled_) return;
    uint8_t* base = base_;
    for (int i = 0; i < 3; ++i) REX_STORE_U32(kTimes[i], saved_[i]);
  }
  bool run() const { return run_; }

 private:
  static constexpr uint32_t kTimes[3] = {kFrameMs, kFrameSeconds, kStep30};
  uint8_t* base_;
  uint32_t saved_[3] = {};
  bool run_ = true, doubled_ = false;
};

}  // namespace

namespace tenchuz::frame_rate {

void Initialize() {
#if defined(_WIN32)
  timeBeginPeriod(1);
#endif
  if (REXCVAR_GET(tenchuz_frame_rate) > 60) {
    rex::cvar::SetFlagByName("tenchuz_frame_rate", "60");  // what the menu shows and saves
  }
}

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
  g_full_rate = want == kIntervalOne;
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

REX_EXTERN(__imp__sub_823BF2E0);
// The ambient (wind) update: 12 timers counting frames pick new random gusts
// every few seconds, which sway the lanterns' light and move the water.
REX_HOOK_RAW(sub_823BF2E0) {
  ThirtyHzUpdate update(base);
  if (update.run()) __imp__sub_823BF2E0(ctx, base);
}

REX_EXTERN(__imp__sub_823FC238);
// A menu or HUD sprite (the menus' kunai cursor, a pulsing marker): its
// brightness bounces by 0.05 a frame (+216, speed +220) and it slides in over
// a frame countdown (+224). The call also queues the sprite's draw, so it runs
// every frame; on the frames a 30 Hz update skips, those three go back to
// what they were, and the sprite draws where it was. (Skipping the call made
// the kunai blink at 60 fps.)
REX_HOOK_RAW(sub_823FC238) {
  ThirtyHzUpdate update(base);
  if (update.run()) {
    __imp__sub_823FC238(ctx, base);
    return;
  }
  const uint32_t sprite = ctx.r3.u32;
  const uint32_t pulse = REX_LOAD_U32(sprite + 216), speed = REX_LOAD_U32(sprite + 220),
                 countdown = REX_LOAD_U32(sprite + 224);
  __imp__sub_823FC238(ctx, base);
  REX_STORE_U32(sprite + 216, pulse);
  REX_STORE_U32(sprite + 220, speed);
  REX_STORE_U32(sprite + 224, countdown);
}

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
  tenchuz::widescreen::Update(base);
  if (!Want60()) return;
  static bool writable = false;
  if (!writable) {
    writable = rex::memory::Protect(REX_RAW_ADDR(kStep30), 4, rex::memory::PageAccess::kReadWrite);
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
