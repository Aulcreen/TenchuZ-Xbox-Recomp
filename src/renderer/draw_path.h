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
#include <filesystem>
#include <memory>
#include <string>

#include <rex/ui/d3d12/d3d12_api.h>

#include "pm4_decoder.h"
#include "shader_cache.h"

namespace rex::memory {
class Memory;
}

namespace tenchuz::native {

struct DrawTarget {
  // Colour targets by guest index (RT0-3). UNKNOWN = not bound; its RTV is
  // then a null descriptor (gaps before the last bound target need one).
  DXGI_FORMAT color_formats[4] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
                                  DXGI_FORMAT_UNKNOWN};
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {};
  uint32_t color_mask = 0;  // DrawPath::WrittenColorMask, 4 bits per target
  DXGI_FORMAT depth_format = DXGI_FORMAT_UNKNOWN;  // UNKNOWN = no depth buffer
  D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
  uint32_t width = 0, height = 0;  // host pixels
  uint32_t samples = 1;  // MSAA sample count of every bound target
  uint32_t resolution_scale = 1;  // host pixels per guest pixel, on each axis
  // On the target the front buffer is resolved from, with a wider aspect ratio
  // (widescreen.h): 16/9 over that ratio. Its 2D draws (no depth test, game
  // textures only: the HUD, menus, movies) are scaled by this horizontally
  // around the centre, so they keep 16:9 proportions. 1 elsewhere.
  float hud_scale_x = 1.0f;
  // Diagnostics for the draw list: RB_COLOR_INFO / RB_DEPTH_INFO from the
  // device and the native surfaces' creation numbers.
  uint32_t color_info = 0, depth_info = 0, color_surface = 0, depth_surface = 0;
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
    // Rendered at the resolution scale (DrawTarget::resolution_scale), so the
    // shader scales its unnormalized coordinates and size queries.
    bool resolution_scaled = false;
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

  // Waits until the GPU has finished the previous frame (frames overlap: the
  // CPU records one while the GPU runs the one before). Before a page of the
  // guest-memory mirror that frame read is overwritten.
  virtual void WaitForPreviousFrame() = 0;

  // Diagnostics (--tenchuz_native_log_textures): the resolve destination whose
  // guest memory overlaps [address, address + size), described, or empty.
  virtual std::string ResolveOverlapping(uint32_t address, uint32_t size) = 0;

 protected:
  ~DrawHost() = default;
};

// Why a texture binding got a placeholder (DrawStats::placeholders).
enum PlaceholderReason : uint32_t {
  kPlaceholderNotTexture,   // the fetch constant isn't a texture
  kPlaceholderNoAddress,    // no base or mip address, or a 1D texture too wide
  kPlaceholderUnsupported,  // guest format not supported natively
  kPlaceholderLoadFailed,   // texture creation or load failed
  kPlaceholderSignedView,   // signed fetch of a format with no signed view
  kPlaceholderStacked3D,    // stacked 2D texture sampled as 3D (SDK wrapper not ported)
  kPlaceholderDimension,    // other dimension mismatch (2D vs 3D, cube)
  kPlaceholderReasonCount,
};

struct DrawStats {
  uint32_t pipelines_loaded = 0;  // of pipelines_created: from the pipeline cache, not compiled
  uint32_t issued = 0;
  uint32_t skipped_primitive = 0;  // primitive types not handled yet (fans, rect lists, quads)
  uint32_t skipped_primitive_types = 0;  // bit per xenos::PrimitiveType skipped
  uint32_t skipped_pipeline = 0;   // pipeline creation failed
  uint32_t skipped_culled = 0;     // both faces culled
  uint32_t mrt_draws = 0;          // draws writing more than one colour target
  uint32_t stencil_draws = 0;      // draws with the stencil test on
  uint32_t hud_draws = 0;          // 2D draws kept at 16:9 (DrawTarget::hud_scale_x)
  uint32_t depth_draws = 0;        // draws with the depth test on: the frame shows a 3D scene
  uint32_t watched_pages = 0;      // geometry pages still current from an earlier copy (not copied)
  uint32_t previous_frame_waits = 0;
  uint32_t pipelines_created = 0;  // new pipeline state objects this frame
  double pipeline_ms = 0;          // time spent creating them  // mirror pages rewritten while the previous frame could still read them
  uint32_t skipped_other = 0;
  uint32_t uploaded_kb = 0;
  uint32_t arena_kb = 0;          // dynamic vertices and indices copied per draw
  uint32_t dynamic_rewrites = 0;  // command-ring pages rewritten after this frame copied them
  uint32_t flushes = 0;           // mid-frame submits for those rewrites
  uint32_t reset_converted = 0;   // index buffers copied to map the guest reset index to all-ones
  uint32_t stale_pages = 0;       // static pages rewritten after this frame uploaded them (recheck mode)
  uint32_t textures_resolved = 0;  // fetches given a resolved render target
  uint32_t textures_guest = 0;     // fetches given a loaded guest texture
  uint32_t textures_missing = 0;   // fetches given a placeholder
  uint32_t placeholders[kPlaceholderReasonCount] = {};  // textures_missing by reason
  // Signed bindings of fetches with no signed component: the translator
  // declares a signed and an unsigned binding per fetch, and only samples the
  // signed one for signed components. Given a placeholder, never sampled.
  uint32_t signed_unused = 0;
  // Guest texture loading (texture_loader.h).
  uint32_t texture_loads = 0, texture_loaded_kb = 0, texture_hashed_kb = 0;
  uint32_t texture_unsupported = 0, textures_cached = 0;
  uint32_t texture_written_pages = 0;  // watched texture pages the guest wrote to
};

class DrawPath {
 public:
  DrawPath();
  ~DrawPath();

  // `memory`: guest memory, for texture write watching (null: hash instead).
  // `pipeline_cache`: the file compiled pipelines are kept in between runs
  // (empty: none).
  bool Initialize(ID3D12Device* device, rex::memory::Memory* memory,
                  const std::filesystem::path& pipeline_cache = {});

  // The colour components a draw writes, 4 bits per render target (SDK
  // draw_util::GetNormalizedColorMask): targets the pixel shader writes, with
  // RB_COLOR_MASK, in kColorDepth mode only. RB_COLOR*_INFO from the device.
  static uint32_t WrittenColorMask(const pm4::Decoder& state, uint8_t* base, uint32_t device_address,
                                   const DrawShaders& shaders);

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

  // Lists every draw and operation recorded this frame (frame logging), once
  // the GPU has finished it, with per-draw samples if queries are on.
  void LogFrameDraws(uint64_t frame);

  // Before the frame's command list is closed: resolves its draw queries.
  void EndFrame(ID3D12GraphicsCommandList* list, uint64_t frame);

  // GPU profile of one frame: a timestamp after every draw and operation,
  // logged once the GPU has finished it as GPU time per render target and per
  // operation. Request it before the frame starts; `ResolveProfile` goes at
  // the end of the frame's last command list (the present).
  void SetTimestampFrequency(uint64_t ticks_per_second);
  void RequestProfile();
  void ResolveProfile(ID3D12GraphicsCommandList* list);

  // Whether a draw has asked for the stencil test on a depth buffer without
  // stencil (D32_FLOAT) since the last call; such draws run without it.
  bool TakeStencilMissing();

  // Logs the draws the GPU had started but not finished (breadcrumbs written
  // around every draw), after the device was removed for a hang. Runs once.
  void ReportHang();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tenchuz::native
