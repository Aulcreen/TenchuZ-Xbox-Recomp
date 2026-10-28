// tenchuz - debug tools for finding game variables and the code that writes them.
//
// --tenchuz_dump_memory_at_frame=N: on main-loop frames N .. N+frames-1, write
// the guest's heap (0x40000000-0x7FFFFFFF), image (0x82000000-0x8FFFFFFF) and
// one view of physical memory (0xA0000000-0xBFFFFFFF), committed pages only,
// to <traces>/mem_f<frame>.bin: records of {u32 guest address, u32 size, data}.
// Frame-counted state moves exactly one step per frame however long a dump
// takes, so consecutive dumps show it (tools/find_frame_steps.py).
//
// --tenchuz_watch_write=0xADDR: a hardware write watchpoint (4 bytes) on that
// guest address in the main thread; each new writer is logged by name.
// --tenchuz_watch_reads=true makes it watch reads too.

#include "debug_tools.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(tenchuz_dump_memory_at_frame, 0, "tenchuz", "Dump guest memory on this main-loop frame (0: off)");
REXCVAR_DEFINE_INT32(tenchuz_dump_memory_frames, 3, "tenchuz", "Consecutive frames to dump");
REXCVAR_DEFINE_STRING(tenchuz_watch_write, "", "tenchuz",
                      "Guest address (hex) whose writes from the main thread are logged by function");
REXCVAR_DEFINE_BOOL(tenchuz_watch_reads, false, "tenchuz", "tenchuz_watch_write logs reads too");

namespace tenchuz::debug_tools {
namespace {

std::mutex g_mutex;
std::filesystem::path g_dir;
uint64_t g_frame = 0;

constexpr int kMaxWriters = 64;
constexpr int kStackDepth = 24;
std::atomic<uint64_t> g_writers[kMaxWriters];
uint64_t g_writer_stacks[kMaxWriters][kStackDepth];  // filled before the writer is published
std::atomic<int> g_writer_count{0};
std::atomic<uint64_t> g_watch_hits{0};
int g_logged_writers = 0;

std::atomic<bool> g_image_dump_requested{false};
std::atomic<uint32_t> g_watch_requested{0};
std::atomic<int> g_frame_dumps_requested{0};
int g_frame_dumps_left = 0;

void Dump(uint8_t* base, uint64_t frame, bool image_only = false) {
  std::filesystem::path path;
  {
    std::lock_guard lock(g_mutex);
    path = g_dir / fmt::format("mem_f{}.bin", frame);
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return;
  uint64_t total = 0;
  const std::pair<uint32_t, uint32_t> ranges[] = {
      {0x40000000u, 0x80000000u}, {0x82000000u, 0x90000000u}, {0xA0000000u, 0xC0000000u}};
  for (auto [lo, hi] : ranges) {
    if (image_only && lo != 0x82000000u) continue;
    uint64_t at = lo;
    while (at < hi) {
      MEMORY_BASIC_INFORMATION info;
      if (!VirtualQuery(base + at, &info, sizeof(info))) break;
      const uint64_t region_end =
          std::min<uint64_t>(hi, uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base) + info.RegionSize);
      const bool readable = info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
      if (readable && region_end > at) {
        const uint32_t header[2] = {uint32_t(at), uint32_t(region_end - at)};
        fwrite(header, sizeof(header), 1, file);
        fwrite(base + at, 1, size_t(region_end - at), file);
        total += region_end - at;
      }
      at = region_end;
    }
  }
  fclose(file);
  REXLOG_INFO("tenchuz: frame {}: dumped {} MB of guest memory to {}", frame, total >> 20, path.string());
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  if (!(info->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
  info->ContextRecord->Dr6 = 0;
  g_watch_hits.fetch_add(1, std::memory_order_relaxed);
  const uint64_t pc = uint64_t(info->ExceptionRecord->ExceptionAddress);
  const int count = g_writer_count.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) {
    if (g_writers[i].load(std::memory_order_relaxed) == pc) return EXCEPTION_CONTINUE_EXECUTION;
  }
  const int slot = g_writer_count.fetch_add(1);
  if (slot < kMaxWriters) {
    // The writer's call stack, unwound from the trapped context.
    CONTEXT context = *info->ContextRecord;
    for (int d = 0; d < kStackDepth; ++d) {
      g_writer_stacks[slot][d] = context.Rip;
      if (!context.Rip) break;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (!function) {
        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
        context.Rsp += 8;
        continue;
      }
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                       nullptr);
    }
    g_writers[slot].store(pc, std::memory_order_release);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

std::string Symbol(uint64_t pc) {
  static bool initialized = [] {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
  }();
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
  info->SizeOfStruct = sizeof(SYMBOL_INFO);
  info->MaxNameLen = 255;
  DWORD64 displacement = 0;
  if (initialized && SymFromAddr(GetCurrentProcess(), pc, &displacement, info)) {
    return fmt::format("{}+0x{:X}", info->Name, displacement);
  }
  HMODULE module = nullptr;  // without symbols: module+offset, for an offline lookup
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(pc), &module)) {
    char name[MAX_PATH] = {};
    GetModuleFileNameA(module, name, MAX_PATH);
    return fmt::format("{}+0x{:X}", std::filesystem::path(name).filename().string(), pc - uint64_t(module));
  }
  return fmt::format("{:X}", pc);
}

// Sets DR0 on the calling (main) thread from a helper thread.
void ArmWatch(uint8_t* base, uint32_t guest) {
  static bool handler = (AddVectoredExceptionHandler(1, OnException), true);
  const DWORD main = GetCurrentThreadId();
  const uint64_t host = uint64_t(base + guest);
  std::thread([main, host] {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, main);
    if (!thread) return;
    SuspendThread(thread);
    CONTEXT context = {};
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(thread, &context);
    context.Dr0 = host;
    // L0 enable, R/W0 = 01 (writes) or 11 (reads and writes), LEN0 = 11 (4 bytes).
    const uint64_t access = REXCVAR_GET(tenchuz_watch_reads) ? 0x3ull : 0x1ull;
    context.Dr7 = (context.Dr7 & ~0xF0003ull) | 0x1ull | (access << 16) | (0x3ull << 18);
    SetThreadContext(thread, &context);
    ResumeThread(thread);
    CloseHandle(thread);
  }).join();
  REXLOG_INFO("tenchuz: watching writes to {:08X} on the main thread", guest);
}

}  // namespace

