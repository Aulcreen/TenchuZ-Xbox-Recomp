// tenchuz - GPU state observer. See gpu_observer.h.

#include "gpu_observer.h"

#include "generated/default/tenchuz_pch.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/thread_state.h>

#include "pm4_decoder.h"

REXCVAR_DEFINE_INT32(tenchuz_gpu_observe_at_frame, 0, "tenchuz",
                     "Record GPU state at every draw of this frame (0 = off)");
REXCVAR_DEFINE_INT32(tenchuz_gpu_observe_every, 0, "tenchuz",
                     "Also record GPU state automatically every N frames (0 = off)");
REXCVAR_DEFINE_BOOL(tenchuz_gpu_stuck_resume, false, "tenchuz",
                    "Live decoding: resume right after D3D kick headers instead of skipping ahead");
REXCVAR_DEFINE_INT32(tenchuz_gpu_observe_debug, 0, "tenchuz",
                     "Log up to N command-buffer syncs while observing (0 = off)");

namespace tenchuz::gpu_observer {
namespace {

constexpr uint32_t kWritePointerOffset = 0x28;  // device: address of last written dword
constexpr uint32_t kDeviceDwords = 0x1000;      // device bytes 0x0000-0x3FFF are recorded
constexpr uint32_t kLimitOffset = 0x30;         // device: end of the current ring segment
constexpr uint32_t kMaxSyncGap = 16u << 10;     // larger jumps mean a segment switch / wrap
constexpr uint32_t kStuckGap = 4u << 10;
constexpr uint32_t kSegmentReserve = 0xA0;    // device +0x2C (segment end) - +0x30 (limit)
constexpr uint32_t kSegmentScan = 16u << 10;  // slack below the lowest write pointer seen (InCommandRing)
constexpr uint32_t kKickHeaderMax = 64;       // bytes the D3D leaves after a kick before the next packets
// The D3D's kick family: the kick (submits the segment, may switch to a new
// one), the end-of-frame kick, and the helpers they call, which leave header
// words the GPU never executes. Decoding pauses inside them.
bool IsKickFunction(uint32_t function) {
  switch (function) {
    case 0x82426DF0: case 0x82426AC8: case 0x82426498: case 0x824261F0:
    case 0x82426710: case 0x82426C50: case 0x824267D0:
      return true;
    default:
      return false;
  }
}
constexpr uint32_t kKickScan = 512;           // bytes of a new segment searched for its first packet
constexpr uint32_t kMagic = 0x424F5A54;         // "TZOB"

enum class State { kOff, kWarmup, kRecord };

// Fixed 16-dword header per draw in the output file.
struct DrawHeader {
  uint32_t api_function;  // top-level D3D call that emitted the draw
  uint32_t opcode;
  uint32_t words[3];
  uint32_t predicated;
  uint32_t vs_address, vs_size, ps_address, ps_size;
  uint32_t bin_select_lo;
  uint32_t reserved[5];
};
static_assert(sizeof(DrawHeader) == 16 * 4);

struct DrawRecord {
  DrawHeader header;
  std::vector<uint32_t> regs;
  std::vector<uint32_t> device;
};

std::atomic<bool> g_active{false};  // decoding (for an observation and/or live consumers)
std::atomic<bool> g_requested{false};
std::atomic<bool> g_live_requested{false};
LiveDrawCallback g_live_callback = nullptr;
bool g_decoding = false;
State g_state = State::kOff;
uint32_t g_device = 0;
uint32_t g_render_thread = 0;
uint32_t g_cursor = 0;
uint32_t g_limit = 0;
uint32_t g_depth = 0;
uint32_t g_top_function = 0;
uint64_t g_frame = 0;
uint64_t g_record_frame = 0;
uint64_t g_resyncs = 0;
int32_t g_debug_lines = 0;
std::unique_ptr<pm4::Decoder> g_decoder;
std::vector<int32_t> g_first_written;  // per register: -1 written in warm-up, n = first draw, INT32_MAX never
std::vector<DrawRecord> g_draws;
std::filesystem::path g_dir = "traces";
uint8_t* g_base = nullptr;
ResyncStats g_resync_stats;
uint32_t g_frame_stuck_address = 0;  // first stuck point this frame (diagnostics)
uint32_t g_kick_depth = 0;           // hook depth of a kick in progress, 0 if none
std::atomic<uint32_t> g_ring_low{0xFFFFFFFFu}, g_ring_high{0};  // physical range written so far

void OnDraw(const pm4::DrawInfo& info) {
  if (g_live_callback && g_live_requested.load(std::memory_order_relaxed)) {
    g_live_callback(info, *g_decoder, g_base, g_depth ? g_top_function : 0);
  }
  if (g_state != State::kRecord) {
    return;
  }
  uint8_t* base = g_base;
  DrawRecord rec{};
  DrawHeader& h = rec.header;
  h.api_function = g_top_function;
  h.opcode = info.opcode;
  for (int i = 0; i < 3; ++i) {
    h.words[i] = info.words[i];
  }
  h.predicated = info.predicated;
  h.vs_address = g_decoder->vertex_shader.address;
  h.vs_size = g_decoder->vertex_shader.size_dwords;
  h.ps_address = g_decoder->pixel_shader.address;
  h.ps_size = g_decoder->pixel_shader.size_dwords;
  h.bin_select_lo = static_cast<uint32_t>(g_decoder->bin_select);
  rec.regs.assign(g_decoder->regs, g_decoder->regs + pm4::kRegisterCount);
  rec.device.resize(kDeviceDwords);
  for (uint32_t i = 0; i < kDeviceDwords; ++i) {
    rec.device[i] = REX_LOAD_U32(g_device + 4 * i);
  }
  const int32_t index = static_cast<int32_t>(g_draws.size());
  for (uint32_t r = 0; r < pm4::kRegisterCount; ++r) {
    if (g_decoder->written[r] && g_first_written[r] == INT32_MAX) {
      g_first_written[r] = index;
    }
  }
  g_draws.push_back(std::move(rec));
}

// Registers set before decoding started, or in a skipped stretch of the ring,
// are only known from the device's register shadow (docs/D3D_MAP.md). Decoded
// packets override these values from here on.
void SeedFromDevice(uint8_t* base) {
  struct Range {
    uint32_t device_offset, first_register, count;
  };
  static constexpr Range kRanges[] = {
      {0x0400, 0x4800, 0xC0},  {0x0700, 0x4000, 0x400}, {0x1700, 0x4400, 0x400},
      {0x2720, 0x4908, 0x20},  {0x2800, 0x2000, 0x10},  {0x284C, 0x2100, 0x15},
      {0x28A0, 0x2180, 0x05},  {0x28B4, 0x2200, 0x0C},  {0x28E4, 0x2280, 0x15},
      {0x2938, 0x2300, 0x26},  {0x29D0, 0x2380, 0x08},
  };
  for (const Range& r : kRanges) {
    for (uint32_t i = 0; i < r.count; ++i) {
      g_decoder->regs[r.first_register + i] = REX_LOAD_U32(g_device + r.device_offset + 4 * i);
    }
  }
  // Bool constants: the device stores the VS and PS halves swapped.
  for (uint32_t i = 0; i < 8; ++i) {
    g_decoder->regs[0x4900 + i] = REX_LOAD_U32(g_device + 0x2700 + 4 * (i ^ 4));
  }
}

// PM4 type-3 opcodes: the SDK's list (rex/graphics xenos PM4_*) plus a few
// other R400 ones.
bool IsKnownOpcode(uint32_t opcode) {
  switch (opcode) {
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x19: case 0x1A: case 0x1C: case 0x1D: case 0x1E:
    case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x2B: case 0x2C:
    case 0x2D: case 0x2E: case 0x2F: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4F: case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D:
    case 0x5E: case 0x60: case 0x61: case 0x62: case 0x63: case 0x64:
      return true;
    default:
      return false;
  }
}

// Size in dwords of the packet `header` starts, or 0 if it can't be a packet
// the D3D writes.
uint32_t PlausiblePacketSize(uint32_t header) {
  const uint32_t type = header >> 30;
  const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
  switch (type) {
    case 0:
      if (header == 0 || count > 0x800 || (header & 0x7FFF) + count > pm4::kRegisterCount) {
        return 0;
      }
      return 1 + count;
    case 1:
      return 3;
    case 2:
      return 1;
    default:
      return count <= 0x2000 && IsKnownOpcode((header >> 8) & 0x7F) ? 1 + count : 0;
  }
}

// Decodes plausible packets from `address` up to `end`, stopping at the first
// implausible or incomplete one. Returns where it stopped.
uint32_t DecodePlausible(uint8_t* base, uint32_t address, uint32_t end) {
  while (address < end) {
    const uint32_t size = PlausiblePacketSize(REX_LOAD_U32(address));
    if (!size || address + size * 4 > end) {
      break;
    }
    address = g_decoder->Decode(base, address, address + size * 4);
  }
  return address;
}

// The earliest address in [from, end) from which the dwords parse as
// plausible packets ending exactly at `end`, or 0. One backward pass: an offset
// reaches `end` if its packet is plausible and the one after it reaches `end`.
uint32_t EarliestPacketStart(uint8_t* base, uint32_t from, uint32_t end) {
  if (from >= end) {
    return 0;
  }
  const uint32_t n = (end - from) / 4;
  from = end - 4 * n;
  static std::vector<uint8_t> reach;
  reach.assign(n + 1, 0);
  reach[n] = 1;
  for (uint32_t i = n; i-- > 0;) {
    const uint32_t size = PlausiblePacketSize(REX_LOAD_U32(from + 4 * i));
    reach[i] = size && i + size <= n && reach[i + size];
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (reach[i]) {
      return from + 4 * i;
    }
  }
  return 0;
}

void Sync(uint8_t* base, uint32_t function, char where) {
  g_base = base;
  const uint32_t next = REX_LOAD_U32(g_device + kWritePointerOffset) + 4;
  const uint32_t limit = REX_LOAD_U32(g_device + kLimitOffset);
  const uint32_t before = g_cursor;
  {
    const uint32_t physical = next & 0x1FFFFFFFu;
    if (physical < g_ring_low.load(std::memory_order_relaxed)) {
      g_ring_low.store(physical, std::memory_order_relaxed);
    }
    if (physical > g_ring_high.load(std::memory_order_relaxed)) {
      g_ring_high.store(physical, std::memory_order_relaxed);
    }
  }
  const uint64_t packets = g_decoder->packets, zeros = g_decoder->zero_headers;
  // A new ring segment (limit changed) or a jump larger than one call writes
  // means the bytes in between are not packets from this observation (often
  // stale data from the previous pass around the ring): resync instead.
  bool resync = false;
  if (g_live_requested.load(std::memory_order_relaxed) && g_cursor &&
      (limit != g_limit || next < g_cursor)) {
    // The D3D switched command buffer segments. The switch (sub_8242A8B8) is
    // hooked, so this sync is right after it: finish the old segment up to its
    // fence and trailer, then continue at the new segment's write pointer,
    // past its header (scanning back for packets would find stale ones).
    ++g_resync_stats.segment;
    DecodePlausible(base, g_cursor, g_limit + kSegmentReserve);
    g_limit = limit;
    g_cursor = next;
  } else {
    resync = limit != g_limit || next < g_cursor || next - g_cursor > kMaxSyncGap;
    if (limit != g_limit) {
      ++g_resync_stats.segment;
    } else if (next < g_cursor) {
      ++g_resync_stats.backward;
    } else if (next - g_cursor > kMaxSyncGap) {
      ++g_resync_stats.gap;
      g_resync_stats.gap_kb += (next - g_cursor) >> 10;
    }
    g_limit = limit;
  }
  if (!resync) {
    // Only what parses as real packets: never interpret stray words.
    g_cursor = DecodePlausible(base, g_cursor, next);
    // Stuck before an implausible header (or one claiming far more than has
    // been written): the D3D reserves blocks in its command buffer (per-tile
    // setup filled in at EndTiling, stale until then) that the GPU never
    // executes. Resume at the first offset from which the packets parse cleanly
    // up to the write pointer, so the packets written after the block are still
    // decoded, and in order with the clears and resolves around them.
    // (A zero dword at the cursor is just not written yet: wait for it.)
    const uint32_t cursor_header = g_cursor < next ? REX_LOAD_U32(g_cursor) : 0;
    const bool resume_enabled = REXCVAR_GET(tenchuz_gpu_stuck_resume);
    if (g_cursor < next && ((resume_enabled && cursor_header && !PlausiblePacketSize(cursor_header)) ||
                            (g_cursor == before && next - g_cursor > kStuckGap))) {
      ++g_resync_stats.stuck;
      // Kick headers are a few dwords; never resume further in (stale data).
      uint32_t resume = resume_enabled ? EarliestPacketStart(base, g_cursor + 4, next) : 0;
      if (resume > g_cursor + kKickHeaderMax) {
        resume = 0;
      }
      if (!g_frame_stuck_address) {
        g_frame_stuck_address = g_cursor;
        static int device_dumps = 0;
        if (device_dumps < 4) {
          ++device_dumps;
          std::string words;
          for (uint32_t i = 0; i < 32; ++i) {
            words += fmt::format("+{:02X}:{:08X} ", 4 * i, REX_LOAD_U32(g_device + 4 * i));
          }
          REXLOG_INFO("tenchuz: observe stuck at {:08X}, device: {}", g_cursor, words);
        }
      }
      static int logged = 0;
      if (logged < 8) {
        ++logged;
        REXLOG_INFO("tenchuz: observe stuck {} {:08X} at {:08X} (header {:08X}), {} bytes written; resume at +{}",
                    where, function, g_cursor, REX_LOAD_U32(g_cursor), next - g_cursor,
                    resume ? int64_t(resume - g_cursor) : -1);
      }
      if (resume) {
        ++g_resync_stats.resumed;
        g_cursor = g_decoder->Decode(base, resume, next);
      } else if (next - g_cursor > kStuckGap) {
        resync = true;
      }
    }
  }
  if (resync) {
    ++g_resyncs;
    g_cursor = next;
    SeedFromDevice(base);
  }
  const int32_t debug = REXCVAR_GET(tenchuz_gpu_observe_debug);
  if (debug > 0 && g_debug_lines < debug) {
    ++g_debug_lines;
    REXLOG_INFO("tenchuz: observe {} {:08X} cursor {:08X} -> {:08X} next {:08X}{} packets +{} zeros +{} limit {:08X}",
                where, function, before, g_cursor, next, resync ? " RESYNC" : "",
                g_decoder->packets - packets, g_decoder->zero_headers - zeros,
                limit);
  }
}

// End of a kick (sub_82426DF0): the D3D finished a command segment with a
// fence, possibly moved to a new segment, left a few header dwords the GPU
// never executes, and may have re-emitted some state after them. Decode the
// fence, then resume at the first packet after the header: within what the
// kick itself wrote (fresh), or the last few hundred bytes of a new segment.
void EndKick(uint8_t* base) {
  g_base = base;
  const uint32_t next = REX_LOAD_U32(g_device + kWritePointerOffset) + 4;
  const uint32_t limit = REX_LOAD_U32(g_device + kLimitOffset);
  ++g_resync_stats.kicks;
  uint32_t from;
  if (limit != g_limit || next < g_cursor) {
    ++g_resync_stats.segment;
    DecodePlausible(base, g_cursor, g_limit + kSegmentReserve);
    g_limit = limit;
    from = next > kKickScan ? next - kKickScan : 0;
  } else {
    g_cursor = DecodePlausible(base, g_cursor, next);
    if (g_cursor >= next) {
      return;
    }
    from = g_cursor + 4;
  }
  const uint32_t resume = EarliestPacketStart(base, from, next);
  g_cursor = resume ? g_decoder->Decode(base, resume, next) : next;
}

bool OnRenderThread() {
  return rex::runtime::ThreadState::GetThreadID() == g_render_thread;
}

void Dump() {
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  const auto path = g_dir / fmt::format("gpu_observe_f{}.bin", g_record_frame);
  std::ofstream out(path, std::ios::binary);
  const uint32_t header[8] = {kMagic,
                              1,
                              static_cast<uint32_t>(g_draws.size()),
                              pm4::kRegisterCount,
                              kDeviceDwords,
                              g_device,
                              static_cast<uint32_t>(g_record_frame),
                              0};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  out.write(reinterpret_cast<const char*>(g_first_written.data()),
            g_first_written.size() * sizeof(int32_t));
  for (const DrawRecord& rec : g_draws) {
    out.write(reinterpret_cast<const char*>(&rec.header), sizeof(DrawHeader));
    out.write(reinterpret_cast<const char*>(rec.regs.data()), rec.regs.size() * 4);
    out.write(reinterpret_cast<const char*>(rec.device.data()), rec.device.size() * 4);
  }
  REXLOG_INFO(
      "tenchuz: GPU observe frame {}: {} draws, {} packets, {} indirect buffers, {} resyncs, "
      "{} zero headers -> {}",
      g_record_frame, g_draws.size(), g_decoder->packets, g_decoder->indirect_buffers, g_resyncs,
      g_decoder->zero_headers, path.string());
  g_draws.clear();
  g_draws.shrink_to_fit();
}

}  // namespace

bool Active() {
  return g_active.load(std::memory_order_relaxed);
}

void Enter(uint32_t function, uint8_t* base) {
  if (!OnRenderThread()) {
    return;
  }
  if (g_depth++ == 0) {
    g_top_function = function;
  }
  if (g_kick_depth) {
    return;  // inside a kick: the stream is consistent again when it returns
  }
  Sync(base, function, '>');
  if (IsKickFunction(function)) {
    g_kick_depth = g_depth;
  }
}

void Exit(uint32_t function, uint8_t* base) {
  if (!OnRenderThread()) {
    return;
  }
  if (g_kick_depth) {
    if (g_depth == g_kick_depth) {
      g_kick_depth = 0;
      EndKick(base);
    }
  } else {
    Sync(base, function, '<');
  }
  if (g_depth > 0) {
    --g_depth;
  }
}

void StartDecoding(uint32_t device, uint8_t* base) {
  g_device = device;
  g_render_thread = rex::runtime::ThreadState::GetThreadID();
  if (!g_decoder) {
    g_decoder = std::make_unique<pm4::Decoder>();
    g_decoder->on_draw = OnDraw;
  }
  g_cursor = REX_LOAD_U32(device + kWritePointerOffset) + 4;
  g_limit = REX_LOAD_U32(device + kLimitOffset);
  SeedFromDevice(base);
  g_depth = 0;
  g_kick_depth = 0;
  g_resyncs = 0;
  g_decoding = true;
  g_active.store(true);
}

void StopDecoding() {
  g_decoding = false;
  g_active.store(false);
}

void OnSwap(uint32_t device, uint8_t* base) {
  if (g_decoding) {
    Sync(base, 0x8241BF50, 'S');  // packets emitted since the last hooked call
  }
  // Diagnostics: what the first stuck point of the frame holds once the frame
  // has been written.
  if (g_frame_stuck_address) {
    static int dumped = 0;
    if (dumped < 6) {
      ++dumped;
      std::string words;
      for (int32_t i = -6; i < 26; ++i) {
        words += fmt::format("{}{:08X} ", i == 0 ? "| " : "", REX_LOAD_U32(g_frame_stuck_address + 4 * i));
      }
      REXLOG_INFO("tenchuz: observe stuck point {:08X} at frame end: {}", g_frame_stuck_address, words);
    }
    g_frame_stuck_address = 0;
  }
  ++g_frame;
  const bool live = g_live_requested.load();

  switch (g_state) {
    case State::kWarmup:
      // Registers written in the warm-up frame are known from the first recorded draw.
      for (uint32_t r = 0; r < pm4::kRegisterCount; ++r) {
        if (g_decoder->written[r]) {
          g_first_written[r] = -1;
        }
      }
      g_state = State::kRecord;
      g_record_frame = g_frame;
      return;
    case State::kRecord:
      g_state = State::kOff;
      Dump();
      if (!live) {
        StopDecoding();
      }
      return;
    case State::kOff:
      break;
  }

  if (live && !g_decoding) {
    StartDecoding(device, base);
    REXLOG_INFO("tenchuz: live GPU state decoding on (device {:08X})", device);
  } else if (!live && g_decoding) {
    StopDecoding();
  }

  const int32_t at_frame = REXCVAR_GET(tenchuz_gpu_observe_at_frame);
  const int32_t every = REXCVAR_GET(tenchuz_gpu_observe_every);
  const bool start = g_requested.exchange(false) ||
                     (at_frame > 0 && g_frame == static_cast<uint64_t>(at_frame)) ||
                     (every > 0 && g_frame % static_cast<uint64_t>(every) == 0);
  if (!start) {
    return;
  }
  if (!g_decoding) {
    StartDecoding(device, base);
  }
  g_first_written.assign(pm4::kRegisterCount, INT32_MAX);
  g_decoder->ResetWritten();
  g_debug_lines = 0;
  g_state = State::kWarmup;
  REXLOG_INFO("tenchuz: GPU observe armed at frame {} (device {:08X})", g_frame, device);
}

void SetLiveDecoding(bool enable, LiveDrawCallback callback) {
  g_live_callback = callback;
  g_live_requested.store(enable);
}

bool InCommandRing(uint32_t address) {
  address &= 0x1FFFFFFFu;
  // Segments are filled from their start, so the lowest write pointer seen is
  // a little past the ring's start: allow one segment-scan window below it.
  const uint32_t low = g_ring_low.load(std::memory_order_relaxed);
  const uint32_t high = g_ring_high.load(std::memory_order_relaxed);
  return low <= high && address + kSegmentScan >= low && address <= high + kSegmentReserve;
}

ResyncStats TakeResyncStats() {
  const ResyncStats s = g_resync_stats;
  g_resync_stats = {};
  return s;
}

void RequestObserve() {
  g_requested.store(true);
}

void SetOutputDir(const std::filesystem::path& dir) {
  g_dir = dir;
}

}  // namespace tenchuz::gpu_observer
