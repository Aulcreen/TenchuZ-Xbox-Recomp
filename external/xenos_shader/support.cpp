// Stand-ins for the few SDK GPU-plugin symbols the vendored shader translator
// references. Not vendored from the SDK: written for tenchuz.
//
// The cvars are deliberately NOT registered: the GPU plugin registers cvars
// with these names in the shared registry, and a second registration would
// clash. The translator only reads them, so plain storage with the SDK
// defaults is enough.

#include <cstdint>
#include <string>

#include <rex/graphics/flags.h>
#include <rex/graphics/util/draw.h>

std::string& FLAGS_dump_shaders_storage_() {
  static std::string value;  // SDK default: "" (no shader dumps)
  return value;
}

bool& FLAGS_use_fuzzy_alpha_epsilon_storage_() {
  static bool value = false;  // SDK default
  return value;
}

namespace rex::graphics::draw_util {
// Same values as the SDK's src/graphics/util/draw.cpp.
const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
}  // namespace rex::graphics::draw_util
