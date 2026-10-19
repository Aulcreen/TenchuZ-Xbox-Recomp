// tenchuz - D3D layer call tracer (native renderer milestone N1).
//
// Every function in the XDK D3D library's address range gets a generated hook
// (d3d_trace_hooks.gen.cpp, from tools/gen_trace_hooks.py) that calls Record()
// while a capture is active. A capture spans one or more whole frames, Swap to
// Swap, and is written to <user>/traces/d3d_trace_f<frame>.tsv for
// tools/analyze_d3d_trace.py.
//
// Triggers: F8 in game, or --tenchuz_d3d_trace_at_frame=N for automated runs.

#pragma once

#include <cstdint>
#include <filesystem>

struct PPCContext;

namespace tenchuz::d3d_trace {

// True while a capture is recording; hooks check this before calling Record().
bool Active();

// Append one call event (guest function address, caller lr, r3-r8).
void Record(uint32_t function, const PPCContext& ctx);

// Frame boundary, called from the D3D Swap hook before the original runs.
void OnSwap(const PPCContext& ctx);

// Capture the next `frames` whole frames.
void RequestCapture(uint32_t frames = 1);

void SetOutputDir(const std::filesystem::path& dir);

}  // namespace tenchuz::d3d_trace
