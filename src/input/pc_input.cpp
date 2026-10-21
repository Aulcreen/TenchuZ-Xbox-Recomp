// tenchuz - keyboard and mouse controls, and scripted input for tests
// (pc_input.h).

#include "pc_input.h"

#include "generated/default/tenchuz_pch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

#include "../debug_tools.h"
#include "../profiler.h"
#include "../ui/game_menu.h"
#include "../renderer/native_renderer.h"

REXCVAR_DEFINE_STRING(tenchuz_input_device, "auto", "tenchuz/Controls",
                      "Input device: auto (a controller when one is connected, else keyboard and mouse), keyboard, "
                      "controller");
REXCVAR_DEFINE_BOOL(tenchuz_keyboard, true, "tenchuz/Controls",
                    "Retired, see tenchuz_input_device (false there becomes controller)");
REXCVAR_DEFINE_BOOL(tenchuz_mouse_camera, true, "tenchuz/Controls", "The mouse moves the camera (right stick)");
REXCVAR_DEFINE_DOUBLE(tenchuz_mouse_sensitivity, 1.0, "tenchuz/Controls", "Mouse camera sensitivity")
    .range(0.1, 5.0);
REXCVAR_DEFINE_BOOL(tenchuz_mouse_invert_y, false, "tenchuz/Controls", "Mouse camera: invert up and down");
REXCVAR_DEFINE_DOUBLE(tenchuz_mouse_deadzone, 0.27, "tenchuz/Controls",
                      "Mouse camera: right-stick deflection where small mouse movements start (the game's "
                      "dead zone is XInput's 8689 of 32767)")
    .range(0.0, 0.9);
REXCVAR_DEFINE_DOUBLE(tenchuz_walk_speed, 0.4, "tenchuz/Controls",
                      "Left-stick deflection while the walk key is held (sneaking)")
    .range(0.1, 1.0);
REXCVAR_DEFINE_STRING(tenchuz_input_script, "", "tenchuz",
                      "Test input: comma-separated <seconds>:<control>[+<control>...][/<hold ms>] (controls as "
                      "in tenchuz_key_*), or <seconds>:shot:<name>, :dump (renderer), :memdump (game globals), :profile, :menu:<seconds open>, :skip2d (wait out loading "
                      "screens, pressing Start), :quit. "
                      "Works without focus");
REXCVAR_DEFINE_BOOL(tenchuz_input_log, false, "tenchuz", "Log the merged pad state when it changes");

// Default bindings. Labels are the game's own names for its default
// controller layout (Options > Control Settings > Controller Settings).
#define TENCHUZ_CONTROLS(X)                                       X(move_up, "Move forward", "W")                                 X(move_down, "Move back", "S")                                  X(move_left, "Move left", "A")                                  X(move_right, "Move right", "D")                                X(walk, "Walk", "C")                                            X(x, "Attack", "LMB")                                           X(rt, "Strafe", "RMB")                                          X(lt, "Aid", "Shift")                                           X(a, "Jump", "Space")                                           X(rb, "Crouch", "Control")                                      X(b, "Draw sword", "F,Backspace")                               X(lb, "Item", "E")                                              X(y, "Reset view", "Q,MMB")                                     X(rs, "First-person view", "V,Mouse4")                          X(ls, "Left stick click", "Z")                                  X(back, "Map", "Tab,M")                                         X(start, "Pause", "Return,P")                                   X(dpad_up, "D-pad up", "1,WheelUp")                             X(dpad_down, "D-pad down", "2,WheelDown")                       X(dpad_left, "D-pad left", "3")                                 X(dpad_right, "D-pad right", "4")                               X(cam_up, "Camera up", "Up")                                    X(cam_down, "Camera down", "Down")                              X(cam_left, "Camera left", "Left")                              X(cam_right, "Camera right", "Right")

