// spiderman_wos - ReXGlue Recompiled Project
// Spider-Man: Web of Shadows (Treyarch NGL engine)

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <filesystem>

class SpidermanWoSApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<SpidermanWoSApp>(new SpidermanWoSApp(ctx, "spiderman_wos",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (paths.game_data_root.empty()) {
      auto portable = exe_dir / "game";
      if (std::filesystem::exists(portable / "default.xex")) {
        paths.game_data_root = portable;
      } else {
        auto dev = exe_dir / ".." / ".." / ".." / ".." / "game";
        if (std::filesystem::exists(dev / "default.xex")) {
          paths.game_data_root = std::filesystem::canonical(dev);
        }
      }
    }
    paths.user_data_root = exe_dir / "user_data";
    paths.cache_root = exe_dir / "user_data" / "cache";
  }

  // RTV render path with Skate3-style cvars.
  // CRITICAL: execute_unclipped_draw_vs_on_cpu prevents RTV black screen by
  // avoiding spurious EDRAM range ownership transfers on unclipped draws.
  // Without this, RTV produces a black screen (render targets corrupted).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // GPU optimizations
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
    rex::cvar::SetFlagByName("async_shader_compilation", "true");
    // Skate3-style cvars — critical for RTV
    rex::cvar::SetFlagByName("protect_zero", "false");
    rex::cvar::SetFlagByName("clear_memory_page_state", "false");
    rex::cvar::SetFlagByName("execute_unclipped_draw_vs_on_cpu", "true");
    rex::cvar::SetFlagByName("occlusion_query_enable", "true");
    // Present optimizations
    rex::cvar::SetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing", "true");
    rex::cvar::SetFlagByName("host_present_from_non_ui_thread", "true");
    // 60 FPS unlock
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "120.0");
    rex::cvar::SetFlagByName("vsync", "false");
    // Shader cache
    rex::cvar::SetFlagByName("store_shaders", "true");
  }
};
