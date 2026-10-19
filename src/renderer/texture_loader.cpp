// tenchuz - guest texture loading. See texture_loader.h.
//
// The format table, load shader table and load procedure follow the SDK's
// D3D12TextureCache (src/graphics/d3d12/texture_cache.cpp) and TextureCache
// (src/graphics/pipeline/texture/cache.cpp) at the vendored commit; the tables
// are copied verbatim. Not ported: resolution scaling, separate signed loads,
// 3D-as-2D wrappers for stacked textures, and memory budgets.

#include "texture_loader.h"

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/cvar.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(tenchuz_native_texture_watch, true, "tenchuz",
                    "Native textures reload when the guest writes to their memory (false: compare hashes every frame)");

namespace tenchuz::native {

namespace gfx = rex::graphics;
namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
using Microsoft::WRL::ComPtr;

namespace shaders {
#include "texture_load_128bpb_cs.h"
#include "texture_load_16bpb_cs.h"
#include "texture_load_32bpb_cs.h"
#include "texture_load_64bpb_cs.h"
#include "texture_load_8bpb_cs.h"
#include "texture_load_bgrg8_rgb8_cs.h"
#include "texture_load_bgrg8_rgbg8_cs.h"
#include "texture_load_ctx1_cs.h"
#include "texture_load_depth_float_cs.h"
#include "texture_load_depth_unorm_cs.h"
#include "texture_load_dxn_rg8_cs.h"
#include "texture_load_dxt1_rgba8_cs.h"
#include "texture_load_dxt3_rgba8_cs.h"
#include "texture_load_dxt3a_cs.h"
#include "texture_load_dxt3aas1111_bgra4_cs.h"
#include "texture_load_dxt5_rgba8_cs.h"
#include "texture_load_dxt5a_r8_cs.h"
#include "texture_load_gbgr8_grgb8_cs.h"
#include "texture_load_gbgr8_rgb8_cs.h"
#include "texture_load_r10g11b11_rgba16_cs.h"
#include "texture_load_r10g11b11_rgba16_snorm_cs.h"
#include "texture_load_r11g11b10_rgba16_cs.h"
#include "texture_load_r11g11b10_rgba16_snorm_cs.h"
#include "texture_load_r4g4b4a4_b4g4r4a4_cs.h"
#include "texture_load_r5g5b5a1_b5g5r5a1_cs.h"
#include "texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs.h"
#include "texture_load_r5g6b5_b5g6r5_cs.h"
}  // namespace shaders

namespace {

constexpr uint32_t kFrames = 2;
constexpr uint32_t kUavRingSize = 4096;  // load destination descriptors per frame slot
constexpr uint64_t kEvictAfterFrames = 600;  // textures unused this long are released
constexpr uint32_t kFullHashMax = 64u << 10;    // larger textures are change-checked by sampling
constexpr uint32_t kHashStride = 2048;
constexpr uint32_t kHashSample = 64;
constexpr uint32_t kGuestPhysicalSize = 512u << 20;
constexpr uint32_t kWatchPageShift = 12;

enum LoadShaderIndex {
  kLoadShaderIndex8bpb,
  kLoadShaderIndex16bpb,
  kLoadShaderIndex32bpb,
  kLoadShaderIndex64bpb,
  kLoadShaderIndex128bpb,
  kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
  kLoadShaderIndexR5G6B5ToB5G6R5,
  kLoadShaderIndexR5G6B5ToRGBA8,
  kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle,
  kLoadShaderIndexRGBA4ToBGRA4,
  kLoadShaderIndexRGBA4ToARGB4,
  kLoadShaderIndexRGBA4ToRGBA8,
  kLoadShaderIndexGBGR8ToGRGB8,
  kLoadShaderIndexGBGR8ToRGB8,
  kLoadShaderIndexBGRG8ToRGBG8,
  kLoadShaderIndexBGRG8ToRGB8,
  kLoadShaderIndexR10G11B11ToRGBA16,
  kLoadShaderIndexR10G11B11ToRGBA16SNorm,
  kLoadShaderIndexR11G11B10ToRGBA16,
  kLoadShaderIndexR11G11B10ToRGBA16SNorm,
  kLoadShaderIndexR16UNormToFloat,
  kLoadShaderIndexR16SNormToFloat,
  kLoadShaderIndexRG16UNormToFloat,
  kLoadShaderIndexRG16SNormToFloat,
  kLoadShaderIndexRGBA16UNormToFloat,
  kLoadShaderIndexRGBA16SNormToFloat,
  kLoadShaderIndexDXT1ToRGBA8,
  kLoadShaderIndexDXT3ToRGBA8,
  kLoadShaderIndexDXT5ToRGBA8,
  kLoadShaderIndexDXNToRG8,
  kLoadShaderIndexDXT3A,
  kLoadShaderIndexDXT3AAs1111ToBGRA4,
  kLoadShaderIndexDXT3AAs1111ToARGB4,
  kLoadShaderIndexDXT5AToR8,
  kLoadShaderIndexCTX1,
  kLoadShaderIndexDepthUnorm,
  kLoadShaderIndexDepthFloat,

