// tenchuz - guest frame statistics. See frame_stats.h.

#include "frame_stats.h"

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h must come first
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include "frame_rate.h"
#include "renderer/d3d_trace.h"
#include "renderer/gpu_observer.h"
#include "renderer/native_renderer.h"

namespace tenchuz {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kLogInterval = std::chrono::seconds(5);

#if defined(_WIN32)
// The game thread presenting no frame for kStuckAfter is logged with its host
// call stack, where recompiled guest functions are sub_<address>: at 2, 6 and
// 10 s, and once it presents again. (The game menu doesn't stop frames.)
// Online play drops a player who stops answering for a few seconds.
constexpr auto kStuckAfter = std::chrono::seconds(2);
std::atomic<Clock::rep> g_last_swap_ticks{0};
HANDLE g_game_thread = nullptr;

// The stopped thread's return addresses. Unwinds with the OS's tables only,
// so it takes no lock the stopped thread might hold.
int StuckStack(DWORD64* pcs, int max) {
  if (SuspendThread(g_game_thread) == DWORD(-1)) return 0;
  CONTEXT context = {};
  context.ContextFlags = CONTEXT_FULL;
  int count = 0;
  if (GetThreadContext(g_game_thread, &context)) {
    while (count < max && context.Rip) {
      pcs[count++] = context.Rip;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (!function) {  // a leaf: the return address is on top of the stack
        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
        context.Rsp += 8;
        continue;
      }
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                       &establisher, nullptr);
    }
  }
  ResumeThread(g_game_thread);
  return count;
}

void LogStuck(double seconds) {
  DWORD64 pcs[32];
  const int count = StuckStack(pcs, 32);
  REXLOG_WARN("tenchuz: game thread presented no frame for {:.1f} s; it is at:", seconds);
  HANDLE process = GetCurrentProcess();
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  for (int i = 0; i < count; ++i) {
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    IMAGEHLP_MODULE64 module = {};
    module.SizeOfStruct = sizeof(module);
    const char* module_name = SymGetModuleInfo64(process, pcs[i], &module) ? module.ModuleName : "?";
    if (SymFromAddr(process, pcs[i], &displacement, symbol)) {
      REXLOG_WARN("  #{:02} {}!{}+0x{:X}", i, module_name, symbol->Name, displacement);
    } else {
      REXLOG_WARN("  #{:02} {}!{:016X}", i, module_name, pcs[i]);
    }
  }
  rex::FlushLogging();
}

void WatchGameThread() {
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  Clock::rep stuck_at = 0;  // the last frame before the reported stop
  int reports = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const Clock::rep last = g_last_swap_ticks.load(std::memory_order_relaxed);
    if (reports && last != stuck_at) {
      const auto stopped = Clock::duration(last - stuck_at);
      REXLOG_WARN("tenchuz: game thread presented again after {:.1f} s",
                  std::chrono::duration<double>(stopped).count());
      reports = 0;
    }
    const auto stopped = Clock::now() - Clock::time_point(Clock::duration(last));
    if (stopped < kStuckAfter * (1 + 2 * reports) || reports >= 3) continue;
    stuck_at = last;
    ++reports;
    LogStuck(std::chrono::duration<double>(stopped).count());
  }
}

void WatchHangs(Clock::time_point now) {
  g_last_swap_ticks.store(now.time_since_epoch().count(), std::memory_order_relaxed);
  if (g_game_thread) return;
  g_game_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                             GetCurrentThreadId());
  if (g_game_thread) std::thread(WatchGameThread).detach();
}
#else
void WatchHangs(Clock::time_point) {}
#endif

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
  WatchHangs(now);
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
