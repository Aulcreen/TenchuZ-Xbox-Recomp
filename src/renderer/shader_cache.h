// tenchuz - native renderer shader cache (milestone N4.1).
//
// Translates the Xenos microcode each draw uses into DXBC with the SDK's
// DxbcShaderTranslator (vendored in external/xenos_shader), cached by microcode
// hash and per-draw modification. The microcode is taken from what the GPU
// would load (IM_LOAD / IM_LOAD_IMMEDIATE in the live-decoded command stream),
// because the D3D library patches vertex fetches into vertex shaders per
// vertex declaration.

#pragma once

#include <cstdint>
#include <memory>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/ui/graphics_provider.h>

#include "pm4_decoder.h"

namespace tenchuz::native {

struct DrawShaders {
  rex::graphics::Shader::Translation* vertex = nullptr;
  rex::graphics::Shader::Translation* pixel = nullptr;  // null for depth-only draws
};

struct ShaderStats {
  uint64_t draws = 0;
  uint64_t unique_shaders = 0;
  uint64_t translations = 0;
  uint64_t copy_mode_draws = 0;  // RB_MODECONTROL in copy mode: resolves, not drawn
  uint64_t failures = 0;
};

class ShaderCache {
 public:
  // `resolution_scale`: host pixels per guest pixel of the render targets, on
  // each axis (the translator reverts it for pixel positions and scales it in
  // for unnormalized fetches from resolution-scaled textures).
  ShaderCache(rex::ui::GraphicsProvider::GpuVendorID vendor, uint32_t resolution_scale);
  ~ShaderCache();

  // Shaders for the draw just decoded. Returns false if the vertex shader is
  // missing or failed to translate.
  bool Prepare(const pm4::DrawInfo& draw, const pm4::Decoder& state, uint8_t* base,
               DrawShaders& out);

  const ShaderStats& stats() const { return stats_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  ShaderStats stats_;
};

}  // namespace tenchuz::native