#define TENCHUZ_DEFINE_KEY_CVAR(id, label, keys) \
  REXCVAR_DEFINE_STRING(tenchuz_key_##id, keys, "tenchuz/Controls", label);
TENCHUZ_CONTROLS(TENCHUZ_DEFINE_KEY_CVAR)

namespace tenchuz::input {
namespace {

using rex::ui::VirtualKey;

enum ControlIndex : uint32_t {
#define TENCHUZ_CONTROL_ENUM(id, label, keys) k_##id,
  TENCHUZ_CONTROLS(TENCHUZ_CONTROL_ENUM)
#undef TENCHUZ_CONTROL_ENUM
      kControlCount
};

struct ControlDef {
  Control control;
  const char* default_keys;
};
const std::array<ControlDef, kControlCount> kControlDefs = {{
#define TENCHUZ_CONTROL_DEF(id, label, keys) {{#id, label}, keys},
    TENCHUZ_CONTROLS(TENCHUZ_CONTROL_DEF)
#undef TENCHUZ_CONTROL_DEF
}};

// Pad buttons (XINPUT_GAMEPAD_*) of the button controls.
uint16_t ButtonOf(uint32_t control) {
  switch (control) {
    case k_a: return 0x1000;
    case k_b: return 0x2000;
    case k_x: return 0x4000;
    case k_y: return 0x8000;
    case k_lb: return 0x0100;
    case k_rb: return 0x0200;
    case k_ls: return 0x0040;
    case k_rs: return 0x0080;
    case k_start: return 0x0010;
    case k_back: return 0x0020;
    case k_dpad_up: return 0x0001;
    case k_dpad_down: return 0x0002;
    case k_dpad_left: return 0x0004;
    case k_dpad_right: return 0x0008;
    default: return 0;
  }
}

// Keys beyond the 256 virtual keys: the mouse wheel.
constexpr uint32_t kWheelUp = 256, kWheelDown = 257, kKeyCount = 258;

uint32_t ParseKey(std::string_view name) {
  if (name == "WheelUp") return kWheelUp;
  if (name == "WheelDown") return kWheelDown;
  if (name == "Mouse4") return uint32_t(VirtualKey::kXButton1);
  if (name == "Mouse5") return uint32_t(VirtualKey::kXButton2);
  const VirtualKey vk = rex::ui::ParseVirtualKey(name);
  return vk == VirtualKey::kNone ? 0 : std::min<uint32_t>(uint32_t(vk), 255);
}

std::string KeyName(uint32_t key) {
  if (key == kWheelUp) return "WheelUp";
  if (key == kWheelDown) return "WheelDown";
  if (key == uint32_t(VirtualKey::kXButton1)) return "Mouse4";
  if (key == uint32_t(VirtualKey::kXButton2)) return "Mouse5";
  return rex::ui::VirtualKeyToString(VirtualKey(key));
}

std::vector<uint32_t> ParseKeys(std::string_view text) {
  std::vector<uint32_t> keys;
  while (!text.empty()) {
    const size_t comma = text.find(',');
    std::string_view token = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
    while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
    if (const uint32_t key = ParseKey(token)) keys.push_back(key);
  }
  return keys;
}

// Pad state as the game reads it (X_INPUT_GAMEPAD).
struct Pad {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int32_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const Pad&) const = default;
};

// A test script step.
struct ScriptStep {
  double at = 0;         // seconds since start
  double hold = 0.15;    // seconds
  uint32_t controls = 0;  // bit per control
  std::string command;   // shot:<name>, dump, quit, look
  std::string argument;
  double look_x = 0, look_y = 0;  // look: mouse counts per second
  bool done = false;
};

class Input final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void Attach(rex::ui::Window* window) {
    window_ = window;
    if (!REXCVAR_GET(tenchuz_keyboard) && REXCVAR_GET(tenchuz_input_device) == "auto") {
      rex::cvar::SetFlagByName("tenchuz_input_device", "controller");  // keyboard turned off before
    }
    focused_ = window->HasFocus();
    RebuildBindings();
    ParseScript();
    window->AddInputListener(this, 1);
    window->AddListener(this);
    for (const ControlDef& def : kControlDefs) {
      rex::cvar::RegisterChangeCallback(std::string("tenchuz_key_") + def.control.id,
                                        [this](std::string_view, std::string_view) { RebuildBindings(); });
    }
  }

  void Detach() {
    if (!window_) return;
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
    window_ = nullptr;
  }

  void RebuildBindings() {
    std::lock_guard lock(mutex_);
    for (uint32_t i = 0; i < kControlCount; ++i) {
      bindings_[i] = ParseKeys(rex::cvar::GetFlagByName(std::string("tenchuz_key_") + kControlDefs[i].control.id));
    }
  }

  // --- Window events (UI thread) ---

  void OnKeyDown(rex::ui::KeyEvent& e) override { Key(uint32_t(e.virtual_key()), true); }
  void OnKeyUp(rex::ui::KeyEvent& e) override { Key(uint32_t(e.virtual_key()), false); }
  void OnMouseDown(rex::ui::MouseEvent& e) override { Key(MouseKey(e), true); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { Key(MouseKey(e), false); }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (e.scroll_y() == 0) return;
    const uint32_t key = e.scroll_y() > 0 ? kWheelUp : kWheelDown;
    std::lock_guard lock(mutex_);
    if (capturing_) {
      captured_ = KeyName(key);
      capturing_ = false;
      return;
    }
    wheel_polls_[key - kWheelUp] = 3;  // a wheel notch holds its control for 3 polls
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!mouse_captured_) return;
    std::lock_guard lock(mutex_);
    mouse_dx_ += e.dx();
    mouse_dy_ += e.dy();
  }
  void OnGotFocus(rex::ui::UISetupEvent&) override { focused_ = true; }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    focused_ = false;
    {
      std::lock_guard lock(mutex_);
      key_down_.fill(false);  // key-ups landing on another window never arrive
    }
    ApplyCapture(false);
  }

  // --- Menu (UI thread) ---

  void SetMenuOpen(bool open) {
    {
      std::lock_guard lock(mutex_);
      menu_open_ = open;
      key_down_.fill(false);
      mouse_dx_ = mouse_dy_ = 0;
    }
    if (open) {
      ApplyCapture(false);
    }
    menu_cv_.notify_all();
  }
  bool MenuOpen() const { return menu_open_; }
  Device ActiveDevice() const { return active_device_.load(); }

  bool WaitWhileMenuOpen() {
    if (!menu_open_) return false;
    std::unique_lock lock(mutex_);
    menu_cv_.wait(lock, [this] { return !menu_open_; });
    return true;
  }

  void BeginCapture() {
    std::lock_guard lock(mutex_);
    capturing_ = true;
    captured_.clear();
  }
  std::string TakeCaptured() {
    std::lock_guard lock(mutex_);
    return std::exchange(captured_, {});
  }
  void CancelCapture() {
    std::lock_guard lock(mutex_);
    capturing_ = false;
    captured_.clear();
  }

  // --- Polling (game thread) ---

  // Merges keyboard, mouse and script input into the state the game read
  // (`result`: 0 if a controller is connected). Returns the new result.
  uint32_t Poll(uint8_t* base, uint32_t user, uint32_t state, uint32_t result) {
    if (user != 0 || !state) {
      if (menu_open_ && result == 0) Clear(base, state);
      return result;
    }
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const double dt = std::clamp(std::chrono::duration<double>(now - last_poll_).count(), 1.0 / 240.0, 1.0 / 15.0);
    last_poll_ = now;

    // The device the game reads: tenchuz_input_device, where "auto" picks a
    // connected controller and otherwise the keyboard and mouse.
    const bool controller_connected = result == 0;
    const std::string choice = REXCVAR_GET(tenchuz_input_device);
    const Device device = choice == "controller" ? Device::kController
                          : choice == "keyboard" ? Device::kKeyboard
                          : controller_connected ? Device::kController
                                                 : Device::kKeyboard;
    if (device != active_device_.exchange(device) || controller_connected != controller_seen_) {
      controller_seen_ = controller_connected;
      REXLOG_INFO("tenchuz: input: {} ({}, controller {})",
                  device == Device::kController ? "controller" : "keyboard and mouse", choice,
                  controller_connected ? "connected" : "not connected");
    }
    const bool keyboard = device == Device::kKeyboard;
    Pad pad;
    if (controller_connected && !keyboard) pad = Read(base, state);
    if (menu_open_) {
      if (result == 0 || keyboard) {
        Clear(base, state);
        return 0;
      }
      return result;
    }
    script_look_x_ = script_look_y_ = 0;
    const uint32_t script = ScriptControls(now);
    uint32_t held = script;
    float mouse_x = 0, mouse_y = 0;
    {
      std::lock_guard lock(mutex_);
      if (keyboard && focused_) {
        for (uint32_t i = 0; i < kControlCount; ++i) {
          for (uint32_t key : bindings_[i]) {
            const bool down = key >= kWheelUp ? wheel_polls_[key - kWheelUp] > 0 : key_down_[key];
            if (down) held |= 1u << i;
          }
        }
      }
      for (uint32_t& n : wheel_polls_) n = n ? n - 1 : 0;
      mouse_x = mouse_dx_ + float(script_look_x_ * dt);
      mouse_y = mouse_dy_ + float(script_look_y_ * dt);
      mouse_dx_ = mouse_dy_ = 0;
    }
    const bool script_look = script_look_x_ != 0 || script_look_y_ != 0;
    const bool mouse = keyboard && REXCVAR_GET(tenchuz_mouse_camera) && focused_;
    UpdateCapture(mouse);

    for (uint32_t i = 0; i < kControlCount; ++i) {
      if (held & (1u << i)) pad.buttons |= ButtonOf(i);
    }
    if (held & (1u << k_lt)) pad.lt = 0xFF;
    if (held & (1u << k_rt)) pad.rt = 0xFF;
    constexpr int32_t kFull = 32767;
    const int32_t walk =
        (held & (1u << k_walk)) ? int32_t(kFull * REXCVAR_GET(tenchuz_walk_speed)) : kFull;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    if (held & (1u << k_move_left)) lx -= walk;
    if (held & (1u << k_move_right)) lx += walk;
    if (held & (1u << k_move_up)) ly += walk;
    if (held & (1u << k_move_down)) ly -= walk;
    if (held & (1u << k_cam_left)) rx -= kFull;
    if (held & (1u << k_cam_right)) rx += kFull;
    if (held & (1u << k_cam_up)) ry += kFull;
    if (held & (1u << k_cam_down)) ry -= kFull;
    if (lx && ly) {  // diagonals keep the stick's length
      lx = int32_t(lx * 0.7071f);
      ly = int32_t(ly * 0.7071f);
    }
    if ((mouse && mouse_captured_) || script_look) {
      // Counts per second to deflection: full deflection at 2500 counts/s
      // at sensitivity 1, past the game's dead zone.
      const double sens = REXCVAR_GET(tenchuz_mouse_sensitivity);
      const double dead = REXCVAR_GET(tenchuz_mouse_deadzone);
      const double invert = REXCVAR_GET(tenchuz_mouse_invert_y) ? -1.0 : 1.0;
      auto axis = [&](double counts, double& smoothed) {
        smoothed = 0.5 * smoothed + 0.5 * (counts / dt);  // two polls of smoothing
        const double v = std::clamp(smoothed * sens / 2500.0, -1.0, 1.0);
        if (std::abs(v) < 1e-3) return 0;
        return int32_t(std::copysign(dead + (1.0 - dead) * std::abs(v), v) * kFull);
      };
      rx += axis(mouse_x, smooth_x_);
      ry += axis(-mouse_y * invert, smooth_y_);
    } else {
      smooth_x_ = smooth_y_ = 0;
    }
    pad.lx = std::clamp(pad.lx + lx, -kFull, kFull);
    pad.ly = std::clamp(pad.ly + ly, -kFull, kFull);
    pad.rx = std::clamp(pad.rx + rx, -kFull, kFull);
    pad.ry = std::clamp(pad.ry + ry, -kFull, kFull);

    const bool connected = keyboard || controller_connected || script;
    if (!connected) return result;
    if (REXCVAR_GET(tenchuz_input_log) && !(pad == last_pad_)) {
      REXLOG_INFO("tenchuz: input buttons {:04X} lt {} rt {} left {} {} right {} {}", pad.buttons, pad.lt, pad.rt,
                  pad.lx, pad.ly, pad.rx, pad.ry);
    }
    if (!(pad == last_pad_)) ++packet_;
    last_pad_ = pad;
    Write(base, state, pad);
    return 0;
  }

 private:
  static uint32_t MouseKey(const rex::ui::MouseEvent& e) {
    switch (e.button()) {
      case rex::ui::MouseEvent::Button::kLeft: return uint32_t(VirtualKey::kLButton);
      case rex::ui::MouseEvent::Button::kRight: return uint32_t(VirtualKey::kRButton);
      case rex::ui::MouseEvent::Button::kMiddle: return uint32_t(VirtualKey::kMButton);
      case rex::ui::MouseEvent::Button::kX1: return uint32_t(VirtualKey::kXButton1);
      case rex::ui::MouseEvent::Button::kX2: return uint32_t(VirtualKey::kXButton2);
      default: return 0;
    }
  }

  void Key(uint32_t key, bool down) {
    if (!key || key >= 256) return;
    std::lock_guard lock(mutex_);
    if (capturing_ && down) {
      if (key == uint32_t(VirtualKey::kEscape)) return;  // the menu cancels the capture
      captured_ = KeyName(key);
      capturing_ = false;
      return;
    }
    key_down_[key] = down && !menu_open_;
  }

  static Pad Read(uint8_t* base, uint32_t state) {
    Pad pad;
    pad.buttons = REX_LOAD_U16(state + 4);
    pad.lt = REX_LOAD_U8(state + 6);
    pad.rt = REX_LOAD_U8(state + 7);
    pad.lx = int16_t(REX_LOAD_U16(state + 8));
    pad.ly = int16_t(REX_LOAD_U16(state + 10));
    pad.rx = int16_t(REX_LOAD_U16(state + 12));
    pad.ry = int16_t(REX_LOAD_U16(state + 14));
    return pad;
  }

  void Write(uint8_t* base, uint32_t state, const Pad& pad) const {
    REX_STORE_U32(state + 0, packet_);
    REX_STORE_U16(state + 4, pad.buttons);
    REX_STORE_U8(state + 6, pad.lt);
    REX_STORE_U8(state + 7, pad.rt);
    REX_STORE_U16(state + 8, uint16_t(int16_t(pad.lx)));
    REX_STORE_U16(state + 10, uint16_t(int16_t(pad.ly)));
    REX_STORE_U16(state + 12, uint16_t(int16_t(pad.rx)));
    REX_STORE_U16(state + 14, uint16_t(int16_t(pad.ry)));
  }

  void Clear(uint8_t* base, uint32_t state) const { Write(base, state, Pad{}); }

  // The mouse is captured (hidden, relative motion) while the camera uses it,
  // the window has focus and the menu is closed.
  void UpdateCapture(bool want) {
    if (want == capture_wanted_ || !window_) return;
    capture_wanted_ = want;
    window_->app_context().CallInUIThreadDeferred([this, want] {
      if (!menu_open_) ApplyCapture(want && focused_);
    });
  }

  void ApplyCapture(bool capture) {  // UI thread
    if (!window_ || capture == mouse_captured_) return;
    if (capture) {
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      window_->CaptureMouse();
      if (!window_->SetRelativeMouseMode(true)) {
        REXLOG_WARN("tenchuz: pointer lock unavailable, mouse camera off");
      }
      mouse_captured_ = true;
    } else {
      window_->SetRelativeMouseMode(false);
      window_->ReleaseMouse();
      window_->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      mouse_captured_ = false;
      capture_wanted_ = false;  // re-requested by the next poll that wants it
    }
    std::lock_guard lock(mutex_);
    mouse_dx_ = mouse_dy_ = 0;
  }

  void ParseScript() {
    const std::string text = REXCVAR_GET(tenchuz_input_script);
    std::string_view rest(text);
    while (!rest.empty()) {
      const size_t comma = rest.find(',');
      std::string_view item = rest.substr(0, comma);
      rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
      const size_t colon = item.find(':');
      if (colon == std::string_view::npos) continue;
      ScriptStep step;
      step.at = std::strtod(std::string(item.substr(0, colon)).c_str(), nullptr);
      std::string_view what = item.substr(colon + 1);
      if (const size_t slash = what.find('/'); slash != std::string_view::npos) {
        step.hold = std::strtod(std::string(what.substr(slash + 1)).c_str(), nullptr) / 1000.0;
        what = what.substr(0, slash);
      }
      if (what.starts_with("look:")) {  // look:<counts/s x>:<counts/s y>, mouse movement
        const std::string args(what.substr(5));
        step.command = "look";
        step.look_x = std::strtod(args.c_str(), nullptr);
        const size_t c = args.find(':');
        step.look_y = c == std::string::npos ? 0 : std::strtod(args.c_str() + c + 1, nullptr);
        script_.push_back(std::move(step));
        continue;
      }
      if (what.starts_with("shot:") || what == "dump" || what == "memdump" || what == "skip2d" || what == "profile" || what.starts_with("menu") ||
          what == "quit") {
        const size_t c = what.find(':');
        step.command = std::string(what.substr(0, c));
        step.argument = c == std::string_view::npos ? std::string() : std::string(what.substr(c + 1));
      } else {
        while (!what.empty()) {
          const size_t plus = what.find('+');
          const std::string_view id = what.substr(0, plus);
          what = plus == std::string_view::npos ? std::string_view() : what.substr(plus + 1);
          for (uint32_t i = 0; i < kControlCount; ++i) {
            if (id == kControlDefs[i].control.id) step.controls |= 1u << i;
          }
        }
      }
      script_.push_back(std::move(step));
    }
    // Steps run in time order however they were listed.
    std::stable_sort(script_.begin(), script_.end(),
                     [](const ScriptStep& a, const ScriptStep& b) { return a.at < b.at; });
    if (!script_.empty()) {
      REXLOG_INFO("tenchuz: input script, {} steps", script_.size());
    }
  }

  uint32_t ScriptControls(std::chrono::steady_clock::time_point now) {
    if (script_.empty()) return 0;
    const double real = std::chrono::duration<double>(now - start_).count();
    if (skip_.active) return SkipLoading(real);
    const double t = real - frozen_;
    uint32_t controls = 0;
    for (ScriptStep& step : script_) {
      if (t < step.at) break;
      if (step.command == "look") {
        if (t < step.at + step.hold) {
          script_look_x_ = step.look_x;
          script_look_y_ = step.look_y;
        }
        continue;
      }
      if (!step.command.empty()) {
        if (!step.done) {
          step.done = true;
          if (step.command == "skip2d") {
            skip_ = {true, 0, 0, step.at, real};
            return 0;
          }
          RunCommand(step);
        }
        continue;
      }
      if (t < step.at + step.hold) controls |= step.controls;
    }
    return controls;
  }

  // "skip2d": the script clock stops while the game goes through 2D screens
  // (loading, mission text), pressing Start every 2 s on them, and goes on
  // once a 3D scene has been drawn for 45 frames in a row.
  uint32_t SkipLoading(double real) {
    const int depth = native::LastFrameDepthDraws();
    frozen_ = real - skip_.at;
    if (skip_.phase == 0) {
      if (depth == 0) skip_.phase = 1;
      return 0;
    }
    skip_.run_3d = depth > 0 ? skip_.run_3d + 1 : 0;
    if (skip_.run_3d >= 45) {
      REXLOG_INFO("tenchuz: input script: 3D after {:.1f} s of 2D screens", real - skip_.since);
      skip_.active = false;
      return 0;
    }
    const double in = std::fmod(real - skip_.since, 2.0);
    return depth == 0 && in > 1.0 && in < 1.15 ? 1u << k_start : 0;
  }

  static void RunCommand(const ScriptStep& step) {
    REXLOG_INFO("tenchuz: input script {} {} at {:.1f} s", step.command, step.argument, step.at);
    if (step.command == "shot") {
      native::RequestScreenshot(std::filesystem::path("logs") / (step.argument + ".png"));
    } else if (step.command == "dump") {
      native::RequestFrameDump();
    } else if (step.command == "memdump") {
      debug_tools::RequestImageDump();
    } else if (step.command == "profile") {
      profiler::RequestCapture();
    } else if (step.command == "menu") {
      // The menu pauses the game, script included: "menu:<seconds>" closes it again.
      // menu:<seconds>[:<tab>]
      const size_t colon = step.argument.find(':');
      game_menu::RequestToggle(colon == std::string::npos ? -1 : std::atoi(step.argument.c_str() + colon + 1));
      if (const double seconds = std::atof(step.argument.c_str()); seconds > 0) {
        std::thread([seconds] {
          std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
          game_menu::RequestToggle();
        }).detach();
      }
    } else if (step.command == "quit") {
      std::_Exit(0);
    }
  }

  rex::ui::Window* window_ = nullptr;
  std::mutex mutex_;
  std::condition_variable menu_cv_;
  std::array<std::vector<uint32_t>, kControlCount> bindings_;
  std::array<bool, 256> key_down_{};
  std::array<uint32_t, 2> wheel_polls_{};
  float mouse_dx_ = 0, mouse_dy_ = 0;
  double smooth_x_ = 0, smooth_y_ = 0;
  std::atomic<bool> focused_{false};
  std::atomic<bool> menu_open_{false};
  std::atomic<bool> mouse_captured_{false};
  bool capture_wanted_ = false;
  std::atomic<Device> active_device_{Device::kKeyboard};
  bool controller_seen_ = false;
  bool capturing_ = false;
  std::string captured_;
  std::vector<ScriptStep> script_;
  struct Skip {
    bool active = false;
    int phase = 0;   // 0: waiting for a 2D screen, 1: on 2D screens
    int run_3d = 0;  // 3D frames in a row
    double at = 0;   // the step's script time (the clock stops there)
    double since = 0;
  } skip_;
  double frozen_ = 0;  // seconds the script clock has stood still
  double script_look_x_ = 0, script_look_y_ = 0;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_poll_ = start_;
  Pad last_pad_;
  uint32_t packet_ = 1;
};

