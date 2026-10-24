// tenchuz - controllers through the Steam Input API (steam_input.h).

#include "steam_input.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <string>

#include <steam/steam_api.h>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "../net/transport.h"

namespace tenchuz::input {
namespace {

// The manifest's "gameplay" action set: a button action per pad button (named
// as the controls in pc_input.cpp), and the sticks and triggers.
struct ButtonAction {
  const char* name;
  uint16_t button;  // XINPUT_GAMEPAD_*
};
constexpr ButtonAction kButtons[] = {
    {"a", 0x1000},      {"b", 0x2000},         {"x", 0x4000},         {"y", 0x8000},          {"lb", 0x0100},
    {"rb", 0x0200},     {"ls", 0x0040},        {"rs", 0x0080},        {"start", 0x0010},      {"back", 0x0020},
    {"dpad_up", 0x0001}, {"dpad_down", 0x0002}, {"dpad_left", 0x0004}, {"dpad_right", 0x0008},
};
constexpr int kButtonCount = int(std::size(kButtons));

const char* TypeName(ESteamInputType type) {
  switch (type) {
    case k_ESteamInputType_XBox360Controller: return "Xbox 360";
    case k_ESteamInputType_XBoxOneController: return "Xbox One";
    case k_ESteamInputType_PS3Controller: return "PS3";
    case k_ESteamInputType_PS4Controller: return "PS4";
    case k_ESteamInputType_PS5Controller: return "PS5";
    case k_ESteamInputType_SwitchProController: return "Switch Pro";
    case k_ESteamInputType_SteamDeckController: return "Steam Deck";
    case k_ESteamInputType_SteamController: return "Steam Controller";
    case k_ESteamInputType_GenericGamepad: return "generic";
    default: return "other";
  }
}

// The stick with the larger deflection wins.
void MergeStick(float x, float y, int16_t& out_x, int16_t& out_y) {
  const auto value = [](float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); };
  const int16_t sx = value(x), sy = value(y);
  if (int32_t(sx) * sx + int32_t(sy) * sy > int32_t(out_x) * out_x + int32_t(out_y) * out_y) {
    out_x = sx;
    out_y = sy;
  }
}

class SteamControllers {
 public:
  bool Read(PadState& pad) {
    if (state_ == State::kOff || (state_ == State::kWaiting && !Start())) return false;
    SteamInput()->RunFrame();
    InputHandle_t handles[STEAM_INPUT_MAX_COUNT];
    const int count = SteamInput()->GetConnectedControllers(handles);
    if (count != count_) {
      count_ = count;
      std::string types;
      for (int i = 0; i < count; ++i) types += std::string(i ? ", " : " (") + TypeName(SteamInput()->GetInputTypeForHandle(handles[i]));
      REXLOG_INFO("tenchuz: input: Steam Input has {} controller(s){}{}", count, types, count ? ")" : "");
    }
    if (count <= 0 || !Resolve()) return count > 0;
    pad = {};
    for (int i = 0; i < count; ++i) {
      const InputHandle_t controller = handles[i];
      SteamInput()->ActivateActionSet(controller, set_);
      for (int b = 0; b < kButtonCount; ++b) {
        if (SteamInput()->GetDigitalActionData(controller, buttons_[b]).bState) pad.buttons |= kButtons[b].button;
      }
      const InputAnalogActionData_t left = SteamInput()->GetAnalogActionData(controller, left_stick_);
      const InputAnalogActionData_t right = SteamInput()->GetAnalogActionData(controller, right_stick_);
      MergeStick(left.x, left.y, pad.lx, pad.ly);
      MergeStick(right.x, right.y, pad.rx, pad.ry);
      const auto trigger = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
      pad.lt = std::max(pad.lt, trigger(SteamInput()->GetAnalogActionData(controller, left_trigger_).x));
      pad.rt = std::max(pad.rt, trigger(SteamInput()->GetAnalogActionData(controller, right_trigger_).x));
    }
    return true;
  }

 private:
  enum class State { kWaiting, kOn, kOff };

  // Once attached to Steam: Steam Input with this game's manifest.
  bool Start() {
    if (!net::SteamReady()) return false;
#if defined(__APPLE__)
    const std::filesystem::path folder = rex::filesystem::GetExecutableFolder().parent_path() / "Resources";
#else
    const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
#endif
    const std::filesystem::path manifest = folder / "steam_input" / "tenchuz_input_manifest.vdf";
    if (!std::filesystem::exists(manifest)) {
      REXLOG_WARN("tenchuz: input: {} is missing; controllers Steam takes over read nothing", manifest.string());
      state_ = State::kOff;
      return false;
    }
    if (!SteamInput()->Init(true)) {  // true: RunFrame() comes from Read()
      REXLOG_WARN("tenchuz: input: Steam Input didn't start");
      state_ = State::kOff;
      return false;
    }
    SteamInput()->SetInputActionManifestFilePath(manifest.string().c_str());
    REXLOG_INFO("tenchuz: input: Steam Input on, actions from {}", manifest.string());
    state_ = State::kOn;
    return true;
  }

  // The action handles, there once Steam has loaded the manifest.
  bool Resolve() {
    if (set_) return true;
    set_ = SteamInput()->GetActionSetHandle("gameplay");
    if (!set_) return false;
    for (int b = 0; b < kButtonCount; ++b) buttons_[b] = SteamInput()->GetDigitalActionHandle(kButtons[b].name);
    left_stick_ = SteamInput()->GetAnalogActionHandle("left_stick");
    right_stick_ = SteamInput()->GetAnalogActionHandle("right_stick");
    left_trigger_ = SteamInput()->GetAnalogActionHandle("left_trigger");
    right_trigger_ = SteamInput()->GetAnalogActionHandle("right_trigger");
    return true;
  }

  State state_ = State::kWaiting;
  int count_ = 0;
  InputActionSetHandle_t set_ = 0;
  InputDigitalActionHandle_t buttons_[kButtonCount] = {};
  InputAnalogActionHandle_t left_stick_ = 0, right_stick_ = 0, left_trigger_ = 0, right_trigger_ = 0;
};

SteamControllers g_controllers;

}  // namespace

bool ReadSteamControllers(PadState& pad) { return g_controllers.Read(pad); }

}  // namespace tenchuz::input
