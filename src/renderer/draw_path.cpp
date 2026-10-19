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
#include <cmath>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/bit.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(tenchuz_native_ring_flush, false, "tenchuz",
                    "Native draws: flush mid-frame when command-ring vertex pages are rewritten");
REXCVAR_DEFINE_BOOL(tenchuz_native_debug_textures, false, "tenchuz",
                    "Native draws sample a UV-gradient checker instead of white (geometry check)");

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
  if (aniso != xenos::AnisoFilter::kDisabled) {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << (uint32_t(aniso) - 1);
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
  std::unordered_map<uint32_t, uint64_t> page_hashes;  // command-ring pages copied this frame

  struct Frame {
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
  };
  ComPtr<ID3D12Resource> breadcrumbs;
  volatile uint32_t* breadcrumbs_map = nullptr;  // [2 * slot] started, [2 * slot + 1] completed
  D3D12_GPU_VIRTUAL_ADDRESS breadcrumbs_gpu = 0;
  ID3D12GraphicsCommandList* breadcrumb_list = nullptr;
  ComPtr<ID3D12GraphicsCommandList2> breadcrumb_list2;
  std::array<std::vector<DrawRecord>, kFrames> records;
  std::array<uint64_t, kFrames> record_frames{};
  uint64_t frame_counter = 0;
  std::mutex records_mutex;
  std::atomic<bool> hang_reported{false};

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
    if (breadcrumb_list2) {
      const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER started = {breadcrumbs_gpu + 8 * slot, number};
      const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN;
      breadcrumb_list2->WriteBufferImmediate(1, &started, &mode);
    }
    return number;
  }

  void MarkEnd(uint32_t number) {
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
  DrawStats stats;
  uint32_t logged_errors = 0;

  bool Initialize(ID3D12Device* d) {
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
      f.constants->Map(0, &none, reinterpret_cast<void**>(&f.constants_map));
      f.constants_gpu = f.constants->GetGPUVirtualAddress();
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2 + kFrames * kViewRingSize;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&view_heap)))) return false;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap.NumDescriptors = kSamplerHeapSize;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&sampler_heap)))) return false;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 3;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&staging_heap)))) return false;
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
    }

    loader_ready = loader.Initialize(device, shared.Get(), kSharedMemorySize);
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
        std::memcpy(shared_map + offset, guest, kPageSize);
        stats.uploaded_kb += kPageSize >> 10;
      }
      return;
    }
    while (page < page_end) {
      if (uploaded_pages[page >> 6] & (1ull << (page & 63))) {
        ++page;
        continue;
      }
      const uint32_t run_start = page;
      while (page < page_end && !(uploaded_pages[page >> 6] & (1ull << (page & 63)))) {
        uploaded_pages[page >> 6] |= 1ull << (page & 63);
        ++page;
      }
      const uint32_t offset = run_start << kPageShift;
      const uint32_t bytes = (page - run_start) << kPageShift;
      std::memcpy(shared_map + offset, REX_RAW_ADDR(0xA0000000u | offset), bytes);
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
    const auto depth = regs.Get<reg::RB_DEPTHCONTROL>();
    const uint32_t blend0 = regs[gfx::XE_GPU_REG_RB_BLENDCONTROL0];
    const uint32_t color_mask = regs[gfx::XE_GPU_REG_RB_COLOR_MASK] & 0xF;
    uint64_t key = XXH3_64bits(&shaders, sizeof(shaders));
    const uint32_t state[] = {uint32_t(uintptr_t(root)), depth.value & 0x7E, blend0, color_mask,
                              uint32_t(target.color_format), uint32_t(target.depth_format),
                              uint32_t(topology), uint32_t(strip_cut), gs_key.key};
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
    D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[0];
    const uint32_t src = blend0 & 0x1F, op = (blend0 >> 5) & 7, dst = (blend0 >> 8) & 0x1F;
    const uint32_t asrc = (blend0 >> 16) & 0x1F, aop = (blend0 >> 21) & 7, adst = (blend0 >> 24) & 0x1F;
    rt.BlendEnable = !(src == 1 && dst == 0 && op == 0 && asrc == 1 && adst == 0 && aop == 0);
    rt.SrcBlend = BlendFactor(src);
    rt.DestBlend = BlendFactor(dst);
    rt.BlendOp = BlendOp(op);
    rt.SrcBlendAlpha = AlphaBlendFactor(asrc);
    rt.DestBlendAlpha = AlphaBlendFactor(adst);
    rt.BlendOpAlpha = BlendOp(aop);
    rt.RenderTargetWriteMask = UINT8(color_mask);
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // culling comes in N4.4
    desc.RasterizerState.DepthClipEnable = TRUE;
    if (target.depth_format != DXGI_FORMAT_UNKNOWN) {
      desc.DepthStencilState.DepthEnable = depth.z_enable;
      desc.DepthStencilState.DepthWriteMask =
          depth.z_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
      desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(uint32_t(depth.zfunc) + 1);
      desc.DSVFormat = target.depth_format;
    }
    desc.PrimitiveTopologyType = topology;
    desc.IBStripCutValue = strip_cut;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = target.color_format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
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
    // viewport covering the whole render target.
    float vp[6];
    for (int i = 0; i < 6; ++i) {
      const uint32_t bits = REX_LOAD_U32(device_address + kDevVport + 4 * i);
      std::memcpy(&vp[i], &bits, 4);
    }
    const float xs = vte.vport_x_scale_ena ? vp[0] : 1.0f, xo = vte.vport_x_offset_ena ? vp[1] : 0.0f;
    const float ys = vte.vport_y_scale_ena ? vp[2] : 1.0f, yo = vte.vport_y_offset_ena ? vp[3] : 0.0f;
    const float zs = vte.vport_z_scale_ena ? vp[4] : 1.0f, zo = vte.vport_z_offset_ena ? vp[5] : 0.0f;
    const float w = float(target.width), h = float(target.height);
    sc.ndc_scale[0] = xs * 2.0f / w;
    sc.ndc_offset[0] = xo * 2.0f / w - 1.0f;
    sc.ndc_scale[1] = -ys * 2.0f / h;
    sc.ndc_offset[1] = 1.0f - yo * 2.0f / h;
    sc.ndc_scale[2] = zs;
    sc.ndc_offset[2] = zo;

    float alpha_ref;
    const uint32_t alpha_ref_bits = regs[gfx::XE_GPU_REG_RB_ALPHA_REF];
    std::memcpy(&alpha_ref, &alpha_ref_bits, 4);
    sc.alpha_test_reference = alpha_ref;
    // RT0 colour info from the device: the decoded value is stale in tiled passes.
    reg::RB_COLOR_INFO color_info;
    color_info.value = REX_LOAD_U32(device_address + kDevColorInfo);
    const int32_t exp_bias = int32_t(color_info.value << 6) >> 26;  // bits 20-25, signed
    for (float& bias : sc.color_exp_bias) {
      bias = std::ldexp(1.0f, exp_bias);
    }
    // Gamma render targets are plain 8-bit UNORM here: the shader encodes.
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      sc.flags |= Translator::kSysFlag_ConvertColor0ToGamma;
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

  // Writes an SRV for one shader texture binding at `cpu`: a resolved render
  // target, else a loaded guest texture. False if neither fits the binding.
  bool BindTexture(const gfx::DxbcShader::TextureBinding& binding, const gfx::RegisterFile& regs,
                   ID3D12GraphicsCommandList* list, DrawHost* textures, uint8_t* base,
                   D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
    const xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kTexture &&
        fetch.type != xenos::FetchConstantType::kInvalidTexture) {
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
        device->CreateShaderResourceView(texture.resource, &view, cpu);
        ++stats.textures_resolved;
        return true;
      }
    }
    if (!loader_ready) {
      return false;
    }
    TextureLoader::Texture texture;
    if (!loader.Get(list, fetch, base, [this, base](uint32_t physical, uint32_t size) { Upload(base, physical, size); },
                    texture)) {
      return false;
    }
    view.Format = binding.is_signed ? texture.format_signed : texture.format_unsigned;
    if (view.Format == DXGI_FORMAT_UNKNOWN) {
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
          return false;
        }
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = texture.mip_levels;
        view.Texture2DArray.ArraySize = texture.array_size;
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        if (!is_3d) {
          return false;  // stacked 2D sampled as 3D: SDK 3D-as-2D wrapper, not ported
        }
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        view.Texture3D.MipLevels = texture.mip_levels;
        break;
      case xenos::FetchOpDimension::kCube:
        if (!is_cube) {
          return false;
        }
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.TextureCube.MipLevels = texture.mip_levels;
        break;
    }
    device->CreateShaderResourceView(texture.resource, &view, cpu);
    ++stats.textures_guest;
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
                                           uint8_t* base, bool& ok) {
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
      if (BindTexture(binding, regs, list, textures, base, cpu)) {
        cpu.ptr += view_inc;
        continue;
      }
      ++stats.textures_missing;
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
DrawPath::~DrawPath() = default;

bool DrawPath::Initialize(ID3D12Device* device) {
  return impl_->Initialize(device);
}

void DrawPath::BeginFrame(uint32_t slot) {
  impl_->slot = slot % kFrames;
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
  if (impl_->loader_ready) {
    impl_->loader.BeginFrame(slot);
  }
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
  }
  return s;
}

