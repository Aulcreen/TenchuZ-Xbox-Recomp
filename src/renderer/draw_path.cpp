// tenchuz - native draw path. See draw_path.h.
//
// Mirrors the SDK D3D12 backend's binding contract (src/graphics/d3d12/
// command_processor.cpp: root signature, UpdateBindings, system constants)
// without its EDRAM model: render targets are our native, untiled surfaces.

#include "draw_path.h"

#include "texture_loader.h"
#include "../../external/xenos_shader/geometry_shader.h"

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/bit.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_INT32(tenchuz_native_anisotropy, 16, "tenchuz",
                     "Anisotropic filtering for the game's mipmapped textures: 0 = the game's own, 2, 4, 8 or 16");
REXCVAR_DEFINE_BOOL(tenchuz_native_profile_timeline, false, "tenchuz",
                    "GPU profiles (gpuprofile) also list every draw and operation's time in order");
REXCVAR_DEFINE_BOOL(tenchuz_native_ring_flush, false, "tenchuz",
                    "Native draws: flush mid-frame when command-ring vertex pages are rewritten");
REXCVAR_DEFINE_BOOL(tenchuz_native_geometry_watch, true, "tenchuz",
                    "Copy vertex and index data to the GPU only when the game writes it (false: every frame)");
REXCVAR_DEFINE_INT32(tenchuz_native_recheck_pages, 0, "tenchuz",
                     "Native draws: 1 = count static vertex/index pages whose guest memory changed after "
                     "this frame uploaded them, 2 = also upload them again");
REXCVAR_DEFINE_BOOL(tenchuz_native_check_late_writes, false, "tenchuz",
                    "Native draws: at frame end, log guest vertex/index data that changed after it was copied");
REXCVAR_DEFINE_STRING(tenchuz_native_trace_vs, "", "tenchuz",
                      "Native draws: log the constants, vertex and index ranges of draws with this VS hash (hex)");
REXCVAR_DEFINE_INT32(tenchuz_native_log_coverage, 0, "tenchuz",
                     "With draw queries: every frame, list the draws that passed more than N samples");
REXCVAR_DEFINE_BOOL(tenchuz_native_draw_queries, false, "tenchuz",
                    "Native draws: occlusion query per draw, listed with tenchuz_native_log_draws");
REXCVAR_DEFINE_BOOL(tenchuz_native_debug_textures, false, "tenchuz",
                    "Native draws sample a UV-gradient checker instead of white (geometry check)");
REXCVAR_DEFINE_BOOL(tenchuz_native_alpha_test_tolerance, true, "tenchuz",
                    "Compare alpha tests at 8-bit precision (fixes speckled characters on NVIDIA and other GPUs)");
REXCVAR_DEFINE_BOOL(tenchuz_native_log_textures, false, "tenchuz",
                    "Native draws: log each distinct texture fetch once, with how it was bound and any "
                    "resolve destination a guest texture overlaps");
REXCVAR_DEFINE_BOOL(tenchuz_native_log_offscreen, false, "tenchuz",
                    "Native draws: log screen-space draws (vertex shaders without float constants) whose "
                    "vertices reach far off screen or aren't finite");

namespace tenchuz::native {

namespace gfx = rex::graphics;
namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;
using Microsoft::WRL::ComPtr;
using Translator = gfx::DxbcShaderTranslator;

namespace {

constexpr uint64_t kSharedMemorySize = 512ull << 20;  // guest physical address space
// Per frame slot, after the mirror in the same buffer: copies of dynamic
// (command-ring) vertices and indices, one per draw.
constexpr uint32_t kArenaSize = 32u << 20;
constexpr uint32_t kArenaMaxSpan = 1u << 20;  // larger ranges go through the mirror
constexpr uint32_t kFrames = 2;
constexpr uint32_t kConstantRingSize = 16u << 20;     // per frame
constexpr uint32_t kViewRingSize = 16384;             // shader-visible descriptors per frame
constexpr uint32_t kSamplerTableSize = 16;    // default linear/wrap table at the heap start
constexpr uint32_t kSamplerHeapSize = 2048;   // shader-visible sampler heap maximum
constexpr uint32_t kDevColorInfo = 0x2804;    // RB_COLOR_INFO in the device shadow
constexpr uint32_t kDevColor1Info = 0x280C;   // RB_COLOR1_INFO (RT2, RT3 at +4, +8)
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPageSize = 1u << kPageShift;
constexpr uint32_t kCbAlign = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

// Untiled viewport registers PA_CL_VPORT_* (0x210F-0x2114) in the D3D device
// shadow (docs/D3D_MAP.md): the game's full-frame viewport even in tiled passes.
constexpr uint32_t kDevVport = 0x2888;

// Root parameters (bindful layout of the SDK's D3D12 command processor).
enum RootParameter : uint32_t {
  kRootFetchConstants,
  kRootFloatConstantsVertex,
  kRootFloatConstantsPixel,
  kRootSystemConstants,
  kRootBoolLoopConstants,
  kRootSharedMemory,
  kRootBaseCount,
};

using SerializeRootFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                         ID3DBlob**, ID3DBlob**);

// Guest fetch swizzle -> host SRV component mapping (SDK TextureCache::
// GuestToHostSwizzle); D3D12 uses the same 3-bit encoding (4 = 0, 5 = 1).
uint32_t GuestToHostSwizzle(uint32_t guest_swizzle, uint32_t host_format_swizzle) {
  uint32_t host_swizzle = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t component = (guest_swizzle >> (3 * i)) & 0b111;
    const uint32_t host = component >= 4 ? (component & 0b101) : (host_format_swizzle >> (3 * component)) & 0b111;
    host_swizzle |= host << (3 * i);
  }
  return host_swizzle;
}

D3D12_TEXTURE_ADDRESS_MODE AddressMode(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case xenos::ClampMode::kMirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case xenos::ClampMode::kClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;  // edge, halfway
  }
}

// Host sampler for a shader sampler binding (SDK D3D12TextureCache::
// GetSamplerParameters + WriteSampler).
D3D12_SAMPLER_DESC SamplerDesc(const gfx::DxbcShader::SamplerBinding& binding,
                               const xenos::xe_gpu_texture_fetch_t& fetch) {
  D3D12_SAMPLER_DESC desc;
  std::memset(&desc, 0, sizeof(desc));
  xenos::ClampMode clamp_x, clamp_y, clamp_z;
  gfx::texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);
  desc.AddressU = AddressMode(clamp_x);
  desc.AddressV = AddressMode(clamp_y);
  desc.AddressW = AddressMode(clamp_z);
  const bool border = desc.AddressU == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
                      desc.AddressV == D3D12_TEXTURE_ADDRESS_MODE_BORDER ||
                      desc.AddressW == D3D12_TEXTURE_ADDRESS_MODE_BORDER;
  if (border) {
    switch (fetch.border_color) {
      case xenos::BorderColor::k_ABGR_White:
        desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 1.0f;
        break;
      case xenos::BorderColor::k_ACBYCR_Black:
        desc.BorderColor[0] = desc.BorderColor[2] = 0.5f;
        break;
      case xenos::BorderColor::k_ACBCRY_Black:
        desc.BorderColor[1] = desc.BorderColor[2] = 0.5f;
        break;
      default:
        break;
    }
  }
  uint32_t mip_min_level, mip_max_level;
  gfx::texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                      &mip_min_level, &mip_max_level);
  auto pick = [](xenos::TextureFilter shader, xenos::TextureFilter fetch_filter) {
    return shader == xenos::TextureFilter::kUseFetchConst ? fetch_filter : shader;
  };
  const xenos::TextureFilter mag = pick(binding.mag_filter, fetch.mag_filter);
  const xenos::TextureFilter min = pick(binding.min_filter, fetch.min_filter);
  const xenos::TextureFilter mip = pick(binding.mip_filter, fetch.mip_filter);
  const bool mip_base_map = mip == xenos::TextureFilter::kBaseMap;
  xenos::AnisoFilter aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                 ? fetch.aniso_filter
                                 : binding.aniso_filter;
  aniso = std::min(aniso, xenos::AnisoFilter::kMax_16_1);
  uint32_t max_anisotropy = aniso != xenos::AnisoFilter::kDisabled ? 1u << (uint32_t(aniso) - 1) : 1;
  // --tenchuz_native_anisotropy raises it for linear, mipmapped textures (the
  // 3D scene's); point-sampled and unmipmapped ones (2D, resolves) keep theirs.
  const int32_t forced = std::clamp(REXCVAR_GET(tenchuz_native_anisotropy), 0, 16);
  if (forced > 1 && min == xenos::TextureFilter::kLinear && mag == xenos::TextureFilter::kLinear &&
      !mip_base_map && mip_max_level > mip_min_level) {
    max_anisotropy = std::max(max_anisotropy, uint32_t(forced));
  }
  if (max_anisotropy > 1) {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = max_anisotropy;
  } else {
    auto type = [](bool linear) { return linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT; };
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(type(min == xenos::TextureFilter::kLinear),
                                            type(mag == xenos::TextureFilter::kLinear),
                                            type(mip == xenos::TextureFilter::kLinear),
                                            D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    desc.MaxAnisotropy = 1;
  }
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  desc.MinLOD = float(mip_min_level);
  if (mip_base_map) {
    desc.MaxLOD = desc.MinLOD + (aniso == xenos::AnisoFilter::kDisabled ? 0.25f : 0.0f);
  } else {
    desc.MaxLOD = D3D12_FLOAT32_MAX;
  }
  return desc;
}

D3D12_BLEND BlendFactor(uint32_t f) {
  switch (f) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}

D3D12_BLEND AlphaBlendFactor(uint32_t f) {
  // Colour factors are not allowed for alpha blending in D3D12.
  switch (f) {
    case 4: return D3D12_BLEND_SRC_ALPHA;
    case 5: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_ALPHA;
    case 9: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return BlendFactor(f);
  }
}

D3D12_BLEND_OP BlendOp(uint32_t op) {
  switch (op) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}

// Rasterizer state, as the SDK's pipeline cache derives it for its RTV path
// (PipelineCache::GetCurrentStateDescription, draw_util::
// GetPreferredFacePolygonOffset): D3D12 has one fill mode and depth bias for
// both faces, so those of the face that isn't culled are used. Draws with both
// faces culled are dropped before this.
struct RasterState {
  D3D12_CULL_MODE cull = D3D12_CULL_MODE_NONE;
  bool front_counter_clockwise = false;
  bool wireframe = false;
  bool depth_clip = true;
  int32_t depth_bias = 0;
  float slope_scaled_depth_bias = 0.0f;
};

// The driver's DXBC conversion has crashed (a null read) on a pipeline whose
// shaders came from a mis-decoded load that still translated: such a pipeline
// fails (E_UNEXPECTED) instead of taking the game down.
HRESULT CreatePipelineGuarded(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                              ID3D12PipelineState** out) {
  __try {
    return device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(out));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *out = nullptr;
    return E_UNEXPECTED;
  }
}