void OnMainLoopFrame(uint8_t* base) {
  const uint64_t frame = ++g_frame;
  const int at = REXCVAR_GET(tenchuz_dump_memory_at_frame);
  if (at > 0 && frame >= uint64_t(at) && frame < uint64_t(at) + uint64_t(REXCVAR_GET(tenchuz_dump_memory_frames))) {
    Dump(base, frame);
  }
  if (g_image_dump_requested.exchange(false)) {
    Dump(base, frame, true);
  }
  if (const int n = g_frame_dumps_requested.exchange(0)) {
    g_frame_dumps_left = n;
  }
  if (g_frame_dumps_left > 0) {
    --g_frame_dumps_left;
    Dump(base, frame);
  }
  static uint32_t watching = 0;
  if (const uint32_t guest = g_watch_requested.exchange(0)) {
    watching = guest;
    ArmWatch(base, guest);
  }
  const std::string& watch = REXCVAR_GET(tenchuz_watch_write);
  if (!watching && !watch.empty()) {
    watching = uint32_t(std::stoul(watch, nullptr, 16));
    ArmWatch(base, watching);
  }
  if (!watching) return;
  const int count = std::min(g_writer_count.load(), kMaxWriters);
  for (; g_logged_writers < count; ++g_logged_writers) {
    const uint64_t pc = g_writers[g_logged_writers].load();
    if (!pc) break;  // not published yet
    std::string stack;
    for (int d = 1; d < kStackDepth && g_writer_stacks[g_logged_writers][d]; ++d) {
      const std::string name = Symbol(g_writer_stacks[g_logged_writers][d]);
      if (name.find("sub_") == std::string::npos && name.find("xstart") == std::string::npos &&
          name.find("tenchuz.exe+") == std::string::npos)
        continue;
      stack += " < " + name;
    }
    REXLOG_INFO("tenchuz: {} {:08X} from {} (frame {}, {} hits so far){}",
                REXCVAR_GET(tenchuz_watch_reads) ? "access to" : "write to", watching, Symbol(pc), frame,
                g_watch_hits.load(), stack);
  }
}

void RequestWatch(uint32_t guest) { g_watch_requested = guest; }

void RequestFrameDumps(int frames) { g_frame_dumps_requested = frames; }

void RequestImageDump() { g_image_dump_requested = true; }

void SetOutputDir(const std::filesystem::path& dir) {
  std::lock_guard lock(g_mutex);
  g_dir = dir;
}

}  // namespace tenchuz::debug_tools
