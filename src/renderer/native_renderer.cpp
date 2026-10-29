// tenchuz - native renderer. See native_renderer.h.
//
// N3 frame skeleton: native render targets keyed by their EDRAM placement,
// clears, resolves into native textures keyed by guest physical address, and
// presentation of the front buffer the game swaps to. Tiling (BeginTiling /
// EndTiling / SetPredication) is ignored: everything is rendered once at full
// size. N4: the game's draws are issued with translated shaders (draw_path.h),
// sampling the resolved native textures where they fetch a resolve destination.

#include "native_renderer.h"

#include "../frame_stats.h"

#include "../png_writer.h"

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include <psapi.h>  // after windows.h (d3d12_api.h)

#include "draw_path.h"
#include "hook_common.h"
#include "native_gpu.h"
#include "shader_cache.h"
#include "texture_loader.h"
#include "../widescreen.h"

REXCVAR_DEFINE_BOOL(tenchuz_native_output, false, "tenchuz",
                    "Start with native renderer output instead of the emulator's (F10 toggles)");
REXCVAR_DEFINE_INT32(tenchuz_native_log_frames, 0, "tenchuz",
                     "Log native renderer operations for N frames");
REXCVAR_DEFINE_INT32(tenchuz_native_log_at_frame, 0, "tenchuz",
                     "First native frame to log (with tenchuz_native_log_frames)");
REXCVAR_DEFINE_BOOL(tenchuz_native_shadow, false, "tenchuz",
                    "Run the native renderer in the background while the emulator presents");
REXCVAR_DEFINE_INT32(tenchuz_native_dump_at_frame, 0, "tenchuz",
                     "Write the resolved images of this native frame to logs/frame_dumps (F11: from the next frame)");
REXCVAR_DEFINE_BOOL(tenchuz_native_log_draws, false, "tenchuz",
                    "With frame logging, also list every draw (shaders, buffers) and operation in order");
REXCVAR_DEFINE_INT32(tenchuz_native_dump_frames, 1, "tenchuz",
                     "Number of consecutive frames dumped from tenchuz_native_dump_at_frame or F11 (also logged)");
REXCVAR_DEFINE_INT32(tenchuz_native_log_every, 0, "tenchuz",
                     "Log one native frame every N frames (0 = off)");
REXCVAR_DEFINE_BOOL(tenchuz_native_msaa, true, "tenchuz",
                    "Native render targets use the guest's MSAA sample count (RB_SURFACE_INFO)");
REXCVAR_DEFINE_INT32(tenchuz_native_resolution_scale, 1, "tenchuz",
                     "Internal resolution of native rendering, in multiples of the game's (1-4)");
REXCVAR_DEFINE_BOOL(tenchuz_native_log_presents, false, "tenchuz",
                    "Log every native frame end: the front buffer, when it was last resolved, and the frame's "
                    "draws, resolves and clears");
REXCVAR_DEFINE_INT32(tenchuz_native_trace_syncs_at, 0, "tenchuz",
                     "First native frame whose command-stream syncs, kick ends and draw packets are logged");
REXCVAR_DEFINE_INT32(tenchuz_native_trace_syncs_frames, 1, "tenchuz",
                     "Number of frames traced from tenchuz_native_trace_syncs_at");
REXCVAR_DEFINE_BOOL(tenchuz_native_check_presents, false, "tenchuz",
                    "Log the brightness of every presented native frame and of the presenter's output "
                    "(flicker hunting)");
REXCVAR_DEFINE_INT32(tenchuz_native_msaa_samples, 0, "tenchuz",
                     "MSAA samples of the game's multisampled render targets: 0 = the game's (2x), 4 = 4x");
REXCVAR_DEFINE_STRING(tenchuz_native_post_effect, "none", "tenchuz",
                      "Native output post effect: none, fxaa, fxaa_extreme (the emulator's swap_post_effect)")
    .allowed({"none", "fxaa", "fxaa_extreme"});
REXCVAR_DEFINE_DOUBLE(tenchuz_native_sharpening, 0.0, "tenchuz",
                      "Contrast adaptive sharpening (AMD FidelityFX CAS) of the presented image: 0 = off, up to 1")
    .range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(tenchuz_native_hardware_resolve, true, "tenchuz",
                    "MSAA colour resolves that average every sample use the GPU's own resolve");
REXCVAR_DEFINE_BOOL(tenchuz_native_depth32, true, "tenchuz",
                    "Depth buffers are 32-bit float until the game uses stencil (else always depth + stencil)");
REXCVAR_DEFINE_BOOL(tenchuz_dred, false, "tenchuz",
                    "Enable D3D12 DRED (auto-breadcrumbs, page faults) for GPU hang reports, without the debug layer");

using Microsoft::WRL::ComPtr;

namespace tenchuz::native {
namespace {

// The SDK's FXAA pass (its swap_post_effect), vendored in external/xenos_shader.
namespace shaders {
#include "fxaa_cs.h"
#include "fxaa_extreme_cs.h"
}  // namespace shaders

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kPm4XeSwap = 0x64;  // SDK-specific swap packet written by VdSwap

// D3D device register shadow (docs/D3D_MAP.md).
constexpr uint32_t kDevSurfaceInfo = 0x2800;        // RB_SURFACE_INFO
constexpr uint32_t kDevColorInfo = 0x2804;          // RB_COLOR_INFO (RT1-3 at +4/+8/+0xC after depth)
constexpr uint32_t kDevDepthInfo = 0x2808;          // RB_DEPTH_INFO
constexpr uint32_t kDevColor1Info = 0x280C;         // RB_COLOR1_INFO
constexpr uint32_t kDevScreenScissorBr = 0x283C;    // PA_SC_SCREEN_SCISSOR_BR
constexpr uint32_t kDevCopyControl = 0x2998;        // RB_COPY_CONTROL (only during Resolve, see OnD3DCall)
constexpr uint32_t kDevCopyDestBase = 0x299C;       // RB_COPY_DEST_BASE
constexpr uint32_t kDevCopyDestPitch = 0x29A0;      // RB_COPY_DEST_PITCH
constexpr uint32_t kDevCopyDestInfo = 0x29A4;       // RB_COPY_DEST_INFO (only during Resolve, see OnD3DCall)
constexpr uint32_t kApiResolve = 0x8241D770;        // D3DDevice_Resolve
constexpr uint32_t kApiEndTiling = 0x8241CC58;      // D3DDevice_EndTiling
constexpr uint32_t kDevDepthClear = 0x29AC;         // RB_DEPTH_CLEAR
constexpr uint32_t kDevRenderTarget0 = 0x2F90;      // bound D3DSurface* RT0-3 (+4 each)
constexpr uint32_t kDevDepthStencil = 0x2FA0;       // bound depth-stencil D3DSurface*
constexpr uint32_t kDevViewport = 0x3060;           // D3DVIEWPORT9 copy: X, Y, Width, Height (dwords)

std::atomic<bool> g_present_native{false};
std::atomic<uint8_t*> g_base{nullptr};
uint32_t g_device = 0;
std::atomic<int> g_last_depth_draws{-1};  // depth-tested draws of the last presented frame

rex::ui::Presenter* g_presenter = nullptr;
const rex::ui::d3d12::D3D12Provider* g_provider = nullptr;
ID3D12Device* g_d3d = nullptr;
bool g_ready = false;
// Host pixels per guest pixel on each axis of render targets and resolves
// (--tenchuz_native_resolution_scale).
uint32_t g_scale = 1;
std::filesystem::path g_cache_dir;  // SetCacheDir: the pipeline cache's folder
uint64_t g_frame = 0;
int32_t g_log_frames = 0;
// One shader cache per resolution scale (1-4), kept for switching back; the
// draw path's pipelines are keyed by their translations, so they stay apart.
std::array<std::unique_ptr<ShaderCache>, 5> g_shader_caches;
ShaderCache* g_shaders = nullptr;  // the current scale's
std::unique_ptr<DrawPath> g_draw_path;
uint32_t g_frame_draws = 0, g_frame_draws_ready = 0;
uint32_t g_frame_resolves = 0, g_frame_clears = 0;  // --tenchuz_native_log_presents
uint32_t g_frame_resolves_skipped = 0;  // per-tile repeats (AlreadyResolved)
std::map<uint64_t, uint32_t> g_draw_kinds;  // (API function, opcode, predicated) -> draws, logged frames

// --- D3D12 plumbing ---------------------------------------------------------

struct FrameSlot {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12CommandAllocator> present_allocator;
  uint64_t fence_value = 0;
  // Resources replaced during this slot's frame, released once its fence passes
  // (earlier commands in that frame and the previous one may still use them).
  std::vector<ComPtr<ID3D12Resource>> retired;
};
std::array<FrameSlot, kFramesInFlight> g_slots;
ComPtr<ID3D12GraphicsCommandList> g_list;          // recorded across the game's frame
ComPtr<ID3D12GraphicsCommandList> g_present_list;  // blit into the presenter's output
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fence_event = nullptr;
uint64_t g_fence_counter = 0;
bool g_recording = false;

ComPtr<ID3D12DescriptorHeap> g_rtv_heap, g_dsv_heap, g_shader_heap;
uint32_t g_rtv_size = 0, g_dsv_size = 0, g_shader_size = 0;
D3D12_CPU_DESCRIPTOR_HANDLE g_null_rtv = {};  // for unbound targets before the last bound one
uint32_t g_rtv_used = 0, g_dsv_used = 0;
constexpr uint32_t kRtvCapacity = 512, kDsvCapacity = 128;

ComPtr<ID3D12RootSignature> g_blit_root;
ComPtr<ID3D12PipelineState> g_blit_pso;
ComPtr<ID3D12PipelineState> g_resolve_pso;  // resolve with format conversion (same root as the blit)
ComPtr<ID3D12PipelineState> g_resolve_msaa_pso;        // ... from a multisampled colour target
ComPtr<ID3D12PipelineState> g_resolve_depth_msaa_pso;  // one sample of a multisampled depth buffer
ComPtr<ID3D12PipelineState> g_blood_dry_pso, g_blood_under_pso;  // blood maps' dried layers
ComPtr<ID3D12PipelineState> g_resolve_depth_pso;       // a single-sampled depth buffer (or part of one)
bool g_msaa_2x = false, g_msaa_4x = false;  // host support for every target format
// FXAA on the presented image (--tenchuz_native_post_effect), like the SDK's
// swap: the blit writes the image with its luma in alpha to g_fxaa_source,
// and the SDK's FXAA shader filters that into the presenter's output.
ComPtr<ID3D12RootSignature> g_fxaa_root;
ComPtr<ID3D12PipelineState> g_fxaa_pso, g_fxaa_extreme_pso;
ComPtr<ID3D12Resource> g_fxaa_source;
D3D12_RESOURCE_STATES g_fxaa_source_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr DXGI_FORMAT kFxaaSourceFormat = DXGI_FORMAT_R16G16B16A16_UNORM;  // the SDK's
// Sharpening (--tenchuz_native_sharpening) after the blit and FXAA: they
// write g_cas_source, and CAS filters that into the presenter's output.
ComPtr<ID3D12PipelineState> g_cas_pso;  // the blit's root signature
ComPtr<ID3D12Resource> g_cas_source;
D3D12_RESOURCE_STATES g_cas_source_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
// Per frame slot: blit source SRV, blit destination UAV, FXAA source SRV and
// FXAA destination UAV, CAS source SRV and destination UAV; then the resolves'.
constexpr uint32_t kPresentDescriptors = 6;
constexpr uint32_t kResolveDescriptors = 512;  // per frame slot, 2 per converting resolve
std::array<uint32_t, kFramesInFlight> g_resolve_descriptors_used{};

// --- Native surfaces ---------------------------------------------------------

struct Surface {
  ComPtr<ID3D12Resource> resource;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0, height = 0;  // guest pixels
  uint32_t scale = 1;              // the resource is width x height times this
  bool depth = false;
  bool red_blue_swapped = false;  // resolved with RB_COPY_DEST_INFO.copy_dest_swap
  bool unordered_access = false;  // created for converting resolves
  uint64_t resolved_frame = 0;    // native frame of the last resolve into it
  uint32_t guest_bytes = 0;       // resolve destinations: the guest memory a resolve covers
  uint32_t id = 0;                // creation number (draw list diagnostics)
  uint32_t samples = 1;
  // Render targets: draws and clears so far. Resolve destinations: what the
  // last resolve copied (source, its writes then, and the copy's settings),
  // so the per-tile repeats of a resolve in a tiled pass are copied once.
  uint64_t writes = 0;
  const void* resolved_from = nullptr;
  uint64_t resolved_writes = 0, resolved_settings = 0;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_CPU_DESCRIPTOR_HANDLE view = {};  // RTV or DSV (render targets only)