RasterState GetRasterState(const gfx::RegisterFile& regs, bool polygonal, uint32_t resolution_scale) {
  RasterState r;
  const auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  float scale = 0.0f, offset = 0.0f;
  if (polygonal) {
    r.front_counter_clockwise = mode.face == 0;
    r.cull = mode.cull_front ? D3D12_CULL_MODE_FRONT : (mode.cull_back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE);
    // No point fill mode in D3D12: points are drawn as wireframe too.
    r.wireframe = mode.poly_mode == xenos::PolygonModeEnable::kDualMode &&
                  ((!mode.cull_front && mode.polymode_front_ptype != xenos::PolygonType::kTriangles) ||
                   (!mode.cull_back && mode.polymode_back_ptype != xenos::PolygonType::kTriangles));
    if (mode.poly_offset_front_enable && !mode.cull_front) {
      scale = regs.Get<float>(gfx::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      offset = regs.Get<float>(gfx::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (mode.poly_offset_back_enable && !mode.cull_back && !scale && !offset) {
      scale = regs.Get<float>(gfx::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      offset = regs.Get<float>(gfx::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
    }
  } else if (mode.poly_offset_para_enable) {
    scale = regs.Get<float>(gfx::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
    offset = regs.Get<float>(gfx::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
  }
  // Integer bias in the guest depth format's units. Our depth buffers are
  // D32_FLOAT, where a unit is 2^(exponent of the primitive's max z - 23):
  // the same as unorm24's for z in [0.5, 1), smaller nearer the camera.
  r.depth_bias = gfx::draw_util::GetD3D10IntegerPolygonOffset(regs.Get<reg::RB_DEPTH_INFO>().depth_format, offset);
  // The slope is per host pixel: scaled up like the SDK's ROV path, so the
  // bias stays what it is per guest pixel at a resolution scale.
  r.slope_scaled_depth_bias = scale * xenos::kPolygonOffsetScaleSubpixelUnit * float(resolution_scale);
  r.depth_clip = !regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  return r;
}

// RB_COLOR_INFO of render target `index` from the device: the decoded value
// is stale in tiled passes.
reg::RB_COLOR_INFO DeviceColorInfo(uint8_t* base, uint32_t device_address, uint32_t index) {
  reg::RB_COLOR_INFO info;
  info.value = REX_LOAD_U32(device_address + (index ? kDevColor1Info + 4 * (index - 1) : kDevColorInfo));
  return info;
}

constexpr gfx::Register kBlendControl[4] = {gfx::XE_GPU_REG_RB_BLENDCONTROL0, gfx::XE_GPU_REG_RB_BLENDCONTROL1,
                                            gfx::XE_GPU_REG_RB_BLENDCONTROL2, gfx::XE_GPU_REG_RB_BLENDCONTROL3};

// SDK draw_util::GetNormalizedDepthControl: depth and stencil only in
// kColorDepth / kDepthOnly mode, and no depth test that always passes
// without writing.
reg::RB_DEPTHCONTROL NormalizedDepthControl(const gfx::RegisterFile& regs) {
  const xenos::EdramMode mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  reg::RB_DEPTHCONTROL dc;
  dc.value = 0;
  if (mode != xenos::EdramMode::kColorDepth && mode != xenos::EdramMode::kDepthOnly) {
    return dc;
  }
  dc = regs.Get<reg::RB_DEPTHCONTROL>();
  if (dc.z_enable && !dc.z_write_enable && dc.zfunc == xenos::CompareFunction::kAlways) {
    dc.z_enable = 0;
  }
  return dc;
}

// Stencil reference and masks: D3D12 has one set for both faces, so like the
// SDK, the back-face set only when only back faces are drawn.
reg::RB_STENCILREFMASK StencilRefMask(const gfx::RegisterFile& regs, reg::RB_DEPTHCONTROL dc, bool polygonal) {
  const bool back = polygonal && dc.backface_enable && regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front;
  reg::RB_STENCILREFMASK ref_mask;
  ref_mask.value = regs[back ? gfx::XE_GPU_REG_RB_STENCILREFMASK_BF : gfx::XE_GPU_REG_RB_STENCILREFMASK];
  return ref_mask;
}

D3D12_DEPTH_STENCILOP_DESC StencilOps(xenos::StencilOp fail, xenos::StencilOp depth_fail, xenos::StencilOp pass,
                                      xenos::CompareFunction func) {
  // Both enumerations list the same operations in the same order, from 0 / 1.
  return {D3D12_STENCIL_OP(uint32_t(fail) + 1), D3D12_STENCIL_OP(uint32_t(depth_fail) + 1),
          D3D12_STENCIL_OP(uint32_t(pass) + 1), D3D12_COMPARISON_FUNC(uint32_t(func) + 1)};
}

}  // namespace

struct DrawPath::Impl {
  ID3D12Device* device = nullptr;
  SerializeRootFn serialize_root = nullptr;

  // Guest physical memory mirror the translated shaders read vertices from (and
  // the input assembler reads indices from). Upload heap: CPU-written per draw.
  ComPtr<ID3D12Resource> shared;
  uint8_t* shared_map = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS shared_gpu = 0;
  std::vector<uint64_t> uploaded_pages = std::vector<uint64_t>(kSharedMemorySize >> kPageShift >> 6);
  // The last frame (frame_counter) whose draws read each mirror page. The
  // GPU may still be running the previous frame while this one is recorded.
  std::vector<uint32_t> page_used_frame = std::vector<uint32_t>(kSharedMemorySize >> kPageShift);
  DrawHost* frame_host = nullptr;  // the host of the draw being recorded (WaitForPreviousFrame)
  uint32_t waited_frame = 0;       // frame_counter at the last WaitForPreviousFrame

  // Before overwriting mirror pages [page, page_end) in place: if the
  // previous frame read any of them, let it finish first.
  void BeforeOverwrite(uint32_t page, uint32_t page_end) {
    const uint32_t previous = uint32_t(frame_counter) - 1;
    if (waited_frame == uint32_t(frame_counter) || !frame_host) return;
    for (uint32_t p = page; p < page_end; ++p) {
      if (page_used_frame[p] == previous) {
        frame_host->WaitForPreviousFrame();
        waited_frame = uint32_t(frame_counter);
        ++stats.previous_frame_waits;
        return;
      }
    }
  }
  std::unordered_map<uint32_t, uint64_t> page_hashes;  // command-ring pages copied this frame
  // --tenchuz_native_recheck_pages: hash of each static page when this frame
  // uploaded it, and what the current draw is uploading (for the log).
  std::unordered_map<uint32_t, uint64_t> static_page_hashes;
  // Per page: the loader's write sequence when it was last copied, plus 1 (0:
  // never), for watched geometry (Upload).
  std::vector<uint32_t> page_copy_sequence = std::vector<uint32_t>(kSharedMemorySize >> kPageShift);
  uint32_t upload_context = 0, upload_context_size = 0;
  uint32_t stale_logged = 0;
  // --tenchuz_native_check_late_writes: every guest range copied this frame
  // (record index of the draw it was copied for), re-hashed at frame end.
  struct CopyCheck {
    uint32_t record, address, size;
    uint64_t hash;
    char kind;  // 'A' per-draw arena copy, 'S' static pages
  };
  std::vector<CopyCheck> copy_checks;
  uint64_t trace_vs = 0;  // --tenchuz_native_trace_vs
  uint64_t trace_frame = 0;
  bool log_offscreen = false;  // --tenchuz_native_log_offscreen

  // One line per traced draw: every float constant the VS uses, per-component
  // ranges of the first vertex stream's first 8 floats, and the index range.
  void TraceDraw(uint8_t* base, const gfx::Shader& vs, const gfx::RegisterFile& regs, uint32_t vb_address,
                 uint32_t vb_size, uint32_t vb_stride, uint32_t count, bool indexed, bool index32, uint32_t ib_base,
                 xenos::Endian index_endian) {
    std::string out = fmt::format("tenchuz: NATIVE trace frame {} vs {:016X} count {}", trace_frame,
                                  vs.ucode_data_hash(), count);
    const auto& map = vs.constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map.float_bitmap[i];
      uint32_t index;
      while (rex::bit_scan_forward(bits, &index)) {
        bits &= ~(1ull << index);
        const uint32_t c = i * 64 + index;
        float v[4];
        std::memcpy(v, &regs.values[gfx::XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4], 16);
        out += fmt::format(" c{}=({:g},{:g},{:g},{:g})", c, v[0], v[1], v[2], v[3]);
      }
    }
    const uint32_t vertices = vb_stride ? vb_size / vb_stride : 0;
    if (vertices && vb_stride >= 4) {
      const uint32_t comps = std::min(8u, vb_stride / 4);
      float lo[8], hi[8];
      uint32_t bad = 0;
      for (uint32_t k = 0; k < comps; ++k) { lo[k] = 1e30f; hi[k] = -1e30f; }
      for (uint32_t v = 0; v < vertices; ++v) {
        for (uint32_t k = 0; k < comps; ++k) {
          const uint32_t bits = REX_LOAD_U32(0xA0000000u | (vb_address + v * vb_stride + 4 * k));
          float f;
          std::memcpy(&f, &bits, 4);
          if (!std::isfinite(f)) { ++bad; continue; }
          lo[k] = std::min(lo[k], f);
          hi[k] = std::max(hi[k], f);
        }
      }
      out += fmt::format(" | vb {:08X} {} verts, non-finite {}:", vb_address, vertices, bad);
      for (uint32_t k = 0; k < comps; ++k) out += fmt::format(" [{:g},{:g}]", lo[k], hi[k]);
    }
    if (indexed && count) {
      uint32_t lo = UINT32_MAX, hi = 0, restarts = 0, beyond = 0;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx;
        if (index32) {
          idx = REX_LOAD_U32(0xA0000000u | (ib_base + 4 * i));  // big-endian load = k8in32
        } else {
          const uint8_t* b = REX_RAW_ADDR(0xA0000000u | (ib_base + 2 * i));
          idx = index_endian == xenos::Endian::k8in16 ? uint32_t(b[0] << 8 | b[1]) : uint32_t(b[1] << 8 | b[0]);
        }
        if (idx == (index32 ? 0xFFFFFFFFu : 0xFFFFu)) { ++restarts; continue; }
        lo = std::min(lo, idx);
        hi = std::max(hi, idx);
        if (idx >= vertices) ++beyond;
      }
      out += fmt::format(" | ib {:08X} {}-bit endian {} range [{},{}] restarts {} beyond-vb {}", ib_base,
                         index32 ? 32 : 16, uint32_t(index_endian), lo, hi, restarts, beyond);
    }
    REXLOG_INFO("{}", out);
  }
  bool check_late_writes = false;
  uint8_t* guest_base = nullptr;
  void NoteCopy(uint8_t* base, uint32_t address, uint32_t size, char kind) {
    if (!check_late_writes) return;
    guest_base = base;
    uint32_t record = 0;
    {
      std::lock_guard lock(records_mutex);
      record = uint32_t(records[slot].size());
    }
    copy_checks.push_back({record, address, size, XXH3_64bits(REX_RAW_ADDR(0xA0000000u | address), size), kind});
  }

  // Texture views made once and copied into each draw's table after, rather
  // than made through the driver for every binding of every draw. Keyed by
  // the texture and the view; each entry holds its texture, so its address
  // can't come back as another texture's. Evicted entries (least recently
  // used, past kViewCacheSize) keep their textures until the GPU is done with
  // the frames that may still read them (Frame::retired).
  static constexpr uint32_t kViewCacheSize = 4096;
  ComPtr<ID3D12DescriptorHeap> view_cache_heap;  // CPU only
  struct CachedView {
    uint32_t index = 0;  // in view_cache_heap
    ComPtr<ID3D12Resource> resource;
    uint64_t last_used = 0;
  };
  std::unordered_map<uint64_t, CachedView> view_cache;
  std::vector<uint32_t> view_cache_free;
  uint64_t view_cache_clock = 0;

  // Writes a view of `resource` described by `view` at `cpu`, from the cache.
  void CreateTextureView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& view,
                         D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
    if (!view_cache_heap) {
      device->CreateShaderResourceView(resource, &view, cpu);
      return;
    }
    const uint64_t key = XXH3_64bits(&view, sizeof(view)) * 0x9E3779B97F4A7C15ull ^ uint64_t(uintptr_t(resource));
    auto it = view_cache.find(key);
    if (it == view_cache.end() || it->second.resource.Get() != resource) {
      if (it != view_cache.end()) {  // a hash collision: replaced
        frames[slot].retired.push_back(std::move(it->second.resource));
        view_cache_free.push_back(it->second.index);
        view_cache.erase(it);
      }
      if (view_cache_free.empty()) {
        // Full: the least recently used half goes.
        std::vector<std::pair<uint64_t, uint64_t>> ages;
        ages.reserve(view_cache.size());
        for (const auto& [k, v] : view_cache) ages.emplace_back(v.last_used, k);
        std::nth_element(ages.begin(), ages.begin() + ages.size() / 2, ages.end());
        for (size_t i = 0; i < ages.size() / 2; ++i) {
          auto old = view_cache.find(ages[i].second);
          frames[slot].retired.push_back(std::move(old->second.resource));
          view_cache_free.push_back(old->second.index);
          view_cache.erase(old);
        }
      }
      CachedView entry;
      entry.index = view_cache_free.back();
      view_cache_free.pop_back();
      entry.resource = resource;
      D3D12_CPU_DESCRIPTOR_HANDLE at = view_cache_heap->GetCPUDescriptorHandleForHeapStart();
      at.ptr += SIZE_T(entry.index) * view_inc;
      device->CreateShaderResourceView(resource, &view, at);
      it = view_cache.emplace(key, std::move(entry)).first;
    }
    it->second.last_used = ++view_cache_clock;
    D3D12_CPU_DESCRIPTOR_HANDLE from = view_cache_heap->GetCPUDescriptorHandleForHeapStart();
    from.ptr += SIZE_T(it->second.index) * view_inc;
    device->CopyDescriptorsSimple(1, cpu, from, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }

  struct Frame {
    std::vector<ComPtr<ID3D12Resource>> retired;  // evicted cached views' textures, freed when the GPU is done
    ComPtr<ID3D12Resource> constants;
    uint8_t* constants_map = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS constants_gpu = 0;
    uint32_t constants_used = 0;
    uint32_t views_used = 0;
    uint32_t arena_used = 0;
  };
  std::array<Frame, kFrames> frames;
  uint32_t slot = 0;

  ComPtr<ID3D12DescriptorHeap> view_heap;     // shader visible: [0..1] shared memory, then rings
  ComPtr<ID3D12DescriptorHeap> sampler_heap;  // shader visible: default table, then cached sets
  uint32_t sampler_inc = 0;
  uint32_t samplers_used = kSamplerTableSize;
  std::unordered_map<uint64_t, uint32_t> sampler_sets;  // descs hash -> first heap index
  ComPtr<ID3D12DescriptorHeap> staging_heap;  // CPU: dummy texture SRVs (2D array, 3D, cube)
  uint32_t view_inc = 0;
  std::array<ComPtr<ID3D12Resource>, 3> dummy_textures;
  // --tenchuz_native_debug_textures: 2D dummy is a 64x64 checker (R = u, G = v),
  // copied in on the first draw.
  static constexpr uint32_t kDebugSize = 64;
  bool debug_textures = false;
  ComPtr<ID3D12Resource> debug_upload;
  ComPtr<ID3D12Resource> retired_upload;

  // GPU hang breadcrumbs: per frame slot, how many draws the GPU has started and
  // completed (WriteBufferImmediate markers), and what each draw was.
  struct DrawRecord {
    const char* op;  // null for draws
    uint64_t vs, ps;
    uint32_t prim, count, indexed, ib_base, ib_size, vb_base, vb_size, vb_stride;
    uint32_t arena;  // 1: vertices copied to the arena, 2: indices
    uint32_t query;  // occlusion query index + 1 (--tenchuz_native_draw_queries), 0 = none
    uint32_t target_width, target_height, samples;  // host pixels
    uint32_t color_format;  // DXGI format of RT0 (the GPU profile groups by it)
    uint32_t guest_textures, resolved_textures;
    uint32_t textures[6];  // bound texture addresses, bit 0 set for resolved targets (draw list)
    bool depth_test, hud;  // hud: kept at 16:9 (DrawTarget::hud_scale_x)
    float ndc_scale[2], ndc_offset[2];
    // The packed VS float constants, still in the slot's constant ring when
    // the slot's draw list is logged; hashed only then (it costs a lot per draw).
    const uint8_t* vs_constants;
    uint32_t vs_constants_size;
    uint32_t packet;             // guest address of the draw packet
    uint32_t color_info, depth_info, color_surface, depth_surface;  // DrawTarget diagnostics
  };

  // --tenchuz_native_log_offscreen: the x / y range of a screen-space draw's
  // vertices (the first two floats of each), logged when it reaches more than
  // 256 pixels off a 1280x720 screen or isn't finite.
  void LogOffscreen(uint8_t* base, const DrawRecord& r) {
    const uint32_t vertices = std::min(r.vb_size / r.vb_stride, 4096u);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    uint32_t bad = 0;
    for (uint32_t v = 0; v < vertices; ++v) {
      for (uint32_t k = 0; k < 3 && 4 * k + 4 <= r.vb_stride; ++k) {
        const uint32_t bits = REX_LOAD_U32(0xA0000000u | (r.vb_base + v * r.vb_stride + 4 * k));
        float f;
        std::memcpy(&f, &bits, 4);
        if (!std::isfinite(f)) {
          ++bad;
          continue;
        }
        lo[k] = std::min(lo[k], f);
        hi[k] = std::max(hi[k], f);
      }
    }
    if (!bad && lo[0] >= -256.0f && hi[0] <= 1536.0f && lo[1] >= -256.0f && hi[1] <= 976.0f) {
      return;
    }
    REXLOG_INFO("tenchuz: NATIVE offscreen frame {} vs {:016X} ps {:016X} prim {} count {} vb {:08X}+{:X} stride {}; "
                "target {}x{} x{}{}{}, textures {} game {} resolved; x [{:g},{:g}] y [{:g},{:g}] z [{:g},{:g}], "
                "{} not finite",
                trace_frame, r.vs, r.ps, r.prim, r.count, r.vb_base, r.vb_size, r.vb_stride, r.target_width,
                r.target_height, r.samples, r.depth_test ? " depth" : "", r.hud ? " hud" : "", r.guest_textures,
                r.resolved_textures, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], bad);
  }

  ComPtr<ID3D12Resource> breadcrumbs;
  volatile uint32_t* breadcrumbs_map = nullptr;  // [2 * slot] started, [2 * slot + 1] completed
  D3D12_GPU_VIRTUAL_ADDRESS breadcrumbs_gpu = 0;
  ID3D12GraphicsCommandList* breadcrumb_list = nullptr;
  ComPtr<ID3D12GraphicsCommandList2> breadcrumb_list2;
  std::array<std::vector<DrawRecord>, kFrames> records;
  // --tenchuz_native_draw_queries: samples each draw passed, resolved at the
  // end of its frame and listed when the slot comes round again.
  static constexpr uint32_t kQueriesPerFrame = 2048;
  ComPtr<ID3D12QueryHeap> query_heap;
  ComPtr<ID3D12Resource> query_readback;
  const uint64_t* query_map = nullptr;
  std::array<uint32_t, kFrames> queries_used{};
  std::array<bool, kFrames> log_pending{};
  bool stencil_missing = false;  // TakeStencilMissing

  // GPU profile (RequestProfile): timestamp 0 at the frame's first record,
  // then one after each record, in the same order as records[slot].
  static constexpr uint32_t kTimestampsPerFrame = 8192;
  ComPtr<ID3D12QueryHeap> timestamp_heap;
  ComPtr<ID3D12Resource> timestamp_readback;
  const uint64_t* timestamp_map = nullptr;
  uint64_t timestamp_frequency = 0;
  std::atomic<bool> profile_requested{false};
  std::array<bool, kFrames> profiling{};        // this slot's frame is being profiled
  std::array<bool, kFrames> profile_resolved{};  // ... and its timestamps were resolved
  std::array<uint32_t, kFrames> timestamps_used{};
  ID3D12GraphicsCommandList* timestamp_list = nullptr;

  void Timestamp(ID3D12GraphicsCommandList* list) {
    if (!profiling[slot] || timestamps_used[slot] >= kTimestampsPerFrame) return;
    list->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                   slot * kTimestampsPerFrame + timestamps_used[slot]++);
    timestamp_list = list;
  }

  // Logs the slot's profile: GPU time per render target, per operation, and
  // the slowest draws. Runs once the slot's GPU work has finished.
  void LogProfile(uint32_t s) {
    const uint32_t used = timestamps_used[s];
    const auto& list = records[s];
    if (!profile_resolved[s] || used < 2 || !timestamp_frequency) {
      REXLOG_INFO("tenchuz: NATIVE GPU profile: no timestamps (frame not presented?)");
      return;
    }
    const uint64_t* ts = timestamp_map + uint64_t(s) * kTimestampsPerFrame;
    const double to_ms = 1000.0 / double(timestamp_frequency);
    const uint32_t n = std::min<uint32_t>(used - 1, uint32_t(list.size()));
    struct Group {
      double ms = 0;
      uint32_t count = 0;
    };
    std::map<std::string, Group> by_target, by_op;
    std::vector<std::pair<double, uint32_t>> draws, ops;
    double total = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const double ms = ts[i + 1] >= ts[i] ? double(ts[i + 1] - ts[i]) * to_ms : 0.0;
      total += ms;
      const DrawRecord& r = list[i];
      if (r.op) {
        Group& g = by_op[r.op];
        g.ms += ms;
        ++g.count;
        ops.emplace_back(ms, i);
        continue;
      }
      Group& g = by_target[fmt::format("{}x{} x{} {} fmt {}", r.target_width, r.target_height, r.samples,
                                       r.depth_test ? "3D" : "2D", r.color_format)];
      g.ms += ms;
      ++g.count;
      draws.emplace_back(ms, i);
    }
    REXLOG_INFO("tenchuz: NATIVE GPU profile of frame {}: {:.2f} ms on the GPU over {} draws and operations",
                record_frames[s], total, n);
    auto log_groups = [&](const char* what, std::map<std::string, Group>& groups) {
      std::vector<std::pair<std::string, Group>> sorted(groups.begin(), groups.end());
      std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
      for (const auto& [name, g] : sorted) {
        REXLOG_INFO("tenchuz: NATIVE GPU profile   {} {}: {:.2f} ms ({:.0f}%), {}", what, name, g.ms,
                    total > 0 ? 100.0 * g.ms / total : 0.0, g.count);
      }
    };
    log_groups("target", by_target);
    log_groups("operation", by_op);
    if (REXCVAR_GET(tenchuz_native_profile_timeline)) {
      for (uint32_t i = 0; i < n; ++i) {
        const DrawRecord& r = list[i];
        const double ms = ts[i + 1] >= ts[i] ? double(ts[i + 1] - ts[i]) * to_ms : 0.0;
        if (r.op) {
          REXLOG_INFO("tenchuz: NATIVE GPU timeline #{} {:.3f} ms {} {:08X} {}x{}", i, ms, r.op, r.prim,
                      r.count >> 16, r.count & 0xFFFF);
        } else {
          REXLOG_INFO("tenchuz: NATIVE GPU timeline #{} {:.3f} ms draw ps {:08X} {}x{} x{}{} count {}", i, ms,
                      uint32_t(r.ps), r.target_width, r.target_height, r.samples, r.depth_test ? " depth" : "",
                      r.count);
        }
      }
    }
    std::sort(ops.begin(), ops.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t k = 0; k < std::min<size_t>(ops.size(), 12); ++k) {
      const DrawRecord& r = list[ops[k].second];
      REXLOG_INFO("tenchuz: NATIVE GPU profile   op #{}: {:.3f} ms, {} {:08X} {}x{}", ops[k].second, ops[k].first,
                  r.op, r.prim, r.count >> 16, r.count & 0xFFFF);
    }
    std::sort(draws.begin(), draws.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t k = 0; k < std::min<size_t>(draws.size(), 12); ++k) {
      const DrawRecord& r = list[draws[k].second];
      REXLOG_INFO("tenchuz: NATIVE GPU profile   draw #{}: {:.3f} ms, vs {:08X} ps {:08X}, {} {} on {}x{} x{} fmt {}{}, "
                  "textures {:X} {:X} {:X} {:X} {:X} {:X}",
                  draws[k].second, draws[k].first, uint32_t(r.vs), uint32_t(r.ps), r.count,
                  r.indexed ? "indices" : "vertices", r.target_width, r.target_height, r.samples, r.color_format,
                  r.depth_test ? " depth" : "", r.textures[0], r.textures[1], r.textures[2], r.textures[3],
                  r.textures[4], r.textures[5]);
    }
  }
  std::array<uint64_t, kFrames> log_frame{};
  std::array<uint64_t, kFrames> query_frame{};  // frame number of each slot's queries
  std::array<uint64_t, kFrames> record_frames{};
  uint64_t frame_counter = 0;
  std::mutex records_mutex;
  std::atomic<bool> hang_reported{false};

  // Lists a finished frame slot's draws and operations in order.
  void LogDraws(uint32_t s) {
    std::lock_guard lock(records_mutex);
    REXLOG_INFO("tenchuz: NATIVE draw list of frame {} ({} entries)", log_frame[s], records[s].size());
    for (uint32_t i = 0; i < records[s].size(); ++i) {
      const DrawRecord& r = records[s][i];
      if (r.op) {
        REXLOG_INFO("tenchuz: NATIVE #{} {} {:08X} {:08X}", i, r.op, r.prim, r.count);
        continue;
      }
      const std::string samples =
          r.query && query_map ? fmt::format(" samples {}", query_map[r.query - 1]) : std::string();
      std::string bound;
      for (uint32_t t : r.textures) {
        if (t) bound += fmt::format(" {:08X}{}", t & ~1u, (t & 1) ? "R" : "");
      }
      REXLOG_INFO("tenchuz: NATIVE #{} draw vs {:016X} ps {:016X} prim {} count {} {} ib {:08X}+{:X} vb {:08X}+{:X} "
                  "stride {}{}{}{}; target {}x{} x{}{}, textures {} game {} resolved{}; ndc {:g} {:g} {:g} {:g}, "
                  "vs constants {:08X}, packet {:08X}, rt {:08X} #{} depth {:08X} #{}, bound{}",
                  i, r.vs, r.ps, r.prim, r.count, r.indexed ? "indexed" : "auto", r.ib_base, r.ib_size,
                  r.vb_base, r.vb_size, r.vb_stride, (r.arena & 1) ? " vb-arena" : "",
                  (r.arena & 2) ? " ib-arena" : "", samples, r.target_width, r.target_height, r.samples,
                  r.depth_test ? " depth" : "", r.guest_textures, r.resolved_textures, r.hud ? ", hud" : "",
                  r.ndc_scale[0], r.ndc_scale[1], r.ndc_offset[0], r.ndc_offset[1],
                  r.vs_constants ? uint32_t(XXH3_64bits(r.vs_constants, r.vs_constants_size)) : 0u, r.packet,
                  r.color_info, r.color_surface, r.depth_info, r.depth_surface, bound);
    }
  }

  // Records `record` and writes its "started" marker; returns its number.
  uint32_t MarkStart(ID3D12GraphicsCommandList* list, const DrawRecord& record) {
    if (!breadcrumbs_map) {
      return 0;
    }
    if (list != breadcrumb_list) {
      breadcrumb_list = list;
      breadcrumb_list2.Reset();
      list->QueryInterface(IID_PPV_ARGS(&breadcrumb_list2));
    }
    uint32_t number;
    {
      std::lock_guard lock(records_mutex);
      records[slot].push_back(record);
      number = uint32_t(records[slot].size());
    }
    if (number == 1) Timestamp(list);  // the frame's start
    if (breadcrumb_list2) {
      const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER started = {breadcrumbs_gpu + 8 * slot, number};
      const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
      breadcrumb_list2->WriteBufferImmediate(1, &started, &mode);
    }
    return number;
  }

  void MarkEnd(uint32_t number) {
    if (number && breadcrumb_list) Timestamp(breadcrumb_list);
    if (breadcrumb_list2 && number) {
      const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER completed = {breadcrumbs_gpu + 8 * slot + 4, number};
      const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
      breadcrumb_list2->WriteBufferImmediate(1, &completed, &mode);
    }
  }

  TextureLoader loader;
  bool loader_ready = false;

  std::unordered_map<uint32_t, ComPtr<ID3D12RootSignature>> root_signatures;
  std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> pipelines;

  // Compiled pipelines kept between runs (ID3D12PipelineLibrary): a new
  // pipeline costs the driver's shader compile once (up to a few hundred ms,
  // a visible hitch), and loads from the library in well under a millisecond
  // after. Named by a hash of everything in its description; saved at most
  // every 30 s while new ones come in, and at exit.
  ComPtr<ID3D12PipelineLibrary> pipeline_library;
  std::vector<uint8_t> pipeline_library_blob;  // the library reads from it while it lives
  std::filesystem::path pipeline_library_path;
  uint32_t pipelines_stored = 0;  // since the last save
  std::chrono::steady_clock::time_point pipeline_library_saved = std::chrono::steady_clock::now();

  void OpenPipelineLibrary(const std::filesystem::path& path) {
    ComPtr<ID3D12Device1> device1;
    if (path.empty() || FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
      return;
    }
    pipeline_library_path = path;
    if (FILE* file = _wfopen(path.c_str(), L"rb")) {
      std::fseek(file, 0, SEEK_END);
      const long size = std::ftell(file);
      std::fseek(file, 0, SEEK_SET);
      if (size > 0) {
        pipeline_library_blob.resize(size_t(size));
        if (std::fread(pipeline_library_blob.data(), 1, size_t(size), file) != size_t(size)) {
          pipeline_library_blob.clear();
        }
      }
      std::fclose(file);
    }
    HRESULT hr = E_FAIL;
    if (!pipeline_library_blob.empty()) {
      hr = device1->CreatePipelineLibrary(pipeline_library_blob.data(), pipeline_library_blob.size(),
                                          IID_PPV_ARGS(&pipeline_library));
      if (FAILED(hr)) {  // a new driver or GPU: start over
        REXLOG_INFO("tenchuz: N4 pipeline cache {} not usable ({:08X}); starting a new one", path.string(),
                    uint32_t(hr));
        pipeline_library_blob.clear();
      }
    }
    if (FAILED(hr)) {
      hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&pipeline_library));
    }
    if (FAILED(hr)) {
      pipeline_library.Reset();
      REXLOG_WARN("tenchuz: N4 no pipeline cache ({:08X})", uint32_t(hr));
      return;
    }
    REXLOG_INFO("tenchuz: N4 pipeline cache {} ({} KB)", path.string(), pipeline_library_blob.size() >> 10);
  }

  void SavePipelineLibrary() {
    if (!pipeline_library || !pipelines_stored) {
      return;
    }
    std::vector<uint8_t> data(pipeline_library->GetSerializedSize());
    if (data.empty() || FAILED(pipeline_library->Serialize(data.data(), data.size()))) {
      return;
    }
    std::error_code error;
    std::filesystem::create_directories(pipeline_library_path.parent_path(), error);
    const std::filesystem::path temp = pipeline_library_path.wstring() + L".tmp";
    if (FILE* file = _wfopen(temp.c_str(), L"wb")) {
      const bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
      std::fclose(file);
      if (ok) {
        std::filesystem::rename(temp, pipeline_library_path, error);
      }
    }
    pipelines_stored = 0;
    pipeline_library_saved = std::chrono::steady_clock::now();
  }

  static uint64_t PipelineName(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state = desc;  // without the pointers
    state.pRootSignature = nullptr;
    state.VS = state.PS = state.GS = state.DS = state.HS = {};
    state.InputLayout = {};
    state.StreamOutput = {};
    uint64_t name = XXH3_64bits(&state, sizeof(state));
    for (const D3D12_SHADER_BYTECODE& code : {desc.VS, desc.PS, desc.GS}) {
      name = name * 0x9E3779B97F4A7C15ull ^ (code.BytecodeLength ? XXH3_64bits(code.pShaderBytecode, code.BytecodeLength) : 0);
    }
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
      const D3D12_INPUT_ELEMENT_DESC& e = desc.InputLayout.pInputElementDescs[i];
      name = name * 0x9E3779B97F4A7C15ull ^ XXH3_64bits(e.SemanticName, std::strlen(e.SemanticName));
      const uint32_t fields[] = {e.SemanticIndex, uint32_t(e.Format), e.InputSlot, e.AlignedByteOffset,
                                 uint32_t(e.InputSlotClass), e.InstanceDataStepRate};
      name = name * 0x9E3779B97F4A7C15ull ^ XXH3_64bits(fields, sizeof(fields));
    }
    return name;
  }
  DrawStats stats;
  std::unordered_set<uint64_t> logged_fetches;  // --tenchuz_native_log_textures
  uint32_t draw_textures[6] = {};  // the current draw's bound textures (DrawRecord::textures)
  uint32_t draw_texture_count = 0;
  uint32_t logged_errors = 0;

  bool Initialize(ID3D12Device* d, rex::memory::Memory* memory) {
    device = d;
    serialize_root = reinterpret_cast<SerializeRootFn>(
        GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12SerializeRootSignature"));
    if (!serialize_root) {
      return false;
    }
    auto buffer_desc = [](uint64_t size) {
      D3D12_RESOURCE_DESC desc = {};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width = size;
      desc.Height = 1;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      return desc;
    };
    D3D12_RESOURCE_DESC desc = buffer_desc(kSharedMemorySize + uint64_t(kFrames) * kArenaSize);
    if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesUpload,
                                               D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&shared)))) {
      return false;
    }
    SetDebugName(shared.Get(), "tenchuz shared memory mirror + arenas");
    D3D12_RANGE none = {0, 0};
    shared->Map(0, &none, reinterpret_cast<void**>(&shared_map));
    shared_gpu = shared->GetGPUVirtualAddress();
    for (Frame& f : frames) {
      desc = buffer_desc(kConstantRingSize);
      if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesUpload,
                                                 D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&f.constants)))) {
        return false;
      }
      SetDebugName(f.constants.Get(), "tenchuz constant ring");
      f.constants->Map(0, &none, reinterpret_cast<void**>(&f.constants_map));
      f.constants_gpu = f.constants->GetGPUVirtualAddress();
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2 + kFrames * kViewRingSize;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&view_heap)))) return false;
    SetDebugName(view_heap.Get(), "tenchuz draw view heap");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap.NumDescriptors = kSamplerHeapSize;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&sampler_heap)))) return false;
    SetDebugName(sampler_heap.Get(), "tenchuz draw sampler heap");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 3;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&staging_heap)))) return false;
    {
      D3D12_DESCRIPTOR_HEAP_DESC cache = {};
      cache.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      cache.NumDescriptors = kViewCacheSize;
      if (SUCCEEDED(device->CreateDescriptorHeap(&cache, IID_PPV_ARGS(&view_cache_heap)))) {
        for (uint32_t i = kViewCacheSize; i-- > 0;) view_cache_free.push_back(i);
      }
    }
    view_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Shared memory SRV (raw) and a null UAV (memexport is not supported yet).
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = UINT(kSharedMemorySize / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(shared.Get(), &srv, cpu);
    cpu.ptr += view_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = UINT(kSharedMemorySize / 4);
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(nullptr, nullptr, &uav, cpu);

    // Dummy textures: their SRVs force every component to 1, so their contents
    // never matter (white until real textures arrive in N4.3), except the
    // debug checker.
    debug_textures = REXCVAR_GET(tenchuz_native_debug_textures);
    const D3D12_RESOURCE_DIMENSION dims[3] = {D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                              D3D12_RESOURCE_DIMENSION_TEXTURE3D,
                                              D3D12_RESOURCE_DIMENSION_TEXTURE2D};
    for (int i = 0; i < 3; ++i) {
      D3D12_RESOURCE_DESC tex = {};
      tex.Dimension = dims[i];
      const bool debug = debug_textures && i == 0;
      tex.Width = debug ? kDebugSize : 1;
      tex.Height = debug ? kDebugSize : 1;
      tex.DepthOrArraySize = i == 2 ? 6 : 1;
      tex.MipLevels = 1;
      tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      tex.SampleDesc.Count = 1;
      if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                                 D3D12_HEAP_FLAG_NONE, &tex,
                                                 debug ? D3D12_RESOURCE_STATE_COPY_DEST
                                                       : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 nullptr, IID_PPV_ARGS(&dummy_textures[i])))) {
        return false;
      }
      SetDebugName(dummy_textures[i].Get(), "tenchuz placeholder texture");
      // Zeros, like the SDK's null texture descriptors (unused signed views,
      // textures that can't be loaded), so a missing texture is never white.
      D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
      view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      view.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
          D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
          D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
      if (debug) {
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      }
      if (i == 0) {
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.ArraySize = 1;
      } else if (i == 1) {
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipLevels = 1;
      } else {
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.TextureCube.MipLevels = 1;
      }
      D3D12_CPU_DESCRIPTOR_HANDLE dst = staging_heap->GetCPUDescriptorHandleForHeapStart();
      dst.ptr += SIZE_T(i) * view_inc;
      device->CreateShaderResourceView(dummy_textures[i].Get(), &view, dst);
    }

    if (debug_textures) {
      desc = buffer_desc(kDebugSize * kDebugSize * 4);  // row pitch 256: placement-aligned
      if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesUpload,
                                                 D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&debug_upload)))) {
        return false;
      }
      uint8_t* texels = nullptr;
      debug_upload->Map(0, &none, reinterpret_cast<void**>(&texels));
      for (uint32_t y = 0; y < kDebugSize; ++y) {
        for (uint32_t x = 0; x < kDebugSize; ++x) {
          uint8_t* t = texels + 4 * (y * kDebugSize + x);
          t[0] = uint8_t(x * 255 / (kDebugSize - 1));
          t[1] = uint8_t(y * 255 / (kDebugSize - 1));
          t[2] = ((x / 8 + y / 8) & 1) ? 255 : 64;
          t[3] = 255;
        }
      }
      debug_upload->Unmap(0, nullptr);
    }

    desc = buffer_desc(256);
    if (SUCCEEDED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                                  D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&breadcrumbs)))) {
      void* mapped = nullptr;
      D3D12_RANGE all = {0, 256};
      breadcrumbs->Map(0, &all, &mapped);
      breadcrumbs_map = static_cast<volatile uint32_t*>(mapped);
      breadcrumbs_gpu = breadcrumbs->GetGPUVirtualAddress();
      SetDebugName(breadcrumbs.Get(), "tenchuz breadcrumbs");
    }

    {
      D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimestampsPerFrame * kFrames, 0};
      const D3D12_RESOURCE_DESC rd = buffer_desc(uint64_t(kTimestampsPerFrame) * kFrames * 8);
      if (SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&timestamp_heap))) &&
          SUCCEEDED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                                    D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&timestamp_readback)))) {
        void* mapped = nullptr;
        D3D12_RANGE all = {0, SIZE_T(rd.Width)};
        timestamp_readback->Map(0, &all, &mapped);
        timestamp_map = static_cast<const uint64_t*>(mapped);
      } else {
        timestamp_heap.Reset();
      }
    }

    if (REXCVAR_GET(tenchuz_native_draw_queries)) {
      D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQueriesPerFrame * kFrames, 0};
      desc = buffer_desc(uint64_t(kQueriesPerFrame) * kFrames * 8);
      if (SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap))) &&
          SUCCEEDED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&query_readback)))) {
        void* mapped = nullptr;
        D3D12_RANGE all = {0, SIZE_T(desc.Width)};
        query_readback->Map(0, &all, &mapped);
        query_map = static_cast<const uint64_t*>(mapped);
      } else {
        query_heap.Reset();
      }
    }

    loader_ready = loader.Initialize(device, shared.Get(), kSharedMemorySize, memory);
    if (!loader_ready) {
      REXLOG_WARN("tenchuz: N4 texture loader unavailable");
    }

    D3D12_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.MaxAnisotropy = 1;
    sampler_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    D3D12_CPU_DESCRIPTOR_HANDLE scpu = sampler_heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kSamplerTableSize; ++i) {
      device->CreateSampler(&sampler, scpu);
      scpu.ptr += sampler_inc;
    }
    return true;
  }

  // --- Per-draw helpers -------------------------------------------------------

  void* AllocConstants(uint32_t size, D3D12_GPU_VIRTUAL_ADDRESS& gpu) {
    Frame& f = frames[slot];
    const uint32_t offset = (f.constants_used + kCbAlign - 1) & ~(kCbAlign - 1);
    const uint32_t aligned = (std::max(size, 16u) + kCbAlign - 1) & ~(kCbAlign - 1);
    if (offset + aligned > kConstantRingSize) {
      return nullptr;
    }
    f.constants_used = offset + aligned;
    gpu = f.constants_gpu + offset;
    return f.constants_map + offset;
  }

  // Copy a guest physical range into the mirror, in whole 4 KB pages (the host
  // commit granularity, so whole pages of a guest allocation are readable),
  // each page at most once per frame. Guest memory is not watched for writes
  // yet, so everything a frame references is copied again each frame.
  //
  // Pages in the D3D's command ring (dynamic vertices and indices) are reused
  // within a frame: if one changed since it was copied, the draws recorded so
  // far are submitted and finished first, then the new contents are copied.
  void Upload(uint8_t* base, uint32_t physical, uint32_t size, DrawHost* host = nullptr) {
    physical &= 0x1FFFFFFFu;
    if (!size) {
      return;
    }
    const uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(physical) + size, kSharedMemorySize));
    uint32_t page = physical >> kPageShift;
    const uint32_t page_end = (end + kPageSize - 1) >> kPageShift;
    if (host && REXCVAR_GET(tenchuz_native_ring_flush) && host->InCommandRing(physical)) {
      bool flushed = false;
      for (; page < page_end; ++page) {
        const uint32_t offset = page << kPageShift;
        const uint8_t* guest = REX_RAW_ADDR(0xA0000000u | offset);
        const uint64_t hash = XXH3_64bits(guest, kPageSize);
        const bool uploaded = uploaded_pages[page >> 6] & (1ull << (page & 63));
        auto it = page_hashes.find(page);
        if (uploaded && it != page_hashes.end() && it->second == hash) {
          continue;
        }
        if (uploaded) {
          ++stats.dynamic_rewrites;
          if (!flushed) {
            host->FlushAndWait();
            ++stats.flushes;
            flushed = true;
          }
        }
        uploaded_pages[page >> 6] |= 1ull << (page & 63);
        page_hashes[page] = hash;
        BeforeOverwrite(page, page + 1);
        page_used_frame[page] = uint32_t(frame_counter);
        std::memcpy(shared_map + offset, guest, kPageSize);
        stats.uploaded_kb += kPageSize >> 10;
      }
      return;
    }
    const int32_t recheck = REXCVAR_GET(tenchuz_native_recheck_pages);
    // Geometry outside the D3D's command ring changes rarely: copy it once and
    // again only after the guest writes it (the texture loader's page watch).
    // Ring pages are rewritten all the time and are copied every frame.
    const bool watch = recheck == 0 && loader_ready && loader.WatchingWrites() &&
                       REXCVAR_GET(tenchuz_native_geometry_watch) && !(host && host->InCommandRing(physical));
    while (page < page_end) {
      if (uploaded_pages[page >> 6] & (1ull << (page & 63))) {
        if (recheck > 0) {
          // Already uploaded this frame: has the guest rewritten it since?
          const uint32_t offset = page << kPageShift;
          const uint8_t* guest = REX_RAW_ADDR(0xA0000000u | offset);
          const uint64_t hash = XXH3_64bits(guest, kPageSize);
          uint64_t& uploaded_hash = static_page_hashes[page];
          if (hash != uploaded_hash) {
            ++stats.stale_pages;
            if (stale_logged++ < 6) {
              REXLOG_INFO("tenchuz: N4 page {:08X} changed after its upload this frame (draw data {:08X} "
                          "+{:X}{})",
                          offset, upload_context, upload_context_size,
                          host && host->InCommandRing(offset) ? ", command ring" : "");
            }
            if (recheck >= 2) {
              std::memcpy(shared_map + offset, guest, kPageSize);  // this frame's own page
              uploaded_hash = hash;
              stats.uploaded_kb += kPageSize >> 10;
            }
          }
        }
        ++page;
        continue;
      }
      // Watched pages copied before and not written since are still current
      // in the mirror (it persists across frames).
      auto current = [&](uint32_t p) {
        const uint32_t copied = page_copy_sequence[p];
        return watch && copied && !loader.WrittenSince(p << kPageShift, kPageSize, copied - 1);
      };
      if (current(page)) {
        uploaded_pages[page >> 6] |= 1ull << (page & 63);
        page_used_frame[page] = uint32_t(frame_counter);
        ++stats.watched_pages;
        ++page;
        continue;
      }
      const uint32_t run_start = page;
      while (page < page_end && !(uploaded_pages[page >> 6] & (1ull << (page & 63))) &&
             (page == run_start || !current(page))) {
        uploaded_pages[page >> 6] |= 1ull << (page & 63);
        if (recheck > 0) {
          static_page_hashes[page] = XXH3_64bits(REX_RAW_ADDR(0xA0000000u | (page << kPageShift)), kPageSize);
        }
        ++page;
      }
      const uint32_t offset = run_start << kPageShift;
      const uint32_t bytes = (page - run_start) << kPageShift;
      BeforeOverwrite(run_start, page);
      std::fill(page_used_frame.begin() + run_start, page_used_frame.begin() + page, uint32_t(frame_counter));
      if (watch) {
        // Sequence first, then protect, then copy: a write from here on is
        // newer than the copy and makes the pages stale.
        const uint32_t sequence = loader.WriteSequence();
        loader.Watch(offset, bytes);
        for (uint32_t p = run_start; p < page; ++p) page_copy_sequence[p] = sequence + 1;
      }
      std::memcpy(shared_map + offset, REX_RAW_ADDR(0xA0000000u | offset), bytes);
      NoteCopy(base, offset, bytes, 'S');
      stats.uploaded_kb += bytes >> 10;
    }
  }

  ID3D12RootSignature* RootSignature(uint32_t vs_textures, uint32_t vs_samplers, uint32_t ps_textures,
                                     uint32_t ps_samplers) {
    const uint32_t key = vs_textures | (vs_samplers << 8) | (ps_textures << 16) | (ps_samplers << 24);
    auto it = root_signatures.find(key);
    if (it != root_signatures.end()) {
      return it->second.Get();
    }
    D3D12_ROOT_PARAMETER params[kRootBaseCount + 4] = {};
    auto cbv = [&](uint32_t index, Translator::CbufferRegister reg, D3D12_SHADER_VISIBILITY vis) {
      params[index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      params[index].Descriptor.ShaderRegister = uint32_t(reg);
      params[index].ShaderVisibility = vis;
    };
    cbv(kRootFetchConstants, Translator::CbufferRegister::kFetchConstants, D3D12_SHADER_VISIBILITY_ALL);
    cbv(kRootFloatConstantsVertex, Translator::CbufferRegister::kFloatConstants,
        D3D12_SHADER_VISIBILITY_VERTEX);
    cbv(kRootFloatConstantsPixel, Translator::CbufferRegister::kFloatConstants,
        D3D12_SHADER_VISIBILITY_PIXEL);
    cbv(kRootSystemConstants, Translator::CbufferRegister::kSystemConstants, D3D12_SHADER_VISIBILITY_ALL);
    cbv(kRootBoolLoopConstants, Translator::CbufferRegister::kBoolLoopConstants,
        D3D12_SHADER_VISIBILITY_ALL);
    D3D12_DESCRIPTOR_RANGE shared_ranges[2] = {};
    shared_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shared_ranges[0].NumDescriptors = 1;
    shared_ranges[0].BaseShaderRegister = uint32_t(Translator::SRVMainRegister::kSharedMemory);
    shared_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    shared_ranges[1].NumDescriptors = 1;
    shared_ranges[1].BaseShaderRegister = uint32_t(Translator::UAVRegister::kSharedMemory);
    shared_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    params[kRootSharedMemory].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRootSharedMemory].DescriptorTable.NumDescriptorRanges = 2;
    params[kRootSharedMemory].DescriptorTable.pDescriptorRanges = shared_ranges;
    params[kRootSharedMemory].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    uint32_t count = kRootBaseCount;
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    auto table = [&](D3D12_DESCRIPTOR_RANGE_TYPE type, uint32_t n, uint32_t first_register,
                     D3D12_SHADER_VISIBILITY vis) {
      if (!n) return;
      D3D12_DESCRIPTOR_RANGE& r = ranges[count - kRootBaseCount];
      r.RangeType = type;
      r.NumDescriptors = n;
      r.BaseShaderRegister = first_register;
      params[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[count].DescriptorTable.NumDescriptorRanges = 1;
      params[count].DescriptorTable.pDescriptorRanges = &r;
      params[count].ShaderVisibility = vis;
      ++count;
    };
    const uint32_t first_texture = uint32_t(Translator::SRVMainRegister::kBindfulTexturesStart);
    table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, ps_textures, first_texture, D3D12_SHADER_VISIBILITY_PIXEL);
    table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, ps_samplers, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, vs_textures, first_texture, D3D12_SHADER_VISIBILITY_VERTEX);
    table(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, vs_samplers, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = count;
    desc.pParameters = params;
    ComPtr<ID3DBlob> blob, errors;
    ComPtr<ID3D12RootSignature> root;
    if (FAILED(serialize_root(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&root)))) {
      return nullptr;
    }
    return root_signatures.emplace(key, root).first->second.Get();
  }

  // Geometry shaders (SDK generator), by key.
  std::unordered_map<uint32_t, std::vector<uint32_t>> geometry_shaders;

  const std::vector<uint32_t>* GeometryShader(gfx::vendored::GeometryShaderKey key) {
    auto it = geometry_shaders.find(key.key);
    if (it == geometry_shaders.end()) {
      std::vector<uint32_t> code;
      gfx::vendored::CreateDxbcGeometryShader(key, code);
      it = geometry_shaders.emplace(key.key, std::move(code)).first;
    }
    return &it->second;
  }

  ID3D12PipelineState* Pipeline(ID3D12RootSignature* root, const DrawShaders& shaders,
                                const gfx::RegisterFile& regs, const DrawTarget& target,
                                D3D12_PRIMITIVE_TOPOLOGY_TYPE topology,
                                D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut,
                                gfx::vendored::GeometryShaderKey gs_key) {
    const bool polygonal = topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    const reg::RB_DEPTHCONTROL depth = NormalizedDepthControl(regs);
    const reg::RB_STENCILREFMASK stencil = StencilRefMask(regs, depth, polygonal);
    uint32_t blend[4] = {};
    uint32_t rt_count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (target.color_formats[i] != DXGI_FORMAT_UNKNOWN) {
        rt_count = i + 1;
        blend[i] = regs[kBlendControl[i]];
      }
    }
    const RasterState raster = GetRasterState(regs, polygonal, target.resolution_scale);
    uint32_t slope_bits;
    std::memcpy(&slope_bits, &raster.slope_scaled_depth_bias, 4);
    uint64_t key = XXH3_64bits(&shaders, sizeof(shaders));
    const uint32_t state[] = {uint32_t(uintptr_t(root)), depth.value, stencil.value & 0xFFFF00,
                              blend[0], blend[1], blend[2], blend[3], target.color_mask,
                              uint32_t(target.color_formats[0]), uint32_t(target.color_formats[1]),
                              uint32_t(target.color_formats[2]), uint32_t(target.color_formats[3]),
                              uint32_t(target.depth_format), uint32_t(topology), uint32_t(strip_cut), gs_key.key,
                              uint32_t(raster.cull) | (uint32_t(raster.front_counter_clockwise) << 4) |
                                  (uint32_t(raster.wireframe) << 5) | (uint32_t(raster.depth_clip) << 6),
                              uint32_t(raster.depth_bias), slope_bits, target.samples};
    key ^= XXH3_64bits(state, sizeof(state)) * 0x9E3779B97F4A7C15ull;
    auto it = pipelines.find(key);
    if (it != pipelines.end()) {
      return it->second.Get();
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    const auto& vs = shaders.vertex->translated_binary();
    desc.VS = {vs.data(), vs.size()};
    if (shaders.pixel) {
      const auto& ps = shaders.pixel->translated_binary();
      desc.PS = {ps.data(), ps.size()};
    }
    if (gs_key.type != gfx::vendored::PipelineGeometryShader::kNone) {
      const std::vector<uint32_t>* gs = GeometryShader(gs_key);
      desc.GS = {gs->data(), gs->size() * sizeof(uint32_t)};
    }
    desc.BlendState.IndependentBlendEnable = rt_count > 1;
    for (uint32_t i = 0; i < rt_count; ++i) {
      desc.RTVFormats[i] = target.color_formats[i];
      D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[i];
      rt.RenderTargetWriteMask = UINT8((target.color_mask >> (4 * i)) & 0xF);
      if (target.color_formats[i] == DXGI_FORMAT_UNKNOWN || !rt.RenderTargetWriteMask) {
        rt.RenderTargetWriteMask = 0;
        continue;
      }
      const uint32_t b = blend[i];
      const uint32_t src = b & 0x1F, op = (b >> 5) & 7, dst = (b >> 8) & 0x1F;
      const uint32_t asrc = (b >> 16) & 0x1F, aop = (b >> 21) & 7, adst = (b >> 24) & 0x1F;
      rt.BlendEnable = !(src == 1 && dst == 0 && op == 0 && asrc == 1 && adst == 0 && aop == 0);
      rt.SrcBlend = BlendFactor(src);
      rt.DestBlend = BlendFactor(dst);
      rt.BlendOp = BlendOp(op);
      rt.SrcBlendAlpha = AlphaBlendFactor(asrc);
      rt.DestBlendAlpha = AlphaBlendFactor(adst);
      rt.BlendOpAlpha = BlendOp(aop);
    }
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = raster.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = raster.cull;
    desc.RasterizerState.FrontCounterClockwise = raster.front_counter_clockwise;
    desc.RasterizerState.DepthBias = raster.depth_bias;
    desc.RasterizerState.SlopeScaledDepthBias = raster.slope_scaled_depth_bias;
    desc.RasterizerState.DepthClipEnable = raster.depth_clip;
    if (target.depth_format != DXGI_FORMAT_UNKNOWN) {
      D3D12_DEPTH_STENCIL_DESC& ds = desc.DepthStencilState;
      ds.DepthEnable = depth.z_enable;
      ds.DepthWriteMask = depth.z_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
      ds.DepthFunc = D3D12_COMPARISON_FUNC(uint32_t(depth.zfunc) + 1);
      if (depth.stencil_enable && target.depth_format != DXGI_FORMAT_D32_FLOAT) {
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = UINT8(stencil.stencilmask);
        ds.StencilWriteMask = UINT8(stencil.stencilwritemask);
        ds.FrontFace = StencilOps(depth.stencilfail, depth.stencilzfail, depth.stencilzpass, depth.stencilfunc);
        ds.BackFace = polygonal && depth.backface_enable
                          ? StencilOps(depth.stencilfail_bf, depth.stencilzfail_bf, depth.stencilzpass_bf,
                                       depth.stencilfunc_bf)
                          : ds.FrontFace;
      }
      desc.DSVFormat = target.depth_format;
    }
    desc.PrimitiveTopologyType = topology;
    desc.IBStripCutValue = strip_cut;
    desc.NumRenderTargets = rt_count;
    desc.SampleDesc.Count = target.samples;
    ComPtr<ID3D12PipelineState> pso;
    const auto pso_start = std::chrono::steady_clock::now();
    wchar_t library_name[24] = {};
    HRESULT hr = E_FAIL;
    if (pipeline_library) {
      std::swprintf(library_name, 24, L"p%016llX", static_cast<unsigned long long>(PipelineName(desc)));
      hr = pipeline_library->LoadGraphicsPipeline(library_name, &desc, IID_PPV_ARGS(pso.ReleaseAndGetAddressOf()));
      if (SUCCEEDED(hr)) {
        ++stats.pipelines_loaded;
      }
    }
    if (FAILED(hr)) {
      hr = CreatePipelineGuarded(device, desc, pso.ReleaseAndGetAddressOf());
      if (SUCCEEDED(hr) && pipeline_library && SUCCEEDED(pipeline_library->StorePipeline(library_name, pso.Get()))) {
        ++pipelines_stored;
      }
    }
    const double pso_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pso_start).count();
    ++stats.pipelines_created;
    stats.pipeline_ms += pso_ms;
    if (FAILED(hr)) {
      if (logged_errors++ < 16) {
        REXLOG_WARN("tenchuz: N4 pipeline creation failed ({:08X}) vs {:016X} ps {:016X}", uint32_t(hr),
                    shaders.vertex->shader().ucode_data_hash(),
                    shaders.pixel ? shaders.pixel->shader().ucode_data_hash() : 0);
      }
      pipelines.emplace(key, nullptr);
      return nullptr;
    }
    return pipelines.emplace(key, pso).first->second.Get();
  }

  void FillSystemConstants(Translator::SystemConstants& sc, const gfx::RegisterFile& regs,
                           uint8_t* base, uint32_t device_address, xenos::PrimitiveType prim,
                           bool indexed, xenos::Endian index_endian, const DrawTarget& target) {
    std::memset(&sc, 0, sizeof(sc));
    const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    uint32_t flags = 0;
    if (vte.vtx_xy_fmt) flags |= Translator::kSysFlag_XYDividedByW;
    if (vte.vtx_z_fmt) flags |= Translator::kSysFlag_ZDividedByW;
    if (vte.vtx_w0_fmt) flags |= Translator::kSysFlag_WNotReciprocal;
    if (prim == xenos::PrimitiveType::kLineList || prim == xenos::PrimitiveType::kLineStrip) {
      flags |= Translator::kSysFlag_PrimitiveLine;
    } else if (prim != xenos::PrimitiveType::kPointList) {
      flags |= Translator::kSysFlag_PrimitivePolygonal;
    }
    if (regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
      flags |= Translator::kSysFlag_DepthFloat24;
    }
    const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
    const uint32_t alpha_func =
        color_control.alpha_test_enable ? uint32_t(color_control.alpha_func) : uint32_t(7);  // always
    if (alpha_func & 1) flags |= Translator::kSysFlag_AlphaPassIfLess;
    if (alpha_func & 2) flags |= Translator::kSysFlag_AlphaPassIfEqual;
    if (alpha_func & 4) flags |= Translator::kSysFlag_AlphaPassIfGreater;
    sc.flags = flags;

    sc.vertex_index_endian = indexed ? index_endian : xenos::Endian::kNone;
    sc.vertex_index_offset = regs[gfx::XE_GPU_REG_VGT_INDX_OFFSET];
    sc.vertex_index_min = regs[gfx::XE_GPU_REG_VGT_MIN_VTX_INDX];
    sc.vertex_index_max = regs[gfx::XE_GPU_REG_VGT_MAX_VTX_INDX];
    if (sc.vertex_index_max <= sc.vertex_index_min) {  // never set: no clamping
      sc.vertex_index_min = 0;
      sc.vertex_index_max = 0xFFFFFF;
    }

    // Guest viewport (untiled, from the device shadow) -> host NDC for a host
    // viewport covering the whole render target, in guest pixels (the target
    // is resolution_scale times larger on the host).
    float vp[6];
    for (int i = 0; i < 6; ++i) {
      const uint32_t bits = REX_LOAD_U32(device_address + kDevVport + 4 * i);
      std::memcpy(&vp[i], &bits, 4);
    }
    // Direct3D 9 pixel centres (at .0): half a pixel added to the offset, like
    // the SDK's GetHostViewportInfo.
    const float half_pixel =
        regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
    const float xs = vte.vport_x_scale_ena ? vp[0] : 1.0f;
    const float xo = (vte.vport_x_offset_ena ? vp[1] : 0.0f) + half_pixel;
    const float ys = vte.vport_y_scale_ena ? vp[2] : 1.0f;
    const float yo = (vte.vport_y_offset_ena ? vp[3] : 0.0f) + half_pixel;
    const float zs = vte.vport_z_scale_ena ? vp[4] : 1.0f, zo = vte.vport_z_offset_ena ? vp[5] : 0.0f;
    const float w = float(target.width) / float(target.resolution_scale);
    const float h = float(target.height) / float(target.resolution_scale);
    sc.ndc_scale[0] = xs * 2.0f / w;
    sc.ndc_offset[0] = xo * 2.0f / w - 1.0f;
    sc.ndc_scale[1] = -ys * 2.0f / h;
    sc.ndc_offset[1] = 1.0f - yo * 2.0f / h;
    sc.ndc_scale[2] = zs;
    sc.ndc_offset[2] = zo;

    float alpha_ref;
    const uint32_t alpha_ref_bits = regs[gfx::XE_GPU_REG_RB_ALPHA_REF];
    std::memcpy(&alpha_ref, &alpha_ref_bits, 4);
    // The alpha test with half an 8-bit step of tolerance: some passes keep
    // only fully opaque texels (alpha >= 1.0, >= 254/255), and a texel of 255
    // filters to exactly 1.0 on AMD (and the Xbox 360) but a hair less on
    // NVIDIA and WARP, which then dropped random pixels (speckled characters
    // and floors). Moving the reference half a step the forgiving way makes
    // alpha compare at the 8 bits it was authored in. Equal / not equal keep it.
    if (REXCVAR_GET(tenchuz_native_alpha_test_tolerance)) {
      constexpr float kHalfStep = 0.5f / 255.0f;
      const uint32_t func = uint32_t(color_control.alpha_func);
      if (func == 6 || func == 1) alpha_ref -= kHalfStep;  // greater or equal, less
      if (func == 4 || func == 3) alpha_ref += kHalfStep;  // greater, less or equal
    }
    sc.alpha_test_reference = alpha_ref;
    // Like the SDK's system constants: alpha to coverage (the shader writes the
    // sample mask) and the host sample count.
    sc.alpha_to_mask = color_control.alpha_to_mask_enable ? (color_control.value >> 24) | (1 << 8) : 0;
    sc.sample_count_log2[0] = target.samples >= 4 ? 1 : 0;
    sc.sample_count_log2[1] = target.samples >= 2 ? 1 : 0;
    // Per render target: 2^exp_bias scale, and gamma targets are plain 8-bit
    // UNORM here, so the shader encodes.
    for (uint32_t i = 0; i < 4; ++i) {
      const reg::RB_COLOR_INFO color_info = DeviceColorInfo(base, device_address, i);
      const int32_t exp_bias = int32_t(color_info.value << 6) >> 26;  // bits 20-25, signed
      sc.color_exp_bias[i] = std::ldexp(1.0f, exp_bias);
      if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        sc.flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }

  // Per fetch constant used: signedness / gamma of each swizzled component.
  void FillTextureSigns(Translator::SystemConstants& sc, const gfx::RegisterFile& regs,
                        const gfx::DxbcShader* shader) {
    if (!shader) {
      return;
    }
    for (const auto& binding : shader->GetTextureBindingsAfterTranslation()) {
      const uint32_t index = binding.fetch_constant;
      const uint32_t shift = 8 * (index & 3);
      uint32_t& word = sc.texture_swizzled_signs[index >> 2];
      word = (word & ~(0xFFu << shift)) |
             (uint32_t(gfx::texture_util::SwizzleSigns(regs.GetTextureFetch(index))) << shift);
    }
  }

  // Sampler descriptor table for a shader, cached by the set of descriptors.
  D3D12_GPU_DESCRIPTOR_HANDLE SamplerTable(const gfx::DxbcShader& shader, const gfx::RegisterFile& regs) {
    const auto& bindings = shader.GetSamplerBindingsAfterTranslation();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = sampler_heap->GetGPUDescriptorHandleForHeapStart();
    std::array<D3D12_SAMPLER_DESC, kSamplerTableSize> descs;
    const uint32_t n = std::min<uint32_t>(uint32_t(bindings.size()), kSamplerTableSize);
    for (uint32_t i = 0; i < n; ++i) {
      descs[i] = SamplerDesc(bindings[i], regs.GetTextureFetch(bindings[i].fetch_constant));
    }
    const uint64_t key = XXH3_64bits(descs.data(), sizeof(D3D12_SAMPLER_DESC) * n) ^ n;
    auto it = sampler_sets.find(key);
    if (it == sampler_sets.end()) {
      if (samplers_used + n > kSamplerHeapSize) {
        return gpu;  // heap full: default linear / wrap table
      }
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = sampler_heap->GetCPUDescriptorHandleForHeapStart();
      cpu.ptr += SIZE_T(samplers_used) * sampler_inc;
      for (uint32_t i = 0; i < n; ++i) {
        device->CreateSampler(&descs[i], cpu);
        cpu.ptr += sampler_inc;
      }
      it = sampler_sets.emplace(key, samplers_used).first;
      samplers_used += n;
    }
    gpu.ptr += UINT64(it->second) * sampler_inc;
    return gpu;
  }

  // --tenchuz_native_log_textures: each distinct fetch once, with how it was
  // bound. Native resolves aren't written to guest memory, so a guest texture
  // overlapping a resolve destination loads stale data there.
  void LogFetch(const xenos::xe_gpu_texture_fetch_t& fetch, const gfx::DxbcShader::TextureBinding& binding,
                DrawHost* host, const char* how) {
    const uint64_t key = XXH3_64bits(&fetch, sizeof(fetch)) ^ (uint64_t(binding.dimension) << 1) ^
                         uint64_t(binding.is_signed);
    if (!logged_fetches.insert(key).second) {
      return;
    }
    const uint32_t address = fetch.base_address << 12;
    const gfx::FormatInfo* info = gfx::FormatInfo::Get(fetch.format);
    const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
    const uint32_t pitch = fetch.pitch ? uint32_t(fetch.pitch) << 5 : width;
    const uint32_t bytes = ((pitch + 31) & ~31u) * ((height + 31) & ~31u) * (info ? info->bits_per_pixel : 32) / 8;
    std::string overlap = host ? host->ResolveOverlapping(address, std::max(bytes, 1u)) : std::string();
    if (fetch.mip_address && host && overlap.empty()) {
      overlap = host->ResolveOverlapping(fetch.mip_address << 12, std::max(bytes / 2, 1u));
      if (!overlap.empty()) {
        overlap = "mips " + overlap;
      }
    }
    REXLOG_INFO("tenchuz: NATIVE fetch {:08X} {}x{} fmt {} dim {} (binding dim {}{}) tiled {} pitch {} mips {}-{} "
                "mip {:08X} swizzle {:03X} -> {}{}{}",
                address, width, height, int(fetch.format), int(fetch.dimension), int(binding.dimension),
                binding.is_signed ? " signed" : "", uint32_t(fetch.tiled), pitch, uint32_t(fetch.mip_min_level),
                uint32_t(fetch.mip_max_level), fetch.mip_address << 12, uint32_t(fetch.swizzle), how,
                overlap.empty() ? "" : " OVERLAPS RESOLVE ", overlap);
  }

  // Writes an SRV for one shader texture binding at `cpu`: a resolved render
  // target, else a loaded guest texture. False if neither fits the binding,
  // with the reason. Sets the fetch constant's bit in `resolution_scaled` (the
  // textures_resolution_scaled system constant) for a resolution-scaled one.
  bool BindTexture(const gfx::DxbcShader::TextureBinding& binding, const gfx::RegisterFile& regs,
                   ID3D12GraphicsCommandList* list, DrawHost* textures, uint8_t* base,
                   D3D12_CPU_DESCRIPTOR_HANDLE cpu, PlaceholderReason& reason, uint32_t& resolution_scaled) {
    const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kTexture &&
        fetch.type != xenos::FetchConstantType::kInvalidTexture) {
      reason = kPlaceholderNotTexture;
      return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    if (textures && !binding.is_signed && binding.dimension == xenos::FetchOpDimension::k2D) {
      DrawHost::Texture texture;
      if (textures->Find(list, fetch.base_address << 12, fetch.size_2d.width + 1, fetch.size_2d.height + 1,
                         texture)) {
        view.Format = texture.format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Shader4ComponentMapping = GuestToHostSwizzle(fetch.swizzle, texture.host_format_swizzle) |
                                       D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.ArraySize = 1;
        CreateTextureView(texture.resource, view, cpu);
        if (texture.resolution_scaled) {
          resolution_scaled |= 1u << binding.fetch_constant;
        }
        ++stats.textures_resolved;
        if (draw_texture_count < 6) draw_textures[draw_texture_count++] = (fetch.base_address << 12) | 1;
        if (REXCVAR_GET(tenchuz_native_log_textures)) {
          LogFetch(fetch, binding, textures, "resolved");
        }
        return true;
      }
    }
    if (!loader_ready) {
      reason = kPlaceholderLoadFailed;
      return false;
    }
    TextureLoader::Texture texture;
    switch (loader.Get(list, fetch, base,
                       [this, base](uint32_t physical, uint32_t size) { Upload(base, physical, size); }, texture)) {
      case TextureLoader::Result::kLoaded:
        break;
      case TextureLoader::Result::kNoAddress:
        reason = kPlaceholderNoAddress;
        return false;
      case TextureLoader::Result::kUnsupported:
        reason = kPlaceholderUnsupported;
        return false;
      default:
        reason = kPlaceholderLoadFailed;
        return false;
    }
    view.Format = binding.is_signed ? texture.format_signed : texture.format_unsigned;
    if (view.Format == DXGI_FORMAT_UNKNOWN) {
      reason = binding.is_signed ? kPlaceholderSignedView : kPlaceholderUnsupported;
      return false;
    }
    view.Shader4ComponentMapping = GuestToHostSwizzle(fetch.swizzle, texture.host_format_swizzle) |
                                   D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    const bool is_3d = texture.dimension == xenos::DataDimension::k3D;
    const bool is_cube = texture.dimension == xenos::DataDimension::kCube;
    switch (binding.dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        if (is_3d) {
          reason = kPlaceholderDimension;
          return false;
        }
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = texture.mip_levels;
        view.Texture2DArray.ArraySize = texture.array_size;
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        if (!is_3d) {
          reason = kPlaceholderStacked3D;  // the SDK's 3D-as-2D wrapper isn't ported
          return false;
        }
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipLevels = texture.mip_levels;
        break;
      case xenos::FetchOpDimension::kCube:
        if (!is_cube) {
          reason = kPlaceholderDimension;
          return false;
        }
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.TextureCube.MipLevels = texture.mip_levels;
        break;
    }
    CreateTextureView(texture.resource, view, cpu);
    ++stats.textures_guest;
    if (draw_texture_count < 6) draw_textures[draw_texture_count++] = fetch.base_address << 12;
    if (REXCVAR_GET(tenchuz_native_log_textures)) {
      LogFetch(fetch, binding, textures, "guest");
    }
    return true;
  }

  // Copies guest physical [physical, physical + size) into this frame's arena.
  // Returns the copy's offset in the shared buffer, or 0 if the arena is full.
  uint64_t CopyToArena(uint8_t* base, uint32_t physical, uint32_t size) {
    Frame& f = frames[slot];
    // Same address modulo 16 as the guest data: raw views start on 16-byte
    // boundaries (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT).
    const uint32_t offset = ((f.arena_used + 15) & ~15u) + (physical & 15);
    if (!size || size > kArenaMaxSpan || offset + size > kArenaSize) {
      return 0;
    }
    f.arena_used = offset + size;
    const uint64_t buffer_offset = kSharedMemorySize + uint64_t(slot) * kArenaSize + offset;
    std::memcpy(shared_map + buffer_offset, REX_RAW_ADDR(0xA0000000u | physical), size);
    NoteCopy(base, physical, size, 'A');
    stats.arena_kb += (size + 1023) >> 10;
    return buffer_offset;
  }

  // Shared-memory table (SRV + null UAV) through which guest physical
  // `physical` reads the arena copy at `buffer_offset`: the raw view starts
  // (buffer_offset - physical) bytes into the buffer.
  bool ArenaTable(uint64_t buffer_offset, uint32_t physical, uint32_t end, D3D12_GPU_DESCRIPTOR_HANDLE& out) {
    Frame& f = frames[slot];
    if (f.views_used + 2 > kViewRingSize) {
      return false;
    }
    const uint32_t first = 2 + slot * kViewRingSize + f.views_used;
    f.views_used += 2;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(first) * view_inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = (buffer_offset - physical) / 4;
    srv.Buffer.NumElements = UINT((uint64_t(end) + 3) / 4);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(shared.Get(), &srv, cpu);
    cpu.ptr += view_inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = UINT(kSharedMemorySize / 4);
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(nullptr, nullptr, &uav, cpu);
    out = view_heap->GetGPUDescriptorHandleForHeapStart();
    out.ptr += UINT64(first) * view_inc;
    return true;
  }

  // Descriptor table for a shader's texture bindings: native textures where
  // there are any, else placeholders of the binding's dimension.
  D3D12_GPU_DESCRIPTOR_HANDLE TextureTable(const gfx::DxbcShader& shader, const gfx::RegisterFile& regs,
                                           ID3D12GraphicsCommandList* list, DrawHost* textures,
                                           uint8_t* base, bool& ok, uint32_t& resolution_scaled) {
    const auto& bindings = shader.GetTextureBindingsAfterTranslation();
    Frame& f = frames[slot];
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
    if (bindings.empty()) {
      return gpu;
    }
    if (f.views_used + bindings.size() > kViewRingSize) {
      ok = false;
      return gpu;
    }
    const uint32_t first = 2 + slot * kViewRingSize + f.views_used;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = view_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(first) * view_inc;
    gpu = view_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(first) * view_inc;
    for (const auto& binding : bindings) {
      // The signed binding of a fetch is only sampled for its signed
      // components: with none, it gets the placeholder without a texture
      // lookup or load (like the SDK, which loads signed views only then).
      if (binding.is_signed &&
          !gfx::texture_util::IsAnySignSigned(gfx::texture_util::SwizzleSigns(regs.GetTextureFetch(binding.fetch_constant)))) {
        ++stats.signed_unused;
      } else {
        PlaceholderReason reason = kPlaceholderLoadFailed;
        if (BindTexture(binding, regs, list, textures, base, cpu, reason, resolution_scaled)) {
          cpu.ptr += view_inc;
          continue;
        }
        ++stats.textures_missing;
        ++stats.placeholders[reason];
      }
      uint32_t dummy = 0;
      if (binding.dimension == xenos::FetchOpDimension::k3DOrStacked) dummy = 1;
      if (binding.dimension == xenos::FetchOpDimension::kCube) dummy = 2;
      D3D12_CPU_DESCRIPTOR_HANDLE src = staging_heap->GetCPUDescriptorHandleForHeapStart();
      src.ptr += SIZE_T(dummy) * view_inc;
      device->CopyDescriptorsSimple(1, cpu, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      cpu.ptr += view_inc;
    }
    f.views_used += uint32_t(bindings.size());
    return gpu;
  }
};

DrawPath::DrawPath() : impl_(std::make_unique<Impl>()) {}
DrawPath::~DrawPath() {
  impl_->SavePipelineLibrary();
}

bool DrawPath::Initialize(ID3D12Device* device, rex::memory::Memory* memory,
                          const std::filesystem::path& pipeline_cache) {
  if (!impl_->Initialize(device, memory)) {
    return false;
  }
  impl_->OpenPipelineLibrary(pipeline_cache);
  return true;
}

uint32_t DrawPath::WrittenColorMask(const pm4::Decoder& state, uint8_t* base, uint32_t device_address,
                                    const DrawShaders& shaders) {
  const auto& regs = *reinterpret_cast<const gfx::RegisterFile*>(state.regs);
  if (!shaders.pixel || regs.Get<reg::RB_MODECONTROL>().edram_mode != xenos::EdramMode::kColorDepth) {
    return 0;
  }
  const uint32_t writes = shaders.pixel->shader().writes_color_targets();
  const uint32_t rb_color_mask = regs[gfx::XE_GPU_REG_RB_COLOR_MASK];
  uint32_t mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(writes & (1u << i))) {
      continue;
    }
    const uint32_t components =
        (1u << xenos::GetColorRenderTargetFormatComponentCount(DeviceColorInfo(base, device_address, i).color_format)) -
        1;
    const uint32_t rt_mask = (rb_color_mask >> (4 * i)) & components;
    if (rt_mask) {
      // Components the format lacks count as written (no read-merge path).
      mask |= (rt_mask | (0xF & ~components)) << (4 * i);
    }
  }
  return mask;
}

