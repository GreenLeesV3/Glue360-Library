// jurassic_hunted - ReXGlue Recompiled Project
// Performance-optimized configuration for Jurassic: The Hunted (CloakNT engine)

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <filesystem>

class JurassicHuntedApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<JurassicHuntedApp>(new JurassicHuntedApp(ctx, "jurassic_hunted",
        PPCImageConfig));
  }

  // Resolve game data path — the TOML cvar is loaded AFTER path resolution
  // in SetupEnvironment, so we must set it here explicitly.
  // Uses exe_dir/game (works with create_game_junction deploy option).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (paths.game_data_root.empty()) {
      auto game_subdir = exe_dir / "game";
      if (std::filesystem::exists(game_subdir / "default.xex")) {
        paths.game_data_root = game_subdir;
      }
    }
    paths.user_data_root = exe_dir / "user_data";
    paths.cache_root = exe_dir / "user_data" / "cache";
  }

  // Tuned cvars run before Runtime::Setup() so the backend picks them up.
  // NOTE: Do NOT set gamma_render_target_as_unorm16, snorm16_render_target_full_range,
  // mrt_edram_used_range_clamp_to_min, readback_resolve, anisotropic_override, or
  // swap_post_effect — the SDK defaults produce correct rendering for the CloakNT
  // engine. Overriding them caused dark/shadowy gameplay.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Render path — RTV (host render targets) is the SDK default on AMD and
    // supports the gamma pipeline. ROV forces gamma off, causing dark rendering.
    rex::cvar::SetFlagByName("render_target_path_d3d12", "rtv");
    // Texture cache — defaults too low for 3.16GB game data
    rex::cvar::SetFlagByName("texture_cache_memory_limit_hard", "4096");
    rex::cvar::SetFlagByName("texture_cache_memory_limit_soft", "2048");
    rex::cvar::SetFlagByName("texture_cache_memory_limit_soft_lifetime", "120");
    rex::cvar::SetFlagByName("texture_cache_memory_limit_render_to_texture", "256");
    // Performance — reduce command list submission frequency
    rex::cvar::SetFlagByName("d3d12_submit_on_primary_buffer_end", "false");
    // Shader cache persistence
    rex::cvar::SetFlagByName("store_shaders", "true");
  }
};
