// tenchuz - controllers that Steam Input has taken over.
//
// Attached to Steam (net/transport_steam.cpp), the game runs as App ID 480,
// Spacewar, which has Steam Input on for every controller type. Steam's
// overlay, which SteamAPI_InitEx() loads into the game, then hides the real
// controller (XInput reports no controller at all), and Spacewar's own Steam
// Input configuration drives no virtual controller: the game would read
// nothing, whether Steam came before or after the game. So while Steam has
// controllers, they're read through the Steam Input API with this game's
// action manifest (steam_input/ next to the exe, in Resources on macOS;
// source in src/input/steam_input/), whose configuration maps every pad
// control to an action of the same name. Like any game's configuration, Steam
// applies it only while the game window has focus.

#pragma once

#include <cstdint>

#ifndef TENCHUZ_STEAM
#define TENCHUZ_STEAM 0
#endif

namespace tenchuz::input {

// Pad state as XINPUT_GAMEPAD has it.
struct PadState {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

#if TENCHUZ_STEAM
// Game thread, once a frame: every controller Steam Input has, merged into
// `pad`. False while it has none (or the game isn't attached to Steam).
bool ReadSteamControllers(PadState& pad);
#else
inline bool ReadSteamControllers(PadState&) { return false; }
#endif

}  // namespace tenchuz::input