void DrawPath::BeginFrame(uint32_t slot) {
  impl_->slot = slot % kFrames;
  impl_->frames[impl_->slot].retired.clear();
  if (impl_->pipelines_stored &&
      std::chrono::steady_clock::now() - impl_->pipeline_library_saved > std::chrono::seconds(30)) {
    impl_->SavePipelineLibrary();
  }
  if (impl_->profiling[impl_->slot]) {
    impl_->LogProfile(impl_->slot);  // the slot's GPU work has finished
    impl_->profiling[impl_->slot] = false;
  }
  impl_->profile_resolved[impl_->slot] = false;
  impl_->timestamps_used[impl_->slot] = 0;
  if (impl_->timestamp_heap && impl_->profile_requested.exchange(false)) {
    impl_->profiling[impl_->slot] = true;
  }
  if (impl_->log_pending[impl_->slot]) {
    impl_->log_pending[impl_->slot] = false;
    impl_->LogDraws(impl_->slot);  // the slot's GPU work has finished: query results are in
  }
  if (const int32_t threshold = REXCVAR_GET(tenchuz_native_log_coverage);
      threshold > 0 && impl_->query_map && impl_->queries_used[impl_->slot]) {
    std::lock_guard lock(impl_->records_mutex);
    std::string big;
    const auto& records = impl_->records[impl_->slot];
    for (uint32_t i = 0; i < records.size(); ++i) {
      if (records[i].query && impl_->query_map[records[i].query - 1] > uint64_t(threshold)) {
        big += fmt::format(" #{}:{}", i, impl_->query_map[records[i].query - 1]);
      }
    }
    REXLOG_INFO("tenchuz: NATIVE coverage frame {}:{}", impl_->query_frame[impl_->slot], big);
  }
  impl_->queries_used[impl_->slot] = 0;
  impl_->copy_checks.clear();
  {
    const std::string trace = REXCVAR_GET(tenchuz_native_trace_vs);
    impl_->trace_vs = trace.empty() ? 0 : std::strtoull(trace.c_str(), nullptr, 16);
  }
  impl_->check_late_writes = REXCVAR_GET(tenchuz_native_check_late_writes);
  impl_->log_offscreen = REXCVAR_GET(tenchuz_native_log_offscreen);
  {
    std::lock_guard lock(impl_->records_mutex);
    impl_->records[impl_->slot].clear();
    impl_->record_frames[impl_->slot] = ++impl_->frame_counter;
    if (impl_->breadcrumbs_map) {
      impl_->breadcrumbs_map[2 * impl_->slot] = 0;
      impl_->breadcrumbs_map[2 * impl_->slot + 1] = 0;
    }
  }
  impl_->frames[impl_->slot].constants_used = 0;
  impl_->frames[impl_->slot].views_used = 0;
  impl_->frames[impl_->slot].arena_used = 0;
  std::fill(impl_->uploaded_pages.begin(), impl_->uploaded_pages.end(), 0);
  impl_->page_hashes.clear();
  impl_->static_page_hashes.clear();
  impl_->stale_logged = 0;
  if (impl_->loader_ready) {
    impl_->loader.BeginFrame(slot);
  }
}

