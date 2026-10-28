// tenchuz - guest frame statistics.
//
// Counts frames at the game's D3D Swap (sub_8241BF50), feeds the F3 debug
// overlay, and logs the frame rate every few seconds so test runs can be
// compared without anyone watching the screen. On Windows, a game thread that
// stops presenting for 2 s is logged with its call stack.

#pragma once

#include <rex/ui/overlay/debug_overlay.h>

namespace tenchuz {

rex::ui::FrameStats GetFrameStats();

// The game's wait for the presentation interval (SynchronizeToPresentationInterval):
// the end of its work on a frame, for the frame-time log.
void OnPresentWait();

}  // namespace tenchuz
