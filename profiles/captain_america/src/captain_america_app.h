// Captain America: Super Soldier — RexGlue application integration.

#pragma once

#include <filesystem>

#include <rex/cvar.h>
#include <rex/rex_app.h>

class CaptainAmericaApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<CaptainAmericaApp>(
        new CaptainAmericaApp(ctx, "captain_america", PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (paths.game_data_root.empty()) {
      const auto portable = exe_dir / "game";
      if (std::filesystem::exists(portable / "default.xex")) {
        paths.game_data_root = portable;
      }
    }
    paths.user_data_root = exe_dir / "user_data";
    paths.cache_root = exe_dir / "user_data" / "cache";
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true");
    rex::cvar::SetFlagByName("async_shader_compilation", "true");
    rex::cvar::SetFlagByName("store_shaders", "true");
  }

};