void DrawPath::LogFrameDraws(uint64_t frame) {
  impl_->log_pending[impl_->slot] = true;
  impl_->log_frame[impl_->slot] = frame;
}

void DrawPath::EndFrame(ID3D12GraphicsCommandList* list, uint64_t frame) {
  Impl& m = *impl_;
  m.trace_frame = frame + 1;
  m.query_frame[m.slot] = frame;
  if (m.check_late_writes && !m.copy_checks.empty() && m.guest_base) {
    uint8_t* base = m.guest_base;
    uint32_t changed = 0, changed_kb = 0;
    for (const Impl::CopyCheck& c : m.copy_checks) {
      if (XXH3_64bits(REX_RAW_ADDR(0xA0000000u | c.address), c.size) == c.hash) continue;
      if (changed++ < 12) {
        REXLOG_INFO("tenchuz: NATIVE frame {} late write: draw #{} {} {:08X}+{:X} changed after it was copied",
                    frame, c.record, c.kind == 'A' ? "arena" : "static", c.address, c.size);
      }
      changed_kb += (c.size + 1023) >> 10;
    }
    if (changed) {
      REXLOG_INFO("tenchuz: NATIVE frame {}: {} of {} copied ranges ({} KB) changed after they were copied", frame,
                  changed, m.copy_checks.size(), changed_kb);
    }
  }
  if (m.query_heap && m.queries_used[m.slot]) {
    const uint32_t first = m.slot * Impl::kQueriesPerFrame;
    list->ResolveQueryData(m.query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, first, m.queries_used[m.slot],
                           m.query_readback.Get(), uint64_t(first) * 8);
  }
}

