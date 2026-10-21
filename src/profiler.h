// tenchuz - sampling profiler for the game's threads.
//
// F7 (or --tenchuz_profile_at=<seconds after start>) picks the busiest
// threads (--tenchuz_profile_threads) and samples their call stacks about
// 1000 times a second for --tenchuz_profile_seconds. It then writes the
// functions with the most samples, by themselves and including their callees,
// to <user>/traces/profile_<n>.txt.

#pragma once

#include <filesystem>

namespace tenchuz::profiler {

// Called by the main loop once a frame (frame_rate.cpp's frame-timer hook):
// remembers the main thread and starts a requested capture.
void OnMainLoopFrame();
void RequestCapture();
void SetOutputDir(const std::filesystem::path& dir);

}  // namespace tenchuz::profiler