  uint32_t host_width() const { return width * scale; }
  uint32_t host_height() const { return height * scale; }
};

std::unordered_map<uint64_t, Surface> g_targets;   // EDRAM base | format | pitch
// Resolve destinations by guest physical address and size (some addresses are
// resolved to at two sizes every frame), and the latest one per address.
std::unordered_map<uint64_t, Surface> g_textures;
std::unordered_map<uint32_t, Surface*> g_latest_texture;

uint64_t TextureKey(uint32_t address, uint32_t width, uint32_t height) {
  return uint64_t(address) | (uint64_t(width & 0x3FFF) << 32) | (uint64_t(height & 0x3FFF) << 46);
}

// Bytes per pixel of the host formats render targets and resolves use.
uint32_t BytesPerPixel(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
      return 2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
      return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      return 16;
    default:
      return 4;
  }
}

DXGI_FORMAT ColorFormat(uint32_t xenos_format) {
  switch (xenos_format) {
    case 0:   // k_8_8_8_8
    case 1:   // k_8_8_8_8_GAMMA
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2:   // k_2_10_10_10
    case 10:  // k_2_10_10_10_AS_10_10_10_10
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 4:   // k_16_16
    case 6:   // k_16_16_FLOAT
      return DXGI_FORMAT_R16G16_FLOAT;
    case 14:  // k_32_FLOAT
      return DXGI_FORMAT_R32_FLOAT;
    case 15:  // k_32_32_FLOAT
      return DXGI_FORMAT_R32G32_FLOAT;
    default:  // 3, 5, 7, 12: 16-bit-per-channel and float 10-bit formats
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
  }
}

void Transition(Surface& s, D3D12_RESOURCE_STATES to) {
  if (s.state == to) {
    return;
  }
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = s.resource.Get();
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = s.state;
  barrier.Transition.StateAfter = to;
  g_list->ResourceBarrier(1, &barrier);
  s.state = to;
}

// `width` x `height` in guest pixels; the resource is `scale` times larger.
bool CreateTexture(Surface& s, DXGI_FORMAT format, uint32_t width, uint32_t height, bool depth,
                   bool render_target, bool unordered_access = false, uint32_t samples = 1, uint32_t scale = 1) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width * scale;
  desc.Height = height * scale;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  // Depth surfaces are typeless so depth resolves can read them as R32_FLOAT
  // (D32_FLOAT_S8X24 is planar in D3D12: its depth plane).
  desc.Format = format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? DXGI_FORMAT_R32G8X24_TYPELESS
                : format == DXGI_FORMAT_D32_FLOAT        ? DXGI_FORMAT_R32_TYPELESS
                                                         : format;
  desc.SampleDesc.Count = samples;
  desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                     : (render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                      : (unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                          : D3D12_RESOURCE_FLAG_NONE));
  D3D12_RESOURCE_STATES initial =
      depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
            : (render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = format;
  if (FAILED(g_d3d->CreateCommittedResource(
          &rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc, initial,
          (depth || render_target) ? &clear : nullptr, IID_PPV_ARGS(&s.resource)))) {
    return false;
  }
  static uint32_t surfaces_created = 0;
  s.id = ++surfaces_created;
  s.format = format;
  s.width = width;
  s.height = height;
  s.scale = scale;
  s.depth = depth;
  s.unordered_access = unordered_access;
  s.samples = samples;
  s.state = initial;
  if (depth && g_dsv_used < kDsvCapacity) {
    s.view = g_dsv_heap->GetCPUDescriptorHandleForHeapStart();
    s.view.ptr += SIZE_T(g_dsv_used++) * g_dsv_size;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = format;
    dsv.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    g_d3d->CreateDepthStencilView(s.resource.Get(), &dsv, s.view);
  } else if (render_target && g_rtv_used < kRtvCapacity) {
    s.view = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    s.view.ptr += SIZE_T(g_rtv_used++) * g_rtv_size;
    g_d3d->CreateRenderTargetView(s.resource.Get(), nullptr, s.view);
  }
  return true;
}

uint32_t Dev(uint8_t* base, uint32_t offset) {
  return REX_LOAD_U32(g_device + offset);
}

// Logical size of the render target bound with the last SetRenderTarget(0, …):
// the D3D resets the viewport to the full surface there (RB_SURFACE_INFO only
// holds the per-tile EDRAM pitch, and the scissor is left at its 8192 maximum).
uint32_t g_bound_width = 0, g_bound_height = 0;

void BoundSize(uint8_t* base, uint32_t& width, uint32_t& height) {
  width = g_bound_width ? g_bound_width : Dev(base, kDevViewport + 8);
  height = g_bound_height ? g_bound_height : Dev(base, kDevViewport + 12);
  if (!width || width > 8192) width = 1280;
  if (!height || height > 8192) height = 720;
}

// The guest's MSAA sample count for the bound targets (RB_SURFACE_INFO.
// msaa_samples, from the device: the decoded value is stale in tiled passes).
uint32_t GuestSampleCount(uint8_t* base) {
  switch ((Dev(base, kDevSurfaceInfo) >> 16) & 3) {
    case 1: return 2;
    case 2: return 4;
    default: return 1;
  }
}

// Host MSAA sample count of the bound targets: the guest's, or 4 for the
// guest's multisampled targets with --tenchuz_native_msaa_samples=4. Lower
// when the host can't multisample every target format at that count.
uint32_t SampleCount(uint8_t* base) {
  if (!REXCVAR_GET(tenchuz_native_msaa)) {
    return 1;
  }
  const uint32_t guest = GuestSampleCount(base);
  const uint32_t wanted = guest > 1 && REXCVAR_GET(tenchuz_native_msaa_samples) >= 4 ? 4 : guest;
  if (wanted == 4 && g_msaa_4x) return 4;
  if (wanted >= 2 && g_msaa_2x) return 2;
  return 1;
}

Surface* ColorTarget(uint8_t* base, uint32_t index) {
  const uint32_t info = index == 0 ? Dev(base, kDevColorInfo) : Dev(base, kDevColor1Info + 4 * (index - 1));
  uint32_t width, height;
  BoundSize(base, width, height);
  const uint32_t format = (info >> 16) & 0xF;
  const uint32_t samples = SampleCount(base);
  const uint64_t key = (uint64_t(info & 0xFFF) << 40) | (uint64_t(samples) << 36) | (uint64_t(format) << 32) |
                       (uint64_t(width) << 16) | height;
  auto it = g_targets.find(key);
  if (it != g_targets.end()) {
    return &it->second;
  }
  Surface s;
  if (!CreateTexture(s, ColorFormat(format), width, height, false, true, false, samples, g_scale)) {
    return nullptr;
  }
  SetDebugName(s.resource.Get(), fmt::format("tenchuz RT edram {:03X} format {} {}x{} x{}", info & 0xFFF, format,
                                             s.host_width(), s.host_height(), samples));
  return &g_targets.emplace(key, std::move(s)).first->second;
}

// Depth buffers: the game's depth is 24-bit float (RB_DEPTH_INFO.depth_format
// D24FS8), kept as 32-bit float. Stencil doubles every depth read and write
// (D32_FLOAT_S8X24 is 64 bits a sample), and gameplay hasn't used it, so it's
// added only once a draw asks for the stencil test (ApplyRenderSettings).
bool g_depth_stencil = false;

DXGI_FORMAT DepthFormat() {
  return g_depth_stencil || !REXCVAR_GET(tenchuz_native_depth32) ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT
                                                                  : DXGI_FORMAT_D32_FLOAT;
}

Surface* DepthTarget(uint8_t* base) {
  const uint32_t info = Dev(base, kDevDepthInfo);
  uint32_t width, height;
  BoundSize(base, width, height);
  const uint32_t samples = SampleCount(base);
  const uint64_t key = (1ull << 63) | (uint64_t(info & 0xFFF) << 40) | (uint64_t(samples) << 36) |
                       (uint64_t(width) << 16) | height;
  auto it = g_targets.find(key);
  if (it != g_targets.end()) {
    return &it->second;
  }
  Surface s;
  if (!CreateTexture(s, DepthFormat(), width, height, true, false, false, samples, g_scale)) {
    return nullptr;
  }
  SetDebugName(s.resource.Get(), fmt::format("tenchuz depth edram {:03X} {}x{} x{}", info & 0xFFF, s.host_width(),
                                             s.host_height(), samples));
  return &g_targets.emplace(key, std::move(s)).first->second;
}

bool Active() {
  return g_ready && (g_present_native.load(std::memory_order_relaxed) || REXCVAR_GET(tenchuz_native_shadow));
}

bool Logging() {
  const int32_t every = REXCVAR_GET(tenchuz_native_log_every);
  if (every > 0 && g_frame % uint64_t(every) == 0) {
    return true;
  }
  return g_log_frames > 0 && g_frame >= uint64_t(std::max(0, REXCVAR_GET(tenchuz_native_log_at_frame)));
}

// The fence value the render thread is blocked on, and why (GPU hang reports).
std::atomic<uint64_t> g_waiting_fence{0};
std::atomic<const char*> g_waiting_for{nullptr};

void WaitForFence(uint64_t value, const char* what) {
  if (g_fence->GetCompletedValue() >= value) {
    return;
  }
  g_waiting_for.store(what);
  g_waiting_fence.store(value);
  g_fence->SetEventOnCompletion(value, g_fence_event);
  WaitForSingleObject(g_fence_event, INFINITE);
  g_waiting_fence.store(0);
}

// Wait until the GPU is done with this frame slot's command allocators.
FrameSlot& WaitForSlot() {
  FrameSlot& slot = g_slots[g_frame % kFramesInFlight];
  WaitForFence(slot.fence_value, "frame slot");
  return slot;
}

uint64_t g_previous_frame_fence = 0;  // the last fence signalled before this frame's recording

void BeginRecording() {
  if (g_recording) {
    return;
  }
  FrameSlot& slot = WaitForSlot();
  g_resolve_descriptors_used[g_frame % kFramesInFlight] = 0;
  // The GPU may still be running the previous frame while this one is
  // recorded. Draws read a guest-memory mirror that is updated in place, so
  // the draw path waits for that frame (FrameHost::WaitForPreviousFrame) only
  // before overwriting a page it read. (The D3D's ring of dynamic vertices
  // goes to per-slot arenas.)
  g_previous_frame_fence = g_fence_counter;
  slot.retired.clear();
  slot.allocator->Reset();
  g_list->Reset(slot.allocator.Get(), nullptr);
  if (g_draw_path) {
    g_draw_path->BeginFrame(uint32_t(g_frame % kFramesInFlight));
  }
  g_recording = true;
}

float LoadFloat(uint8_t* base, uint32_t address) {
  const uint32_t bits = REX_LOAD_U32(address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// Breadcrumbs (draw_path.h) around native operations, for GPU hang reports.
uint32_t BeginOp(const char* what, uint32_t a, uint32_t b) {
  return g_draw_path ? g_draw_path->BeginOp(g_list.Get(), what, a, b) : 0;
}

void EndOp(uint32_t op) {
  if (g_draw_path) {
    g_draw_path->EndOp(op);
  }
}

void ClearColor(Surface& s, const float color[4]) {
  ++g_frame_clears;
  ++s.writes;
  const uint32_t op = BeginOp("clear colour", s.width, s.height);
  Transition(s, D3D12_RESOURCE_STATE_RENDER_TARGET);
  g_list->ClearRenderTargetView(s.view, color, 0, nullptr);
  EndOp(op);
}

void ClearDepth(Surface& s, float z, uint8_t stencil, D3D12_CLEAR_FLAGS flags) {
  if (s.format == DXGI_FORMAT_D32_FLOAT) {
    flags = D3D12_CLEAR_FLAGS(flags & ~D3D12_CLEAR_FLAG_STENCIL);
    if (!flags) {
      return;
    }
  }
  ++g_frame_clears;
  ++s.writes;
  const uint32_t op = BeginOp("clear depth", s.width, s.height);
  Transition(s, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  g_list->ClearDepthStencilView(s.view, flags, z, stencil, 0, nullptr);
  EndOp(op);
}

// Clears a Resolve asks for (D3DRESOLVE_CLEARRENDERTARGET / _CLEARDEPTHSTENCIL)
// run before the next draw or clear, not right away: in tiled passes the D3D
// resolves once per tile, and we copy the whole untiled target each time.
struct PendingClear {
  Surface* color = nullptr;
  float rgba[4] = {};
  Surface* depth = nullptr;
  float z = 0.0f;
  uint8_t stencil = 0;
  D3D12_CLEAR_FLAGS depth_flags = D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL);
};
PendingClear g_pending_clear;

void FlushPendingClears() {
  if (g_pending_clear.color) {
    ClearColor(*g_pending_clear.color, g_pending_clear.rgba);
  }
  if (g_pending_clear.depth) {
    ClearDepth(*g_pending_clear.depth, g_pending_clear.z, g_pending_clear.stencil, g_pending_clear.depth_flags);
  }
  g_pending_clear = {};
}

// The clear colour of ClearF / Resolve: the D3DVECTOR4 (r, g, b, a) the game
// passes, or with none, the D3D fills all four from the constant 0.0f at
// 0x820024DC: transparent black. (Not RB_COLOR_CLEAR, which keeps the last
// clear's colour: the blood maps, cleared without a colour, then got alpha 1
// and blended bloodied characters entirely to black.)
constexpr uint32_t kNullClearConstant = 0x820024DC;

void ClearColorArgument(uint8_t* base, uint32_t color_ptr, float out[4]) {
  for (int i = 0; i < 4; ++i) {
    out[i] = LoadFloat(base, color_ptr ? color_ptr + 4 * i : kNullClearConstant);
  }
}

// --- Presentation ------------------------------------------------------------

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                      ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                      ID3DBlob**);
using SerializeRootFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                         ID3DBlob**, ID3DBlob**);

// Resolves from multisampled targets. `samples`: first sample (bits 0-1) and
// how many are averaged (bits 2-4), from RB_COPY_CONTROL.copy_sample_select.
// The resolve shaders copy a width x height area from src_at in the source to
// dst_at in the destination (each packed x | y << 16): a Resolve's source
// rectangle and destination point.
constexpr char kResolveMsaaShader[] = R"(
Texture2DMS<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Params : register(b0) { uint width; uint height; float scale; uint samples; uint src_at; uint dst_at; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  int2 s = int2(id.xy) + int2(src_at & 0xFFFF, src_at >> 16);
  uint first = samples & 3, count = samples >> 2;
  float4 sum = 0;
  for (uint i = 0; i < count; ++i) sum += src.Load(s, first + i);
  dst[id.xy + uint2(dst_at & 0xFFFF, dst_at >> 16)] = sum / float(count) * scale;
}
)";
constexpr char kResolveDepthMsaaShader[] = R"(
Texture2DMS<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Params : register(b0) { uint width; uint height; float scale; uint samples; uint src_at; uint dst_at; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  int2 s = int2(id.xy) + int2(src_at & 0xFFFF, src_at >> 16);
  dst[id.xy + uint2(dst_at & 0xFFFF, dst_at >> 16)] = src.Load(s, samples & 3);
}
)";
constexpr char kResolveDepthShader[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Params : register(b0) { uint width; uint height; float scale; uint samples; uint src_at; uint dst_at; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  int2 s = int2(id.xy) + int2(src_at & 0xFFFF, src_at >> 16);
  dst[id.xy + uint2(dst_at & 0xFFFF, dst_at >> 16)] = src.Load(int3(s, 0));
}
)";

// The present blit applies the display gamma ramp like the SDK's
// apply_gamma_table: an 8-bit index per channel into the 256-entry LUT, whose
// entries hold 10-bit blue, green and red from bit 0 up (DC_LUT_30_COLOR).
// For FXAA it writes the luma to alpha, like apply_gamma_table_fxaa_luma.
constexpr char kBlitShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
StructuredBuffer<uint> ramp : register(t1);
cbuffer Params : register(b0) { uint src_w; uint src_h; uint dst_w; uint dst_h; uint use_ramp; uint luma; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= dst_w || id.y >= dst_h) return;
  uint2 s = uint2(id.x * src_w / dst_w, id.y * src_h / dst_h);
  float3 color = src.Load(int3(s, 0)).rgb;
  if (use_ramp) {
    uint3 i = uint3(saturate(color) * 255.0 + 0.5);
    color = float3((ramp[i.r] >> 20) & 0x3FF, (ramp[i.g] >> 10) & 0x3FF, ramp[i.b] & 0x3FF) / 1023.0;
  }
  dst[id.xy] = float4(color, luma ? dot(color, float3(0.299, 0.587, 0.114)) : 1.0);
}
)";
constexpr uint32_t kBlitConstants = 6;

// The display gamma ramp the D3D last wrote to the 256-entry LUT
// (sub_8242A4F8), as DC_LUT_30_COLOR values, and its per-frame-slot upload.
std::mutex g_gamma_mutex;
std::array<uint32_t, 256> g_gamma_ramp{};
bool g_gamma_ramp_set = false;
ComPtr<ID3D12Resource> g_gamma_upload;
uint8_t* g_gamma_upload_map = nullptr;
constexpr uint32_t kGammaRampBytes = 256 * 4;

// Resolve with conversion: the EDRAM copy scales by 2^copy_dest_exp_bias and
// converts to the destination format (writing a UNORM destination clamps).
constexpr char kResolveShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Params : register(b0) { uint width; uint height; float scale; uint pad; uint src_at; uint dst_at; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  int2 s = int2(id.xy) + int2(src_at & 0xFFFF, src_at >> 16);
  dst[id.xy + uint2(dst_at & 0xFFFF, dst_at >> 16)] = src.Load(int3(s, 0)) * scale;
}
)";

// Blood maps' dried layers (SetBloodMapResolve): the map's colours are
// premultiplied (splats blended over transparent black). Drying keeps the
// map darkened by `scale`; under puts the layer beneath newer splats.
constexpr char kBloodDryShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Params : register(b0) { uint width; uint height; float scale; uint pad; uint src_at; uint dst_at; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  float4 c = src.Load(int3(id.xy, 0));
  dst[id.xy] = float4(c.rgb * scale, c.a);
}
)";
constexpr char kBloodUnderShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Params : register(b0) { uint width; uint height; float scale; uint pad; uint src_at; uint dst_at; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  float4 top = dst[id.xy];
  dst[id.xy] = top + src.Load(int3(id.xy, 0)) * (1.0 - top.a);
}
)";
constexpr float kBloodDrying = 0.7f;  // a dried layer's colour, of what it was

