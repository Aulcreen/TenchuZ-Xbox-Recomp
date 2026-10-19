// Geometry shader generator vendored from the ReXGlue SDK's D3D12 pipeline
// cache (see geometry_shader.cpp). Key and enum as in
// include/rex/graphics/d3d12/pipeline_cache.h.

#pragma once

#include <cstdint>
#include <vector>

namespace rex::graphics::vendored {

enum class PipelineGeometryShader : uint32_t {
  kNone,
  kPointList,
  kRectangleList,
  kQuadList,
};

union GeometryShaderKey {
  uint32_t key;
  struct {
    PipelineGeometryShader type : 2;
    uint32_t interpolator_count : 5;
    uint32_t user_clip_plane_count : 3;
    uint32_t user_clip_plane_cull : 1;
    uint32_t has_vertex_kill_and : 1;
    uint32_t has_point_size : 1;
    uint32_t has_point_coordinates : 1;
    // PA_CL_CLIP_CNTL::ps_ucp_mode for point primitives.
    uint32_t point_ps_ucp_mode : 2;
  };

  GeometryShaderKey() : key(0) {}
};

// DXBC container of the geometry shader for `key`, whose input signature
// matches the translated vertex shader's outputs.
void CreateDxbcGeometryShader(GeometryShaderKey key, std::vector<uint32_t>& shader_out);

}  // namespace rex::graphics::vendored
