// tenchuz - native renderer shader cache. See shader_cache.h.
//
// Modification selection follows the SDK's D3D12 pipeline cache
// (GetCurrentVertexShaderModification / GetCurrentPixelShaderModification).

#include "shader_cache.h"

#include "generated/default/tenchuz_pch.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>

REXCVAR_DEFINE_BOOL(tenchuz_native_dump_shaders, false, "tenchuz",
                    "Write each new shader's microcode disassembly to logs/shaders/");

namespace tenchuz::native {

namespace gfx = rex::graphics;
namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;

// The SDK's ucode analysis assumes a valid program (garbage can crash its
// disassembler), so it runs under a structured exception handler.
bool AnalyzeGuarded(gfx::Shader& shader, rex::string::StringBuffer& disasm_buffer) {
  __try {
    shader.AnalyzeUcode(disasm_buffer);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

struct ShaderCache::Impl {
  explicit Impl(rex::ui::GraphicsProvider::GpuVendorID vendor)
      : translator(vendor, /*bindless_resources_used=*/false, /*edram_rov_used=*/false) {}

  gfx::DxbcShaderTranslator translator;
  rex::string::StringBuffer disasm_buffer;
  std::unordered_map<uint64_t, std::unique_ptr<gfx::Shader>> shaders;
  uint32_t logged_failures = 0;

  gfx::Shader* GetShader(xenos::ShaderType type, const pm4::ShaderLoad& load, uint8_t* base,
                         ShaderStats& stats) {
    // Xenos programs are at most a few thousand dwords; anything else is not a
    // shader the D3D loaded (garbage from a mis-decoded packet).
    if (!load.size_dwords || load.size_dwords > 0x4000 || (!load.immediate && !load.address)) {
      return nullptr;
    }
    // IM_LOAD points at physical memory (mirrored at 0xA0000000); IM_LOAD_IMMEDIATE
    // carries the microcode after its three header dwords.
    const uint32_t guest =
        load.immediate ? load.address + 12 : (0xA0000000u | (load.address & 0x1FFFFFFFu));
    const auto* ucode = reinterpret_cast<const uint32_t*>(REX_RAW_ADDR(guest));
    const uint64_t hash = XXH3_64bits(ucode, size_t(load.size_dwords) * 4);
    const uint64_t key = hash ^ (type == xenos::ShaderType::kPixel ? 0x9E3779B97F4A7C15ull : 0);
    auto it = shaders.find(key);
    if (it != shaders.end()) {
      return it->second.get();
    }
    // DxbcShader: the translator records texture/sampler bindings on it.
    auto shader = std::make_unique<gfx::DxbcShader>(type, hash, ucode, load.size_dwords);
    if (!AnalyzeGuarded(*shader, disasm_buffer)) {
      // Not a real shader (a mis-decoded load): never use it, never retry it.
      if (logged_failures++ < 16) {
        REXLOG_WARN("tenchuz: N4 {} shader {:016X} at {:08X} could not be analyzed",
                    type == xenos::ShaderType::kPixel ? "pixel" : "vertex", hash, load.address);
      }
      ++stats.failures;
      shaders.emplace(key, nullptr);
      return nullptr;
    }
    ++stats.unique_shaders;
    if (REXCVAR_GET(tenchuz_native_dump_shaders)) {
      std::error_code ec;
      std::filesystem::create_directories("logs/shaders", ec);
      std::ofstream(fmt::format("logs/shaders/{:016X}.{}.txt", hash,
                                type == xenos::ShaderType::kPixel ? "ps" : "vs"))
          << shader->ucode_disassembly();
    }
    return shaders.emplace(key, std::move(shader)).first->second.get();
  }

  gfx::Shader::Translation* Translate(gfx::Shader& shader, uint64_t modification,
                                      ShaderStats& stats) {
    bool is_new = false;
    gfx::Shader::Translation* translation = shader.GetOrCreateTranslation(modification, &is_new);
    if (is_new) {
      ++stats.translations;
      if (!translator.TranslateAnalyzedShader(*translation)) {
        ++stats.failures;
        if (logged_failures++ < 16) {
          REXLOG_WARN("tenchuz: N4 {} shader {:016X} modification {:016X} failed to translate",
                      shader.type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
                      shader.ucode_data_hash(), modification);
        }
      }
    }
    return translation->is_valid() ? translation : nullptr;
  }
};

ShaderCache::ShaderCache(rex::ui::GraphicsProvider::GpuVendorID vendor)
    : impl_(std::make_unique<Impl>(vendor)) {}

ShaderCache::~ShaderCache() = default;

bool ShaderCache::Prepare(const pm4::DrawInfo& draw, const pm4::Decoder& state, uint8_t* base,
                          DrawShaders& out) {
  ++stats_.draws;
  // The decoder's register array has the RegisterFile layout (values[0x5003]).
  const auto& regs = *reinterpret_cast<const gfx::RegisterFile*>(state.regs);
  out = {};

  gfx::Shader* vs = impl_->GetShader(xenos::ShaderType::kVertex, state.vertex_shader, base, stats_);
  gfx::Shader* ps = impl_->GetShader(xenos::ShaderType::kPixel, state.pixel_shader, base, stats_);
  if (!vs || !vs->is_ucode_analyzed()) {
    return false;
  }

  const auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  // Primitive type from the draw initiator (DRAW_INDX: second payload dword).
  const uint32_t initiator = draw.opcode == 0x22 || draw.opcode == 0x34 ? draw.words[1] : draw.words[0];
  const auto prim_type = xenos::PrimitiveType(initiator & 0x3F);

  uint32_t ps_param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      ps ? (vs->writes_interpolators() &
            ps->GetInterpolatorInputMask(sq_program_cntl, regs.Get<reg::SQ_CONTEXT_MISC>(),
                                         ps_param_gen_pos))
         : 0;

  gfx::DxbcShaderTranslator::Modification vs_mod(impl_->translator.GetDefaultVertexShaderModification(
      vs->GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg),
      gfx::Shader::HostVertexShaderType::kVertex));
  vs_mod.vertex.interpolator_mask = interpolator_mask;
  const auto clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const uint32_t user_clip_planes = clip_cntl.clip_disable ? 0 : clip_cntl.ucp_ena;
  vs_mod.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  vs_mod.vertex.user_clip_plane_cull = uint32_t(user_clip_planes && clip_cntl.ucp_cull_only_ena);
  vs_mod.vertex.point_ps_ucp_mode = clip_cntl.ps_ucp_mode;
  vs_mod.vertex.vertex_kill_and =
      uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b100) && !clip_cntl.vtx_kill_or);
  vs_mod.vertex.output_point_size = uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                                             prim_type == xenos::PrimitiveType::kPointList);
  out.vertex = impl_->Translate(*vs, vs_mod.value, stats_);

  if (ps && ps->is_ucode_analyzed()) {
    gfx::DxbcShaderTranslator::Modification ps_mod(impl_->translator.GetDefaultPixelShaderModification(
        ps->GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                                regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                                regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = uint32_t(prim_type == xenos::PrimitiveType::kPointList);
    }
    out.pixel = impl_->Translate(*ps, ps_mod.value, stats_);
  }
  return out.vertex != nullptr;
}

}  // namespace tenchuz::native
