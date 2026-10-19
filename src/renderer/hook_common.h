// tenchuz - shared plumbing for hand-written D3D hooks.
//
// Functions the native renderer hooks itself are excluded from the generated
// trace hooks (tools/gen_trace_hooks.py --exclude), so their hooks must do the
// same tracing/observing work. Usage:
//
//   REX_HOOK_RAW(sub_8241B308) {
//     tenchuz::HookScope scope(0x8241B308, ctx, base);
//     ... capture arguments ...
//     __imp__sub_8241B308(ctx, base);
//     ... act on the result ...
//   }

#pragma once

#include <cstdint>

#include "d3d_trace.h"
#include "gpu_observer.h"

struct PPCContext;

namespace tenchuz {

class HookScope {
 public:
  HookScope(uint32_t function, const PPCContext& ctx, uint8_t* base)
      : function_(function), base_(base), observe_(gpu_observer::Active()) {
    if (d3d_trace::Active()) {
      d3d_trace::Record(function, ctx);
    }
    if (observe_) {
      gpu_observer::Enter(function, base);
    }
  }
  ~HookScope() {
    if (observe_) {
      gpu_observer::Exit(function_, base_);
    }
  }
  HookScope(const HookScope&) = delete;
  HookScope& operator=(const HookScope&) = delete;

 private:
  uint32_t function_;
  uint8_t* base_;
  bool observe_;
};

}  // namespace tenchuz