// Contrast adaptive sharpening, after AMD FidelityFX CAS (its non-scaling
// path): each pixel is pushed away from its 4 neighbours by an amount that
// shrinks where the 3x3 neighbourhood already has strong contrast, so edges
// don't ring. peak is -1 / lerp(8, 5, sharpness).
constexpr char kCasShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Params : register(b0) { uint width; uint height; float peak; };
float3 At(int2 p) { return src.Load(int3(clamp(p, int2(0, 0), int2(width - 1, height - 1)), 0)).rgb; }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) return;
  int2 p = int2(id.xy);
  float3 a = At(p + int2(-1, -1)), b = At(p + int2(0, -1)), c = At(p + int2(1, -1));
  float3 d = At(p + int2(-1, 0)), e = At(p), f = At(p + int2(1, 0));
  float3 g = At(p + int2(-1, 1)), h = At(p + int2(0, 1)), i = At(p + int2(1, 1));
  // Soft minimum and maximum: the cross plus the whole 3x3.
  float3 mn = min(min(min(d, e), min(f, b)), h);
  mn += min(mn, min(min(a, c), min(g, i)));
  float3 mx = max(max(max(d, e), max(f, b)), h);
  mx += max(mx, max(max(a, c), max(g, i)));
  float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
  float3 w = amp * peak;
  float3 color = saturate(((b + d + f + h) * w + e) / (1.0 + 4.0 * w));
  dst[id.xy] = float4(color, 1.0);
}
)";
constexpr uint32_t kCasConstants = 3;

bool CreateBlit() {
  HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
  auto compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
  auto serialize = reinterpret_cast<SerializeRootFn>(
      GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12SerializeRootSignature"));
  if (!compile || !serialize) {
    return false;
  }
  ComPtr<ID3DBlob> code, errors;
  if (FAILED(compile(kBlitShader, sizeof(kBlitShader) - 1, "blit", nullptr, nullptr, "main", "cs_5_0",
                     0, 0, &code, &errors))) {
    REXLOG_ERROR("tenchuz: blit shader: {}",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    return false;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].OffsetInDescriptorsFromTableStart = 1;
  D3D12_ROOT_PARAMETER params[3] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 2;
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = kBlitConstants;
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;  // gamma ramp (present only)
  params[2].Descriptor.ShaderRegister = 1;
  D3D12_ROOT_SIGNATURE_DESC root = {};
  root.NumParameters = 3;
  root.pParameters = params;
  ComPtr<ID3DBlob> root_blob;
  if (FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &errors)) ||
      FAILED(g_d3d->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                        IID_PPV_ARGS(&g_blit_root)))) {
    return false;
  }
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = g_blit_root.Get();
  pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  if (FAILED(g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_blit_pso)))) {
    return false;
  }
  // FXAA, with the SDK's root signature: FxaaConstants (size, 1 / size), the
  // destination UAV and source SRV tables, and a linear clamp sampler.
  {
    D3D12_DESCRIPTOR_RANGE dest = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE source = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER fxaa_params[3] = {};
    fxaa_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaa_params[0].Constants.Num32BitValues = 4;
    fxaa_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_params[1].DescriptorTable = {1, &dest};
    fxaa_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_params[2].DescriptorTable = {1, &source};
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC fxaa_root = {3, fxaa_params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> fxaa_blob;
    D3D12_COMPUTE_PIPELINE_STATE_DESC fxaa_pso = {};
    if (SUCCEEDED(serialize(&fxaa_root, D3D_ROOT_SIGNATURE_VERSION_1, &fxaa_blob, &errors)) &&
        SUCCEEDED(g_d3d->CreateRootSignature(0, fxaa_blob->GetBufferPointer(), fxaa_blob->GetBufferSize(),
                                             IID_PPV_ARGS(&g_fxaa_root)))) {
      fxaa_pso.pRootSignature = g_fxaa_root.Get();
      fxaa_pso.CS = {shaders::fxaa_cs, sizeof(shaders::fxaa_cs)};
      g_d3d->CreateComputePipelineState(&fxaa_pso, IID_PPV_ARGS(&g_fxaa_pso));
      fxaa_pso.CS = {shaders::fxaa_extreme_cs, sizeof(shaders::fxaa_extreme_cs)};
      g_d3d->CreateComputePipelineState(&fxaa_pso, IID_PPV_ARGS(&g_fxaa_extreme_pso));
    }
    if (!g_fxaa_pso || !g_fxaa_extreme_pso) {
      REXLOG_WARN("tenchuz: native FXAA unavailable (root signature or pipeline creation failed)");
    }
  }
  ComPtr<ID3DBlob> resolve_code;
  if (FAILED(compile(kResolveShader, sizeof(kResolveShader) - 1, "resolve", nullptr, nullptr, "main",
                     "cs_5_0", 0, 0, &resolve_code, &errors))) {
    REXLOG_ERROR("tenchuz: resolve shader: {}",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    return false;
  }
  pso.CS = {resolve_code->GetBufferPointer(), resolve_code->GetBufferSize()};
  if (FAILED(g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_resolve_pso)))) {
    return false;
  }
  ComPtr<ID3DBlob> cas_code;
  if (SUCCEEDED(compile(kCasShader, sizeof(kCasShader) - 1, "cas", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                        &cas_code, &errors))) {
    pso.CS = {cas_code->GetBufferPointer(), cas_code->GetBufferSize()};
    g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_cas_pso));
  }
  if (!g_cas_pso) {
    REXLOG_WARN("tenchuz: native sharpening unavailable: {}",
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "pipeline creation failed");
  }
  for (auto [source, out] : {std::pair{kResolveMsaaShader, &g_resolve_msaa_pso},
                             std::pair{kResolveDepthMsaaShader, &g_resolve_depth_msaa_pso},
                             std::pair{kResolveDepthShader, &g_resolve_depth_pso}}) {
    ComPtr<ID3DBlob> msaa_code;
    if (FAILED(compile(source, std::strlen(source), "resolve_msaa", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                       &msaa_code, &errors))) {
      REXLOG_ERROR("tenchuz: MSAA resolve shader: {}",
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
      return false;
    }
    pso.CS = {msaa_code->GetBufferPointer(), msaa_code->GetBufferSize()};
    if (FAILED(g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(out)))) {
      return false;
    }
  }
  for (auto [source, out] :
       {std::pair{kBloodDryShader, &g_blood_dry_pso}, std::pair{kBloodUnderShader, &g_blood_under_pso}}) {
    ComPtr<ID3DBlob> blood_code;
    if (SUCCEEDED(compile(source, std::strlen(source), "blood", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                          &blood_code, &errors))) {
      pso.CS = {blood_code->GetBufferPointer(), blood_code->GetBufferSize()};
      g_d3d->CreateComputePipelineState(&pso, IID_PPV_ARGS(out));
    }
  }
  // 2x / 4x MSAA for every format a target can have (and the depth format).
  for (uint32_t count : {2u, 4u}) {
    bool supported = true;
    for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16_FLOAT,
                               DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                               DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_D32_FLOAT}) {
      D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {format, count};
      supported = supported && SUCCEEDED(g_d3d->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                                                                    &levels, sizeof(levels))) &&
                  levels.NumQualityLevels;
    }
    (count == 2 ? g_msaa_2x : g_msaa_4x) = supported;
  }
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = kFramesInFlight * kGammaRampBytes;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_RANGE none = {0, 0};
  if (FAILED(g_d3d->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
                                            &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&g_gamma_upload))) ||
      FAILED(g_gamma_upload->Map(0, &none, reinterpret_cast<void**>(&g_gamma_upload_map)))) {
    return false;
  }
  SetDebugName(g_gamma_upload.Get(), "tenchuz gamma ramp upload");
  return true;
}