  kLoadShaderCount,
  kLoadShaderIndexUnknown = kLoadShaderCount,
};

struct LoadShaderInfo {
  uint32_t source_bpe_log2;
  uint32_t dest_bpe_log2;
  uint32_t bytes_per_host_block;
  uint32_t guest_x_blocks_per_thread_log2;
};

constexpr uint32_t kLoadGuestXThreadsPerGroupLog2 = 2;
constexpr uint32_t kLoadGuestYBlocksPerGroupLog2 = 5;

struct LoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t host_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
};

const LoadShaderInfo kLoadShaderInfos[kLoadShaderCount] = {
    // k8bpb
    {3, 4, 1, 4},
    // k16bpb
    {4, 4, 2, 4},
    // k32bpb
    {4, 4, 4, 3},
    // k64bpb
    {4, 4, 8, 2},
    // k128bpb
    {4, 4, 16, 1},
    // kR5G5B5A1ToB5G5R5A1
    {4, 4, 2, 4},
    // kR5G6B5ToB5G6R5
    {4, 4, 2, 4},
    // kR5G6B5ToRGBA8
    {4, 4, 4, 4},
    // kR5G5B6ToB5G6R5WithRBGASwizzle
    {4, 4, 2, 4},
    // kRGBA4ToBGRA4
    {4, 4, 2, 4},
    // kRGBA4ToARGB4
    {4, 4, 2, 4},
    // kRGBA4ToRGBA8
    {4, 4, 4, 4},
    // kGBGR8ToGRGB8
    {4, 4, 4, 3},
    // kGBGR8ToRGB8
    {4, 4, 8, 3},
    // kBGRG8ToRGBG8
    {4, 4, 4, 3},
    // kBGRG8ToRGB8
    {4, 4, 8, 3},
    // kR10G11B11ToRGBA16
    {4, 4, 8, 3},
    // kR10G11B11ToRGBA16SNorm
    {4, 4, 8, 3},
    // kR11G11B10ToRGBA16
    {4, 4, 8, 3},
    // kR11G11B10ToRGBA16SNorm
    {4, 4, 8, 3},
    // kR16UNormToFloat
    {4, 4, 2, 4},
    // kR16SNormToFloat
    {4, 4, 2, 4},
    // kRG16UNormToFloat
    {4, 4, 4, 3},
    // kRG16SNormToFloat
    {4, 4, 4, 3},
    // kRGBA16UNormToFloat
    {4, 4, 8, 2},
    // kRGBA16SNormToFloat
    {4, 4, 8, 2},
    // kDXT1ToRGBA8
    {4, 4, 4, 2},
    // kDXT3ToRGBA8
    {4, 4, 4, 1},
    // kDXT5ToRGBA8
    {4, 4, 4, 1},
    // kDXNToRG8
    {4, 4, 2, 1},
    // kDXT3A
    {4, 4, 1, 2},
    // kDXT3AAs1111ToBGRA4
    {4, 4, 2, 2},
    // kDXT3AAs1111ToARGB4
    {4, 4, 2, 2},
    // kDXT5AToR8
    {4, 4, 1, 2},
    // kCTX1
    {4, 4, 2, 2},
    // kDepthUnorm
    {4, 4, 4, 3},
    // kDepthFloat
    {4, 4, 4, 3},
};

struct HostFormat {
  DXGI_FORMAT dxgi_format_resource;
  DXGI_FORMAT dxgi_format_unsigned;
  LoadShaderIndex load_shader;
  DXGI_FORMAT dxgi_format_signed;
  LoadShaderIndex load_shader_signed;
  bool is_block_compressed;
  DXGI_FORMAT dxgi_format_uncompressed;
  LoadShaderIndex load_shader_decompress;
  uint32_t swizzle;
};

const HostFormat kHostFormats[64] = {
    // k_1_REVERSE
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1_5_5_5
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, kLoadShaderIndexR5G5B5A1ToB5G5R5A1,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM, kLoadShaderIndexR5G6B5ToB5G6R5,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5
    // On the host, green bits in blue, blue bits in green.
    {DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
     kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {DXGI_FORMAT_R8_TYPELESS, DXGI_FORMAT_R8_UNORM, kLoadShaderIndex8bpb, DXGI_FORMAT_R8_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_B
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_8
    {DXGI_FORMAT_R8G8_TYPELESS, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndex16bpb,
     DXGI_FORMAT_R8G8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_Cr_Y1_Cb_Y0_REP
    // Red and blue swapped in the load shader for simplicity.
    // TODO(Triang3l): The DXGI_FORMAT_R8G8B8A8_U/SNORM conversion is usable for
    // the signed version, separate unsigned and signed load shaders completely
    // (as one doesn't need decompression for this format, while another does).
    {DXGI_FORMAT_G8R8_G8B8_UNORM, DXGI_FORMAT_G8R8_G8B8_UNORM, kLoadShaderIndexGBGR8ToGRGB8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexGBGR8ToRGB8, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_Y1_Cr_Y0_Cb_REP
    // Red and blue swapped in the load shader for simplicity.
    // TODO(Triang3l): The DXGI_FORMAT_R8G8B8A8_U/SNORM conversion is usable for
    // the signed version, separate unsigned and signed load shaders completely
    // (as one doesn't need decompression for this format, while another does).
    {DXGI_FORMAT_R8G8_B8G8_UNORM, DXGI_FORMAT_R8G8_B8G8_UNORM, kLoadShaderIndexBGRG8ToRGBG8,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM,
     kLoadShaderIndexBGRG8ToRGB8, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_A
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_4_4_4_4
    // Red and blue swapped in the load shader for simplicity.
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexRGBA4ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    // Not usable as a texture, also has -32...32 range.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // R32_FLOAT for depth because shaders would require an additional SRV to
    // sample stencil, which we don't provide.
    // k_24_8
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthUnorm,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_24_8_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexDepthFloat,
     DXGI_FORMAT_R32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16
    {DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_SNORM,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16
    {DXGI_FORMAT_R16G16_TYPELESS, DXGI_FORMAT_R16G16_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_EXPAND
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_EXPAND
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_EXPAND
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_FLOAT
    {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, kLoadShaderIndex16bpb, DXGI_FORMAT_R16_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_FLOAT
    {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R16G16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_16_16_16_FLOAT
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R16G16B16A16_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_FLOAT
    {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kLoadShaderIndex32bpb, DXGI_FORMAT_R32_FLOAT,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_32_FLOAT
    {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndex64bpb,
     DXGI_FORMAT_R32G32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_32_32_32_32_FLOAT
    {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndex128bpb,
     DXGI_FORMAT_R32G32B32A32_FLOAT, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_AS_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_32_AS_8_8_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_16_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16_16_MPEG_INTERLACED
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXN
    {DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexDXNToRG8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_R8G8B8A8_SNORM, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT1_AS_16_16_16_16
    {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT1ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT2_3_AS_16_16_16_16
    {DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC2_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT3ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT4_5_AS_16_16_16_16
    {DXGI_FORMAT_BC3_UNORM, DXGI_FORMAT_BC3_UNORM, kLoadShaderIndex128bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8G8B8A8_UNORM, kLoadShaderIndexDXT5ToRGBA8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, kLoadShaderIndex32bpb,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR11G11B10ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR11G11B10ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_11_11_10_AS_16_16_16_16
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_UNORM,
     kLoadShaderIndexR10G11B11ToRGBA16, DXGI_FORMAT_R16G16B16A16_SNORM,
     kLoadShaderIndexR10G11B11ToRGBA16SNorm, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT3A
    // R8_UNORM has the same size as BC2, but doesn't have the 4x4 size
    // alignment requirement.
    {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT3A, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM, kLoadShaderIndex64bpb, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, true, DXGI_FORMAT_R8_UNORM, kLoadShaderIndexDXT5AToR8,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, kLoadShaderIndexCTX1, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1
    {DXGI_FORMAT_B4G4R4A4_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM, kLoadShaderIndexDXT3AAs1111ToBGRA4,
     DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_FLOAT_EDRAM
    // Not usable as a texture.
    {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown, DXGI_FORMAT_UNKNOWN,
     kLoadShaderIndexUnknown, false, DXGI_FORMAT_UNKNOWN, kLoadShaderIndexUnknown,
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};

DXGI_FORMAT UintPow2Format(uint32_t bpe_log2) {
  switch (bpe_log2) {
    case 0: return DXGI_FORMAT_R8_UINT;
    case 1: return DXGI_FORMAT_R16_UINT;
    case 2: return DXGI_FORMAT_R32_UINT;
    case 3: return DXGI_FORMAT_R32G32_UINT;
    default: return DXGI_FORMAT_R32G32B32A32_UINT;
  }
}

// Texture identity from a fetch constant (SDK TextureCache::
// BindingInfoFromFetchConstant / TextureKey, without the signed and scaled bits).
struct Key {
  uint32_t base_page, mip_page;
  uint32_t width, height, depth_or_array_size;
  uint32_t pitch, mip_max_level;
  xenos::DataDimension dimension;
  xenos::TextureFormat format;
  xenos::Endian endianness;
  bool tiled, packed_mips;

  uint64_t Hash() const {
    const uint32_t words[] = {base_page, mip_page, width, height, depth_or_array_size, pitch, mip_max_level,
                              uint32_t(dimension), uint32_t(format), uint32_t(endianness),
                              uint32_t(tiled) | (uint32_t(packed_mips) << 1)};
    return XXH3_64bits(words, sizeof(words));
  }
};

bool KeyFromFetch(const xenos::xe_gpu_texture_fetch_t& fetch, Key& key) {
  if (fetch.type != xenos::FetchConstantType::kTexture &&
      fetch.type != xenos::FetchConstantType::kInvalidTexture) {
    return false;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1, &depth_minus_1,
                                                 &base_page, &mip_page, nullptr, &mip_max_level);
  if (base_page == 0 && mip_page == 0) {
    return false;
  }
  if (fetch.dimension == xenos::DataDimension::k1D &&
      (width_minus_1 >= xenos::kTexture2DCubeMaxWidthHeight || fetch.tiled || fetch.packed_mips)) {
    return false;
  }
  key.base_page = base_page;
  key.mip_page = mip_page;
  key.width = width_minus_1 + 1;
  key.height = height_minus_1 + 1;
  key.depth_or_array_size = depth_minus_1 + 1;
  key.pitch = fetch.pitch;
  key.mip_max_level = mip_max_level;
  key.dimension = fetch.dimension;
  key.format = gfx::GetBaseFormat(fetch.format);
  key.endianness = fetch.endianness;
  key.tiled = fetch.tiled;
  key.packed_mips = fetch.packed_mips;
  return true;
}

}  // namespace

struct TextureLoader::Impl {
  ID3D12Device* device = nullptr;
  ID3D12Resource* shared_memory = nullptr;
  ComPtr<ID3D12RootSignature> root_signature;
  std::array<ComPtr<ID3D12PipelineState>, kLoadShaderCount> pipelines;
  ComPtr<ID3D12DescriptorHeap> heap;  // [0..2] source SRVs (R32, R32G32, R32G32B32A32), then UAV rings
  uint32_t descriptor_size = 0;
  uint32_t slot = 0;
  std::array<uint32_t, kFrames> uavs_used{};
  std::array<std::vector<ComPtr<ID3D12Resource>>, kFrames> scratch;  // released when the slot comes back
  uint64_t frame = 0;
  Stats stats;
  uint32_t logged_unsupported = 0;

  // Write watching. A guest write to a watched page (the SDK protects it, and
  // the first write triggers the invalidation callbacks once, from its access
  // violation handler) stamps the page with a new sequence number; a texture
  // reloads when a page it covers is newer than its last load. Per-page
  // sequences rather than dirty bits, as textures can share pages.
  rex::memory::Memory* memory = nullptr;
  void* watch_handle = nullptr;
  std::atomic<uint32_t> write_sequence{0};
  std::vector<std::atomic<uint32_t>> page_written =
      std::vector<std::atomic<uint32_t>>(kGuestPhysicalSize >> kWatchPageShift);
  std::atomic<uint32_t> written_pages{0};

  // Invalidation callback: runs with the SDK's global lock held, on whatever
  // thread wrote, so it only touches atomics. Returning the exact range keeps
  // other subscribers' wider ranges from unprotecting pages still watched here.
  static std::pair<uint32_t, uint32_t> OnWritten(void* context, uint32_t start, uint32_t length, bool) {
    auto* self = static_cast<Impl*>(context);
    start &= kGuestPhysicalSize - 1;
    const uint64_t end = std::min<uint64_t>(uint64_t(start) + length, kGuestPhysicalSize);
    const uint32_t sequence = self->write_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    for (uint64_t page = start >> kWatchPageShift; page < (end + (1u << kWatchPageShift) - 1) >> kWatchPageShift;
         ++page) {
      self->page_written[page].store(sequence, std::memory_order_relaxed);
      self->written_pages.fetch_add(1, std::memory_order_relaxed);
    }
    return {start, length};
  }

  // Whether a page in [start, start + size) was written after `sequence`.
  bool WrittenSince(uint32_t start, uint32_t size, uint32_t sequence) const {
    if (!size) {
      return false;
    }
    start &= kGuestPhysicalSize - 1;
    const uint64_t end = std::min<uint64_t>(uint64_t(start) + size, kGuestPhysicalSize);
    for (uint64_t page = start >> kWatchPageShift; page < (end + (1u << kWatchPageShift) - 1) >> kWatchPageShift;
         ++page) {
      if (page_written[page].load(std::memory_order_relaxed) > sequence) {
        return true;
      }
    }
    return false;
  }

  void Watch(uint32_t start, uint32_t size) {
    if (size) {
      memory->EnablePhysicalMemoryAccessCallbacks(start & (kGuestPhysicalSize - 1), size, true, false);
    }
  }

  struct Entry {
    Key key;
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    texture_util::TextureGuestLayout layout;
    LoadShaderIndex load_shader = kLoadShaderIndexUnknown;
    bool decompress = false;
    uint64_t base_hash = 0, mips_hash = 0;
    uint32_t loaded_sequence = 0;  // write sequence when last loaded (watching)
    uint64_t checked_frame = 0;  // also the last frame it was used
    bool loaded = false;
  };
  std::unordered_map<uint64_t, Entry> entries;

  D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * descriptor_size;
    return h;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * descriptor_size;
    return h;
  }

  bool Initialize(ID3D12Device* d, ID3D12Resource* shared, uint64_t shared_size, rex::memory::Memory* mem) {
    device = d;
    shared_memory = shared;
    if (mem && REXCVAR_GET(tenchuz_native_texture_watch)) {
      memory = mem;
      watch_handle = memory->RegisterPhysicalMemoryInvalidationCallback(&Impl::OnWritten, this);
    }
    auto serialize = reinterpret_cast<HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                       D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**)>(
        GetProcAddress(GetModuleHandleA("d3d12.dll"), "D3D12SerializeRootSignature"));
    if (!serialize) {
      return false;
    }
    // SDK D3D12TextureCache::Initialize: constants, source SRV table, destination UAV table.
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(LoadConstants) / sizeof(uint32_t);
    D3D12_DESCRIPTOR_RANGE source = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE dest = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &source};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &dest};
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3;
    desc.pParameters = params;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&root_signature)))) {
      return false;
    }

    D3D12_SHADER_BYTECODE load_shader_code[kLoadShaderCount] = {};
    load_shader_code[kLoadShaderIndex8bpb] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_8bpb_cs, sizeof(shaders::texture_load_8bpb_cs)};
    load_shader_code[kLoadShaderIndex16bpb] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_16bpb_cs, sizeof(shaders::texture_load_16bpb_cs)};
    load_shader_code[kLoadShaderIndex32bpb] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_32bpb_cs, sizeof(shaders::texture_load_32bpb_cs)};
    load_shader_code[kLoadShaderIndex64bpb] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_64bpb_cs, sizeof(shaders::texture_load_64bpb_cs)};
    load_shader_code[kLoadShaderIndex128bpb] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_128bpb_cs, sizeof(shaders::texture_load_128bpb_cs)};
    load_shader_code[kLoadShaderIndexR5G5B5A1ToB5G5R5A1] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b5a1_b5g5r5a1_cs,
                              sizeof(shaders::texture_load_r5g5b5a1_b5g5r5a1_cs)};
    load_shader_code[kLoadShaderIndexR5G6B5ToB5G6R5] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_r5g6b5_b5g6r5_cs, sizeof(shaders::texture_load_r5g6b5_b5g6r5_cs)};
    load_shader_code[kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs,
                              sizeof(shaders::texture_load_r5g5b6_b5g6r5_swizzle_rbga_cs)};
    load_shader_code[kLoadShaderIndexRGBA4ToBGRA4] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r4g4b4a4_b4g4r4a4_cs,
                              sizeof(shaders::texture_load_r4g4b4a4_b4g4r4a4_cs)};
    load_shader_code[kLoadShaderIndexGBGR8ToGRGB8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_gbgr8_grgb8_cs, sizeof(shaders::texture_load_gbgr8_grgb8_cs)};
    load_shader_code[kLoadShaderIndexGBGR8ToRGB8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_gbgr8_rgb8_cs, sizeof(shaders::texture_load_gbgr8_rgb8_cs)};
    load_shader_code[kLoadShaderIndexBGRG8ToRGBG8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_bgrg8_rgbg8_cs, sizeof(shaders::texture_load_bgrg8_rgbg8_cs)};
    load_shader_code[kLoadShaderIndexBGRG8ToRGB8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_bgrg8_rgb8_cs, sizeof(shaders::texture_load_bgrg8_rgb8_cs)};
    load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_r10g11b11_rgba16_cs, sizeof(shaders::texture_load_r10g11b11_rgba16_cs)};
    load_shader_code[kLoadShaderIndexR10G11B11ToRGBA16SNorm] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r10g11b11_rgba16_snorm_cs,
                              sizeof(shaders::texture_load_r10g11b11_rgba16_snorm_cs)};
    load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_r11g11b10_rgba16_cs, sizeof(shaders::texture_load_r11g11b10_rgba16_cs)};
    load_shader_code[kLoadShaderIndexR11G11B10ToRGBA16SNorm] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_r11g11b10_rgba16_snorm_cs,
                              sizeof(shaders::texture_load_r11g11b10_rgba16_snorm_cs)};
    load_shader_code[kLoadShaderIndexDXT1ToRGBA8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_dxt1_rgba8_cs, sizeof(shaders::texture_load_dxt1_rgba8_cs)};
    load_shader_code[kLoadShaderIndexDXT3ToRGBA8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_dxt3_rgba8_cs, sizeof(shaders::texture_load_dxt3_rgba8_cs)};
    load_shader_code[kLoadShaderIndexDXT5ToRGBA8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_dxt5_rgba8_cs, sizeof(shaders::texture_load_dxt5_rgba8_cs)};
    load_shader_code[kLoadShaderIndexDXNToRG8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_dxn_rg8_cs, sizeof(shaders::texture_load_dxn_rg8_cs)};
    load_shader_code[kLoadShaderIndexDXT3A] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_dxt3a_cs, sizeof(shaders::texture_load_dxt3a_cs)};
    load_shader_code[kLoadShaderIndexDXT3AAs1111ToBGRA4] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_dxt3aas1111_bgra4_cs,
                              sizeof(shaders::texture_load_dxt3aas1111_bgra4_cs)};
    load_shader_code[kLoadShaderIndexDXT5AToR8] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_dxt5a_r8_cs, sizeof(shaders::texture_load_dxt5a_r8_cs)};
    load_shader_code[kLoadShaderIndexCTX1] =
        D3D12_SHADER_BYTECODE{shaders::texture_load_ctx1_cs, sizeof(shaders::texture_load_ctx1_cs)};
    load_shader_code[kLoadShaderIndexDepthUnorm] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_depth_unorm_cs, sizeof(shaders::texture_load_depth_unorm_cs)};
    load_shader_code[kLoadShaderIndexDepthFloat] = D3D12_SHADER_BYTECODE{
        shaders::texture_load_depth_float_cs, sizeof(shaders::texture_load_depth_float_cs)};
    for (uint32_t i = 0; i < kLoadShaderCount; ++i) {
      if (!load_shader_code[i].pShaderBytecode) {
        continue;
      }
      D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
      pso.pRootSignature = root_signature.Get();
      pso.CS = load_shader_code[i];
      if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipelines[i])))) {
        REXLOG_WARN("tenchuz: N4 texture load pipeline {} creation failed", i);
      }
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 3 + kFrames * kUavRingSize;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
      return false;
    }
    descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Shared memory as R32 / R32G32 / R32G32B32A32 typed buffers (SDK
    // D3D12SharedMemory::WriteUintPow2SRVDescriptor).
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t bpe_log2 = 2 + i;
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
      srv.Format = UintPow2Format(bpe_log2);
      srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Buffer.NumElements = UINT(shared_size >> bpe_log2);
      device->CreateShaderResourceView(shared_memory, &srv, Cpu(i));
    }
    return true;
  }

  // Hash of the guest memory a texture's base and mips occupy. Large ranges
  // are sampled (64 bytes of every 2 KB): content changes (video frames,
  // streamed textures) touch all of it, and full hashes of every texture used
  // cost ~100 MB of reads per frame in gameplay.
  uint64_t HashRange(uint8_t* base, uint32_t page, uint32_t size) {
    if (!page || !size) {
      return 0;
    }
    const uint8_t* data = REX_RAW_ADDR(0xA0000000u | (page << 12));
    if (size <= kFullHashMax) {
      stats.hashed_kb += size >> 10;
      return XXH3_64bits(data, size);
    }
    uint64_t hash = size;
    for (uint32_t offset = 0; offset + kHashSample <= size; offset += kHashStride) {
      hash = XXH3_64bits_withSeed(data + offset, kHashSample, hash);
    }
    stats.hashed_kb += (size / kHashStride * kHashSample) >> 10;
    return hash;
  }

  ID3D12Resource* CreateResource(const Entry& e, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = e.key.dimension == xenos::DataDimension::k3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                  : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = e.key.width;
    desc.Height = e.key.height;
    desc.DepthOrArraySize = UINT16(e.key.depth_or_array_size);
    desc.MipLevels = UINT16(e.key.mip_max_level + 1);
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                               D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&resource)))) {
      return nullptr;
    }
    return resource;
  }

  void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
               D3D12_RESOURCE_STATES to) {
    if (from == to) {
      return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
  }

  // SDK D3D12TextureCache::LoadTextureDataFromResidentMemoryImpl, unscaled,
  // base and mips together.
  bool Load(ID3D12GraphicsCommandList* list, Entry& e) {
    const Key& key = e.key;
    const texture_util::TextureGuestLayout& guest_layout = e.layout;
    const bool load_base = guest_layout.base.level_data_extent_bytes != 0;
    const bool load_mips = guest_layout.mips_total_extent_bytes != 0;
    if (!load_base && !load_mips) {
      return false;
    }
    ID3D12PipelineState* pipeline = pipelines[e.load_shader].Get();
    if (!pipeline || uavs_used[slot] >= kUavRingSize) {
      return false;
    }
    const LoadShaderInfo& load_shader_info = kLoadShaderInfos[e.load_shader];
    const bool is_3d = key.dimension == xenos::DataDimension::k3D;
    const uint32_t width = key.width, height = key.height;
    const uint32_t depth = is_3d ? key.depth_or_array_size : 1;
    const uint32_t array_size = is_3d ? 1 : key.depth_or_array_size;
    const gfx::FormatInfo* guest_format_info = gfx::FormatInfo::Get(key.format);
    const uint32_t block_width = guest_format_info->block_width;
    const uint32_t block_height = guest_format_info->block_height;
    const uint32_t bytes_per_block = guest_format_info->bytes_per_block();
    const uint32_t level_first = load_base ? 0 : 1;
    const uint32_t level_last = load_mips ? key.mip_max_level : 0;
    const uint32_t level_packed = guest_layout.packed_level;
    const uint32_t level_stored_first = std::min(level_first, level_packed);
    const uint32_t level_stored_last = std::min(level_last, level_packed);
    uint32_t loop_level_first, loop_level_last;
    if (level_packed == 0) {
      loop_level_first = uint32_t(level_first != 0);
      loop_level_last = uint32_t(level_last != 0);
    } else {
      loop_level_first = level_stored_first;
      loop_level_last = level_stored_last;
    }

    const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
    const bool host_block_compressed = host_format.is_block_compressed && !e.decompress;
    const uint32_t host_block_width = host_block_compressed ? block_width : 1;
    const uint32_t host_block_height = host_block_compressed ? block_height : 1;
    uint32_t host_x_blocks_per_thread = 1u << load_shader_info.guest_x_blocks_per_thread_log2;
    if (!host_block_compressed) {
      host_x_blocks_per_thread *= block_width;
    }
    UINT64 copy_buffer_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT host_slice_layout_base = {};
    UINT64 host_slice_size_base = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT host_slice_layouts_mips[xenos::kTextureMaxMips] = {};
    UINT64 host_slice_sizes_mips[xenos::kTextureMaxMips] = {};
    const DXGI_FORMAT host_copy_format =
        e.decompress ? host_format.dxgi_format_uncompressed : host_format.dxgi_format_resource;
    for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
      const bool is_base = loop_level == 0;
      const uint32_t level = level_packed == 0 ? 0 : loop_level;
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
      layout.Offset = copy_buffer_size;
      layout.Footprint.Format = host_copy_format;
      if (level == level_packed) {
        const texture_util::TextureGuestLayout::Level& packed = is_base ? guest_layout.base : guest_layout.mips[level];
        layout.Footprint.Width = packed.x_extent_blocks * block_width;
        layout.Footprint.Height = packed.y_extent_blocks * block_height;
        layout.Footprint.Depth = packed.z_extent;
      } else {
        layout.Footprint.Width = std::max(width >> level, 1u);
        layout.Footprint.Height = std::max(height >> level, 1u);
        layout.Footprint.Depth = std::max(depth >> level, 1u);
      }
      layout.Footprint.Width = rex::round_up(layout.Footprint.Width, UINT(host_block_width));
      layout.Footprint.Height = rex::round_up(layout.Footprint.Height, UINT(host_block_height));
      layout.Footprint.RowPitch =
          rex::align(rex::round_up(layout.Footprint.Width / host_block_width, host_x_blocks_per_thread) *
                         load_shader_info.bytes_per_host_block,
                     uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
      const UINT64 slice_size =
          rex::align(UINT64(layout.Footprint.RowPitch) * (layout.Footprint.Height / host_block_height) *
                         layout.Footprint.Depth,
                     UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT));
      (is_base ? host_slice_size_base : host_slice_sizes_mips[level]) = slice_size;
      copy_buffer_size += slice_size * array_size;
    }

    // Scratch buffer the load shader writes, released when this frame slot
    // comes back around.
    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = copy_buffer_size;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> copy_buffer;
    if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                               D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&copy_buffer)))) {
      return false;
    }
    SetDebugName(copy_buffer.Get(), "tenchuz texture load scratch");
    scratch[slot].push_back(copy_buffer);
    const uint32_t uav_index = 3 + slot * kUavRingSize + uavs_used[slot]++;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = UintPow2Format(load_shader_info.dest_bpe_log2);
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = UINT(copy_buffer_size >> load_shader_info.dest_bpe_log2);
    device->CreateUnorderedAccessView(copy_buffer.Get(), nullptr, &uav, Cpu(uav_index));

    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root_signature.Get());
    list->SetPipelineState(pipeline);
    list->SetComputeRootDescriptorTable(1, Gpu(load_shader_info.source_bpe_log2 - 2));
    list->SetComputeRootDescriptorTable(2, Gpu(uav_index));

    LoadConstants load_constants = {};
    const bool is_3d_tiling = is_3d;
    load_constants.is_tiled_3d_endian_scale = uint32_t(key.tiled) | (uint32_t(is_3d_tiling) << 1) |
                                              (uint32_t(key.endianness) << 2) | (1u << 4) | (1u << 7);
    const uint32_t guest_x_blocks_per_group_log2 =
        kLoadGuestXThreadsPerGroupLog2 + load_shader_info.guest_x_blocks_per_thread_log2;
    for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
      const bool is_base = loop_level == 0;
      const uint32_t level = level_packed == 0 ? 0 : loop_level;
      const uint32_t guest_address = (is_base ? key.base_page : key.mip_page) << 12;
      load_constants.guest_offset = guest_address;
      if (!is_base) {
        load_constants.guest_offset += guest_layout.mip_offsets_bytes[level];
      }
      const texture_util::TextureGuestLayout::Level& level_guest_layout =
          is_base ? guest_layout.base : guest_layout.mips[level];
      uint32_t level_guest_pitch = level_guest_layout.row_pitch_bytes;
      if (key.tiled) {
        level_guest_pitch /= bytes_per_block;  // shaders expect blocks for tiled textures
      }
      load_constants.guest_pitch_aligned = level_guest_pitch;
      load_constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
      uint32_t level_width, level_height, level_depth;
      if (level == level_packed) {
        level_width = level_guest_layout.x_extent_blocks * block_width;
        level_height = level_guest_layout.y_extent_blocks * block_height;
        level_depth = level_guest_layout.z_extent;
      } else {
        level_width = std::max(width >> level, 1u);
        level_height = std::max(height >> level, 1u);
        level_depth = std::max(depth >> level, 1u);
      }
      load_constants.size_blocks[0] = (level_width + (block_width - 1)) / block_width;
      load_constants.size_blocks[1] = (level_height + (block_height - 1)) / block_height;
      load_constants.size_blocks[2] = level_depth;
      load_constants.height_texels = level_height;
      const uint32_t group_count_x =
          (load_constants.size_blocks[0] + ((1u << guest_x_blocks_per_group_log2) - 1)) >>
          guest_x_blocks_per_group_log2;
      const uint32_t group_count_y =
          (load_constants.size_blocks[1] + ((1u << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
          kLoadGuestYBlocksPerGroupLog2;
      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& host_layout =
          is_base ? host_slice_layout_base : host_slice_layouts_mips[level];
      const uint32_t host_slice_size = uint32_t(is_base ? host_slice_size_base : host_slice_sizes_mips[level]);
      load_constants.host_offset = uint32_t(host_layout.Offset);
      load_constants.host_pitch = host_layout.Footprint.RowPitch;
      for (uint32_t slice = 0; slice < array_size; ++slice) {
        list->SetComputeRoot32BitConstants(0, sizeof(load_constants) / sizeof(uint32_t), &load_constants, 0);
        list->Dispatch(group_count_x, group_count_y, load_constants.size_blocks[2]);
        load_constants.guest_offset += level_guest_layout.array_slice_stride_bytes;
        load_constants.host_offset += host_slice_size;
      }
    }

    // Copy into the texture.
    Barrier(list, e.resource.Get(), e.state, D3D12_RESOURCE_STATE_COPY_DEST);
    e.state = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier(list, copy_buffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const uint32_t texture_level_count = key.mip_max_level + 1;
    D3D12_TEXTURE_COPY_LOCATION location_source = {}, location_dest = {};
    location_source.pResource = copy_buffer.Get();
    location_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    location_dest.pResource = e.resource.Get();
    location_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for (uint32_t level = level_first; level <= level_last; ++level) {
      const uint32_t guest_level = std::min(level, level_packed);
      location_source.PlacedFootprint = level ? host_slice_layouts_mips[guest_level] : host_slice_layout_base;
      location_dest.SubresourceIndex = level;
      const UINT64 host_slice_size = level ? host_slice_sizes_mips[guest_level] : host_slice_size_base;
      D3D12_BOX source_box;
      const D3D12_BOX* source_box_ptr = nullptr;
      if (level >= level_packed) {
        uint32_t offset_blocks_x, offset_blocks_y, offset_z;
        texture_util::GetPackedMipOffset(width, height, depth, key.format, level, offset_blocks_x,
                                         offset_blocks_y, offset_z);
        source_box.left = offset_blocks_x * block_width;
        source_box.top = offset_blocks_y * block_height;
        source_box.front = offset_z;
        source_box.right = source_box.left + rex::align(std::max(width >> level, 1u), host_block_width);
        source_box.bottom = source_box.top + rex::align(std::max(height >> level, 1u), host_block_height);
        source_box.back = source_box.front + std::max(depth >> level, 1u);
        source_box_ptr = &source_box;
      }
      for (uint32_t slice = 0; slice < array_size; ++slice) {
        list->CopyTextureRegion(&location_dest, 0, 0, 0, &location_source, source_box_ptr);
        location_dest.SubresourceIndex += texture_level_count;
        location_source.PlacedFootprint.Offset += host_slice_size;
      }
    }
    ++stats.loads;
    stats.loaded_kb += (guest_layout.base.level_data_extent_bytes + guest_layout.mips_total_extent_bytes) >> 10;
    return true;
  }

  Result Get(ID3D12GraphicsCommandList* list, const xenos::xe_gpu_texture_fetch_t& fetch, uint8_t* base,
             const Upload& upload, Texture& out) {
    Key key;
    if (!KeyFromFetch(fetch, key)) {
      return Result::kNoAddress;
    }
    const uint64_t hash = key.Hash();
    auto it = entries.find(hash);
    if (it == entries.end()) {
      Entry e;
      e.key = key;
      const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
      // SDK IsDecompressionNeeded, without unaligned block texture support.
      const gfx::FormatInfo* info = gfx::FormatInfo::Get(key.format);
      e.decompress = host_format.dxgi_format_uncompressed != DXGI_FORMAT_UNKNOWN &&
                     ((key.width & (info->block_width - 1)) || (key.height & (info->block_height - 1)));
      e.load_shader = e.decompress ? host_format.load_shader_decompress : host_format.load_shader;
      const DXGI_FORMAT format = e.decompress ? host_format.dxgi_format_uncompressed : host_format.dxgi_format_resource;
      if (e.load_shader == kLoadShaderIndexUnknown || !pipelines[e.load_shader] || format == DXGI_FORMAT_UNKNOWN) {
        ++stats.unsupported;
        if (logged_unsupported++ < 16) {
          REXLOG_WARN("tenchuz: N4 texture format {} ({}x{}) not supported natively", uint32_t(key.format),
                      key.width, key.height);
        }
        return Result::kUnsupported;
      }
      e.layout = texture_util::GetGuestTextureLayout(key.dimension, key.pitch, key.width, key.height,
                                                     key.depth_or_array_size, key.tiled, key.format,
                                                     key.packed_mips, key.base_page != 0, key.mip_max_level);
      e.resource.Attach(CreateResource(e, format));
      if (!e.resource) {
        return Result::kFailed;
      }
      SetDebugName(e.resource.Get(), fmt::format("tenchuz texture {:08X} {}x{} format {}", key.base_page << 12,
                                                 key.width, key.height, uint32_t(key.format)));
      it = entries.emplace(hash, std::move(e)).first;
    }
    Entry& e = it->second;

    // Reload when the guest data changed (checked once per frame).
    if (e.checked_frame != frame) {
      e.checked_frame = frame;
      const uint32_t base_address = key.base_page << 12, base_size = e.layout.base.level_data_extent_bytes;
      const uint32_t mips_address = key.mip_page << 12, mips_size = e.layout.mips_total_extent_bytes;
      uint64_t base_hash = 0, mips_hash = 0;
      uint32_t sequence = 0;
      bool reload;
      if (memory) {
        reload = !e.loaded || (key.base_page && WrittenSince(base_address, base_size, e.loaded_sequence)) ||
                 (key.mip_page && WrittenSince(mips_address, mips_size, e.loaded_sequence));
        if (reload) {
          // Watch before copying: a write from here on stamps a newer sequence.
          sequence = write_sequence.load(std::memory_order_relaxed);
          if (key.base_page) {
            Watch(base_address, base_size);
          }
          if (key.mip_page) {
            Watch(mips_address, mips_size);
          }
        }
      } else {
        base_hash = HashRange(base, key.base_page, base_size);
        mips_hash = HashRange(base, key.mip_page, mips_size);
        reload = !e.loaded || base_hash != e.base_hash || mips_hash != e.mips_hash;
      }
      if (reload) {
        if (key.base_page) {
          upload(base_address, base_size);
        }
        if (key.mip_page) {
          upload(mips_address, mips_size);
        }
        if (Load(list, e)) {
          e.loaded = true;
          e.base_hash = base_hash;
          e.mips_hash = mips_hash;
          e.loaded_sequence = sequence;
        }
      }
    }
    if (!e.loaded) {
      return Result::kFailed;
    }
    const D3D12_RESOURCE_STATES srv_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barrier(list, e.resource.Get(), e.state, srv_state);
    e.state = srv_state;

    const HostFormat& host_format = kHostFormats[uint32_t(key.format)];
    out.resource = e.resource.Get();
    out.format_unsigned = e.decompress ? host_format.dxgi_format_uncompressed : host_format.dxgi_format_unsigned;
    out.format_signed = !e.decompress && host_format.load_shader_signed == kLoadShaderIndexUnknown
                            ? host_format.dxgi_format_signed
                            : DXGI_FORMAT_UNKNOWN;
    out.host_format_swizzle = host_format.swizzle;
    out.dimension = key.dimension;
    out.mip_levels = key.mip_max_level + 1;
    out.array_size = key.dimension == xenos::DataDimension::k3D ? 1 : key.depth_or_array_size;
    return Result::kLoaded;
  }
};

TextureLoader::TextureLoader() : impl_(std::make_unique<Impl>()) {}
TextureLoader::~TextureLoader() = default;

bool TextureLoader::Initialize(ID3D12Device* device, ID3D12Resource* shared_memory, uint64_t shared_memory_size,
                               rex::memory::Memory* memory) {
  return impl_->Initialize(device, shared_memory, shared_memory_size, memory);
}

void TextureLoader::BeginFrame(uint32_t slot) {
  impl_->slot = slot % kFrames;
  impl_->uavs_used[impl_->slot] = 0;
  impl_->scratch[impl_->slot].clear();
  ++impl_->frame;
  // Release textures not used for a while (levels and cutscenes stream new
  // ones in); the GPU finished with them long ago.
  if (impl_->frame % 60 == 0) {
    for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
      if (impl_->frame - it->second.checked_frame > kEvictAfterFrames) {
        it = impl_->entries.erase(it);
      } else {
        ++it;
      }
    }
  }
}

TextureLoader::Result TextureLoader::Get(ID3D12GraphicsCommandList* list, const xenos::xe_gpu_texture_fetch_t& fetch,
                                         uint8_t* base, const Upload& upload, Texture& out) {
  return impl_->Get(list, fetch, base, upload, out);
}

TextureLoader::Stats TextureLoader::TakeFrameStats() {
  Stats s = impl_->stats;
  s.textures = uint32_t(impl_->entries.size());
  s.written_pages = impl_->written_pages.exchange(0, std::memory_order_relaxed);
  impl_->stats = {};
  return s;
}

}  // namespace tenchuz::native