Input g_input;
std::vector<Control> g_controls = [] {
  std::vector<Control> controls;
  for (const ControlDef& def : kControlDefs) controls.push_back(def.control);
  return controls;
}();

const ControlDef* FindControl(const char* id) {
  for (const ControlDef& def : kControlDefs) {
    if (std::strcmp(def.control.id, id) == 0) return &def;
  }
  return nullptr;
}

}  // namespace

void Initialize(rex::ui::Window* window) {
  if (window) g_input.Attach(window);
}

void Shutdown() { g_input.Detach(); }

const std::vector<Control>& Controls() { return g_controls; }

std::string Binding(const char* id) { return rex::cvar::GetFlagByName(std::string("tenchuz_key_") + id); }

void SetBinding(const char* id, const std::string& keys) {
  rex::cvar::SetFlagByName(std::string("tenchuz_key_") + id, keys);
  g_input.RebuildBindings();
}

std::string DefaultBinding(const char* id) {
  const ControlDef* def = FindControl(id);
  return def ? def->default_keys : "";
}

void BeginCapture() { g_input.BeginCapture(); }
std::string TakeCaptured() { return g_input.TakeCaptured(); }
void CancelCapture() { g_input.CancelCapture(); }
void SetMenuOpen(bool open) { g_input.SetMenuOpen(open); }
bool MenuOpen() { return g_input.MenuOpen(); }
bool WaitWhileMenuOpen() { return g_input.WaitWhileMenuOpen(); }
Device ActiveDevice() { return g_input.ActiveDevice(); }

}  // namespace tenchuz::input

REX_EXTERN(__imp__sub_824103F8);
// XInputGetState(user r3, XINPUT_STATE* r4): XamInputGetState(user, 0, state).
REX_HOOK_RAW(sub_824103F8) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_824103F8(ctx, base);
  ctx.r3.u64 = tenchuz::input::g_input.Poll(base, user, state, ctx.r3.u32);
}
