// tenchuz - sampling profiler for the game's threads. See profiler.h.
//
// Recompiled guest functions are ordinary host functions that call each other,
// so a thread's host stack is its guest call chain. A capture first measures
// every thread's CPU cycles for a second and keeps the busiest few. Then the
// sampler thread suspends each of them about 1000 times a second, unwinds the
// stack with the exe's unwind tables (nothing that allocates while a thread is
// suspended) and resumes it. Addresses are resolved with the PDB at the end.

#include "profiler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(tenchuz_profile_at, 0, "tenchuz",
                     "Profile the busiest threads starting this many seconds after start (0: off; F7 starts one too)");
REXCVAR_DEFINE_INT32(tenchuz_profile_seconds, 10, "tenchuz", "Length of a profile capture, in seconds");
REXCVAR_DEFINE_INT32(tenchuz_profile_threads, 4, "tenchuz", "How many of the busiest threads a profile samples");

namespace tenchuz::profiler {
namespace {

constexpr int kMaxDepth = 48;
constexpr int kTopLines = 40;

std::atomic<bool> g_requested{false};
std::atomic<bool> g_running{false};
std::atomic<DWORD> g_main_thread{0};
std::mutex g_mutex;
std::filesystem::path g_dir;
int g_count = 0;
const auto g_start = std::chrono::steady_clock::now();

struct Thread {
  DWORD id = 0;
  HANDLE handle = nullptr;
  std::string name;
  double cpu = 0;  // share of one core while measuring
  std::vector<uint64_t> pcs;  // kMaxDepth per sample, 0-terminated
  size_t samples = 0;
};

std::string ThreadName(HANDLE handle, DWORD id) {
  std::string name;
  PWSTR description = nullptr;
  if (SUCCEEDED(GetThreadDescription(handle, &description)) && description) {
    for (const wchar_t* c = description; *c; ++c) name += *c < 0x80 ? char(*c) : '?';
    LocalFree(description);
  }
  if (id == g_main_thread.load()) name += name.empty() ? "main loop" : " (main loop)";
  return name.empty() ? "unnamed" : name;
}

// The busiest threads of this process over one second, by CPU cycles.
std::vector<Thread> BusiestThreads(size_t count) {
  struct Candidate {
    DWORD id;
    HANDLE handle;
    ULONG64 cycles;
  };
  std::vector<Candidate> candidates;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry = {sizeof(entry)};
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
    HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               entry.th32ThreadID);
    if (!handle) continue;
    ULONG64 cycles = 0;
    QueryThreadCycleTime(handle, &cycles);
    candidates.push_back({entry.th32ThreadID, handle, cycles});
  }
  CloseHandle(snapshot);
  LARGE_INTEGER frequency, t0, t1;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&t0);
  const uint64_t tsc0 = __rdtsc();
  Sleep(1000);
  QueryPerformanceCounter(&t1);
  const double tsc_per_second =
      double(__rdtsc() - tsc0) / (double(t1.QuadPart - t0.QuadPart) / double(frequency.QuadPart));
  for (auto& c : candidates) {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(c.handle, &cycles);
    c.cycles = cycles - c.cycles;
  }
  std::sort(candidates.begin(), candidates.end(), [](auto& a, auto& b) { return a.cycles > b.cycles; });
  std::vector<Thread> threads;
  for (auto& c : candidates) {
    if (threads.size() < count && c.cycles) {
      Thread thread;
      thread.id = c.id;
      thread.handle = c.handle;
      thread.name = ThreadName(c.handle, c.id);
      thread.cpu = double(c.cycles) / tsc_per_second;
      threads.push_back(std::move(thread));
    } else {
      CloseHandle(c.handle);
    }
  }
  return threads;
}

