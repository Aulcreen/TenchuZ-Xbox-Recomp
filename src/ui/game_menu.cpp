// tenchuz - the game menu (game_menu.h).
//
// Labels and controls only, no explanations on screen. A "(restart)" tag
// marks settings read at startup.

#include "game_menu.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "../input/pc_input.h"
#include "audio_volume.h"

namespace tenchuz::game_menu {
namespace {

rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::Window* g_window = nullptr;
std::filesystem::path g_config_path;

void Close();

// Closes the menu after the frame it is drawing (and quits the game).
void DeferClose(bool quit) {
  if (!g_window) return;
  g_window->app_context().CallInUIThreadDeferred([quit] {
    Close();
    if (quit && g_window) g_window->RequestClose();
  });
}

// The settings the menu writes to tenchuz.toml (other lines there are kept).
constexpr const char* kSavedSettings[] = {
    "tenchuz_frame_rate",       "fullscreen",           "tenchuz_aspect_ratio", "tenchuz_native_resolution_scale",
    "tenchuz_native_post_effect", "tenchuz_native_msaa_samples", "tenchuz_widescreen_hud", "tenchuz_input_device",
    "tenchuz_mouse_camera",     "tenchuz_mouse_sensitivity", "tenchuz_mouse_invert_y", "tenchuz_walk_speed",
    "tenchuz_volume",           "audio_mute",           "tenchuz_netplay",
};

// Settings the menu used to write: their lines are dropped.
constexpr const char* kRetiredSettings[] = {"tenchuz_keyboard"};

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
bool GetBool(const char* name) { return Get(name) == "true"; }

// Merges the menu's settings into `path`: replaces their lines, keeps the rest.
void Save(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
      const size_t eq = line.find('=');
      std::string key = line.substr(0, eq);
      key.erase(key.find_last_not_of(" \t") + 1);
      bool ours = key.starts_with("tenchuz_key_");
      for (const char* name : kSavedSettings) ours = ours || key == name;
      for (const char* name : kRetiredSettings) ours = ours || key == name;
      if (!ours) lines.push_back(line);
    }
  }
  auto quote = [](const std::string& v) {
    const bool plain = v == "true" || v == "false" ||
                       (!v.empty() && v.find_first_not_of("0123456789.-") == std::string::npos);
    return plain ? v : "\"" + v + "\"";
  };
  for (const char* name : kSavedSettings) {
    lines.push_back(std::string(name) + " = " + quote(Get(name)));
  }
  for (const input::Control& c : input::Controls()) {
    lines.push_back("tenchuz_key_" + std::string(c.id) + " = \"" + input::Binding(c.id) + "\"");
  }
  std::ofstream out(path, std::ios::trunc);
  for (const std::string& line : lines) out << line << "\n";
  if (!out) REXLOG_WARN("tenchuz: game menu: can't write {}", path.string());
}

std::vector<std::string> SplitKeys(const std::string& keys) {
  std::vector<std::string> out;
  std::stringstream ss(keys);
  for (std::string k; std::getline(ss, k, ',');) {
    k.erase(0, k.find_first_not_of(' '));
    k.erase(k.find_last_not_of(' ') + 1);
    if (!k.empty()) out.push_back(k);
  }
  return out;
}

std::string JoinKeys(const std::vector<std::string>& keys) {
  std::string out;
  for (const std::string& k : keys) {
    if (k.empty()) continue;
    if (!out.empty()) out += ",";
    out += k;
  }
  return out;
}

class GameMenu final : public rex::ui::ImGuiDialog {
 public:
  GameMenu(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path)
      : ImGuiDialog(drawer), config_path_(std::move(config_path)) {}

  // Esc while waiting for a key: back out of that instead of closing.
  bool CancelCapture() {
    if (capture_control_ < 0) return false;
    input::CancelCapture();
    capture_control_ = -1;
    return true;
  }

