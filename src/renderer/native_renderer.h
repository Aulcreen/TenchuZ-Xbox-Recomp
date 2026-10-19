// tenchuz - native renderer (milestone N3: frame skeleton).
//
// Presents frames we produce ourselves through the SDK's D3D12 presenter, on
// the same D3D12 device and queue the SDK uses (so the F3/F4 overlays and the
// presenter's scaling keep working). While native output is on, the
// emulator's swap packet is turned into NOPs right after VdSwap writes it
// (mid-asm hook at 0x8241C0E0), so the emulated GPU keeps running fences,
// interrupts and pacing but no longer presents.
//
// F10 toggles between the emulator's image and native output.

#pragma once

#include <cstdint>

namespace rex {
class Runtime;
}

namespace tenchuz::native {

void Initialize(rex::Runtime* runtime);

bool PresentNative();
void TogglePresentNative();

// Writes the resolved images of the next native frame to logs/frame_dumps.
void RequestFrameDump();

// Called from the D3D Swap hook before the original Swap runs (r3 = device).
void OnSwap(uint8_t* base, uint32_t device);

}  // namespace tenchuz::native