// Unwinds one suspended thread context into out[0..kMaxDepth).
void Walk(CONTEXT context, uint64_t* out) {
  int depth = 0;
  while (depth < kMaxDepth - 1 && context.Rip) {
    out[depth++] = context.Rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {  // leaf: the return address is on top of the stack
      context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                     nullptr);
  }
  out[depth] = 0;
}

double Sample(std::vector<Thread>& threads, int seconds) {
  const size_t max_samples = size_t(seconds) * 1100 + 64;
  for (auto& t : threads) t.pcs.assign(max_samples * kMaxDepth, 0);
  timeBeginPeriod(1);
  const auto start = std::chrono::steady_clock::now();
  const auto end = start + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < end) {
    for (auto& t : threads) {
      if (t.samples >= max_samples || SuspendThread(t.handle) == DWORD(-1)) continue;
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t.handle, &context)) {
        Walk(context, &t.pcs[t.samples * kMaxDepth]);
        ++t.samples;
      }
      ResumeThread(t.handle);
    }
    Sleep(1);
  }
  timeEndPeriod(1);
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Function start for an address (from the unwind tables; leaf functions keep
// their address and get resolved by name).
uint64_t FunctionStart(uint64_t pc) {
  DWORD64 image_base = 0;
  PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
  return function ? image_base + function->BeginAddress : pc;
}

using Counts = std::unordered_map<std::string, size_t>;

std::string Table(const char* title, const Counts& counts, size_t total) {
  std::vector<std::pair<std::string, size_t>> rows(counts.begin(), counts.end());
  std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second > b.second; });
  if (rows.size() > size_t(kTopLines)) rows.resize(kTopLines);
  std::string text = fmt::format("\n-- {} ({} samples)\n", title, total);
  for (auto& [name, count] : rows) {
    text += fmt::format("{:6.2f}%  {:6}  {}\n", 100.0 * double(count) / double(std::max<size_t>(1, total)), count,
                        name);
  }
  return text;
}

std::string Report(const std::vector<Thread>& threads, double seconds) {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  // Someone else in the process (the SDK's crash handler) may have started
  // DbgHelp already: then its session is used, and left running.
  const bool own_symbols = SymInitialize(process, nullptr, TRUE);
  const bool symbols = own_symbols || GetLastError() == ERROR_INVALID_PARAMETER;
  if (!own_symbols && symbols) SymRefreshModuleList(process);
  std::unordered_map<uint64_t, std::string> names;
  auto name_of = [&](uint64_t start) -> const std::string& {
    auto it = names.find(start);
    if (it != names.end()) return it->second;
    std::string name = fmt::format("{:X}", start);
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    auto* info = reinterpret_cast<SYMBOL_INFO*>(buffer);
    info->SizeOfStruct = sizeof(SYMBOL_INFO);
    info->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (symbols && SymFromAddr(process, start, &displacement, info)) {
      name = info->Name;
    } else if (HMODULE module = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                             reinterpret_cast<LPCWSTR>(start), &module)) {
      char path[MAX_PATH] = {};
      GetModuleFileNameA(module, path, MAX_PATH);
      const char* file = std::strrchr(path, '\\');
      name = fmt::format("{}+0x{:X}", file ? file + 1 : path, start - uint64_t(module));  // for llvm-symbolizer
    }
    // __imp__sub_X is the body behind the hookable sub_X; count them as one.
    if (name.rfind("__imp__", 0) == 0) name = name.substr(7);
    return names.emplace(start, std::move(name)).first->second;
  };

  std::string text = fmt::format("tenchuz profile: {} threads over {:.1f} s\n", threads.size(), seconds);
  for (const auto& t : threads) {
    text += fmt::format("  thread {} \"{}\": {:.0f}% of a core\n", t.id, t.name, 100.0 * t.cpu);
  }
  for (const auto& t : threads) {
    // self: innermost frame. guest self: innermost recompiled (sub_) frame.
    // inclusive: every function on the stack. Samples in a kernel wait or in
    // D3D Swap (sub_8241BF50, the frame's vblank wait) don't count as work.
    Counts self, guest_self, inclusive, waiting;  // waiting: where a thread blocks, outside the vblank wait
    std::unordered_set<std::string> seen;
    std::vector<const std::string*> frames;
    size_t work = 0;
    for (size_t s = 0; s < t.samples; ++s) {
      const uint64_t* stack = &t.pcs[s * kMaxDepth];
      frames.clear();
      bool idle = false, in_swap = false;
      for (int d = 0; d < kMaxDepth && stack[d]; ++d) {
        const std::string& name = name_of(FunctionStart(stack[d]));
        frames.push_back(&name);
        in_swap |= name == "sub_8241BF50" || name == "sub_8241BDE0";
        if (d == 0) idle |= name.rfind("Zw", 0) == 0 || name.rfind("Nt", 0) == 0;
      }
      if (frames.empty()) continue;
      ++self[*frames[0]];
      if (idle && !in_swap) {
        seen.clear();
        for (const std::string* name : frames) {
          if (seen.insert(*name).second) ++waiting[*name];
        }
      }
      if (idle || in_swap) continue;
      ++work;
      seen.clear();
      bool guest_found = false;
      for (const std::string* name : frames) {
        if (!guest_found && name->rfind("sub_", 0) == 0) {
          ++guest_self[*name];
          guest_found = true;
        }
        if (seen.insert(*name).second) ++inclusive[*name];
      }
    }
    text += fmt::format("\n== thread {} \"{}\": {} samples, {} working ({:.0f}%)\n", t.id, t.name, t.samples, work,
                        100.0 * double(work) / double(std::max<size_t>(1, t.samples)));
    text += Table("self, all samples", self, t.samples);
    text += Table("guest self, working samples", guest_self, work);
    text += Table("inclusive, working samples", inclusive, work);
    text += Table("waiting (blocked outside the vblank wait), inclusive", waiting, t.samples);
  }
  if (own_symbols) SymCleanup(process);
  return text;
}

