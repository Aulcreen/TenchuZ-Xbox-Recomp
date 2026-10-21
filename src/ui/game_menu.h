// tenchuz - the game menu (Esc): display, controls, audio and online
// settings, saved to tenchuz.toml next to the exe. The game pauses while it is
// open (input/pc_input.h).

#pragma once

#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace tenchuz::game_menu {

// After the overlays exist (TenchuzApp::OnCreateDialogs): binds Esc.
void Install(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, const std::filesystem::path& config_path);
void Uninstall();

// Opens or closes the menu from any thread (test scripts), showing `tab`
// (0 Display, 1 Controls, 2 Audio, 3 Online) when it opens.
void RequestToggle(int tab = -1);

// Applies saved settings that need code at startup (volume).
void ApplyStartupSettings();

}  // namespace tenchuz::game_menu
