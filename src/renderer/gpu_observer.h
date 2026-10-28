// tenchuz - GPU state observer (native renderer milestone N2).
//
// While active, every hooked D3D function decodes the command packets it
// emitted (device write pointer at +0x28) with pm4::Decoder. At each draw
// packet it records the decoded GPU registers next to a raw copy of the D3D
// device object. tools/analyze_gpu_observer.py then learns which device
// offsets shadow which GPU registers, which is the state a native renderer
// reads at draw time.
//
// Triggers: F9 in game, --tenchuz_gpu_observe_at_frame=N, or every N frames with
// --tenchuz_gpu_observe_every=N. One warm-up frame
// (decoding only, so registers set earlier are known), then one recorded frame
// written to <user>/traces/gpu_observe_f<frame>.bin.

#pragma once

#include <cstdint>
#include <filesystem>

#include "pm4_decoder.h"

namespace tenchuz::gpu_observer {

bool Active();

// Called by the D3D hooks around the original function (render thread only).
void Enter(uint32_t function, uint8_t* base);
void Exit(uint32_t function, uint8_t* base);

// The outermost hooked D3D function the render thread is in (0 when none).
uint32_t TopFunction();

// Frame boundary from the Swap hook; r3 is the D3D device.
void OnSwap(uint32_t device, uint8_t* base);

void RequestObserve();

// Live decoding for the native renderer: keeps the decoder running every frame
// (starting at the next Swap) and calls `callback` at each draw packet with
// the decoded GPU state and the top-level hooked D3D function that emitted it.
// The callback runs on the render thread inside that D3D call.
using LiveDrawCallback = void (*)(const pm4::DrawInfo& draw, const pm4::Decoder& state,
                                  uint8_t* base, uint32_t api_function);
void SetLiveDecoding(bool enable, LiveDrawCallback callback);

// Called at the start of every hooked D3D call on the render thread while
// decoding (nested ones too), before the call runs.
using CallCallback = void (*)(uint32_t function, uint8_t* base);
void SetCallCallback(CallCallback callback);

// Times decoding jumped ahead (skipping any packets in between) since the last
// call, by reason, and the bytes skipped by gap jumps.
struct ResyncStats {
  uint32_t segment = 0;  // the ring's segment limit changed
  uint32_t gap = 0;      // more than kMaxSyncGap written between two syncs
  uint32_t gap_kb = 0;
  uint32_t backward = 0;
  uint32_t stuck = 0;    // stuck behind a reserved block...
  uint32_t resumed = 0;  // ...and resumed after it (the rest were skipped)
  uint32_t kicks = 0;    // command segments submitted (decoded across exactly)
};
ResyncStats TakeResyncStats();

// Whether guest physical `address` is in the range the D3D's command buffer
// write pointer has covered (its ring of segments).
bool InCommandRing(uint32_t address);

// While on, log every sync that decodes or resyncs, every kick end and every
// decoded draw packet (diagnostics: --tenchuz_native_trace_syncs_at).
void SetSyncTrace(bool on);
void SetOutputDir(const std::filesystem::path& dir);

}  // namespace tenchuz::gpu_observer
