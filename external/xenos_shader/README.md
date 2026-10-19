# Vendored Xenos → DXBC shader translator

Source files copied unmodified from the ReXGlue SDK
(https://github.com/rexglue/rexglue-sdk) at commit
`bd833a2ab69d305f99ddf57418fc38ffec8af93d` (nightly 2026-10-02), the commit the SDK binaries in
`third_party/rexglue-bin` were built from. The SDK ships this code only inside its GPU plugin
(`rexgpu-xenos`), which exports nothing else, so the native renderer compiles it itself.

| File | SDK path |
|---|---|
| `shader.cpp`, `translator.cpp`, `translator_disasm.cpp` | `src/graphics/pipeline/shader/` |
| `dxbc.cpp`, `dxbc_translator*.cpp` | `src/graphics/pipeline/shader/` |
| `ucode.cpp` | `src/graphics/format/ucode.cpp` |
| `geometry_shader.cpp` | `PipelineCache::CreateDxbcGeometryShader` from `src/graphics/d3d12/pipeline_cache.cpp` (body unchanged, as a free function) |
| `shaders/texture_load_*_cs.h` | `src/graphics/shaders/bytecode/d3d12_5_1/` (the unscaled loaders the D3D12 backend uses) |
| `texture/util.cpp`, `texture/info.cpp`, `texture/info_formats.cpp`, `texture/extent.cpp` | `src/graphics/pipeline/texture/` |
| `thirdparty/dxbc/DXBCChecksum.*` | `thirdparty/dxbc/` |

`support.cpp` is not from the SDK: it provides the few GPU-plugin symbols the translator uses
(two read-only cvars and one sample-position table).

License: BSD-3-Clause (ReXGlue, derived from Xenia). See `LICENSE`.