void Run(int seconds, size_t thread_count) {
  auto threads = BusiestThreads(thread_count);
  const double elapsed = Sample(threads, seconds);
  const std::string text = Report(threads, elapsed);
  for (auto& t : threads) CloseHandle(t.handle);

  std::filesystem::path path;
  {
    std::lock_guard lock(g_mutex);
    path = g_dir / fmt::format("profile_{}.txt", ++g_count);
  }
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  std::ofstream(path) << text;
  REXLOG_INFO("tenchuz: profile written to {}", path.string());
  size_t lines = 0;
  for (size_t at = 0, next; at < text.size() && lines < 12; at = next + 1, ++lines) {
    next = text.find('\n', at);
    if (next == std::string::npos) next = text.size();
    REXLOG_INFO("{}", text.substr(at, next - at));
  }
}

}  // namespace

void OnMainLoopFrame() {
  g_main_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
  const int at = REXCVAR_GET(tenchuz_profile_at);
  static bool scheduled_done = false;
  if (at > 0 && !scheduled_done && std::chrono::steady_clock::now() - g_start >= std::chrono::seconds(at)) {
    scheduled_done = true;
    g_requested = true;
  }
  if (!g_requested.exchange(false) || g_running.exchange(true)) return;
  const int seconds = std::max(1, REXCVAR_GET(tenchuz_profile_seconds));
  const size_t thread_count = size_t(std::clamp(REXCVAR_GET(tenchuz_profile_threads), 1, 16));
  REXLOG_INFO("tenchuz: profiling the {} busiest threads for {} s", thread_count, seconds);
  std::thread([seconds, thread_count] {
    Run(seconds, thread_count);
    g_running = false;
  }).detach();
}

void RequestCapture() { g_requested = true; }

void SetOutputDir(const std::filesystem::path& dir) {
  std::lock_guard lock(g_mutex);
  g_dir = dir;
}

}  // namespace tenchuz::profiler
