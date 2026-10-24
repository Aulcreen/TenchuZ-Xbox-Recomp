// tenchuz - wider aspect ratios. See widescreen.h.

#include "widescreen.h"

#include <cmath>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <rex/memory/utils.h>
#endif

REXCVAR_DEFINE_STRING(tenchuz_aspect_ratio, "16:9", "tenchuz",
                      "Aspect ratio the game renders for, as W:H (21:9, 32:9) or a number (2.37); "
                      "wider than 16:9 shows more to the sides");
REXCVAR_DEFINE_BOOL(tenchuz_widescreen_hud, true, "tenchuz",
                    "With a wider aspect ratio, keep the HUD, menus and movies at 16:9 in the middle "
                    "(false: stretch them)");

namespace tenchuz::widescreen {
namespace {

constexpr uint32_t kAspectConstant = 0x8200B73C;  // 16/9, .rdata
constexpr uint32_t kAspect16x9Bits = 0x3FE38E39;

bool Parse(const std::string& text, uint32_t& x, uint32_t& y) {
  const size_t colon = text.find(':');
  try {
    if (colon != std::string::npos) {
      const double w = std::stod(text.substr(0, colon)), h = std::stod(text.substr(colon + 1));
      if (w <= 0.0 || h <= 0.0) return false;
      x = uint32_t(std::lround(w * 1000.0));
      y = uint32_t(std::lround(h * 1000.0));
    } else {
      const double ratio = std::stod(text);
      if (ratio <= 0.0) return false;
      x = uint32_t(std::lround(ratio * 1000.0));
      y = 1000;
    }
  } catch (...) {
    return false;
  }
  // Within 4:3 to 48:9 (three 16:9 displays).
  const double ratio = double(x) / double(y);
  return ratio >= 4.0 / 3.0 - 1e-3 && ratio <= 48.0 / 9.0 + 1e-3;
}

}  // namespace

void AspectFraction(uint32_t& x, uint32_t& y) {
  if (!Parse(REXCVAR_GET(tenchuz_aspect_ratio), x, y)) {
    x = 16;
    y = 9;
  }
}

float AspectRatio() {
  uint32_t x, y;
  AspectFraction(x, y);
  return float(double(x) / double(y));
}

float HudScaleX() {
  const float aspect = AspectRatio();
  return REXCVAR_GET(tenchuz_widescreen_hud) && aspect > 16.0f / 9.0f + 1e-4f ? (16.0f / 9.0f) / aspect : 1.0f;
}

void PatchGame(uint8_t* virtual_membase) {
  uint32_t x, y;
  if (!Parse(REXCVAR_GET(tenchuz_aspect_ratio), x, y)) {
    REXLOG_WARN("tenchuz: --tenchuz_aspect_ratio={} not understood (W:H between 4:3 and 48:9); using 16:9",
                REXCVAR_GET(tenchuz_aspect_ratio));
    return;
  }
  const float aspect = AspectRatio();
  if (std::fabs(aspect - 16.0f / 9.0f) < 1e-4f) {
    return;
  }
  uint8_t* host = virtual_membase + kAspectConstant;
  const uint32_t current = (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) | (uint32_t(host[2]) << 8) | host[3];
  if (current != kAspect16x9Bits) {
    REXLOG_WARN("tenchuz: aspect ratio not patched: {:08X} holds {:08X}, not 16/9", kAspectConstant, current);
    return;
  }
  uint32_t bits;
  std::memcpy(&bits, &aspect, 4);
#if defined(_WIN32)
  DWORD old_protect = 0;
  const bool unprotected = VirtualProtect(host, 4, PAGE_READWRITE, &old_protect);
#else
  rex::memory::PageAccess old_access = rex::memory::PageAccess::kReadOnly;
  const bool unprotected = rex::memory::Protect(host, 4, rex::memory::PageAccess::kReadWrite, &old_access);
#endif
  host[0] = uint8_t(bits >> 24);
  host[1] = uint8_t(bits >> 16);
  host[2] = uint8_t(bits >> 8);
  host[3] = uint8_t(bits);
  if (unprotected) {
#if defined(_WIN32)
    VirtualProtect(host, 4, old_protect, &old_protect);
#else
    rex::memory::Protect(host, 4, old_access);
#endif
  }
  REXLOG_INFO("tenchuz: aspect ratio {:.4f} ({}), patched at {:08X}", aspect, REXCVAR_GET(tenchuz_aspect_ratio),
              kAspectConstant);
}

}  // namespace tenchuz::widescreen
