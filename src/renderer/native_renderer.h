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

#include <filesystem>

#include <cstdint>

namespace rex {
class Runtime;
}

namespace tenchuz::native {

// Before the D3D12 device exists: DRED for GPU hang reports (--tenchuz_dred).
void EnableDeviceDiagnostics();

void Initialize(rex::Runtime* runtime);

bool PresentNative();
void TogglePresentNative();

// Writes the resolved images of the next native frame to logs/frame_dumps.
void RequestFrameDump();

// Where the native renderer keeps its compiled pipelines between runs (before Initialize).
void SetCacheDir(const std::filesystem::path& dir);

// Logs where the GPU time of the next presented frame goes (draw_path.h).
void RequestGpuProfile();

// Saves the next presented frame (display gamma applied) as a PNG.
void RequestScreenshot(const std::filesystem::path& path);

// Called from the D3D Swap hook before the original Swap runs (r3 = device).
void OnSwap(uint8_t* base, uint32_t device);

// Depth-tested draws in the last presented frame (a 3D scene if nonzero), or
// -1 when the native renderer isn't drawing.
int LastFrameDepthDraws();

}  // namespace tenchuz::native
