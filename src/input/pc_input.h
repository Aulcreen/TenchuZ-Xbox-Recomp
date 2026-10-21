// tenchuz - keyboard and mouse controls, and scripted input for tests.
//
// The game reads its controller once a frame through its XInputGetState
// wrapper (sub_824103F8, called from the pad poller sub_8228C7D8). The hook
// merges into that state:
//   - keyboard keys and mouse buttons bound to pad controls (tenchuz_key_*
//     cvars, saved in tenchuz.toml by the game menu);
//   - mouse movement as the right stick (camera), scaled by the time between
//     polls so the camera turns the same at 30 and 60 fps;
//   - a test script (--tenchuz_input_script), which works without focus.
// tenchuz_input_device picks the device: "auto" (the default) reads a
// connected controller and otherwise the keyboard and mouse, "keyboard" and
// "controller" only that one. The keyboard and mouse make user 0 a connected
// controller.
//
// While the game menu is open (game_menu.h), the game sees no input and its
// main loop waits.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::ui {
class Window;
}

namespace tenchuz::input {

void Initialize(rex::ui::Window* window);
void Shutdown();

// A bindable pad control.
struct Control {
  const char* id;     // cvar tenchuz_key_<id>
  const char* label;  // shown in the menu
};
const std::vector<Control>& Controls();

// Key names bound to a control (comma-separated, as in the cvar), and setting
// them. Names are the SDK's (ParseVirtualKey), plus WheelUp, WheelDown, Mouse4
// and Mouse5.
std::string Binding(const char* id);
void SetBinding(const char* id, const std::string& keys);
std::string DefaultBinding(const char* id);

// The menu's key capture: the next key or mouse button pressed, as a name, or
// empty while none has been.
void BeginCapture();
std::string TakeCaptured();
void CancelCapture();

// The device the game is reading now (with "auto", what was detected).
enum class Device { kKeyboard, kController };
Device ActiveDevice();

// The game menu: open blocks game input and frees the cursor.
void SetMenuOpen(bool open);
bool MenuOpen();

// Main loop, once a frame (game thread): waits while the menu is open.
// Returns true if it waited.
bool WaitWhileMenuOpen();

}  // namespace tenchuz::input