void DrawPath::SetTimestampFrequency(uint64_t ticks_per_second) { impl_->timestamp_frequency = ticks_per_second; }

void DrawPath::RequestProfile() { impl_->profile_requested = true; }

bool DrawPath::TakeStencilMissing() { return std::exchange(impl_->stencil_missing, false); }

void DrawPath::ResolveProfile(ID3D12GraphicsCommandList* list) {
  Impl& m = *impl_;
  if (!m.profiling[m.slot] || !m.timestamps_used[m.slot]) return;
  const uint32_t first = m.slot * Impl::kTimestampsPerFrame;
  list->ResolveQueryData(m.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, m.timestamps_used[m.slot],
                         m.timestamp_readback.Get(), uint64_t(first) * 8);
  m.profile_resolved[m.slot] = true;
}

void DrawPath::ReportHang() {
  Impl& m = *impl_;
  if (!m.breadcrumbs_map || m.hang_reported.exchange(true)) {
    return;
  }
  std::lock_guard lock(m.records_mutex);
  for (uint32_t s = 0; s < kFrames; ++s) {
    const uint32_t started = m.breadcrumbs_map[2 * s], completed = m.breadcrumbs_map[2 * s + 1];
    const auto& records = m.records[s];
    REXLOG_ERROR("tenchuz: N4 GPU hang report: frame {} (slot {}): {} draws recorded, GPU started {}, completed {}",
                 m.record_frames[s], s, records.size(), started, completed);
    if (completed >= records.size()) {
      continue;
    }
    const uint32_t last = std::min<uint32_t>(std::max(started, completed + 1), uint32_t(records.size()));
    for (uint32_t i = completed; i < last && i < completed + 8; ++i) {
      const Impl::DrawRecord& r = records[i];
      if (r.op) {
        REXLOG_ERROR("tenchuz: N4   unfinished #{}: {} {:08X} {:08X}", i, r.op, r.prim, r.count);
        continue;
      }
      REXLOG_ERROR("tenchuz: N4   unfinished draw #{}: vs {:016X} ps {:016X} prim {} count {} {} "
                   "ib {:08X}+{:X} vb {:08X}+{:X} stride {}",
                   i, r.vs, r.ps, r.prim, r.count, r.indexed ? "indexed" : "auto", r.ib_base, r.ib_size,
                   r.vb_base, r.vb_size, r.vb_stride);
    }
  }
  if (auto logger = rex::GetLogger()) {
    logger->flush();
  }
}