// Host format of a resolve destination (RB_COPY_DEST_INFO.copy_dest_format, a
// xenos::ColorFormat), or UNKNOWN to keep the render target's format.
DXGI_FORMAT ResolveDestFormat(uint32_t color_format) {
  switch (color_format) {
    case 2: return DXGI_FORMAT_R8_UNORM;                // k_8
    case 6: case 14: case 50:                           // k_8_8_8_8, _A, _AS_16_16_16_16
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 7: case 54: return DXGI_FORMAT_R10G10B10A2_UNORM;  // k_2_10_10_10 (_AS_16_16_16_16)
    case 10: return DXGI_FORMAT_R8G8_UNORM;             // k_8_8
    case 16: case 17: return DXGI_FORMAT_R11G11B10_FLOAT;  // k_10_11_11, k_11_11_10
    case 30: return DXGI_FORMAT_R16_FLOAT;
    case 31: return DXGI_FORMAT_R16G16_FLOAT;
    case 32: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 36: return DXGI_FORMAT_R32_FLOAT;
    case 37: return DXGI_FORMAT_R32G32_FLOAT;
    case 38: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

Surface* g_present_source = nullptr;
uint32_t g_present_front_buffer = 0;  // its guest address

// The render target each colour resolve destination was last resolved from,
// and the one the presented front buffer came from (widescreen HUD draws).
std::unordered_map<uint32_t, Surface*> g_resolve_sources;
Surface* g_final_target = nullptr;

// --tenchuz_native_check_presents: each present copies its source (the native
// front buffer) and the presenter's output to a readback buffer of its frame
// slot, and the mean brightness of both, in four horizontal bands, is logged
// once the slot comes round again.
struct PresentCheck {
  ComPtr<ID3D12Resource> buffer;
  uint64_t size = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT source = {}, output = {};
  DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
  uint64_t frame = 0;  // 0: nothing pending
  uint32_t front_buffer = 0;
};
std::array<PresentCheck, kFramesInFlight> g_present_checks;

// Mean brightness (0-255) of four horizontal bands of a mapped copy.
std::array<float, 4> BandMeans(const uint8_t* data, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, DXGI_FORMAT format) {
  std::array<float, 4> sums{}, counts{};
  const uint32_t w = fp.Footprint.Width, h = fp.Footprint.Height;
  for (uint32_t y = 0; y < h; y += 8) {
    const uint8_t* row = data + fp.Offset + size_t(y) * fp.Footprint.RowPitch;
    const uint32_t band = std::min(3u, y * 4 / h);
    for (uint32_t x = 0; x < w; x += 8) {
      uint32_t v;
      std::memcpy(&v, row + 4 * x, 4);
      float r, g, b;
      if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        r = float(v & 0x3FF) / 4.0f, g = float((v >> 10) & 0x3FF) / 4.0f, b = float((v >> 20) & 0x3FF) / 4.0f;
      } else {
        r = float(v & 0xFF), g = float((v >> 8) & 0xFF), b = float((v >> 16) & 0xFF);
      }
      sums[band] += (r + g + b) / 3.0f;
      counts[band] += 1.0f;
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    sums[i] = counts[i] ? sums[i] / counts[i] : 0.0f;
  }
  return sums;
}

// Logs the check pending in this frame slot (its GPU work is done).
void LogPresentCheck(PresentCheck& check) {
  if (!check.frame || !check.buffer) {
    return;
  }
  uint8_t* data = nullptr;
  const D3D12_RANGE range = {0, SIZE_T(check.size)};
  if (SUCCEEDED(check.buffer->Map(0, &range, reinterpret_cast<void**>(&data)))) {
    const auto source = BandMeans(data, check.source, check.source_format);
    const auto output = BandMeans(data, check.output, rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat);
    REXLOG_INFO("tenchuz: NATIVE check frame {} fb {:08X}: source bands {:.0f} {:.0f} {:.0f} {:.0f}, output bands "
                "{:.0f} {:.0f} {:.0f} {:.0f}",
                check.frame, check.front_buffer, source[0], source[1], source[2], source[3], output[0], output[1],
                output[2], output[3]);
    const D3D12_RANGE none = {0, 0};
    check.buffer->Unmap(0, &none);
  }
  check.frame = 0;
}

bool RefreshGuestOutput(rex::ui::Presenter::GuestOutputRefreshContext& context) {
  auto& d3d12_context =
      static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
  ID3D12Resource* output = d3d12_context.resource_uav_capable();
  const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
  const uint32_t width = uint32_t(output_desc.Width), height = output_desc.Height;
  FrameSlot& slot = g_slots[g_frame % kFramesInFlight];

  // FXAA's source: the size of the output, replaced when that changes.
  const std::string& effect = REXCVAR_GET(tenchuz_native_post_effect);
  ID3D12PipelineState* fxaa_pso =
      effect == "fxaa" ? g_fxaa_pso.Get() : (effect == "fxaa_extreme" ? g_fxaa_extreme_pso.Get() : nullptr);
  if (fxaa_pso && g_fxaa_source) {
    const D3D12_RESOURCE_DESC desc = g_fxaa_source->GetDesc();
    if (desc.Width != width || desc.Height != height) {
      slot.retired.push_back(std::move(g_fxaa_source));  // the previous frame's present may still read it
    }
  }
  if (fxaa_pso && !g_fxaa_source) {
    Surface fxaa_source;
    if (CreateTexture(fxaa_source, kFxaaSourceFormat, width, height, false, false, true)) {
      g_fxaa_source = std::move(fxaa_source.resource);
      g_fxaa_source_state = fxaa_source.state;
      SetDebugName(g_fxaa_source.Get(), "tenchuz FXAA source");
    } else {
      fxaa_pso = nullptr;
    }
  }
  // Sharpening's source, likewise.
  const float sharpening = float(std::clamp(REXCVAR_GET(tenchuz_native_sharpening), 0.0, 1.0));
  bool cas = sharpening > 0.0f && g_cas_pso;
  if (cas && g_cas_source) {
    const D3D12_RESOURCE_DESC desc = g_cas_source->GetDesc();
    if (desc.Width != width || desc.Height != height) {
      slot.retired.push_back(std::move(g_cas_source));
    }
  }
  if (cas && !g_cas_source) {
    Surface cas_source;
    if (CreateTexture(cas_source, kFxaaSourceFormat, width, height, false, false, true)) {
      g_cas_source = std::move(cas_source.resource);
      g_cas_source_state = cas_source.state;
      SetDebugName(g_cas_source.Get(), "tenchuz sharpening source");
    } else {
      cas = false;
    }
  }

  slot.present_allocator->Reset();
  g_present_list->Reset(slot.present_allocator.Get(), nullptr);
  auto barrier = [](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
  };
  Surface& source = *g_present_source;
  // The blit writes the presenter's output, or FXAA's source, or sharpening's;
  // FXAA writes the output or sharpening's source.
  ID3D12Resource* filtered = cas ? g_cas_source.Get() : output;
  ID3D12Resource* blit_dest = fxaa_pso ? g_fxaa_source.Get() : filtered;
  D3D12_RESOURCE_BARRIER barriers[4];
  UINT barrier_count = 0;
  if (source.state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
    barriers[barrier_count++] =
        barrier(source.resource.Get(), source.state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    source.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  }
  barriers[barrier_count++] = barrier(output, rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (fxaa_pso && g_fxaa_source_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    barriers[barrier_count++] =
        barrier(g_fxaa_source.Get(), g_fxaa_source_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_fxaa_source_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  if (cas && g_cas_source_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
    barriers[barrier_count++] =
        barrier(g_cas_source.Get(), g_cas_source_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_cas_source_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  }
  g_present_list->ResourceBarrier(barrier_count, barriers);

  // kPresentDescriptors per frame slot: blit source SRV and destination UAV,
  // then FXAA's and sharpening's source SRV and destination UAV.
  const uint32_t first = uint32_t(g_frame % kFramesInFlight) * kPresentDescriptors;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_shader_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_shader_heap->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(first) * g_shader_size;
  gpu.ptr += UINT64(first) * g_shader_size;
  auto cpu_at = [&](uint32_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + SIZE_T(i) * g_shader_size}; };
  auto gpu_at = [&](uint32_t i) { return D3D12_GPU_DESCRIPTOR_HANDLE{gpu.ptr + UINT64(i) * g_shader_size}; };
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = source.format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  g_d3d->CreateShaderResourceView(source.resource.Get(), &srv, cpu_at(0));
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = (fxaa_pso || cas) ? kFxaaSourceFormat : rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  g_d3d->CreateUnorderedAccessView(blit_dest, nullptr, &uav, cpu_at(1));

  ID3D12DescriptorHeap* heaps[] = {g_shader_heap.Get()};
  g_present_list->SetDescriptorHeaps(1, heaps);
  g_present_list->SetComputeRootSignature(g_blit_root.Get());
  g_present_list->SetPipelineState(g_blit_pso.Get());
  g_present_list->SetComputeRootDescriptorTable(0, gpu);
  const uint32_t ramp_offset = uint32_t(g_frame % kFramesInFlight) * kGammaRampBytes;
  bool use_ramp;
  {
    std::lock_guard lock(g_gamma_mutex);
    use_ramp = g_gamma_ramp_set;
    std::memcpy(g_gamma_upload_map + ramp_offset, g_gamma_ramp.data(), kGammaRampBytes);
  }
  g_present_list->SetComputeRootShaderResourceView(2, g_gamma_upload->GetGPUVirtualAddress() + ramp_offset);
  const uint32_t constants[kBlitConstants] = {source.host_width(), source.host_height(), width, height,
                                              uint32_t(use_ramp), uint32_t(fxaa_pso != nullptr)};
  g_present_list->SetComputeRoot32BitConstants(1, kBlitConstants, constants, 0);
  const uint32_t op =
      g_draw_path ? g_draw_path->BeginOp(g_present_list.Get(), "present blit", source.width, source.height) : 0;
  g_present_list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  if (g_draw_path) {
    g_draw_path->EndOp(op);
  }

  if (fxaa_pso) {
    // Like the SDK's swap: FxaaConstants, 16x8 thread groups.
    const D3D12_RESOURCE_BARRIER to_source = barrier(g_fxaa_source.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_present_list->ResourceBarrier(1, &to_source);
    g_fxaa_source_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    srv.Format = kFxaaSourceFormat;
    g_d3d->CreateShaderResourceView(g_fxaa_source.Get(), &srv, cpu_at(2));
    uav.Format = cas ? kFxaaSourceFormat : rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
    g_d3d->CreateUnorderedAccessView(filtered, nullptr, &uav, cpu_at(3));
    g_present_list->SetComputeRootSignature(g_fxaa_root.Get());
    g_present_list->SetPipelineState(fxaa_pso);
    uint32_t fxaa_constants[4] = {width, height, 0, 0};
    const float size_inv[2] = {1.0f / float(width), 1.0f / float(height)};
    std::memcpy(&fxaa_constants[2], size_inv, sizeof(size_inv));
    g_present_list->SetComputeRoot32BitConstants(0, 4, fxaa_constants, 0);
    g_present_list->SetComputeRootDescriptorTable(1, gpu_at(3));
    g_present_list->SetComputeRootDescriptorTable(2, gpu_at(2));
    const uint32_t fxaa_op = g_draw_path ? g_draw_path->BeginOp(g_present_list.Get(), "present FXAA", width, height) : 0;
    g_present_list->Dispatch((width + 15) / 16, (height + 7) / 8, 1);
    if (g_draw_path) {
      g_draw_path->EndOp(fxaa_op);
    }
  }

  if (cas) {
    const D3D12_RESOURCE_BARRIER to_source = barrier(g_cas_source.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_present_list->ResourceBarrier(1, &to_source);
    g_cas_source_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    srv.Format = kFxaaSourceFormat;
    g_d3d->CreateShaderResourceView(g_cas_source.Get(), &srv, cpu_at(4));
    uav.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
    g_d3d->CreateUnorderedAccessView(output, nullptr, &uav, cpu_at(5));
    g_present_list->SetComputeRootSignature(g_blit_root.Get());
    g_present_list->SetPipelineState(g_cas_pso.Get());
    g_present_list->SetComputeRootDescriptorTable(0, gpu_at(4));
    g_present_list->SetComputeRootShaderResourceView(2, g_gamma_upload->GetGPUVirtualAddress() + ramp_offset);
    const float peak = -1.0f / (8.0f + (5.0f - 8.0f) * sharpening);
    uint32_t cas_constants[kCasConstants] = {width, height, 0};
    std::memcpy(&cas_constants[2], &peak, 4);
    g_present_list->SetComputeRoot32BitConstants(1, kCasConstants, cas_constants, 0);
    const uint32_t cas_op = g_draw_path ? g_draw_path->BeginOp(g_present_list.Get(), "present CAS", width, height) : 0;
    g_present_list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    if (g_draw_path) {
      g_draw_path->EndOp(cas_op);
    }
  }

  D3D12_RESOURCE_STATES output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  if (REXCVAR_GET(tenchuz_native_check_presents) &&
      (source.format == DXGI_FORMAT_R8G8B8A8_UNORM || source.format == DXGI_FORMAT_R10G10B10A2_UNORM)) {
    PresentCheck& check = g_present_checks[g_frame % kFramesInFlight];
    const D3D12_RESOURCE_DESC source_desc = source.resource->GetDesc();
    UINT64 source_size = 0, output_size = 0;
    g_d3d->GetCopyableFootprints(&source_desc, 0, 1, 0, &check.source, nullptr, nullptr, &source_size);
    const UINT64 output_offset = (source_size + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                                 ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    g_d3d->GetCopyableFootprints(&output_desc, 0, 1, output_offset, &check.output, nullptr, nullptr, &output_size);
    const uint64_t size = output_offset + output_size;
    if (!check.buffer || check.size < size) {
      D3D12_RESOURCE_DESC buffer = {};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = size;
      buffer.Height = 1;
      buffer.DepthOrArraySize = 1;
      buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      check.buffer.Reset();
      check.size = 0;
      if (SUCCEEDED(g_d3d->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                                   D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&check.buffer)))) {
        check.size = size;
      }
    }
    if (check.buffer) {
      D3D12_RESOURCE_BARRIER to_copy[2] = {
          barrier(source.resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COPY_SOURCE),
          barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
      g_present_list->ResourceBarrier(2, to_copy);
      D3D12_TEXTURE_COPY_LOCATION from = {source.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
      D3D12_TEXTURE_COPY_LOCATION to = {check.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
      to.PlacedFootprint = check.source;
      g_present_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      from.pResource = output;
      to.PlacedFootprint = check.output;
      g_present_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      const D3D12_RESOURCE_BARRIER source_back = barrier(source.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      g_present_list->ResourceBarrier(1, &source_back);
      output_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
      check.source_format = source.format;
      check.frame = g_frame;
      check.front_buffer = g_present_front_buffer;
    }
  }
  const D3D12_RESOURCE_BARRIER output_back =
      barrier(output, output_state, rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
  g_present_list->ResourceBarrier(1, &output_back);
  if (g_draw_path) {
    g_draw_path->ResolveProfile(g_present_list.Get());
  }
  g_present_list->Close();

  ID3D12CommandList* lists[] = {g_present_list.Get()};
  g_provider->GetDirectQueue()->ExecuteCommandLists(1, lists);
  // Like the SDK's swap: FXAA's output has more than 8 bits of precision.
  context.SetIs8bpc(fxaa_pso == nullptr);
  return true;
}

// Textures for the game's texture fetches: resolved render targets, looked up
// by guest address and size (else the latest resolve to that address).
class FrameHost final : public DrawHost {
 public:
  bool InCommandRing(uint32_t address) override { return gpu_observer::InCommandRing(address); }

  void WaitForPreviousFrame() override { WaitForFence(g_previous_frame_fence, "previous frame"); }

  void FlushAndWait() override {
    ID3D12CommandQueue* queue = g_provider->GetDirectQueue();
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const uint64_t value = ++g_fence_counter;
    queue->Signal(g_fence.Get(), value);
    WaitForFence(value, "mid-frame flush");
    FrameSlot& slot = g_slots[g_frame % kFramesInFlight];
    slot.allocator->Reset();
    g_list->Reset(slot.allocator.Get(), nullptr);
  }

  bool Find(ID3D12GraphicsCommandList* list, uint32_t address, uint32_t width, uint32_t height,
            Texture& out) override {
    (void)list;  // g_list, which Transition records on
    Surface* s = nullptr;
    auto it = g_textures.find(TextureKey(address, width, height));
    if (it != g_textures.end()) {
      s = &it->second;
    } else if (auto latest = g_latest_texture.find(address); latest != g_latest_texture.end()) {
      s = latest->second;
    }
    if (!s || !s->resource) {
      return false;
    }
    Transition(*s, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    out.resource = s->resource.Get();
    out.format = s->format;
    out.resolution_scaled = s->scale > 1;
    // Guest X/Y/Z/W -> host components: RGBA, R/B swapped by the resolve, or
    // the SDK's RRRR / RGGG for one- and two-component formats.
    switch (s->format) {
      case DXGI_FORMAT_R32_TYPELESS:  // resolved depth
        out.format = DXGI_FORMAT_R32_FLOAT;
        out.host_format_swizzle = 0;
        break;
      case DXGI_FORMAT_R32_FLOAT:
      case DXGI_FORMAT_R16_FLOAT:
      case DXGI_FORMAT_R8_UNORM:
        out.host_format_swizzle = 0;
        break;
      case DXGI_FORMAT_R16G16_FLOAT:
      case DXGI_FORMAT_R32G32_FLOAT:
      case DXGI_FORMAT_R8G8_UNORM:
        out.host_format_swizzle = 0 | (1 << 3) | (1 << 6) | (1 << 9);
        break;
      default:
        out.host_format_swizzle = s->red_blue_swapped ? (2 | (1 << 3) | (0 << 6) | (3 << 9))
                                                      : (0 | (1 << 3) | (2 << 6) | (3 << 9));
        break;
    }
    return true;
  }

  std::string ResolveOverlapping(uint32_t address, uint32_t size) override {
    for (const auto& [key, s] : g_textures) {
      const uint32_t dest = uint32_t(key);
      if (s.guest_bytes && address < dest + s.guest_bytes && dest < address + size) {
        return fmt::format("{:08X} {}x{} fmt {} ({} bytes, last frame {})", dest, s.width, s.height, int(s.format),
                           s.guest_bytes, s.resolved_frame);
      }
    }
    return {};
  }
};
FrameHost g_frame_host;

// D3D functions whose internal draws (clear quads, resolves, per-tile setup)
// the native renderer replaces with its own operations.
bool IsInternalDraw(uint32_t api_function) {
  switch (api_function) {
    case 0x8241B308:  // ClearF
    case kApiResolve:
    case kApiEndTiling:
    case 0x8241BBC0:
      return true;
    default:
      return false;
  }
}

// RB_COPY_DEST_INFO of the Resolve call in progress. Resolve keeps it in the
// device (+0x29A4) while it emits the copy and writes 0 there before returning,
// and in tiled passes its packets go to per-tile blocks the observer can't
// decode yet, so it's read from the device at the D3D calls Resolve makes.
bool g_in_resolve = false;
bool g_tiling = false;  // between BeginTiling and EndTiling
std::string g_resolve_area;  // the Resolve call's source rectangle and destination point, for the log
// The Resolve call's source rectangle (guest pixels; right/bottom 0 = the whole
// target) and destination point.
uint32_t g_rect_left = 0, g_rect_top = 0, g_rect_right = 0, g_rect_bottom = 0;
uint32_t g_dest_x = 0, g_dest_y = 0;

// The area a resolve copies, in host pixels of `src` (scaled like it), and
// whether it's the whole of both surfaces.
struct ResolveArea {
  uint32_t src_x = 0, src_y = 0, dst_x = 0, dst_y = 0, width = 0, height = 0;
  bool whole = true;
  uint32_t src_at() const { return src_x | (src_y << 16); }
  uint32_t dst_at() const { return dst_x | (dst_y << 16); }
  uint64_t key() const { return uint64_t(src_at()) | (uint64_t(dst_at()) << 32); }
};

// Clipped to the source and to a destination of dst_width x dst_height (guest pixels).
ResolveArea GetResolveArea(const Surface& src, uint32_t dst_width, uint32_t dst_height) {
  ResolveArea a;
  uint32_t left = 0, top = 0, right = src.width, bottom = src.height;
  if (g_rect_right > g_rect_left && g_rect_bottom > g_rect_top) {
    left = std::min(g_rect_left, src.width);
    top = std::min(g_rect_top, src.height);
    right = std::min(g_rect_right, src.width);
    bottom = std::min(g_rect_bottom, src.height);
  }
  // In a tiled pass the untiled target is complete before its tiles are
  // resolved (the clears between tiles wait, see OnClear), so a tile's part
  // copied to the same place is the whole target, copied once; the other
  // tiles' parts then match it and are skipped (AlreadyResolved).
  if (g_tiling && left == g_dest_x && top == g_dest_y) {
    left = top = 0;
    right = src.width;
    bottom = src.height;
    g_dest_x = g_dest_y = 0;
  }
  const uint32_t dx = std::min(g_dest_x, dst_width), dy = std::min(g_dest_y, dst_height);
  const uint32_t w = std::min(right - left, dst_width - dx), h = std::min(bottom - top, dst_height - dy);
  const uint32_t s = src.scale;
  a.src_x = left * s;
  a.src_y = top * s;
  a.dst_x = dx * s;
  a.dst_y = dy * s;
  a.width = w * s;
  a.height = h * s;
  a.whole = left == 0 && top == 0 && dx == 0 && dy == 0 && w == std::min(src.width, dst_width) &&
            h == std::min(src.height, dst_height);
  return a;
}
uint32_t g_copy_dest_info = 0;
uint32_t g_copy_control = 0;  // RB_COPY_CONTROL, the same way

void OnD3DCall(uint32_t function, uint8_t* base) {
  if (function == 0x8241BDE0) {  // SynchronizeToPresentationInterval: the frame's work is done
    tenchuz::OnPresentWait();
  }
  if (function == kApiEndTiling) {
    g_tiling = false;
  }
  if (g_in_resolve && function != kApiResolve && g_device) {
    if (const uint32_t info = Dev(base, kDevCopyDestInfo)) {
      g_copy_dest_info = info;
    }
    if (const uint32_t control = Dev(base, kDevCopyControl)) {
      g_copy_control = control;
    }
  }
}

// N4: called for every draw packet the D3D emits (live command-stream decoding).
void OnLiveDraw(const pm4::DrawInfo& draw, const pm4::Decoder& state, uint8_t* base,
                uint32_t api_function) {
  if (!Active() || !g_shaders || !g_device || IsInternalDraw(api_function)) {
    return;
  }
  ++g_frame_draws;
  if (Logging()) {
    ++g_draw_kinds[(uint64_t(api_function) << 16) | (draw.opcode << 1) | uint32_t(draw.predicated)];
  }
  DrawShaders shaders;
  if (!g_shaders->Prepare(draw, state, base, shaders)) {
    return;
  }
  ++g_frame_draws_ready;
  if (!g_draw_path) {
    return;
  }
  // The colour targets the draw writes (bound in the D3D too), and the depth
  // buffer if one is bound.
  const uint32_t color_mask = DrawPath::WrittenColorMask(state, base, g_device, shaders);
  Surface* colors[4] = {};
  bool any_color = false;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((color_mask >> (4 * i)) & 0xF) && Dev(base, kDevRenderTarget0 + 4 * i)) {
      colors[i] = ColorTarget(base, i);
      any_color = any_color || colors[i];
    }
  }
  if (!any_color && !Dev(base, kDevDepthStencil)) {
    return;
  }
  BeginRecording();
  FlushPendingClears();
  Surface* depth = Dev(base, kDevDepthStencil) ? DepthTarget(base) : nullptr;
  if (!any_color && !depth) {
    return;
  }
  DrawTarget target;
  target.color_mask = color_mask;
  target.color_info = Dev(base, kDevColorInfo);
  target.depth_info = Dev(base, kDevDepthInfo);
  target.color_surface = colors[0] ? colors[0]->id : 0;
  target.depth_surface = depth ? depth->id : 0;
  if (colors[0] && colors[0] == g_final_target) {
    target.hud_scale_x = widescreen::HudScaleX();
  }
  for (uint32_t i = 0; i < 4; ++i) {
    target.rtvs[i] = g_null_rtv;
    if (colors[i]) {
      ++colors[i]->writes;
      Transition(*colors[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
      target.color_formats[i] = colors[i]->format;
      target.rtvs[i] = colors[i]->view;
      target.width = colors[i]->host_width();
      target.height = colors[i]->host_height();
      target.samples = colors[i]->samples;
      target.resolution_scale = colors[i]->scale;
    }
  }
  if (depth) {
    ++depth->writes;
    Transition(*depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    target.depth_format = depth->format;
    target.dsv = depth->view;
    target.width = depth->host_width();
    target.height = depth->host_height();
    target.samples = depth->samples;
    target.resolution_scale = depth->scale;
  }
  g_draw_path->Draw(g_list.Get(), draw, state, base, g_device, shaders, target, &g_frame_host);
  if (!g_depth_stencil && g_draw_path->TakeStencilMissing()) {
    g_depth_stencil = true;  // depth buffers get stencil from the next frame (ApplyRenderSettings)
  }
}

// Process and GPU memory (QueryVideoMemoryInfo is per process).
ComPtr<IDXGIAdapter3> g_adapter;

std::string MemoryUsage() {
  std::string out;
  DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, non_local = {};
  if (g_adapter && SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
      SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local))) {
    out = fmt::format("GPU local {} of {} MB, non-local {} of {} MB", local.CurrentUsage >> 20, local.Budget >> 20,
                      non_local.CurrentUsage >> 20, non_local.Budget >> 20);
  }
  PROCESS_MEMORY_COUNTERS_EX process = {};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),
                           sizeof(process))) {
    out += fmt::format("{}process private {} MB", out.empty() ? "" : ", ", process.PrivateUsage >> 20);
  }
  return out;
}

// --- Frame dump (F11 / --tenchuz_native_dump_at_frame) -------------------------

std::atomic<bool> g_dump_requested{false};
int32_t g_dump_remaining = 0;  // frames still to dump after an F11

// Waits for the GPU, then reads back every resolved texture and writes it to
// logs/frame_dumps/<frame>_<address>_<w>x<h>.ppm (depth as grey .pgm).
void DumpResolvedTextures() {
  ID3D12CommandQueue* queue = g_provider->GetDirectQueue();
  auto wait_idle = [&] {
    const uint64_t value = ++g_fence_counter;
    queue->Signal(g_fence.Get(), value);
    WaitForFence(value, "frame dump");
  };
  wait_idle();
  FrameSlot& slot = g_slots[g_frame % kFramesInFlight];
  slot.allocator->Reset();
  g_list->Reset(slot.allocator.Get(), nullptr);
  struct Readback {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    uint32_t address, width, height;
    DXGI_FORMAT format;
  };
  std::vector<Readback> readbacks;
  for (auto& [key, s] : g_textures) {
    if (!s.resource || (s.format != DXGI_FORMAT_R8G8B8A8_UNORM && s.format != DXGI_FORMAT_R32_TYPELESS)) {
      continue;
    }
    Readback r;
    r.address = uint32_t(key);
    r.width = s.host_width();
    r.height = s.host_height();
    r.format = s.format;
    const D3D12_RESOURCE_DESC desc = s.resource->GetDesc();
    UINT64 total = 0;
    g_d3d->GetCopyableFootprints(&desc, 0, 1, 0, &r.footprint, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_d3d->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback,
                                              D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&r.buffer)))) {
      continue;
    }
    Transition(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to = {r.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    to.PlacedFootprint = r.footprint;
    D3D12_TEXTURE_COPY_LOCATION from = {s.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    g_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    readbacks.push_back(std::move(r));
  }
  g_list->Close();
  ID3D12CommandList* lists[] = {g_list.Get()};
  queue->ExecuteCommandLists(1, lists);
  wait_idle();
  std::error_code ec;
  const std::filesystem::path dir = "logs/frame_dumps";
  std::filesystem::create_directories(dir, ec);
  for (const Readback& r : readbacks) {
    uint8_t* data = nullptr;
    D3D12_RANGE range = {0, SIZE_T(r.footprint.Footprint.RowPitch) * r.height};
    if (FAILED(r.buffer->Map(0, &range, reinterpret_cast<void**>(&data)))) {
      continue;
    }
    const bool depth = r.format == DXGI_FORMAT_R32_TYPELESS;
    const auto path = dir / fmt::format("{}_{:08X}_{}x{}.{}", g_frame, r.address, r.width, r.height,
                                        depth ? "pgm" : "ppm");
    std::ofstream out(path, std::ios::binary);
    out << (depth ? "P5" : "P6") << "\n" << r.width << " " << r.height << "\n255\n";
    std::vector<uint8_t> row(size_t(r.width) * (depth ? 1 : 3));
    for (uint32_t y = 0; y < r.height; ++y) {
      const uint8_t* src = data + size_t(y) * r.footprint.Footprint.RowPitch;
      for (uint32_t x = 0; x < r.width; ++x) {
        if (depth) {
          float z;
          std::memcpy(&z, src + 4 * x, 4);
          row[x] = uint8_t(std::clamp(z, 0.0f, 1.0f) * 255.0f);
        } else {
          row[3 * x] = src[4 * x];
          row[3 * x + 1] = src[4 * x + 1];
          row[3 * x + 2] = src[4 * x + 2];
        }
      }
      out.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    r.buffer->Unmap(0, nullptr);
  }
  REXLOG_INFO("tenchuz: NATIVE frame {} dumped {} resolved textures to {}", g_frame, readbacks.size(),
              dir.string());
}

// RequestScreenshot: the next presented front buffer, with the display gamma
// ramp applied, as a PNG. Waits for the GPU (test runs only).
std::mutex g_screenshot_mutex;
std::filesystem::path g_screenshot_path;

void SaveScreenshot(Surface& s, const std::filesystem::path& path) {
  if (!s.resource || (s.format != DXGI_FORMAT_R8G8B8A8_UNORM && s.format != DXGI_FORMAT_B8G8R8A8_UNORM)) {
    REXLOG_WARN("tenchuz: screenshot {}: front buffer format {} not supported", path.string(), int(s.format));
    return;
  }
  ID3D12CommandQueue* queue = g_provider->GetDirectQueue();
  auto wait_idle = [&] {
    const uint64_t value = ++g_fence_counter;
    queue->Signal(g_fence.Get(), value);
    WaitForFence(value, "screenshot");
  };
  wait_idle();
  FrameSlot& slot = g_slots[g_frame % kFramesInFlight];
  slot.allocator->Reset();
  g_list->Reset(slot.allocator.Get(), nullptr);
  const D3D12_RESOURCE_DESC desc = s.resource->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 total = 0;
  g_d3d->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = total;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  if (FAILED(g_d3d->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
                                            &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&readback)))) {
    g_list->Close();
    return;
  }
  Transition(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION to = {readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  to.PlacedFootprint = footprint;
  D3D12_TEXTURE_COPY_LOCATION from = {s.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  g_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  g_list->Close();
  ID3D12CommandList* lists[] = {g_list.Get()};
  queue->ExecuteCommandLists(1, lists);
  wait_idle();
  uint8_t* data = nullptr;
  const D3D12_RANGE range = {0, SIZE_T(total)};
  if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&data)))) {
    return;
  }
  std::array<uint32_t, 256> ramp;
  bool use_ramp;
  {
    std::lock_guard lock(g_gamma_mutex);
    ramp = g_gamma_ramp;
    use_ramp = g_gamma_ramp_set;
  }
  const uint32_t width = footprint.Footprint.Width, height = footprint.Footprint.Height;
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  const bool bgra = s.format == DXGI_FORMAT_B8G8R8A8_UNORM;
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = data + size_t(y) * footprint.Footprint.RowPitch;
    uint8_t* dst = pixels.data() + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t r = src[4 * x], g = src[4 * x + 1], b = src[4 * x + 2];
      if (bgra) std::swap(r, b);
      if (use_ramp) {
        r = uint8_t(((ramp[r] >> 20) & 0x3FF) >> 2);
        g = uint8_t(((ramp[g] >> 10) & 0x3FF) >> 2);
        b = uint8_t((ramp[b] & 0x3FF) >> 2);
      }
      dst[4 * x] = r, dst[4 * x + 1] = g, dst[4 * x + 2] = b, dst[4 * x + 3] = 255;
    }
  }
  const D3D12_RANGE none = {0, 0};
  readback->Unmap(0, &none);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  const bool ok = WritePng(path, width, height, pixels.data(), width * 4);
  REXLOG_INFO("tenchuz: screenshot frame {} -> {}{}", g_frame, path.string(), ok ? "" : " (failed)");
}

// A changed resolution scale or MSAA count, between frames: the render
// targets and resolve destinations are made again at the new size and sample
// count (the game draws and resolves them anew each frame), with that
// scale's shader cache (and pipelines, made the first time it's used).
Surface g_kept_present;  // what the presenter still shows, until the next present
uint32_t g_applied_msaa = 0;
bool g_applied_stencil = false;

void ApplyRenderSettings() {
  const uint32_t scale = uint32_t(std::clamp(REXCVAR_GET(tenchuz_native_resolution_scale), 1, 4));
  const uint32_t msaa = REXCVAR_GET(tenchuz_native_msaa_samples) >= 4 ? 4 : 0;
  if (scale == g_scale && msaa == g_applied_msaa && g_depth_stencil == g_applied_stencil) {
    return;
  }
  WaitForFence(g_fence_counter, "render settings change");
  if (g_present_source) {
    g_kept_present = std::move(*g_present_source);
    g_present_source = &g_kept_present;
  }
  g_pending_clear = {};
  g_final_target = nullptr;
  g_resolve_sources.clear();
  g_latest_texture.clear();
  g_textures.clear();
  g_targets.clear();
  if (scale != g_scale) {
    g_scale = scale;
    if (!g_shader_caches[g_scale]) {
      g_shader_caches[g_scale] = std::make_unique<ShaderCache>(g_provider->GetAdapterVendorID(), g_scale);
    }
    g_shaders = g_shader_caches[g_scale].get();
  }
  g_applied_msaa = msaa;
  g_applied_stencil = g_depth_stencil;
  REXLOG_INFO("tenchuz: native resolution scale {}x, MSAA {}, depth {}", g_scale,
              msaa ? (g_msaa_4x ? "4x" : "4x unsupported by this GPU") : "the game's",
              g_depth_stencil ? "with stencil (the game used it)" : "32-bit");
}

// How long frames stay on screen, in display refreshes, while the game's
// vblanks follow the display's: each frame is counted from the display vblank
// it's handed over in to the next frame's. Logged every 600 frames; at 60 fps
// on a 180 Hz display every frame should stay 3.

void CountShownFor() {
  static uint64_t last = 0;
  static uint32_t counts[6] = {};  // 1..5 refreshes, 6+
  static uint32_t frames = 0;
  const uint64_t now = native::DisplayVblanks();
  if (now && last && now > last) {
    ++counts[std::min<uint64_t>(now - last, 6) - 1];
    if (++frames == 600) {
      REXLOG_INFO("tenchuz: NATIVE frames shown for 1/2/3/4/5/6+ refreshes: {}/{}/{}/{}/{}/{}", counts[0],
                  counts[1], counts[2], counts[3], counts[4], counts[5]);
      frames = 0;
      std::fill(std::begin(counts), std::end(counts), 0);
    }
  }
  last = now;
}

void EndFrameAndPresent(uint32_t front_buffer, uint32_t width, uint32_t height, bool present) {
  ID3D12CommandQueue* queue = g_provider->GetDirectQueue();
  if (g_recording) {
    FlushPendingClears();
    if (g_draw_path) {
      g_draw_path->EndFrame(g_list.Get(), g_frame);
    }
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    g_recording = false;
  }
  const int32_t dump_at = REXCVAR_GET(tenchuz_native_dump_at_frame);
  const int32_t dump_frames = std::max(1, REXCVAR_GET(tenchuz_native_dump_frames));
  if (g_dump_requested.exchange(false)) {
    // F11: this frame and the next tenchuz_native_dump_frames - 1, logged
    // from the next one (this frame's operations have already gone by).
    g_dump_remaining = dump_frames;
    g_log_frames = std::max(g_log_frames, dump_frames);
  }
  if (g_dump_remaining > 0 ||
      (dump_at > 0 && g_frame >= uint64_t(dump_at) && g_frame < uint64_t(dump_at) + uint64_t(dump_frames))) {
    if (g_dump_remaining > 0) {
      --g_dump_remaining;
    }
    DumpResolvedTextures();
  }
  auto it = g_latest_texture.find(front_buffer);
  {
    std::filesystem::path shot;
    {
      std::lock_guard lock(g_screenshot_mutex);
      if (present && it != g_latest_texture.end()) {
        shot = std::exchange(g_screenshot_path, {});
      } else if (!g_screenshot_path.empty() && g_frame % 30 == 0) {
        REXLOG_INFO("tenchuz: screenshot waiting: frame {} {} front buffer {:08X} {}", g_frame,
                    present ? "present" : "not presented", front_buffer,
                    it != g_latest_texture.end() ? "native" : "not resolved natively");
      }
    }
    if (!shot.empty()) {
      SaveScreenshot(*it->second, shot);
    }
  }
  if (!g_recording) {
    WaitForSlot();  // the present list reuses this slot's allocator
  }
  LogPresentCheck(g_present_checks[g_frame % kFramesInFlight]);
  const DrawStats draw_stats = g_draw_path ? g_draw_path->TakeFrameStats() : DrawStats{};
  if (g_draw_path) g_last_depth_draws.store(int(draw_stats.depth_draws), std::memory_order_relaxed);
  const gpu_observer::ResyncStats rs = gpu_observer::TakeResyncStats();
  {
    // Frames slowed by new shaders: translation and pipeline creation.
    static double translated_before = 0;
    static const ShaderCache* stats_cache = nullptr;
    if (stats_cache != g_shaders) {  // a resolution scale change: that cache's own total
      stats_cache = g_shaders;
      translated_before = g_shaders ? g_shaders->stats().translate_ms : 0;
    }
    const double translated = g_shaders ? g_shaders->stats().translate_ms : 0;
    const double translate_ms = translated - translated_before;
    translated_before = translated;
    if (translate_ms + draw_stats.pipeline_ms >= 4.0) {
      REXLOG_INFO("tenchuz: NATIVE frame {}: {:.1f} ms translating shaders, {} pipelines ({} from the cache) in "
                  "{:.1f} ms",
                  g_frame, translate_ms, draw_stats.pipelines_created, draw_stats.pipelines_loaded,
                  draw_stats.pipeline_ms);
    }
  }
  if (draw_stats.stale_pages) {  // --tenchuz_native_recheck_pages
    REXLOG_INFO("tenchuz: NATIVE frame {}: {} static pages changed after their upload", g_frame,
                draw_stats.stale_pages);
  }
  if (rs.gap || rs.backward || rs.stuck > rs.resumed) {
    REXLOG_INFO("tenchuz: NATIVE frame {} decode skipped packets: {} gap ({} KB), {} backward, {} stuck, "
                "{} segment",
                g_frame, rs.gap, rs.gap_kb, rs.backward, rs.stuck, rs.segment);
  }
  if (Logging() && g_shaders) {
    const ShaderStats& st = g_shaders->stats();
    REXLOG_INFO("tenchuz: NATIVE draws {} ({} with translated shaders); shaders {} unique, {} translations, {} failed; "
                "{} copy-mode draws skipped in total",
                g_frame_draws, g_frame_draws_ready, st.unique_shaders, st.translations, st.failures,
                st.copy_mode_draws);
    REXLOG_INFO("tenchuz: NATIVE issued {} draws ({} to several targets, {} with stencil, {} kept at 16:9); skipped "
                "{} primitive type (types mask {:X}), {} pipeline, {} culled, {} other; uploaded {} KB ({} unchanged pages "
                "kept), {} KB per draw (dynamic); {} reset indices converted; {} waits for the previous frame",
                draw_stats.issued, draw_stats.mrt_draws, draw_stats.stencil_draws, draw_stats.hud_draws,
                draw_stats.skipped_primitive,
                draw_stats.skipped_primitive_types, draw_stats.skipped_pipeline, draw_stats.skipped_culled,
                draw_stats.skipped_other, draw_stats.uploaded_kb, draw_stats.watched_pages, draw_stats.arena_kb,
                draw_stats.reset_converted, draw_stats.previous_frame_waits);
    REXLOG_INFO("tenchuz: NATIVE decode: {} kicks, {} segment switches, {} gap ({} KB), {} backward, {} stuck "
                "({} resumed)",
                rs.kicks, rs.segment, rs.gap, rs.gap_kb, rs.backward, rs.stuck, rs.resumed);
    REXLOG_INFO("tenchuz: NATIVE textures: {} resolved, {} guest, {} placeholder; loaded {} ({} KB), "
                "hashed {} KB, {} written pages, {} unsupported, {} cached",
                draw_stats.textures_resolved, draw_stats.textures_guest, draw_stats.textures_missing,
                draw_stats.texture_loads, draw_stats.texture_loaded_kb, draw_stats.texture_hashed_kb,
                draw_stats.texture_written_pages, draw_stats.texture_unsupported, draw_stats.textures_cached);
    const uint32_t* p = draw_stats.placeholders;
    REXLOG_INFO("tenchuz: NATIVE placeholders: {} not a texture, {} no address, {} unsupported format, {} load "
                "failed, {} no signed view, {} stacked 2D as 3D, {} other dimension; {} unused signed bindings",
                p[kPlaceholderNotTexture], p[kPlaceholderNoAddress], p[kPlaceholderUnsupported],
                p[kPlaceholderLoadFailed], p[kPlaceholderSignedView], p[kPlaceholderStacked3D],
                p[kPlaceholderDimension], draw_stats.signed_unused);
  }
  if (Logging()) {
    REXLOG_INFO("tenchuz: NATIVE memory: {}", MemoryUsage());
  }
  if (Logging() && g_draw_path && REXCVAR_GET(tenchuz_native_log_draws)) {
    g_draw_path->LogFrameDraws(g_frame);
  }
  if (Logging() && !g_draw_kinds.empty()) {
    std::string kinds;
    for (const auto& [key, n] : g_draw_kinds) {
      kinds += fmt::format("{:08X}/{:02X}{} x{}  ", uint32_t(key >> 16), (key >> 1) & 0x7F, (key & 1) ? "p" : "", n);
    }
    REXLOG_INFO("tenchuz: NATIVE draws by function/opcode: {}", kinds);
  }
  g_draw_kinds.clear();
  if (REXCVAR_GET(tenchuz_native_log_presents)) {
    const Surface* shown = it != g_latest_texture.end() ? it->second : nullptr;
    REXLOG_INFO("tenchuz: NATIVE frame {} {} {:08X}: {}; {} draws ({} issued), {} resolves ({} repeats skipped), "
                "{} clears",
                g_frame, present ? "present" : "end", front_buffer,
                shown ? fmt::format("resolved {} frames ago", g_frame - shown->resolved_frame)
                      : std::string("not resolved natively"),
                g_frame_draws, draw_stats.issued, g_frame_resolves, g_frame_resolves_skipped, g_frame_clears);
  }
  g_frame_draws = g_frame_draws_ready = 0;
  g_frame_resolves = g_frame_clears = g_frame_resolves_skipped = 0;
  if (Logging()) {
    REXLOG_INFO("tenchuz: NATIVE {} front buffer {:08X} {}x{}: {}", present ? "present" : "frame end",
                front_buffer, width, height,
                it != g_latest_texture.end() ? "native texture" : "not resolved natively");
  }
  // The next frame's 2D draws on this target keep 16:9 with a wider aspect.
  if (auto source = g_resolve_sources.find(front_buffer); source != g_resolve_sources.end()) {
    g_final_target = source->second;
  }
  if (present && it != g_latest_texture.end()) {
    g_present_source = it->second;
    g_present_front_buffer = front_buffer;
    // At the front buffer's resolution scale and the aspect ratio the game
    // renders for (widescreen.h): the presenter fits it to the window.
    const uint32_t scale = g_present_source->scale;
    uint32_t aspect_x, aspect_y;
    widescreen::AspectFraction(aspect_x, aspect_y);
    g_presenter->RefreshGuestOutput(width * scale, height * scale, aspect_x, aspect_y, RefreshGuestOutput);
    CountShownFor();
  }
  FrameSlot& slot = g_slots[g_frame % kFramesInFlight];
  slot.fence_value = ++g_fence_counter;
  queue->Signal(g_fence.Get(), slot.fence_value);
  if (g_log_frames > 0 && Logging()) {
    --g_log_frames;
  }
  ++g_frame;
  ApplyRenderSettings();
  // --tenchuz_native_trace_syncs_at: the command-stream decoding of the next frame.
  const int32_t trace_at = REXCVAR_GET(tenchuz_native_trace_syncs_at);
  const bool trace = trace_at > 0 && g_frame >= uint64_t(trace_at) &&
                     g_frame < uint64_t(trace_at) + uint64_t(std::max(1, REXCVAR_GET(tenchuz_native_trace_syncs_frames)));
  if (trace) {
    REXLOG_INFO("tenchuz: observe trace of native frame {}", g_frame);
  }
  gpu_observer::SetSyncTrace(trace);
}

bool CreateResources() {
  g_d3d = g_provider->GetDevice();
  for (FrameSlot& slot : g_slots) {
    if (FAILED(g_d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&slot.allocator))) ||
        FAILED(g_d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&slot.present_allocator)))) {
      return false;
    }
  }
  if (FAILED(g_d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_slots[0].allocator.Get(),
                                      nullptr, IID_PPV_ARGS(&g_list))) ||
      FAILED(g_list->Close()) ||
      FAILED(g_d3d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      g_slots[0].present_allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&g_present_list))) ||
      FAILED(g_present_list->Close()) ||
      FAILED(g_d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
    return false;
  }
  g_fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  SetDebugName(g_list.Get(), "tenchuz native frame list");
  SetDebugName(g_present_list.Get(), "tenchuz native present list");
  SetDebugName(g_fence.Get(), "tenchuz native fence");

  D3D12_DESCRIPTOR_HEAP_DESC heap = {};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = kRtvCapacity;
  if (FAILED(g_d3d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_rtv_heap)))) return false;
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heap.NumDescriptors = kDsvCapacity;
  if (FAILED(g_d3d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_dsv_heap)))) return false;
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = kFramesInFlight * (kPresentDescriptors + kResolveDescriptors);
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(g_d3d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_shader_heap)))) return false;
  SetDebugName(g_shader_heap.Get(), "tenchuz present/resolve descriptor heap");
  g_rtv_size = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  {
    g_null_rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_null_rtv.ptr += SIZE_T(g_rtv_used++) * g_rtv_size;
    D3D12_RENDER_TARGET_VIEW_DESC null_rtv = {};
    null_rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g_d3d->CreateRenderTargetView(nullptr, &null_rtv, g_null_rtv);
  }
  g_dsv_size = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  g_shader_size = g_d3d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  return g_fence_event && CreateBlit();
}

