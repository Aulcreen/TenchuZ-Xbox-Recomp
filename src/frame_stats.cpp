// tenchuz - guest frame statistics. See frame_stats.h.

#include "frame_stats.h"

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>

#include "frame_rate.h"
#include "renderer/d3d_trace.h"
#include "renderer/gpu_observer.h"
#include "renderer/native_renderer.h"

namespace tenchuz {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kLogInterval = std::chrono::seconds(5);

std::mutex g_mutex;
Clock::time_point g_last_swap;
Clock::time_point g_window_start;
uint64_t g_frame_count = 0;
double g_smoothed_ms = 0;
// Current logging window.
uint32_t g_window_frames = 0;
double g_window_worst_ms = 0;

void OnGuestSwap() {
  const auto now = Clock::now();
  std::lock_guard lock(g_mutex);
  ++g_frame_count;
  if (g_frame_count == 1) {
    g_last_swap = g_window_start = now;
    return;
  }
  const double ms = std::chrono::duration<double, std::milli>(now - g_last_swap).count();
  g_last_swap = now;
  g_smoothed_ms = g_smoothed_ms == 0 ? ms : g_smoothed_ms * 0.9 + ms * 0.1;
  ++g_window_frames;
  g_window_worst_ms = std::max(g_window_worst_ms, ms);

  const auto elapsed = now - g_window_start;
  if (elapsed >= kLogInterval) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    REXLOG_INFO("tenchuz: frames {:.1f} fps avg, worst frame {:.1f} ms ({} frames)",
                g_window_frames / seconds, g_window_worst_ms, g_frame_count);
    g_window_start = now;
    g_window_frames = 0;
    g_window_worst_ms = 0;
  }
}

}  // namespace

rex::ui::FrameStats GetFrameStats() {
  std::lock_guard lock(g_mutex);
  rex::ui::FrameStats stats;
  stats.frame_count = g_frame_count;
  stats.frame_time_ms = g_smoothed_ms;
  stats.fps = g_smoothed_ms > 0 ? 1000.0 / g_smoothed_ms : 0;
  return stats;
}

}  // namespace tenchuz

REX_EXTERN(__imp__sub_8241BF50);

// D3DDevice_Swap: calls VdSwap once per presented frame.
REX_HOOK_RAW(sub_8241BF50) {
  tenchuz::d3d_trace::OnSwap(ctx);
  tenchuz::gpu_observer::OnSwap(ctx.r3.u32, base);
  tenchuz::native::OnSwap(base, ctx.r3.u32);
  tenchuz::frame_rate::OnSwap(base, ctx.r3.u32);
  tenchuz::OnGuestSwap();
  __imp__sub_8241BF50(ctx, base);
}