bool DrawPath::Draw(ID3D12GraphicsCommandList* list, const pm4::DrawInfo& draw,
                    const pm4::Decoder& state, uint8_t* base, uint32_t device_address,
                    const DrawShaders& shaders, const DrawTarget& target, DrawHost* host) {
  DrawHost* textures = host;
  Impl& m = *impl_;
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
  record.ps = shaders.pixel ? shaders.pixel->shader().ucode_data_hash() : 0;
  record.prim = uint32_t(prim);
  record.count = count;
  record.indexed = indexed;
  for (const auto& binding : vs_shader.vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex &&
        fetch.type != xenos::FetchConstantType::kInvalidVertex) {
      continue;  // not a vertex buffer: nothing sensible to copy
    }
    if (!vertices_in_arena) {
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
      m.Upload(base, ib_base, ib_size, host);
    }
    record.ib_base = ib_base;
    record.ib_size = ib_size;
    ibv.BufferLocation = m.shared_gpu + (ib_arena ? ib_arena : (ib_base & 0x1FFFFFFFu));
    ibv.SizeInBytes = ib_size;
    ibv.Format = index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    // Primitive reset with the all-ones index (the same bytes in either order).
    if ((prim == xenos::PrimitiveType::kTriangleStrip || prim == xenos::PrimitiveType::kLineStrip) &&
        regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) {
      strip_cut = index32 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                          : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
    }
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
  auto pack_floats = [&](const gfx::Shader* shader, uint32_t first_reg, D3D12_GPU_VIRTUAL_ADDRESS& gpu) {
    const uint32_t n = shader ? shader->constant_register_map().float_count : 0;
    auto* out = static_cast<uint8_t*>(m.AllocConstants(16 * std::max(n, 1u), gpu));
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
  auto* system = static_cast<Translator::SystemConstants*>(
      m.AllocConstants(sizeof(Translator::SystemConstants), system_gpu));
  if (!fetch || !bool_loop || !system || !pack_floats(&vs_shader, gfx::XE_GPU_REG_SHADER_CONSTANT_000_X, vs_float_gpu) ||
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
  const D3D12_GPU_DESCRIPTOR_HANDLE ps_table = ps_shader ? m.TextureTable(*ps_shader, regs, list, textures, base, tables_ok)
                                                         : D3D12_GPU_DESCRIPTOR_HANDLE{};
  const D3D12_GPU_DESCRIPTOR_HANDLE vs_table = m.TextureTable(vs_shader, regs, list, textures, base, tables_ok);
  if (!tables_ok) {
    ++m.stats.skipped_other;
    return false;
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
  list->SetGraphicsRootConstantBufferView(kRootSystemConstants, system_gpu);
  list->SetGraphicsRootConstantBufferView(kRootBoolLoopConstants, bool_loop_gpu);
  list->SetGraphicsRootDescriptorTable(kRootSharedMemory, shared_table);
  uint32_t param = kRootBaseCount;
  if (ps_textures) list->SetGraphicsRootDescriptorTable(param++, ps_table);
  if (ps_samplers) list->SetGraphicsRootDescriptorTable(param++, m.SamplerTable(*ps_shader, regs));
  if (vs_textures) list->SetGraphicsRootDescriptorTable(param++, vs_table);
  if (vs_samplers) list->SetGraphicsRootDescriptorTable(param++, m.SamplerTable(vs_shader, regs));

  list->OMSetRenderTargets(1, &target.rtv, FALSE,
                           target.depth_format != DXGI_FORMAT_UNKNOWN ? &target.dsv : nullptr);
  const D3D12_VIEWPORT viewport = {0, 0, float(target.width), float(target.height), 0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, LONG(target.width), LONG(target.height)};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(topology);
  // Breadcrumbs around the draw: started (top of pipe), completed (bottom).
  const uint32_t draw_number = m.MarkStart(list, record);
  if (use_index_buffer) {
    list->IASetIndexBuffer(&ibv);
    list->DrawIndexedInstanced(draw_count, 1, 0, 0, 0);
  } else {
    list->DrawInstanced(count, 1, 0, 0);
  }
  m.MarkEnd(draw_number);
  ++m.stats.issued;
  return true;
}

}  // namespace tenchuz::native
