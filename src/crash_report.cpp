// tenchuz - unhandled-exception reporter. See crash_report.h.

#include "crash_report.h"

#include <rex/logging.h>
#include <rex/system/thread_state.h>

#if defined(_WIN32)
#include <windows.h>
// windows.h must come first
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace tenchuz {

#if defined(_WIN32)
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
volatile LONG g_reporting = 0;

void LogGuestContext() {
  auto* state = rex::runtime::ThreadState::Get();
  if (!state || !state->context()) {
    REXLOG_CRITICAL("  (crash is not on a guest thread)");
    return;
  }
  const PPCContext& c = *state->context();
  REXLOG_CRITICAL("  guest thread {}  lr={:08X} ctr={:08X} last_indirect_target={:08X}",
                  state->thread_id(), static_cast<uint32_t>(c.lr), c.ctr.u32,
                  c.last_indirect_target);
  const PPCRegister* r[32] = {&c.r0,  &c.r1,  &c.r2,  &c.r3,  &c.r4,  &c.r5,  &c.r6,  &c.r7,
                              &c.r8,  &c.r9,  &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15,
                              &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21, &c.r22, &c.r23,
                              &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31};
  for (int i = 0; i < 32; i += 4) {
    REXLOG_CRITICAL("  r{:<2}={:08X} r{:<2}={:08X} r{:<2}={:08X} r{:<2}={:08X}", i, r[i]->u32,
                    i + 1, r[i + 1]->u32, i + 2, r[i + 2]->u32, i + 3, r[i + 3]->u32);
  }
}

void LogBacktrace(const CONTEXT& fault_context) {
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);

  CONTEXT context = fault_context;
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;

  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        !frame.AddrPC.Offset) {
      break;
    }
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
      REXLOG_CRITICAL("  #{:02} {}+0x{:X}", i, symbol->Name, displacement);
    } else {
      REXLOG_CRITICAL("  #{:02} {:016X}", i, frame.AddrPC.Offset);
    }
  }
  SymCleanup(process);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* info) {
  if (InterlockedExchange(&g_reporting, 1) == 0) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    REXLOG_CRITICAL("Unhandled exception 0x{:08X} at host {:016X}", record->ExceptionCode,
                    reinterpret_cast<uint64_t>(record->ExceptionAddress));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
      REXLOG_CRITICAL("  {} of host {:016X}",
                      record->ExceptionInformation[0] == 1 ? "write" : "read",
                      record->ExceptionInformation[1]);
    }
    LogGuestContext();
    LogBacktrace(*info->ContextRecord);
    rex::FlushLogging();
  }
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashReporter() {
  g_previous_filter = SetUnhandledExceptionFilter(UnhandledFilter);
}
#else
void InstallCrashReporter() {}
#endif

}  // namespace tenchuz
