// tenchuz - D3D layer call tracer. See d3d_trace.h.

#include "d3d_trace.h"

#include "generated/default/tenchuz_pch.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

REXCVAR_DEFINE_INT32(tenchuz_d3d_trace_at_frame, 0, "tenchuz",
                     "Capture a D3D trace starting at this frame (0 = off)");
REXCVAR_DEFINE_INT32(tenchuz_d3d_trace_frames, 1, "tenchuz",
                     "Number of whole frames per D3D trace capture");

namespace tenchuz::d3d_trace {
namespace {

constexpr uint32_t kSwapAddress = 0x8241BF50;
constexpr size_t kMaxEvents = size_t(1) << 20;

struct Event {
  uint32_t function;
  uint32_t lr;
  uint32_t thread;
  uint32_t args[6];  // r3-r8
};

std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_requested{0};
std::atomic<size_t> g_count{0};
std::unique_ptr<Event[]> g_events;
std::mutex g_mutex;  // swap-side state below
std::filesystem::path g_dir = "traces";
uint64_t g_frame = 0;
uint64_t g_capture_start = 0;
uint32_t g_frames_left = 0;

void Dump() {
  const size_t count = std::min(g_count.load(), kMaxEvents);
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  const auto path = g_dir / fmt::format("d3d_trace_f{}.tsv", g_capture_start);
  std::ofstream out(path);
  out << "seq\tthread\tfunction\tlr\tr3\tr4\tr5\tr6\tr7\tr8\n";
  for (size_t i = 0; i < count; ++i) {
    const Event& e = g_events[i];
    out << fmt::format("{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\n", i,
                       e.thread, e.function, e.lr, e.args[0], e.args[1], e.args[2], e.args[3],
                       e.args[4], e.args[5]);
  }
  REXLOG_INFO("tenchuz: D3D trace of frames {}-{}: {} events{} -> {}", g_capture_start, g_frame,
              count, g_count.load() > kMaxEvents ? " (buffer full, truncated)" : "",
              path.string());
}

}  // namespace

bool Active() {
  return g_active.load(std::memory_order_relaxed);
}

void Record(uint32_t function, const PPCContext& ctx) {
  const size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
  if (i >= kMaxEvents) {
    return;
  }
  Event& e = g_events[i];
  e.function = function;
  e.lr = static_cast<uint32_t>(ctx.lr);
  e.thread = rex::runtime::ThreadState::GetThreadID();
  e.args[0] = ctx.r3.u32;
  e.args[1] = ctx.r4.u32;
  e.args[2] = ctx.r5.u32;
  e.args[3] = ctx.r6.u32;
  e.args[4] = ctx.r7.u32;
  e.args[5] = ctx.r8.u32;
}

void OnSwap(const PPCContext& ctx) {
  std::lock_guard lock(g_mutex);
  if (g_active.load()) {
    Record(kSwapAddress, ctx);
  }
  ++g_frame;

  if (g_active.load()) {
    if (--g_frames_left == 0) {
      g_active.store(false);
      Dump();
    }
    return;
  }

  uint32_t frames = g_requested.exchange(0);
  const int32_t at_frame = REXCVAR_GET(tenchuz_d3d_trace_at_frame);
  if (!frames && at_frame > 0 && g_frame == static_cast<uint64_t>(at_frame)) {
    frames = static_cast<uint32_t>(std::max(1, REXCVAR_GET(tenchuz_d3d_trace_frames)));
  }
  if (frames) {
    if (!g_events) {
      g_events = std::make_unique<Event[]>(kMaxEvents);
    }
    g_count.store(0);
    g_capture_start = g_frame;
    g_frames_left = frames;
    g_active.store(true);
    REXLOG_INFO("tenchuz: D3D trace started at frame {} ({} frames)", g_frame, frames);
  }
}

void RequestCapture(uint32_t frames) {
  g_requested.store(frames);
}

void SetOutputDir(const std::filesystem::path& dir) {
  std::lock_guard lock(g_mutex);
  g_dir = dir;
}

}  // namespace tenchuz::d3d_trace