uint32_t DrawPath::BeginOp(ID3D12GraphicsCommandList* list, const char* what, uint32_t a, uint32_t b) {
  Impl::DrawRecord record = {};
  record.op = what;
  record.prim = a;
  record.count = b;
  return impl_->MarkStart(list, record);
}

void DrawPath::EndOp(uint32_t op) {
  impl_->MarkEnd(op);
}

DrawStats DrawPath::TakeFrameStats() {
  DrawStats s = impl_->stats;
  impl_->stats = {};
  if (impl_->loader_ready) {
    const TextureLoader::Stats t = impl_->loader.TakeFrameStats();
    s.texture_loads = t.loads;
    s.texture_loaded_kb = t.loaded_kb;
    s.texture_hashed_kb = t.hashed_kb;
    s.texture_unsupported = t.unsupported;
    s.textures_cached = t.textures;
    s.texture_written_pages = t.written_pages;
  }
  return s;
}

bool DrawPath::Draw(ID3D12GraphicsCommandList* list, const pm4::DrawInfo& draw,
                    const pm4::Decoder& state, uint8_t* base, uint32_t device_address,
                    const DrawShaders& shaders, const DrawTarget& target, DrawHost* host) {
  DrawHost* textures = host;
  Impl& m = *impl_;
  m.frame_host = host;
  const auto& regs = *reinterpret_cast<const gfx::RegisterFile*>(state.regs);
  if (!shaders.vertex || !target.width || !target.height) {
    ++m.stats.skipped_other;
    return false;
  }

  // Draw packet: DRAW_INDX = viz query, initiator, DMA base, DMA size;
  // DRAW_INDX_2 = initiator (+ immediate data).
  const bool dma_packet = draw.opcode == 0x22 || draw.opcode == 0x34;
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = dma_packet ? draw.words[1] : draw.words[0];
  const xenos::PrimitiveType prim = initiator.prim_type;
  D3D12_PRIMITIVE_TOPOLOGY topology;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type;
  // Fans and quad lists have no host topology: drawn as triangle lists with
  // generated indices.
  enum class Expand { kNone, kFan, kQuads } expand = Expand::kNone;
  // Rectangle lists (3 vertices each, the 4th derived) go through the SDK's
  // geometry shader; the game draws its full-screen passes with them.
  bool rectangle_list = false;
  switch (prim) {
    case xenos::PrimitiveType::kPointList:
      topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      break;
    case xenos::PrimitiveType::kLineList:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;
    case xenos::PrimitiveType::kTriangleList:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;
    case xenos::PrimitiveType::kRectangleList:
      rectangle_list = true;
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kQuadList:
      expand = prim == xenos::PrimitiveType::kQuadList ? Expand::kQuads : Expand::kFan;
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      break;
    default:
      ++m.stats.skipped_primitive;  // rectangle lists and the rest: N4.4
      m.stats.skipped_primitive_types |= 1u << (uint32_t(prim) & 31);
      return false;
  }
  if (topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) {
    const auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (mode.cull_front && mode.cull_back) {
      ++m.stats.skipped_culled;  // nothing rasterized (memexport isn't supported)
      return false;
    }
  }
  const uint32_t count = initiator.num_indices;
  const bool indexed = initiator.source_select == xenos::SourceSelect::kDMA;
  if (!count || (indexed && !dma_packet) ||
      initiator.source_select == xenos::SourceSelect::kImmediate) {
    ++m.stats.skipped_other;
    return false;
  }

  // Geometry. Static buffers go through the shared memory mirror. Vertices in
  // the D3D's command ring (BeginVertices/EndVertices, 2D and full-screen
  // passes) are rewritten within a frame as the ring wraps, so they are copied
  // per draw into this frame's arena and read through a view that maps their
  // guest addresses onto the copy.
  const auto& vs_shader = static_cast<const gfx::DxbcShader&>(shaders.vertex->shader());
  D3D12_GPU_DESCRIPTOR_HANDLE shared_table = m.view_heap->GetGPUDescriptorHandleForHeapStart();
  bool vertices_in_arena = false;
  {
    uint32_t low = UINT32_MAX, high = 0;
    bool any = false, all_dynamic = host != nullptr;
    for (const auto& binding : vs_shader.vertex_bindings()) {
      const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
      if (fetch.type != xenos::FetchConstantType::kVertex &&
          fetch.type != xenos::FetchConstantType::kInvalidVertex) {
        continue;
      }
      const uint32_t address = (fetch.address << 2) & 0x1FFFFFFFu;
      any = true;
      all_dynamic = all_dynamic && host->InCommandRing(address);
      low = std::min(low, address);
      high = std::max(high, address + (fetch.size << 2));
    }
    if (any && all_dynamic && high > low && high - low <= kArenaMaxSpan) {
      const uint64_t offset = m.CopyToArena(base, low, high - low);
      vertices_in_arena = offset && m.ArenaTable(offset, low, high, shared_table);
    }
  }
  Impl::DrawRecord record = {};
  record.vs = vs_shader.ucode_data_hash();
  record.packet = draw.address;
  record.color_info = target.color_info;
  record.depth_info = target.depth_info;
  record.color_surface = target.color_surface;
  record.depth_surface = target.depth_surface;
  record.ps = shaders.pixel ? shaders.pixel->shader().ucode_data_hash() : 0;
  record.prim = uint32_t(prim);
  record.count = count;
  record.indexed = indexed;
  record.arena = vertices_in_arena ? 1 : 0;
  for (const auto& binding : vs_shader.vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex &&
        fetch.type != xenos::FetchConstantType::kInvalidVertex) {
      continue;  // not a vertex buffer: nothing sensible to copy
    }
    if (!vertices_in_arena) {
      m.upload_context = fetch.address << 2;
      m.upload_context_size = fetch.size << 2;
      m.Upload(base, fetch.address << 2, fetch.size << 2, host);
    }
    if (!record.vb_size) {
      record.vb_base = fetch.address << 2;
      record.vb_size = fetch.size << 2;
      record.vb_stride = binding.stride_words * 4;
    }
  }
  xenos::Endian index_endian = xenos::Endian::kNone;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  D3D12_INDEX_BUFFER_VIEW ibv = {};
  const bool index32 = !indexed || initiator.index_size == xenos::IndexFormat::kInt32;
  const uint32_t index_bytes = index32 ? 4 : 2;
  uint32_t ib_base = 0;
  if (indexed) {
    const uint32_t dma_base = draw.words[2];
    const uint32_t dma_size = draw.words[3];
    ib_base = dma_base & ~(index_bytes - 1);
    const uint32_t ib_size = (dma_size & 0xFFFFFF) * index_bytes;
    index_endian = xenos::Endian(dma_size >> 30);
    uint64_t ib_arena = 0;
    if (host && host->InCommandRing(ib_base & 0x1FFFFFFFu)) {
      ib_arena = m.CopyToArena(base, ib_base & 0x1FFFFFFFu, ib_size);  // dynamic indices
    }
    if (!ib_arena) {
      m.upload_context = ib_base;
      m.upload_context_size = ib_size;
      m.Upload(base, ib_base, ib_size, host);
    }
    record.ib_base = ib_base;
    record.ib_size = ib_size;
    record.arena |= ib_arena ? 2 : 0;
    ibv.BufferLocation = m.shared_gpu + (ib_arena ? ib_arena : (ib_base & 0x1FFFFFFFu));
    ibv.SizeInBytes = ib_size;
    ibv.Format = index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    // Primitive reset. The guest's reset index is VGT_MULTI_PRIM_IB_RESET_INDX
    // (low 24 bits, compared after the endian swap); the host only cuts strips
    // at all-ones (the same bytes in either order). The water in the Ninja
    // Village resets 32-bit strips with 0xFFFF: other values are rewritten to
    // all-ones in a copy of the indices (like the SDK's primitive processor).
    if ((prim == xenos::PrimitiveType::kTriangleStrip || prim == xenos::PrimitiveType::kLineStrip) &&
        regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) {
      const uint32_t reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
      if (index32 || reset <= 0xFFFF) {
        strip_cut = index32 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                            : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
      }
      if (strip_cut != D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED && reset != (index32 ? 0xFFFFFFu : 0xFFFFu)) {
        D3D12_GPU_VIRTUAL_ADDRESS converted_gpu;
        auto* out = count ? static_cast<uint8_t*>(m.AllocConstants(count * index_bytes, converted_gpu)) : nullptr;
        if (!out) {
          ++m.stats.skipped_other;
          return false;
        }
        const uint8_t* guest = REX_RAW_ADDR(0xA0000000u | ib_base);
        for (uint32_t i = 0; i < count; ++i, guest += index_bytes, out += index_bytes) {
          uint32_t raw;
          if (index32) {
            std::memcpy(&raw, guest, 4);
            if ((xenos::GpuSwap(raw, index_endian) & 0xFFFFFF) == reset) {
              raw = 0xFFFFFFFFu;
            }
            std::memcpy(out, &raw, 4);
          } else {
            uint16_t raw16;
            std::memcpy(&raw16, guest, 2);
            if (xenos::GpuSwap(raw16, index_endian) == reset) {
              raw16 = 0xFFFF;
            }
            std::memcpy(out, &raw16, 2);
          }
        }
        ibv.BufferLocation = converted_gpu;
        ibv.SizeInBytes = count * index_bytes;
        ++m.stats.reset_converted;
      }
    }
  }
  if (m.trace_vs && vs_shader.ucode_data_hash() == m.trace_vs) {
    m.TraceDraw(base, vs_shader, regs, record.vb_base, record.vb_size, record.vb_stride, count, indexed, index32,
                ib_base, index_endian);
  }
  uint32_t draw_count = count;
  bool use_index_buffer = indexed;
  if (expand != Expand::kNone) {
    // Triangle-list indices in this frame's upload ring. Guest indices keep
    // their byte order (the VS swaps them); generated ones are host order.
    const uint32_t triangles = expand == Expand::kQuads ? count / 4 * 2 : (count >= 3 ? count - 2 : 0);
    draw_count = triangles * 3;
    D3D12_GPU_VIRTUAL_ADDRESS indices_gpu;
    auto* out = draw_count ? static_cast<uint8_t*>(m.AllocConstants(draw_count * index_bytes, indices_gpu))
                           : nullptr;
    if (!out) {
      ++m.stats.skipped_other;
      return false;
    }
    const uint8_t* guest = indexed ? REX_RAW_ADDR(0xA0000000u | ib_base) : nullptr;
    auto emit = [&](uint32_t i) {
      if (guest) {
        std::memcpy(out, guest + size_t(i) * index_bytes, index_bytes);
      } else {
        std::memcpy(out, &i, 4);
      }
      out += index_bytes;
    };
    if (expand == Expand::kQuads) {
      for (uint32_t q = 0; q < count / 4; ++q) {
        const uint32_t v = 4 * q;
        emit(v);
        emit(v + 1);
        emit(v + 2);
        emit(v);
        emit(v + 2);
        emit(v + 3);
      }
    } else {
      for (uint32_t t = 1; t + 1 < count; ++t) {
        emit(0);
        emit(t);
        emit(t + 1);
      }
    }
    ibv.BufferLocation = indices_gpu;
    ibv.SizeInBytes = draw_count * index_bytes;
    ibv.Format = index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    use_index_buffer = true;
  }

  // Pipeline.
  const auto* ps_shader =
      shaders.pixel ? &static_cast<const gfx::DxbcShader&>(shaders.pixel->shader()) : nullptr;
  const uint32_t vs_textures = uint32_t(vs_shader.GetTextureBindingsAfterTranslation().size());
  const uint32_t vs_samplers = uint32_t(vs_shader.GetSamplerBindingsAfterTranslation().size());
  const uint32_t ps_textures = ps_shader ? uint32_t(ps_shader->GetTextureBindingsAfterTranslation().size()) : 0;
  const uint32_t ps_samplers = ps_shader ? uint32_t(ps_shader->GetSamplerBindingsAfterTranslation().size()) : 0;
  if (vs_samplers > kSamplerTableSize || ps_samplers > kSamplerTableSize) {
    ++m.stats.skipped_other;
    return false;
  }
  ID3D12RootSignature* root = m.RootSignature(vs_textures, vs_samplers, ps_textures, ps_samplers);
  gfx::vendored::GeometryShaderKey gs_key;
  if (rectangle_list) {
    // SDK PipelineCache::GetGeometryShaderKey.
    const Translator::Modification vs_mod(shaders.vertex->modification());
    const Translator::Modification ps_mod(shaders.pixel ? shaders.pixel->modification() : 0);
    gs_key.type = gfx::vendored::PipelineGeometryShader::kRectangleList;
    gs_key.interpolator_count = rex::bit_count(vs_mod.vertex.interpolator_mask);
    gs_key.user_clip_plane_count = vs_mod.vertex.user_clip_plane_count;
    gs_key.user_clip_plane_cull = vs_mod.vertex.user_clip_plane_cull;
    gs_key.has_vertex_kill_and = vs_mod.vertex.vertex_kill_and;
    gs_key.has_point_size = vs_mod.vertex.output_point_size;
    gs_key.has_point_coordinates = shaders.pixel ? ps_mod.pixel.param_gen_point : 0;
    gs_key.point_ps_ucp_mode = vs_mod.vertex.point_ps_ucp_mode;
  }
  ID3D12PipelineState* pso =
      root ? m.Pipeline(root, shaders, regs, target, topology_type, strip_cut, gs_key) : nullptr;
  if (!pso) {
    ++m.stats.skipped_pipeline;
    return false;
  }

  // Constants.
  D3D12_GPU_VIRTUAL_ADDRESS fetch_gpu, vs_float_gpu, ps_float_gpu, system_gpu, bool_loop_gpu;
  auto* fetch = static_cast<uint8_t*>(m.AllocConstants(32 * 6 * 4, fetch_gpu));
  auto* bool_loop = static_cast<uint8_t*>(m.AllocConstants(40 * 4, bool_loop_gpu));
  const uint8_t* vs_float_cpu = nullptr;  // the packed VS constants (draw list hash)
  auto pack_floats = [&](const gfx::Shader* shader, uint32_t first_reg, D3D12_GPU_VIRTUAL_ADDRESS& gpu) {
    const uint32_t n = shader ? shader->constant_register_map().float_count : 0;
    auto* out = static_cast<uint8_t*>(m.AllocConstants(16 * std::max(n, 1u), gpu));
    if (shader == &vs_shader) {
      vs_float_cpu = out;
    }
    if (!out || !shader) return out != nullptr;
    const auto& map = shader->constant_register_map();
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = map.float_bitmap[i];
      uint32_t index;
      while (rex::bit_scan_forward(bits, &index)) {
        bits &= ~(1ull << index);
        std::memcpy(out, &regs.values[first_reg + (i << 8) + (index << 2)], 16);
        out += 16;
      }
    }
    return true;
  };
  // The system constants are built on the stack and copied to the constant
  // ring once (before the CBV is bound): the ring is write-combined upload
  // memory, and the read-modify-writes below cost ~10% of the main thread.
  auto* system_mapped = static_cast<Translator::SystemConstants*>(
      m.AllocConstants(sizeof(Translator::SystemConstants), system_gpu));
  Translator::SystemConstants system_cpu;
  Translator::SystemConstants* const system = &system_cpu;
  if (!fetch || !bool_loop || !system_mapped ||
      !pack_floats(&vs_shader, gfx::XE_GPU_REG_SHADER_CONSTANT_000_X, vs_float_gpu) ||
      !pack_floats(ps_shader, gfx::XE_GPU_REG_SHADER_CONSTANT_256_X, ps_float_gpu)) {
    ++m.stats.skipped_other;
    return false;
  }
  std::memcpy(fetch, &regs.values[gfx::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 32 * 6 * 4);
  std::memcpy(bool_loop, &regs.values[gfx::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 40 * 4);
  m.FillSystemConstants(*system, regs, base, device_address, prim, indexed, index_endian, target);
  m.FillTextureSigns(*system, regs, &vs_shader);
  m.FillTextureSigns(*system, regs, ps_shader);

  bool tables_ok = true;
  uint32_t resolution_scaled = 0;
  const uint32_t guest_textures_before = m.stats.textures_guest;
  const uint32_t resolved_textures_before = m.stats.textures_resolved;
  m.draw_texture_count = 0;
  const D3D12_GPU_DESCRIPTOR_HANDLE ps_table =
      ps_shader ? m.TextureTable(*ps_shader, regs, list, textures, base, tables_ok, resolution_scaled)
                : D3D12_GPU_DESCRIPTOR_HANDLE{};
  const D3D12_GPU_DESCRIPTOR_HANDLE vs_table =
      m.TextureTable(vs_shader, regs, list, textures, base, tables_ok, resolution_scaled);
  if (!tables_ok) {
    ++m.stats.skipped_other;
    return false;
  }
  system->textures_resolution_scaled = resolution_scaled;
  record.target_width = target.width;
  record.target_height = target.height;
  record.samples = target.samples;
  record.color_format = uint32_t(target.color_formats[0]);
  record.guest_textures = m.stats.textures_guest - guest_textures_before;
  record.resolved_textures = m.stats.textures_resolved - resolved_textures_before;
  for (uint32_t i = 0; i < 6; ++i) {
    record.textures[i] = i < m.draw_texture_count ? m.draw_textures[i] : 0;
  }
  record.depth_test = target.depth_format != DXGI_FORMAT_UNKNOWN && NormalizedDepthControl(regs).z_enable;
  m.stats.depth_draws += record.depth_test;
  // Widescreen: 2D draws on the final target keep 16:9 proportions in the
  // middle of the wider image: the HUD, menus and movies. Their vertices are
  // already in screen pixels (a vertex shader without float constants), they
  // have no depth test, and they sample only game textures. 3D draws (which
  // need a matrix), full-screen passes over resolved images and untextured
  // fades keep the whole width.
  if (target.hud_scale_x != 1.0f && !record.depth_test && record.guest_textures && !record.resolved_textures &&
      !vs_shader.constant_register_map().float_count) {
    system->ndc_scale[0] *= target.hud_scale_x;
    system->ndc_offset[0] *= target.hud_scale_x;
    record.hud = true;
    ++m.stats.hud_draws;
  }
  record.ndc_scale[0] = system->ndc_scale[0];
  record.ndc_scale[1] = system->ndc_scale[1];
  record.ndc_offset[0] = system->ndc_offset[0];
  record.ndc_offset[1] = system->ndc_offset[1];
  if (const uint32_t n = vs_shader.constant_register_map().float_count) {
    // The packed VS constants were written just above (pack_floats).
    record.vs_constants = vs_float_cpu;
    record.vs_constants_size = 16 * n;
  }
  if (m.log_offscreen && !vs_shader.constant_register_map().float_count && record.vb_size &&
      record.vb_stride >= 8) {
    m.LogOffscreen(base, record);
  }

  // Record.
  if (m.debug_upload) {
    D3D12_TEXTURE_COPY_LOCATION to = {m.dummy_textures[0].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION from = {m.debug_upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, Impl::kDebugSize, Impl::kDebugSize, 1,
                                      Impl::kDebugSize * 4};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m.dummy_textures[0].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &barrier);
    m.retired_upload = std::move(m.debug_upload);  // kept alive until the GPU has copied it
  }
  ID3D12DescriptorHeap* heaps[] = {m.view_heap.Get(), m.sampler_heap.Get()};
  list->SetDescriptorHeaps(2, heaps);
  list->SetGraphicsRootSignature(root);
  list->SetPipelineState(pso);
  list->SetGraphicsRootConstantBufferView(kRootFetchConstants, fetch_gpu);
  list->SetGraphicsRootConstantBufferView(kRootFloatConstantsVertex, vs_float_gpu);
  list->SetGraphicsRootConstantBufferView(kRootFloatConstantsPixel, ps_float_gpu);
  std::memcpy(system_mapped, system, sizeof(*system));
  list->SetGraphicsRootConstantBufferView(kRootSystemConstants, system_gpu);
  list->SetGraphicsRootConstantBufferView(kRootBoolLoopConstants, bool_loop_gpu);
  list->SetGraphicsRootDescriptorTable(kRootSharedMemory, shared_table);
  uint32_t param = kRootBaseCount;
  if (ps_textures) list->SetGraphicsRootDescriptorTable(param++, ps_table);
  if (ps_samplers) list->SetGraphicsRootDescriptorTable(param++, m.SamplerTable(*ps_shader, regs));
  if (vs_textures) list->SetGraphicsRootDescriptorTable(param++, vs_table);
  if (vs_samplers) list->SetGraphicsRootDescriptorTable(param++, m.SamplerTable(vs_shader, regs));

  uint32_t rt_count = 0, rt_bound = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (target.color_formats[i] != DXGI_FORMAT_UNKNOWN) {
      rt_count = i + 1;
      ++rt_bound;
    }
  }
  m.stats.mrt_draws += rt_bound > 1;
  list->OMSetRenderTargets(rt_count, rt_count ? target.rtvs : nullptr, FALSE,
                           target.depth_format != DXGI_FORMAT_UNKNOWN ? &target.dsv : nullptr);
  if (target.depth_format != DXGI_FORMAT_UNKNOWN) {
    const reg::RB_DEPTHCONTROL depth = NormalizedDepthControl(regs);
    if (depth.stencil_enable && target.depth_format == DXGI_FORMAT_D32_FLOAT) {
      m.stencil_missing = true;  // drawn without the test; the renderer adds stencil from the next frame
    } else if (depth.stencil_enable) {
      list->OMSetStencilRef(
          StencilRefMask(regs, depth, topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE).stencilref);
      ++m.stats.stencil_draws;
    }
  }
  const D3D12_VIEWPORT viewport = {0, 0, float(target.width), float(target.height), 0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, LONG(target.width), LONG(target.height)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(topology);
  uint32_t query = 0;
  if (m.query_heap && m.queries_used[m.slot] < Impl::kQueriesPerFrame) {
    query = m.slot * Impl::kQueriesPerFrame + m.queries_used[m.slot]++;
    record.query = query + 1;
    list->BeginQuery(m.query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
  }
  // Breadcrumbs around the draw: started (top of pipe), completed (bottom).
  const uint32_t draw_number = m.MarkStart(list, record);
  if (use_index_buffer) {
    list->IASetIndexBuffer(&ibv);
    list->DrawIndexedInstanced(draw_count, 1, 0, 0, 0);
  } else {
    list->DrawInstanced(count, 1, 0, 0);
  }
  m.MarkEnd(draw_number);
  if (record.query) {
    list->EndQuery(m.query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
  }
  ++m.stats.issued;
  return true;
}

}  // namespace tenchuz::native