// --- GPU hang diagnostics ------------------------------------------------------

HANDLE g_removed_event = nullptr;
HANDLE g_removed_wait = nullptr;
void (*g_previous_abort_handler)(int) = SIG_DFL;
std::atomic<bool> g_removal_reported{false};

std::string Narrow(const wchar_t* text) {
  std::string out;
  for (; text && *text; ++text) {
    out += *text < 128 ? char(*text) : '?';
  }
  return out;
}

const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
  static constexpr const char* kNames[] = {
      "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect",
      "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
      "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
      "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame",
      "ProcessFrames", "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
      "WriteBufferImmediate"};
  return uint32_t(op) < std::size(kNames) ? kNames[op] : "?";
}

// DRED (--tenchuz_dred, or the SDK's --d3d12_debug): the command lists the GPU
// had not finished, with the operations around the last one completed, and
// the allocations at a page fault's address.
void LogDred() {
  ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(g_d3d->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
  if (FAILED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
    REXLOG_ERROR("tenchuz: DRED off (start with --tenchuz_dred=true for command-list breadcrumbs)");
  } else {
    uint32_t lists = 0, unfinished = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
      ++lists;
      const uint32_t done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
      if (done >= node->BreadcrumbCount || ++unfinished > 12) {
        continue;
      }
      std::string ops;
      for (uint32_t i = done > 4 ? done - 4 : 0; node->pCommandHistory && i < std::min(node->BreadcrumbCount, done + 6);
           ++i) {
        ops += fmt::format(" {}{}{}", i == done ? "[" : "", BreadcrumbOpName(node->pCommandHistory[i]),
                           i == done ? "]" : "");
      }
      REXLOG_ERROR("tenchuz: DRED list '{}' (queue '{}'): {} of {} operations completed:{}",
                   Narrow(node->pCommandListDebugNameW), Narrow(node->pCommandQueueDebugNameW), done,
                   node->BreadcrumbCount, ops);
    }
    REXLOG_ERROR("tenchuz: DRED: {} command lists tracked, {} unfinished", lists, unfinished);
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA) {
    REXLOG_ERROR("tenchuz: DRED page fault at GPU VA {:016X}", fault.PageFaultVA);
    auto list = [](const char* what, const D3D12_DRED_ALLOCATION_NODE* node) {
      for (uint32_t n = 0; node && n < 16; node = node->pNext, ++n) {
        REXLOG_ERROR("tenchuz: DRED   {} allocation '{}' (type {})", what, Narrow(node->ObjectNameW),
                     int(node->AllocationType));
      }
    };
    list("existing", fault.pHeadExistingAllocationNode);
    list("recently freed", fault.pHeadRecentFreedAllocationNode);
  }
}

