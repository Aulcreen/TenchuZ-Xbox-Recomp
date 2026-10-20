// tenchuz - wider aspect ratios (milestone N6.3).
//
// The game's projection takes its aspect ratio from one read-only constant,
// 16/9 at 0x8200B73C. The startup display setup (sub_822A6AE8) copies it to the
// display aspect (0x829BF1B4) that camera init reads (sub_822A6C30 via
// sub_823E0F90), and four rendering functions load it directly.
// --tenchuz_aspect_ratio replaces it before the game starts, and the native
// renderer presents at that aspect.

#pragma once

#include <cstdint>

namespace tenchuz::widescreen {

// The aspect ratio the game renders for (--tenchuz_aspect_ratio), 16/9 by default.
float AspectRatio();

// Display aspect ratio as a fraction for the presenter.
void AspectFraction(uint32_t& x, uint32_t& y);

// With an aspect ratio wider than 16:9 and --tenchuz_widescreen_hud: 16/9 over
// it, the horizontal scale that keeps the HUD, menus and movies at 16:9 in
// the middle of the screen. Else 1.
float HudScaleX();

// Writes the aspect ratio into the game image (after it's loaded, before it runs).
void PatchGame(uint8_t* virtual_membase);

}  // namespace tenchuz::widescreen
