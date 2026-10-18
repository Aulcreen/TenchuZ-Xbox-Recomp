// tenchuz - unhandled-exception reporter.
//
// Logs a symbolized host backtrace (recompiled functions are named
// sub_<guest address>, so it doubles as a guest backtrace) plus the crashing
// guest thread's PPC registers, then lets the process die as before.

#pragma once

namespace tenchuz {

void InstallCrashReporter();

}  // namespace tenchuz