void ReportIfDeviceRemoved() {
  if (!g_d3d) {
    return;
  }
  const HRESULT reason = g_d3d->GetDeviceRemovedReason();
  if (SUCCEEDED(reason) || g_removal_reported.exchange(true)) {
    return;
  }
  const uint64_t waiting = g_waiting_fence.load();
  const char* waiting_for = g_waiting_for.load();
  REXLOG_ERROR("tenchuz: N4 device removed ({:08X}) at native frame {}: render thread {}; frame fences {} / {}, "
               "last signalled {}; {}",
               uint32_t(reason), g_frame,
               waiting ? fmt::format("waiting for fence {} ({})", waiting, waiting_for ? waiting_for : "?")
                       : std::string("not waiting on the GPU"),
               g_slots[0].fence_value, g_slots[1].fence_value, g_fence_counter, MemoryUsage());
  if (g_draw_path) {
    g_draw_path->ReportHang();
  }
  LogDred();
  if (auto logger = rex::GetLogger()) {
    logger->flush();
  }
}

// Our fence completes with UINT64_MAX when the device is removed.
void CALLBACK OnDeviceRemoved(PVOID, BOOLEAN) {
  ReportIfDeviceRemoved();
}

// The SDK aborts right after logging a device removal, possibly before the
// wait callback above has run.
void OnAbort(int signal) {
  ReportIfDeviceRemoved();
  if (g_previous_abort_handler != SIG_DFL && g_previous_abort_handler != SIG_IGN) {
    g_previous_abort_handler(signal);
  }
}

void WatchForDeviceRemoval() {
  g_removed_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (g_removed_event && SUCCEEDED(g_fence->SetEventOnCompletion(UINT64_MAX, g_removed_event))) {
    RegisterWaitForSingleObject(&g_removed_wait, g_removed_event, OnDeviceRemoved, nullptr, INFINITE,
                                WT_EXECUTEONLYONCE);
  }
  g_previous_abort_handler = std::signal(SIGABRT, OnAbort);
}

}  // namespace

void EnableDeviceDiagnostics() {
  if (!REXCVAR_GET(tenchuz_dred)) {
    return;
  }
  using GetDebugInterfaceFn = HRESULT(WINAPI*)(REFIID, void**);
  HMODULE d3d12 = LoadLibraryA("d3d12.dll");
  auto get_debug_interface =
      d3d12 ? reinterpret_cast<GetDebugInterfaceFn>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
  if (!get_debug_interface || FAILED(get_debug_interface(IID_PPV_ARGS(&dred)))) {
    REXLOG_WARN("tenchuz: DRED unavailable");
    return;
  }
  dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  REXLOG_INFO("tenchuz: DRED enabled (command-list breadcrumbs, page faults)");
}

void Initialize(rex::Runtime* runtime) {
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  if (!graphics || !graphics->presenter() || !graphics->provider()) {
    REXLOG_WARN("tenchuz: native renderer unavailable (no presenter/provider)");
    return;
  }
  g_presenter = graphics->presenter();
  g_provider = static_cast<const rex::ui::d3d12::D3D12Provider*>(graphics->provider());
  g_ready = CreateResources();
  if (ComPtr<IDXGIFactory4> factory;
      g_ready && SUCCEEDED(g_provider->GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory)))) {
    factory->EnumAdapterByLuid(g_d3d->GetAdapterLuid(), IID_PPV_ARGS(&g_adapter));
  }
  if (g_ready) {
    g_scale = uint32_t(std::clamp(REXCVAR_GET(tenchuz_native_resolution_scale), 1, 4));
    if (g_scale > 1) {
      REXLOG_INFO("tenchuz: native resolution scale {}x", g_scale);
    }
    g_applied_msaa = REXCVAR_GET(tenchuz_native_msaa_samples) >= 4 ? 4 : 0;
    if (g_applied_msaa) {
      REXLOG_INFO("tenchuz: native MSAA: 4x for the game's multisampled targets{}",
                  g_msaa_4x ? "" : " unsupported by this GPU");
    }
    g_shader_caches[g_scale] = std::make_unique<ShaderCache>(g_provider->GetAdapterVendorID(), g_scale);
    g_shaders = g_shader_caches[g_scale].get();
    g_draw_path = std::make_unique<DrawPath>();
    if (g_draw_path->Initialize(g_d3d, runtime->memory(),
                                g_cache_dir.empty() ? std::filesystem::path() : g_cache_dir / "tenchuz_pipelines.bin")) {
      UINT64 frequency = 0;
      g_provider->GetDirectQueue()->GetTimestampFrequency(&frequency);
      g_draw_path->SetTimestampFrequency(frequency);
    } else {
      REXLOG_WARN("tenchuz: native draw path unavailable (D3D12 resource creation failed)");
      g_draw_path.reset();
    }
    WatchForDeviceRemoval();
  }
  REXLOG_INFO("tenchuz: native renderer {} (F10 toggles native output)",
              g_ready ? "ready" : "failed to create D3D12 resources");
  g_log_frames = REXCVAR_GET(tenchuz_native_log_frames);
  if (g_ready && (REXCVAR_GET(tenchuz_native_output) || NativeGpu())) {
    TogglePresentNative();
  }
}

