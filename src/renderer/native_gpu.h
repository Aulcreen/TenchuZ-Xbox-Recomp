// tenchuz - the guest GPU without emulation (milestone N5).
//
// By default (--tenchuz_native_gpu=true) this replaces the SDK's Xenos GPU
// plugin as the runtime's graphics system; --tenchuz_native_gpu=false runs
// the emulator instead. It creates the D3D12 device and presenter the
// native renderer draws through, maps the GPU's MMIO registers, raises vblank
// and command-stream interrupts, and runs a command processor that executes
// only what the game can observe from the GPU: register writes, memory and
// fence writes, waits, interrupts, occlusion query results and the ring read
// pointer. Draws, resolves and swaps are not executed there: the native
// renderer records them from the D3D calls (draw_path.h, native_renderer.h).

#pragma once

#include <cstdint>
#include <memory>

namespace rex::system {
class IGraphicsSystem;
}

namespace tenchuz::native {

// Whether this run uses the native graphics system (--tenchuz_native_gpu).
bool NativeGpu();

std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeGraphicsSystem();

// The guest vblank rate in Hz (0: the video mode's, 60). The D3D paces frames
// in vblanks, so frame_rate.cpp raises it for frame rates above 60.
void SetVblankRate(double hz);

// Display vblanks counted while the game's vblanks follow the display's
// (tenchuz_display_vblank): 0 while they're on a timer.
uint64_t DisplayVblanks();

}  // namespace tenchuz::native
