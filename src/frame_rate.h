// tenchuz - frame rate. See frame_rate.cpp.

#pragma once

#include <cstdint>

namespace tenchuz::frame_rate {

// At startup: 1 ms timer resolution for the whole process, like PC games.
// Guest sleeps and waits otherwise round up to 15.6 ms scheduler ticks.
void Initialize();

// Called from the D3D Swap hook before the original Swap runs (r3 = device):
// picks the present interval for the frames that follow.
void OnSwap(uint8_t* base, uint32_t device);

}  // namespace tenchuz::frame_rate