bool PresentNative() {
  return g_present_native.load(std::memory_order_relaxed);
}

void RequestScreenshot(const std::filesystem::path& path) {
  std::lock_guard lock(g_screenshot_mutex);
  g_screenshot_path = path;
}

void RequestGpuProfile() {
  if (g_draw_path) {
    g_draw_path->RequestProfile();
  }
}

void SetCacheDir(const std::filesystem::path& dir) { g_cache_dir = dir; }

void RequestFrameDump() {
  g_dump_requested.store(true);
}

void TogglePresentNative() {
  // SDK key binds fire on keyboard auto-repeat too: holding F10 must not flicker.
  static std::chrono::steady_clock::time_point last;
  const auto time = std::chrono::steady_clock::now();
  if (time - last < std::chrono::milliseconds(250)) {
    last = time;
    return;
  }
  last = time;
  const bool now = !g_present_native.load();
  if (!now && NativeGpu()) {
    REXLOG_INFO("tenchuz: no GPU emulation to switch to (--tenchuz_native_gpu)");
    return;
  }
  if (now && !g_ready) {
    REXLOG_WARN("tenchuz: native renderer not ready");
    return;
  }
  g_present_native.store(now);
  REXLOG_INFO("tenchuz: presenting {} output", now ? "native" : "emulator");
  if (now && g_log_frames <= 0) {
    g_log_frames = 3;  // log the first native frames after each switch
  }
}

int LastFrameDepthDraws() { return Active() ? g_last_depth_draws.load(std::memory_order_relaxed) : -1; }

void OnSwap(uint8_t* base, uint32_t device) {
  g_base.store(base, std::memory_order_relaxed);
  g_device = device;
  // The native renderer reads per-draw GPU state from the live-decoded command stream.
  gpu_observer::SetLiveDecoding(Active(), &OnLiveDraw);
  gpu_observer::SetCallCallback(&OnD3DCall);
}

void DumpObject(uint8_t* base, const char* what, uint32_t address) {
  if (address < 0x10000) {
    return;
  }
  std::string words;
  for (uint32_t i = 0; i < 20; ++i) {
    words += fmt::format("{:08X} ", REX_LOAD_U32(address + 4 * i));
  }
  REXLOG_INFO("tenchuz: NATIVE {} {:08X}: {}", what, address, words);
}

void OnClear(uint8_t* base, uint32_t flags, uint32_t color_ptr, double z, uint32_t stencil, uint32_t caller) {
  if (!Active() || !g_device) {
    return;
  }
  BeginRecording();
  FlushPendingClears();
  // In a tiled pass the game resolves each tile's part of a target (a source
  // rectangle) and clears between tiles; the hardware draws the next tile into
  // the cleared EDRAM before resolving it. We render once, untiled, so such a
  // clear would wipe the parts still to be resolved (the shadow map's right
  // half). Clears there wait for the next draw or clear.
  if (g_tiling) {
    if ((flags & 0xF) && Dev(base, kDevRenderTarget0)) {
      if (Surface* s = ColorTarget(base, 0)) {
        g_pending_clear.color = s;
        ClearColorArgument(base, color_ptr, g_pending_clear.rgba);
      }
    }
    if (flags & 0x30) {
      if (Surface* d = DepthTarget(base)) {
        g_pending_clear.depth = d;
        g_pending_clear.z = float(std::clamp(z, 0.0, 1.0));
        g_pending_clear.stencil = uint8_t(stencil);
        g_pending_clear.depth_flags = D3D12_CLEAR_FLAGS(((flags & 0x10) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                        ((flags & 0x20) ? D3D12_CLEAR_FLAG_STENCIL : 0));
      }
    }
    if (Logging()) {
      REXLOG_INFO("tenchuz: NATIVE clear flags {:X} in a tiled pass from {:08X}: waits for the next draw", flags,
                  caller);
    }
    return;
  }
  if (Logging()) {
    DumpObject(base, "RT0 surface", Dev(base, kDevRenderTarget0));
    DumpObject(base, "depth surface", Dev(base, kDevDepthStencil));
    REXLOG_INFO("tenchuz: NATIVE viewport {} {} {}x{}  surface_info {:08X}", Dev(base, kDevViewport),
                Dev(base, kDevViewport + 4), Dev(base, kDevViewport + 8), Dev(base, kDevViewport + 12),
                Dev(base, kDevSurfaceInfo));
  }
  if (flags & 0xF) {
    float color[4];
    ClearColorArgument(base, color_ptr, color);
    for (uint32_t rt = 0; rt < 4; ++rt) {
      if ((flags & (1u << rt)) && Dev(base, kDevRenderTarget0 + 4 * rt)) {
        if (Surface* s = ColorTarget(base, rt)) {
          ClearColor(*s, color);
          if (Logging()) {
            REXLOG_INFO("tenchuz: NATIVE clear RT{} edram {:03X} {}x{} fmt {} color ({:.2f},{:.2f},{:.2f},{:.2f}) "
                        "from {:08X} (colour at {:08X}, flags {:X})",
                        rt, (rt == 0 ? Dev(base, kDevColorInfo) : Dev(base, kDevColor1Info + 4 * (rt - 1))) & 0xFFF,
                        s->width, s->height, int(s->format), color[0], color[1], color[2], color[3], caller,
                        color_ptr, flags);
          }
        }
      }
    }
  }
  D3D12_CLEAR_FLAGS ds = D3D12_CLEAR_FLAGS(((flags & 0x10) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                           ((flags & 0x20) ? D3D12_CLEAR_FLAG_STENCIL : 0));
  if (ds) {
    if (Surface* s = DepthTarget(base)) {
      ClearDepth(*s, float(std::clamp(z, 0.0, 1.0)), uint8_t(stencil), ds);
      if (Logging()) {
        REXLOG_INFO("tenchuz: NATIVE clear depth edram {:03X} {}x{} z {:.3f} from {:08X} in {:08X}",
                    Dev(base, kDevDepthInfo) & 0xFFF, s->width, s->height, z, caller, gpu_observer::TopFunction());
      }
    }
  }
}

// D3D gamma ramp -> display LUT (sub_8242A4F8). `ramp` is a D3DGAMMARAMP:
// 256 red, 256 green, 256 blue big-endian WORDs, of which the D3D keeps the
// top 10 bits. The D3D sets a default ramp at startup, before native decoding
// starts, so this is captured from the call rather than the command stream.
void OnGammaRamp(uint8_t* base, uint32_t ramp) {
  if (!ramp) {
    return;
  }
  std::array<uint32_t, 256> table;
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t red = REX_LOAD_U16(ramp + 2 * i) >> 6;
    const uint32_t green = REX_LOAD_U16(ramp + 512 + 2 * i) >> 6;
    const uint32_t blue = REX_LOAD_U16(ramp + 1024 + 2 * i) >> 6;
    table[i] = (red << 20) | (green << 10) | blue;
  }
  {
    std::lock_guard lock(g_gamma_mutex);
    g_gamma_ramp = table;
    g_gamma_ramp_set = true;
  }
  auto red = [&](uint32_t i) { return (table[i] >> 20) & 0x3FF; };
  REXLOG_INFO("tenchuz: NATIVE gamma ramp (red, of 1023): 0:{} 32:{} 64:{} 128:{} 192:{} 255:{}", red(0), red(32),
              red(64), red(128), red(192), red(255));
}

void OnSetRenderTarget(uint8_t* base, uint32_t index, uint32_t surface) {
  if (!g_device || index != 0 || !surface) {
    return;
  }
  g_bound_width = Dev(base, kDevViewport + 8);
  g_bound_height = Dev(base, kDevViewport + 12);
}

// BeginTiling(flags, count, tile rects (D3DRECT: left, top, right, bottom)):
// the union of the tiles is the full frame we render untiled.
void OnBeginTiling(uint8_t* base, uint32_t count, uint32_t rects) {
  if (g_recording) {
    FlushPendingClears();
  }
  if (!rects || !count || count > 16) {
    return;
  }
  uint32_t right = 0, bottom = 0;
  for (uint32_t i = 0; i < count; ++i) {
    right = std::max(right, REX_LOAD_U32(rects + 16 * i + 8));
    bottom = std::max(bottom, REX_LOAD_U32(rects + 16 * i + 12));
  }
  if (right && bottom && right <= 8192 && bottom <= 8192) {
    g_bound_width = right;
    g_bound_height = bottom;
  }
  g_tiling = count > 1;
  if (Logging()) {
    REXLOG_INFO("tenchuz: NATIVE begin tiling {} tiles -> {}x{}", count, right, bottom);
  }
}

// Whether the GPU can MSAA-resolve `format` itself (cached per format).
bool HardwareResolvable(DXGI_FORMAT format) {
  static std::unordered_map<uint32_t, bool> known;
  auto [it, added] = known.try_emplace(uint32_t(format), false);
  if (added) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format};
    it->second = SUCCEEDED(g_d3d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                 (support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE);
  }
  return it->second;
}

// Whether `dst` already holds this resolve: a tiled pass resolves once per
// tile, and the untiled target we render is copied whole each time, so the
// repeats (same frame, same source unchanged since, same settings) are
// skipped. Records the resolve otherwise.
bool AlreadyResolved(Surface& dst, const Surface& src, uint64_t settings) {
  if (dst.resolved_frame == g_frame && dst.resolved_from == &src && dst.resolved_writes == src.writes &&
      dst.resolved_settings == settings) {
    return true;
  }
  dst.resolved_from = &src;
  dst.resolved_writes = src.writes;
  dst.resolved_settings = settings;
  return false;
}

// The samples a resolve of `src` reads (RB_COPY_CONTROL.copy_sample_select,
// captured during Resolve), packed for the MSAA resolve shaders: first sample
// in bits 0-1, count in bits 2-4. All samples when it wasn't captured, or when
// the selection is every guest sample (`guest_samples`) of a target the host
// multisamples more (--tenchuz_native_msaa_samples).
uint32_t ResolveSamples(const Surface& src, uint32_t guest_samples) {
  uint32_t first = 0, count = src.samples;
  if (g_copy_control && src.samples > 1) {
    const uint32_t select = (g_copy_control >> 4) & 7;
    if (select <= 3) {
      first = std::min(select, src.samples - 1);
      count = 1;
    } else if (select <= 5 && guest_samples > 2) {
      first = select == 5 && src.samples >= 4 ? 2 : 0;
      count = 2;
    }
  }
  return first | (count << 2);
}

// A resolve done by a compute pass (format conversion, exponent bias, or a
// multisampled source): `src` read through an SRV, `dst` written through a UAV.
bool ResolveCompute(Surface& src, DXGI_FORMAT srv_format, Surface& dst, DXGI_FORMAT uav_format,
                    ID3D12PipelineState* pso, uint32_t width, uint32_t height, float scale, uint32_t samples,
                    uint32_t src_at = 0, uint32_t dst_at = 0) {
  uint32_t& used = g_resolve_descriptors_used[g_frame % kFramesInFlight];
  if (!pso || used + 2 > kResolveDescriptors) {
    return false;
  }
  Transition(src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const uint32_t first =
      kFramesInFlight * kPresentDescriptors + uint32_t(g_frame % kFramesInFlight) * kResolveDescriptors + used;
  used += 2;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_shader_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_shader_heap->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(first) * g_shader_size;
  gpu.ptr += UINT64(first) * g_shader_size;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = srv_format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (src.samples > 1) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
  }
  g_d3d->CreateShaderResourceView(src.resource.Get(), &srv, cpu);
  cpu.ptr += g_shader_size;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.Format = uav_format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  g_d3d->CreateUnorderedAccessView(dst.resource.Get(), nullptr, &uav, cpu);
  ID3D12DescriptorHeap* heaps[] = {g_shader_heap.Get()};
  g_list->SetDescriptorHeaps(1, heaps);
  g_list->SetComputeRootSignature(g_blit_root.Get());
  g_list->SetPipelineState(pso);
  g_list->SetComputeRootDescriptorTable(0, gpu);
  uint32_t constants[6] = {width, height, 0, samples, src_at, dst_at};
  std::memcpy(&constants[2], &scale, 4);
  g_list->SetComputeRoot32BitConstants(1, 6, constants, 0);
  g_list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  return true;
}

// Depth resolve into an R32_TYPELESS texture that shaders sample as
// R32_FLOAT: the depth plane (subresource 0 of the planar depth-stencil
// surface) copied whole (depth-stencil copies can't take a box), or one
// sample of a multisampled buffer by a compute pass.
void ResolveDepth(uint8_t* base, uint32_t flags, uint32_t dest) {
  Surface* src = DepthTarget(base);
  if (!src) {
    return;
  }
  const bool msaa = src->samples > 1;
  const uint64_t key = TextureKey(dest, src->width, src->height);
  Surface& dst = g_textures[key];
  if (!dst.resource || dst.format != DXGI_FORMAT_R32_TYPELESS || !dst.unordered_access) {
    if (dst.resource) {
      g_slots[g_frame % kFramesInFlight].retired.push_back(std::move(dst.resource));
    }
    dst = Surface();
    if (!CreateTexture(dst, DXGI_FORMAT_R32_TYPELESS, src->width, src->height, false, false, true, 1, src->scale)) {
      if (g_latest_texture[dest] == &dst) {
        g_latest_texture.erase(dest);
      }
      g_textures.erase(key);
      return;
    }
    SetDebugName(dst.resource.Get(),
                 fmt::format("tenchuz depth resolve {:08X} {}x{}", dest, dst.host_width(), dst.host_height()));
  }
  g_latest_texture[dest] = &dst;
  dst.red_blue_swapped = false;
  const uint32_t samples = ResolveSamples(*src, GuestSampleCount(base));
  const ResolveArea area = GetResolveArea(*src, src->width, src->height);
  if (AlreadyResolved(dst, *src, samples ^ (area.key() << 8))) {
    ++g_frame_resolves_skipped;
    return;
  }
  dst.resolved_frame = g_frame;
  dst.guest_bytes = ((src->width + 31) & ~31u) * ((src->height + 31) & ~31u) * 4;
  ++g_frame_resolves;
  const uint32_t op = BeginOp("resolve depth", dest, (src->width << 16) | src->height);
  // A compute pass either way: depth-stencil copies can't take a box.
  const bool done = ResolveCompute(*src,
                                   src->format == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_FLOAT
                                                                         : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
                                   dst, DXGI_FORMAT_R32_FLOAT,
                                   msaa ? g_resolve_depth_msaa_pso.Get() : g_resolve_depth_pso.Get(), area.width,
                                   area.height, 1.0f, samples & 3, area.src_at(), area.dst_at());
  EndOp(op);
  if (Logging()) {
    REXLOG_INFO("tenchuz: NATIVE resolve flags {:X} depth {}x{} x{} -> {:08X}{}{} (copy control {:08X})", flags,
                src->width, src->height, src->samples, dest, g_resolve_area, done ? "" : " skipped", g_copy_control);
  }
}

// D3DDevice_Resolve. Flags: bits 0-2 the source (render target 0-3, 4 = depth),
// 0x100 / 0x200 clear the render target / depth-stencil afterwards. The source
// comes from the flags because in tiled passes the device's RB_COPY_CONTROL
// shadow is not updated per call (a depth resolve showed render target 0).
// --- Blood maps' dried layers (native_renderer.h) ----------------------------

struct BloodLayer {
  Surface dried;
  bool dry_next = false;
};
constexpr int kBloodMaps = 4;
BloodLayer g_blood_layers[kBloodMaps];
int g_blood_map = -1;            // the map the coming resolve is
bool g_blood_layers_ok = false;  // a map resolved in a format compute can load and store

