// tenchuz - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>

#include "crash_report.h"
#include "frame_stats.h"
#include "renderer/d3d_trace.h"
#include "renderer/gpu_observer.h"
#include "renderer/native_renderer.h"

class TenchuzApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TenchuzApp>(new TenchuzApp(ctx, "tenchuz",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Xenos GPU emulation (staged next to the exe by GPU_PLUGINS in CMakeLists).
    // --gpu_plugin on the command line still takes precedence.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Keep saves and the shader cache beside the game data (<root>/user)
    // instead of Documents\tenchuz, unless --user_data_root was given.
    const auto default_user = rex::filesystem::GetUserFolder() / GetName();
    if (paths.user_data_root == default_user) {
      const auto base = paths.game_data_root.empty()
                            ? rex::filesystem::GetExecutableFolder()
                            : paths.game_data_root.parent_path();
      const bool default_cache = paths.cache_root == default_user / "cache";
      paths.user_data_root = base / "user";
      if (default_cache) {
        paths.cache_root = paths.user_data_root / "cache";
      }
    }
    tenchuz::d3d_trace::SetOutputDir(paths.user_data_root / "traces");
    tenchuz::gpu_observer::SetOutputDir(paths.user_data_root / "traces");
  }

  void OnPostSetup() override {
    // Installed last so no runtime component replaces the top-level filter.
    tenchuz::InstallCrashReporter();
    SetGuestFrameStats(tenchuz::GetFrameStats);
    rex::ui::RegisterBind("bind_d3d_trace", "F8", "Capture a D3D trace of the next frame",
                          [] { tenchuz::d3d_trace::RequestCapture(); });
    rex::ui::RegisterBind("bind_gpu_observe", "F9", "Record GPU state at every draw of a frame",
                          [] { tenchuz::gpu_observer::RequestObserve(); });
    tenchuz::native::Initialize(runtime());
    rex::ui::RegisterBind("bind_native_output", "F10", "Toggle native renderer output",
                          [] { tenchuz::native::TogglePresentNative(); });
    rex::ui::RegisterBind("bind_native_dump", "F11", "Dump the native renderer's resolved images",
                          [] { tenchuz::native::RequestFrameDump(); });

    // Missions bind a texture fetch constant that Xenos treats as invalid.
    // Without this the screen stays black when a mission starts (Xenia needed
    // the same flag). It's defined by the GPU plugin, so it only exists here,
    // after the plugin has loaded.
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
    REXLOG_INFO("tenchuz: gpu_allow_invalid_fetch_constants = {}",
                rex::cvar::GetFlagByName("gpu_allow_invalid_fetch_constants"));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
