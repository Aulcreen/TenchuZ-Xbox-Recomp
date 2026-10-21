// tenchuz - debug tools for finding game variables and the code that writes
// them: per-frame guest memory dumps and a hardware write watchpoint. See
// debug_tools.cpp.

#pragma once

#include <cstdint>
#include <filesystem>

namespace tenchuz::debug_tools {

// Called once per main-loop frame on the main thread (frame_rate.cpp).
void OnMainLoopFrame(uint8_t* base);
void SetOutputDir(const std::filesystem::path& dir);

// Dumps the image range (the game's globals) on the next main-loop frame.
void RequestImageDump();

}  // namespace tenchuz::debug_tools
