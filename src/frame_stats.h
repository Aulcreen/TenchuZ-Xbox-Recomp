// tenchuz - guest frame statistics.
//
// Counts frames at the game's D3D Swap (sub_8241BF50), feeds the F3 debug
// overlay, and logs the frame rate every few seconds so test runs can be
// compared without anyone watching the screen.

#pragma once

#include <rex/ui/overlay/debug_overlay.h>

namespace tenchuz {

rex::ui::FrameStats GetFrameStats();

}  // namespace tenchuz
