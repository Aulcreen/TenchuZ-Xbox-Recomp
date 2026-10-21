// tenchuz - fixes to the prebuilt ReXGlue SDK runtime that don't need an SDK
// rebuild. See sdk_fixes.h.

#include "sdk_fixes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <cwchar>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(tenchuz_timer_thread_sleep, true, "tenchuz",
                    "Let the SDK's timer thread sleep between timers instead of spinning a CPU core");

namespace tenchuz::sdk_fixes {
namespace {

// The SDK's TimerQueue thread waits for its next timer with disruptorplus's
// spin_wait_strategy: it spins, then calls std::this_thread::yield() over and
// over (sleeping 1 ms only every 20th time), so it kept a core 89% busy for
// the whole game. Its yields sleep 1 ms instead (timer resolution is 1 ms,
// frame_rate.h), so timers still fire within about a millisecond. Every
// other thread's yield is unchanged.
void __cdecl TimerAwareYield() {
  thread_local int timer_thread = -1;
  if (timer_thread < 0) {
    timer_thread = 0;
    PWSTR name = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &name)) && name) {
      timer_thread = std::wcscmp(name, L"rex::thread::TimerQueue") == 0;
      LocalFree(name);
    }
  }
  if (timer_thread) {
    Sleep(1);
  } else {
    SwitchToThread();
  }
}

// Points `module`'s import of `function` from `dll` at `replacement`.
bool PatchImport(HMODULE module, const char* dll, const char* function, void* replacement) {
  auto* base = reinterpret_cast<uint8_t*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name;
       ++desc) {
    if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) continue;
    auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
      DWORD old_protect = 0;
      if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old_protect)) {
        return false;
      }
      slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
      VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old_protect, &old_protect);
      return true;
    }
  }
  return false;
}

}  // namespace

void Apply() {
  if (!REXCVAR_GET(tenchuz_timer_thread_sleep)) return;
  for (const wchar_t* name : {L"rexruntimerd.dll", L"rexruntime.dll", L"rexruntimed.dll"}) {
    if (HMODULE runtime = GetModuleHandleW(name)) {
      const bool ok = PatchImport(runtime, "msvcp140.dll", "_Thrd_yield", reinterpret_cast<void*>(&TimerAwareYield));
      REXLOG_INFO("tenchuz: SDK timer thread {}", ok ? "sleeps between timers" : "yield import not found");
      return;
    }
  }
}

}  // namespace tenchuz::sdk_fixes