  void OpenTab(int tab) { open_tab_ = tab; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    scale_ = scale;
    // Dim the paused game.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    const ImVec2 size(640 * scale, 470 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    const int pushed_colors = PushTheme();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * scale, 4 * scale));
    ImGui::Begin("Tenchu Z", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(scale);
    if (ImGui::BeginTabBar("tabs")) {
      static constexpr const char* kTabs[] = {"Display", "Controls", "Audio", "Online"};
      for (int i = 0; i < 4; ++i) {
        const ImGuiTabItemFlags flags = open_tab_ == i ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(kTabs[i], nullptr, flags)) {
          switch (i) {
            case 0: DrawDisplay(); break;
            case 1: DrawControls(); break;
            case 2: DrawAudio(); break;
            default: DrawOnline(); break;
          }
          ImGui::EndTabItem();
        }
      }
      open_tab_ = -1;
      ImGui::EndTabBar();
    }
    // Buttons on the bottom row.
    const float button_w = 140 * scale;
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x,
                               size.y - ImGui::GetFrameHeight() - ImGui::GetStyle().WindowPadding.y));
    if (ImGui::Button("Resume", ImVec2(button_w, 0)) && !closing_) {
      closing_ = true;
      DeferClose(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit game", ImVec2(button_w, 0)) && !closing_) {
      closing_ = true;
      DeferClose(true);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(pushed_colors);
    if (changed_) {
      Save(config_path_);
      changed_ = false;
    }
  }

 private:
  // Ink black and Tenchu red. Returns how many colours to pop.
  static int PushTheme() {
    const ImVec4 red(0.55f, 0.08f, 0.07f, 1.0f), red_hover(0.70f, 0.12f, 0.10f, 1.0f),
        red_active(0.80f, 0.16f, 0.12f, 1.0f), panel(0.06f, 0.05f, 0.05f, 0.96f), frame(0.16f, 0.13f, 0.12f, 1.0f),
        text(0.93f, 0.90f, 0.84f, 1.0f);
    const std::pair<ImGuiCol, ImVec4> colors[] = {
        {ImGuiCol_WindowBg, panel},
        {ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0)},
        {ImGuiCol_PopupBg, panel},
        {ImGuiCol_Border, ImVec4(0.40f, 0.08f, 0.07f, 1.0f)},
        {ImGuiCol_TitleBg, red},
        {ImGuiCol_TitleBgActive, red},
        {ImGuiCol_Text, text},
        {ImGuiCol_FrameBg, frame},
        {ImGuiCol_FrameBgHovered, ImVec4(0.24f, 0.18f, 0.16f, 1.0f)},
        {ImGuiCol_FrameBgActive, ImVec4(0.30f, 0.20f, 0.18f, 1.0f)},
        {ImGuiCol_Button, frame},
        {ImGuiCol_ButtonHovered, red_hover},
        {ImGuiCol_ButtonActive, red_active},
        {ImGuiCol_Header, red},
        {ImGuiCol_HeaderHovered, red_hover},
        {ImGuiCol_HeaderActive, red_active},
        {ImGuiCol_Tab, frame},
        {ImGuiCol_TabHovered, red_hover},
        {ImGuiCol_TabActive, red},
        {ImGuiCol_CheckMark, ImVec4(0.95f, 0.30f, 0.22f, 1.0f)},
        {ImGuiCol_SliderGrab, red_hover},
        {ImGuiCol_SliderGrabActive, red_active},
        {ImGuiCol_Separator, ImVec4(0.40f, 0.08f, 0.07f, 1.0f)},
        {ImGuiCol_TableHeaderBg, ImVec4(0.20f, 0.07f, 0.06f, 1.0f)},
        {ImGuiCol_TableRowBgAlt, ImVec4(1, 1, 1, 0.03f)},
        {ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0.2f)},
        {ImGuiCol_ScrollbarGrab, red},
        {ImGuiCol_ScrollbarGrabHovered, red_hover},
        {ImGuiCol_ScrollbarGrabActive, red_active},
    };
    for (const auto& [index, color] : colors) ImGui::PushStyleColor(index, color);
    return int(std::size(colors));
  }

  void Set(const char* name, const std::string& value) {
    if (Get(name) == value) return;
    rex::cvar::SetFlagByName(name, value);
    changed_ = true;
  }

  // A combo over fixed values, labelled per value.
  void Choice(const char* label, const char* name, const std::vector<std::pair<std::string, std::string>>& options,
              const char* tag = nullptr) {
    const std::string current = Get(name);
    std::string shown = current;
    for (const auto& [value, text] : options) {
      if (value == current) shown = text;
    }
    Row(label, tag);
    if (ImGui::BeginCombo((std::string("##") + name).c_str(), shown.c_str())) {
      for (const auto& [value, text] : options) {
        if (ImGui::Selectable(text.c_str(), value == current)) Set(name, value);
      }
      ImGui::EndCombo();
    }
  }

  void Check(const char* label, const char* name, const char* tag = nullptr) {
    bool on = GetBool(name);
    Row(label, tag);
    if (ImGui::Checkbox((std::string("##") + name).c_str(), &on)) Set(name, on ? "true" : "false");
  }

  void SliderFloat(const char* label, const char* name, float lo, float hi, const char* format) {
    float v = std::stof(Get(name).empty() ? "0" : Get(name));
    Row(label, nullptr);
    if (ImGui::SliderFloat((std::string("##") + name).c_str(), &v, lo, hi, format)) {
      Set(name, std::to_string(v));
    }
  }

  // Label in the first column, control in the second.
  void Row(const char* label, const char* tag) {
    ImGui::AlignTextToFramePadding();
    if (tag) {
      ImGui::Text("%s  %s", label, tag);
    } else {
      ImGui::TextUnformatted(label);
    }
    ImGui::SameLine(ImGui::GetWindowWidth() * 0.42f);
    ImGui::SetNextItemWidth(ImGui::GetWindowWidth() * 0.5f);
  }

  void DrawDisplay() {
    Choice("Frame rate", "tenchuz_frame_rate",
           {{"30", "30"}, {"60", "60"}, {"120", "120"}, {"144", "144"}, {"165", "165"}, {"180", "180"},
            {"240", "240"}});
    Check("Fullscreen", "fullscreen");
    Choice("Aspect ratio", "tenchuz_aspect_ratio",
           {{"16:9", "16:9"}, {"16:10", "16:10"}, {"21:9", "21:9"}, {"32:9", "32:9"}}, "(restart)");
    Check("HUD at 16:9", "tenchuz_widescreen_hud");
    Choice("Resolution", "tenchuz_native_resolution_scale",
           {{"1", "720p (original)"}, {"2", "1440p (2x)"}, {"3", "2160p (3x)"}, {"4", "2880p (4x)"}},
           "(restart)");
    Choice("Anti-aliasing", "tenchuz_native_post_effect",
           {{"none", "Off"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA (strong)"}});
    Choice("MSAA", "tenchuz_native_msaa_samples", {{"0", "2x (original)"}, {"4", "4x"}}, "(restart)");
  }

  void DrawControls() {
    const bool controller = input::ActiveDevice() == input::Device::kController;
    Choice("Input", "tenchuz_input_device",
           {{"auto", controller ? "Auto (controller)" : "Auto (keyboard and mouse)"},
            {"keyboard", "Keyboard and mouse"},
            {"controller", "Controller"}});
    // Keyboard and mouse settings, greyed out while the controller is in use.
    ImGui::BeginDisabled(controller);
    Check("Mouse camera", "tenchuz_mouse_camera");
    SliderFloat("Mouse sensitivity", "tenchuz_mouse_sensitivity", 0.1f, 5.0f, "%.2f");
    Check("Invert mouse Y", "tenchuz_mouse_invert_y");
    SliderFloat("Walk speed", "tenchuz_walk_speed", 0.1f, 1.0f, "%.2f");
    ImGui::Separator();

    // A key picked for a control.
    if (capture_control_ >= 0) {
      const std::string key = input::TakeCaptured();
      if (!key.empty()) {
        const input::Control& c = input::Controls()[size_t(capture_control_)];
        std::vector<std::string> keys = SplitKeys(input::Binding(c.id));
        keys.resize(2);
        keys[size_t(capture_slot_)] = key;
        input::SetBinding(c.id, JoinKeys(keys));
        changed_ = true;
        capture_control_ = -1;
      }
    }

    const float row_h = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("bindings", ImVec2(0, ImGui::GetContentRegionAvail().y - row_h * 2.2f), false);
    ImGui::SetWindowFontScale(scale_);  // child windows don't inherit it
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.2f);
      ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableHeadersRow();
      const auto& controls = input::Controls();
      for (size_t i = 0; i < controls.size(); ++i) {
        const input::Control& c = controls[i];
        std::vector<std::string> keys = SplitKeys(input::Binding(c.id));
        keys.resize(2);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(c.label);
        for (int slot = 0; slot < 2; ++slot) {
          ImGui::TableSetColumnIndex(1 + slot);
          const bool waiting = capture_control_ == int(i) && capture_slot_ == slot;
          const std::string text = waiting ? "..." : (keys[size_t(slot)].empty() ? "-" : keys[size_t(slot)]);
          ImGui::PushID(int(i * 2 + size_t(slot)));
          if (ImGui::Button(text.c_str(), ImVec2(-1, 0)) && !waiting) {
            capture_control_ = int(i);
            capture_slot_ = slot;
            input::BeginCapture();
          }
          if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {  // right-click clears
            keys[size_t(slot)].clear();
            input::SetBinding(c.id, JoinKeys(keys));
            changed_ = true;
          }
          ImGui::PopID();
        }
      }
      ImGui::EndTable();
    }
    ImGui::EndChild();
    if (ImGui::Button("Reset controls")) {
      for (const input::Control& c : input::Controls()) input::SetBinding(c.id, input::DefaultBinding(c.id));
      Set("tenchuz_mouse_sensitivity", "1.000000");
      Set("tenchuz_mouse_invert_y", "false");
      changed_ = true;
    }
    ImGui::EndDisabled();
  }

  void DrawAudio() {
    int volume = std::atoi(Get("tenchuz_volume").c_str());
    Row("Volume", nullptr);
    if (ImGui::SliderInt("##volume", &volume, 0, 100, "%d%%")) {
      Set("tenchuz_volume", std::to_string(volume));
      audio::ApplyVolume();
    }
    Check("Mute", "audio_mute");
  }

  void DrawOnline() {
    Choice("System Link", "tenchuz_netplay",
           {{"auto", "Steam if running"}, {"steam", "Steam"}, {"local", "This PC"}, {"off", "Off"}}, "(restart)");
  }

  std::filesystem::path config_path_;
  bool changed_ = false;
  bool closing_ = false;
  int capture_control_ = -1;
  int capture_slot_ = 0;
  int open_tab_ = -1;  // a tab to show first (tests)
  float scale_ = 1.0f;
};

std::unique_ptr<GameMenu> g_menu;

void Close() {
  if (!g_menu) return;
  g_menu.reset();
  input::SetMenuOpen(false);
}

void Toggle() {
  if (g_menu) {
    if (!g_menu->CancelCapture()) Close();
    return;
  }
  g_menu = std::make_unique<GameMenu>(g_drawer, g_config_path);
  input::SetMenuOpen(true);
}

}  // namespace

void Install(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& config_path) {
  g_drawer = drawer;
  g_window = window;
  g_config_path = config_path;
  if (!drawer) return;
  rex::ui::RegisterBind("bind_game_menu", "Escape", "Game menu", [] { Toggle(); });
}

void Uninstall() {
  rex::ui::UnregisterBind("bind_game_menu");
  g_menu.reset();
}

void ApplyStartupSettings() { audio::ApplyVolume(); }

void RequestToggle(int tab) {
  if (!g_window) return;
  g_window->app_context().CallInUIThreadDeferred([tab] {
    Toggle();
    if (g_menu && tab >= 0) g_menu->OpenTab(tab);
  });
}

}  // namespace tenchuz::game_menu
