// tenchuz - native draw path (milestone N4.2).
//
// Issues the game's draws on our own D3D12 command list with the translated
// shaders (shader_cache.h), following the binding contract of the SDK's
// DxbcShaderTranslator (bindful): root CBVs for fetch / float / system /
// bool-loop constants, a "shared memory" SRV (guest physical memory mirror the
// translated vertex shaders fetch vertices from), and per-stage texture and
// sampler tables. Textures are the renderer's resolved render targets
// (TextureSource) or guest textures (texture_loader.h); anything else is a 1x1
// white placeholder.

#pragma once

#include <cstdint>
#include <memory>

#include <rex/ui/d3d12/d3d12_api.h>

#include "pm4_decoder.h"
#include "shader_cache.h"

namespace tenchuz::native {

struct DrawTarget {
  DXGI_FORMAT color_format = DXGI_FORMAT_UNKNOWN;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
  DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;  // UNKNOWN = no depth buffer
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  uint32_t width = 0, height = 0;
};

// What the draw path needs from the renderer that owns the frame.
class DrawHost {
 public:
  struct Texture {
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // Guest component (X, Y, Z, W) -> host component, 3 bits each, as in the
    // SDK's texture cache (GuestToHostSwizzle).
    uint32_t host_format_swizzle = 0;
  };
  // The texture for a 2D fetch of guest physical `address`, width x height,
  // with any state transition needed for sampling recorded on `list`.
  virtual bool Find(ID3D12GraphicsCommandList* list, uint32_t address, uint32_t width, uint32_t height,
                    Texture& out) = 0;

  // Whether guest physical `address` is in the D3D's command buffer ring, where
  // dynamic vertices and indices live and are rewritten within a frame.
  virtual bool InCommandRing(uint32_t address) = 0;

  // Submits what has been recorded and waits for the GPU to finish it, then
  // continues recording on the same list.
  virtual void FlushAndWait() = 0;

 protected:
  ~DrawHost() = default;
};

struct DrawStats {
  uint32_t issued = 0;
  uint32_t skipped_primitive = 0;  // primitive types not handled yet (fans, rect lists, quads)
  uint32_t skipped_primitive_types = 0;  // bit per xenos::PrimitiveType skipped
  uint32_t skipped_pipeline = 0;   // pipeline creation failed
  uint32_t skipped_other = 0;
  uint32_t uploaded_kb = 0;
  uint32_t arena_kb = 0;          // dynamic vertices and indices copied per draw
  uint32_t dynamic_rewrites = 0;  // command-ring pages rewritten after this frame copied them
  uint32_t flushes = 0;           // mid-frame submits for those rewrites
  uint32_t textures_resolved = 0;  // fetches given a resolved render target
  uint32_t textures_guest = 0;     // fetches given a loaded guest texture
  uint32_t textures_missing = 0;   // fetches given a placeholder
  // Guest texture loading (texture_loader.h).
  uint32_t texture_loads = 0, texture_loaded_kb = 0, texture_hashed_kb = 0;
  uint32_t texture_unsupported = 0, textures_cached = 0;
};

class DrawPath {
 public:
  DrawPath();
  ~DrawPath();

  bool Initialize(ID3D12Device* device);

  // Start of a frame slot (after its fence has been waited on).
  void BeginFrame(uint32_t slot);

  // Records one draw into `list` (render targets already in RENDER_TARGET /
  // DEPTH_WRITE state). `device_address` is the guest D3D device, for the
  // untiled viewport registers.
  bool Draw(ID3D12GraphicsCommandList* list, const pm4::DrawInfo& draw, const pm4::Decoder& state,
            uint8_t* base, uint32_t device_address, const DrawShaders& shaders,
            const DrawTarget& target, DrawHost* host);

  DrawStats TakeFrameStats();

  // Breadcrumbs around a native operation recorded outside Draw (clears,
  // resolve copies), so hang reports can name it. `what` must be a literal.
  uint32_t BeginOp(ID3D12GraphicsCommandList* list, const char* what, uint32_t a, uint32_t b);
  void EndOp(uint32_t op);

  // Logs the draws the GPU had started but not finished (breadcrumbs written
  // around every draw), after the device was removed for a hang. Runs once.
  void ReportHang();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tenchuz::native