// Whether compute passes can read and write `format` through a UAV.
bool LoadStoreUav(DXGI_FORMAT format) {
  static std::unordered_map<uint32_t, bool> known;
  auto [it, added] = known.try_emplace(uint32_t(format), false);
  if (added) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format};
    it->second = SUCCEEDED(g_d3d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                 (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) &&
                 (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
  }
  return it->second;
}

// Before a blood map's resolve overwrites `map`: keeps what it holds as the
// dried layer, when asked.
void DryBloodLayer(BloodLayer& layer, Surface& map) {
  if (!layer.dry_next || !map.resolved_frame) {
    return;
  }
  layer.dry_next = false;
  if (!layer.dried.resource || layer.dried.format != map.format || layer.dried.host_width() != map.host_width() ||
      layer.dried.host_height() != map.host_height()) {
    if (layer.dried.resource) {
      g_slots[g_frame % kFramesInFlight].retired.push_back(std::move(layer.dried.resource));
    }
    layer.dried = Surface();
    if (!CreateTexture(layer.dried, map.format, map.width, map.height, false, false, true, 1, map.scale)) {
      layer.dried = Surface();
      return;
    }
    SetDebugName(layer.dried.resource.Get(), "tenchuz dried blood");
  }
  ResolveCompute(map, map.format, layer.dried, map.format, g_blood_dry_pso.Get(), map.host_width(),
                 map.host_height(), kBloodDrying, 0);
}

void SetBloodMapResolve(int player) { g_blood_map = player >= 0 && player < kBloodMaps ? player : -1; }

void DryBloodMap(int player) {
  if (player >= 0 && player < kBloodMaps) g_blood_layers[player].dry_next = true;
}

void ClearBloodMap(int player) {
  if (player < 0 || player >= kBloodMaps) return;
  BloodLayer& layer = g_blood_layers[player];
  layer.dry_next = false;
  if (layer.dried.resource) {
    g_slots[g_frame % kFramesInFlight].retired.push_back(std::move(layer.dried.resource));
  }
  layer.dried = Surface();
}

bool BloodMapLayers() { return Active() && g_blood_layers_ok; }

void OnResolveStart(uint8_t* base, uint32_t source_rect, uint32_t dest_point) {
  g_resolve_area.clear();
  g_rect_left = source_rect ? REX_LOAD_U32(source_rect) : 0;
  g_rect_top = source_rect ? REX_LOAD_U32(source_rect + 4) : 0;
  g_rect_right = source_rect ? REX_LOAD_U32(source_rect + 8) : 0;
  g_rect_bottom = source_rect ? REX_LOAD_U32(source_rect + 12) : 0;
  g_dest_x = dest_point ? REX_LOAD_U32(dest_point) : 0;
  g_dest_y = dest_point ? REX_LOAD_U32(dest_point + 4) : 0;
  if (Logging()) {
    if (source_rect) {
      g_resolve_area += fmt::format(" rect {},{}-{},{}", REX_LOAD_U32(source_rect), REX_LOAD_U32(source_rect + 4),
                                    REX_LOAD_U32(source_rect + 8), REX_LOAD_U32(source_rect + 12));
    }
    if (dest_point) {
      g_resolve_area += fmt::format(" at {},{}", REX_LOAD_U32(dest_point), REX_LOAD_U32(dest_point + 4));
    }
  }
  g_in_resolve = true;
  g_copy_dest_info = 0;
  g_copy_control = 0;
}

void OnResolve(uint8_t* base, uint32_t flags, uint32_t clear_color_ptr, double clear_z) {
  g_in_resolve = false;
  if (!Active() || !g_device) {
    return;
  }
  BeginRecording();
  const uint32_t source_select = flags & 7;
  const uint32_t dest = Dev(base, kDevCopyDestBase);
  const uint32_t pitch_reg = Dev(base, kDevCopyDestPitch);
  const uint32_t dest_width = pitch_reg & 0x3FFF;
  const uint32_t dest_height = (pitch_reg >> 16) & 0x3FFF;
  if (source_select > 4 || !dest || !dest_width || !dest_height) {
    if (Logging()) {
      REXLOG_INFO("tenchuz: NATIVE resolve flags {:X} -> {:08X} {}x{} skipped", flags, dest, dest_width,
                  dest_height);
    }
    return;
  }
  if (flags & 0x100) {
    if (Surface* s = ColorTarget(base, source_select < 4 ? source_select : 0)) {
      g_pending_clear.color = s;
      ClearColorArgument(base, clear_color_ptr, g_pending_clear.rgba);
    }
  }
  if (flags & 0x200) {
    if (Surface* d = DepthTarget(base)) {
      g_pending_clear.depth = d;
      g_pending_clear.z = float(std::clamp(clear_z, 0.0, 1.0));
      g_pending_clear.stencil = uint8_t(Dev(base, kDevDepthClear));  // RB_DEPTH_CLEAR: stencil in bits 0-7
      g_pending_clear.depth_flags = D3D12_CLEAR_FLAGS(D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL);
    }
  }
  if (source_select == 4) {
    ResolveDepth(base, flags, dest);
    return;
  }
  Surface* src = ColorTarget(base, source_select);
  if (!src) {
    return;
  }
  // The copied area: the destination pitch is padded (a 322-wide target
  // resolves into a 352 pitch), and textures fetch the unpadded size.
  const uint32_t copy_width = std::min(src->width, dest_width);
  const uint32_t copy_height = std::min(src->height, dest_height);
  // The EDRAM copy converts to the destination format and scales by 2^bias:
  // done by a compute pass when either differs from a plain copy.
  const uint32_t dest_info = g_copy_dest_info;
  const int32_t exp_bias = int32_t(dest_info << 10) >> 26;  // bits 16-21
  DXGI_FORMAT dest_format = ResolveDestFormat((dest_info >> 7) & 0x3F);
  if (dest_format == DXGI_FORMAT_UNKNOWN) {
    dest_format = src->format;
  }
  // A compute pass converts, scales, or reads a multisampled source; else a
  // plain copy.
  const bool convert = dest_format != src->format || exp_bias != 0 || src->samples > 1;
  // Except a plain average of every sample: the GPU's own MSAA resolve.
  const bool hardware_resolve = REXCVAR_GET(tenchuz_native_hardware_resolve) && src->samples > 1 &&
                               dest_format == src->format && exp_bias == 0 && HardwareResolvable(src->format);
  const uint64_t key = TextureKey(dest, copy_width, copy_height);
  Surface& dst = g_textures[key];
  // A blood map with a dried layer is written by compute too.
  const bool blood = g_blood_map >= 0 && g_blood_dry_pso && g_blood_under_pso && LoadStoreUav(dest_format);
  if (g_blood_map >= 0) g_blood_layers_ok = blood;
  if (!dst.resource || dst.format != dest_format || ((convert || blood) && !dst.unordered_access)) {
    if (dst.resource) {
      if (Logging()) {
        REXLOG_INFO("tenchuz: NATIVE resolve destination {:08X} {}x{} recreated for format {}", dest,
                    copy_width, copy_height, int(src->format));
      }
      g_slots[g_frame % kFramesInFlight].retired.push_back(std::move(dst.resource));
    }
    dst = Surface();
    if (!CreateTexture(dst, dest_format, copy_width, copy_height, false, false, convert || blood, 1, src->scale)) {
      if (g_latest_texture[dest] == &dst) {
        g_latest_texture.erase(dest);
      }
      g_textures.erase(key);
      return;
    }
    SetDebugName(dst.resource.Get(), fmt::format("tenchuz resolve {:08X} {}x{} format {}", dest, dst.host_width(),
                                                 dst.host_height(), int(dest_format)));
  }
  g_latest_texture[dest] = &dst;
  g_resolve_sources[dest] = src;
  const uint32_t samples = ResolveSamples(*src, GuestSampleCount(base));
  const ResolveArea area = GetResolveArea(*src, copy_width, copy_height);
  if (AlreadyResolved(dst, *src,
                      (uint64_t(samples) | (uint64_t(uint32_t(exp_bias) & 0x3F) << 8) |
                       (uint64_t(dest_format) << 16) | (uint64_t((dest_info >> 24) & 1) << 32)) ^
                          (area.key() * 0x9E3779B97F4A7C15ull))) {
    ++g_frame_resolves_skipped;
    return;
  }
  dst.resolved_frame = g_frame;
  dst.guest_bytes = ((dest_width + 31) & ~31u) * ((dest_height + 31) & ~31u) * BytesPerPixel(dest_format);
  ++g_frame_resolves;
  dst.red_blue_swapped = (dest_info >> 24) & 1;
  BloodLayer* layer = blood ? &g_blood_layers[g_blood_map] : nullptr;
  if (layer) DryBloodLayer(*layer, dst);
  const uint32_t op = BeginOp("resolve copy", dest, (dest_width << 16) | dest_height);
  bool done = true;
  if (hardware_resolve && (samples >> 2) == src->samples && (samples & 3) == 0 && area.whole &&
      dst.host_width() == src->host_width() && dst.host_height() == src->host_height()) {
    Transition(*src, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    Transition(dst, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    g_list->ResolveSubresource(dst.resource.Get(), 0, src->resource.Get(), 0, src->format);
  } else if (convert) {
    // A cross-format copy would be invalid: skipped when there's no pass.
    done = ResolveCompute(*src, src->format, dst, dest_format,
                          src->samples > 1 ? g_resolve_msaa_pso.Get() : g_resolve_pso.Get(), area.width,
                          area.height, std::ldexp(1.0f, exp_bias), samples, area.src_at(), area.dst_at());
  } else {
    Transition(*src, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to = {dst.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION from = {src->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_BOX box = {area.src_x, area.src_y, 0, area.src_x + area.width, area.src_y + area.height, 1};
    g_list->CopyTextureRegion(&to, area.dst_x, area.dst_y, 0, &from, &box);
  }
  if (layer && layer->dried.resource) {
    ResolveCompute(layer->dried, dst.format, dst, dst.format, g_blood_under_pso.Get(), dst.host_width(),
                   dst.host_height(), 1.0f, 0);
  }
  EndOp(op);
  if (Logging()) {
    REXLOG_INFO("tenchuz: NATIVE resolve flags {:X} RT{} {}x{} x{} fmt {} -> {:08X} {}x{} fmt {} bias {}{}{}{}{}{}{} "
                "(dest info {:08X}, copy control {:08X})",
                flags, source_select, src->width, src->height, src->samples, int(src->format), dest, dest_width,
                dest_height, int(dest_format), exp_bias, g_resolve_area, convert ? " (compute)" : "", done ? "" : " skipped",
                dst.red_blue_swapped ? " +swap" : "", (flags & 0x100) ? " +clear colour" : "",
                (flags & 0x200) ? " +clear depth" : "", dest_info, g_copy_control);
  }
}

}  // namespace tenchuz::native

// --- Hooks -------------------------------------------------------------------

REX_EXTERN(__imp__sub_8241A780);
// D3DDevice_SetRenderTarget(index, surface); resets the viewport to the surface size.
REX_HOOK_RAW(sub_8241A780) {
  tenchuz::HookScope scope(0x8241A780, ctx, base);
  const uint32_t index = ctx.r4.u32;
  const uint32_t surface = ctx.r5.u32;
  __imp__sub_8241A780(ctx, base);
  tenchuz::native::OnSetRenderTarget(base, index, surface);
}

REX_EXTERN(__imp__sub_8241C790);
// D3DDevice_BeginTiling(flags, count, tile rects, clear colour, …)
REX_HOOK_RAW(sub_8241C790) {
  tenchuz::HookScope scope(0x8241C790, ctx, base);
  const uint32_t count = ctx.r5.u32;
  const uint32_t rects = ctx.r6.u32;
  __imp__sub_8241C790(ctx, base);
  tenchuz::native::OnBeginTiling(base, count, rects);
}

REX_EXTERN(__imp__sub_8241B308);
// D3DDevice_ClearF(flags, rect, color*, z (f1), stencil)
REX_HOOK_RAW(sub_8241B308) {
  tenchuz::HookScope scope(0x8241B308, ctx, base);
  const uint32_t flags = ctx.r4.u32;
  const uint32_t color_ptr = ctx.r6.u32;
  const double z = ctx.f1.f64;
  const uint32_t stencil = ctx.r7.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_8241B308(ctx, base);
  tenchuz::native::OnClear(base, flags, color_ptr, z, stencil, caller);
}

REX_EXTERN(__imp__sub_8242A4F8);
// D3D: writes a D3DGAMMARAMP (r4) to the display controller's 256-entry LUT
// through the command buffer (r3 = device).
REX_HOOK_RAW(sub_8242A4F8) {
  tenchuz::HookScope scope(0x8242A4F8, ctx, base);
  tenchuz::native::OnGammaRamp(base, ctx.r4.u32);
  __imp__sub_8242A4F8(ctx, base);
}

REX_EXTERN(__imp__sub_8241D770);
// D3DDevice_Resolve(flags, src rect, dest texture, dest point, level, slice,
// clear colour*, clear z (f1), clear stencil, parameters)
REX_HOOK_RAW(sub_8241D770) {
  const uint32_t flags = ctx.r4.u32;
  const uint32_t clear_color = ctx.r10.u32;  // const D3DVECTOR4*
  const double clear_z = ctx.f1.f64;
  const uint32_t source_rect = ctx.r5.u32;  // const D3DRECT*, null for the whole target
  const uint32_t dest_point = ctx.r7.u32;   // const D3DPOINT*, null for (0, 0)
  tenchuz::native::OnResolveStart(base, source_rect, dest_point);
  {
    tenchuz::HookScope scope(0x8241D770, ctx, base);
    __imp__sub_8241D770(ctx, base);
  }  // decodes the packets Resolve wrote, with its copy draw (RB_COPY_DEST_INFO)
  tenchuz::native::OnResolve(base, flags, clear_color, clear_z);
}

// Mid-asm hook right after the D3D Swap function's call to VdSwap
// (0x8241C0E0, config/default.toml). r29 + 4 is the 64-dword block VdSwap
// filled: front-buffer fetch constant, then the SDK's XE_SWAP packet
// (signature, front buffer physical address, width, height). While native
// output is on, we present our own front buffer and the swap packet becomes
// type-2 NOPs so the emulator does not present over it.
void TenchuzAfterVdSwap(PPCRegister& r29) {
  uint8_t* base = tenchuz::native::g_base.load(std::memory_order_relaxed);
  if (!tenchuz::native::Active() || !base) {
    return;
  }
  const bool present = tenchuz::native::PresentNative();
  const uint32_t block = r29.u32 + 4;
  for (uint32_t i = 0; i + 4 < 64; ++i) {
    const uint32_t header = REX_LOAD_U32(block + 4 * i);
    if ((header >> 30) != 3 || ((header >> 8) & 0x7F) != tenchuz::native::kPm4XeSwap) {
      continue;
    }
    const uint32_t front_buffer = REX_LOAD_U32(block + 4 * (i + 2));
    const uint32_t width = REX_LOAD_U32(block + 4 * (i + 3));
    const uint32_t height = REX_LOAD_U32(block + 4 * (i + 4));
    // Without GPU emulation, the native command processor only counts swaps.
    if (present && !tenchuz::native::NativeGpu()) {
      const uint32_t size = 1 + ((header >> 16) & 0x3FFF) + 1;
      for (uint32_t k = 0; k < size && i + k < 64; ++k) {
        REX_STORE_U32(block + 4 * (i + k), 0x80000000u);  // type-2 NOP
      }
    }
    tenchuz::native::EndFrameAndPresent(front_buffer, width, height, present);
    return;
  }
}
