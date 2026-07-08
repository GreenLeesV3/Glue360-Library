// spiderman3 - ReXGlue Recompiled Project
// Performance-optimized configuration with SDK source-informed cvars

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <filesystem>

class Spiderman3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Spiderman3App>(new Spiderman3App(ctx, "spiderman3",
        PPCImageConfig));
  }

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

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // === Render path ===
    // Set both — only the active backend's cvar takes effect
    rex::cvar::SetFlagByName("render_target_path_d3d12", "rov");
    rex::cvar::SetFlagByName("render_target_path_vulkan", "fsi");

    // === 60 FPS unlock ===
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "120.0");

    // === City rendering fixes ===
    rex::cvar::SetFlagByName("gamma_render_target_as_unorm16", "true");
    rex::cvar::SetFlagByName("snorm16_render_target_full_range", "true");
    rex::cvar::SetFlagByName("mrt_edram_used_range_clamp_to_min", "true");
    rex::cvar::SetFlagByName("readback_resolve", "fast");

    // === Visual enhancements ===
    rex::cvar::SetFlagByName("anisotropic_override", "5");
    rex::cvar::SetFlagByName("swap_post_effect", "fxaa");

    // === Performance optimizations ===
    // Shared (both backends)
    rex::cvar::SetFlagByName("host_present_from_non_ui_thread", "true");
    rex::cvar::SetFlagByName("readback_memexport", "true");
    rex::cvar::SetFlagByName("readback_memexport_fast", "true");

    // D3D12-specific (silently ignored on Vulkan)
    rex::cvar::SetFlagByName("d3d12_bindless", "true");
    rex::cvar::SetFlagByName("d3d12_tiled_shared_memory", "true");
    rex::cvar::SetFlagByName("d3d12_submit_on_primary_buffer_end", "false");
    rex::cvar::SetFlagByName("d3d12_pipeline_creation_threads", "2");

    // Vulkan-specific (silently ignored on D3D12)
    rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", "true");
    rex::cvar::SetFlagByName("vulkan_sparse_shared_memory", "false");
    rex::cvar::SetFlagByName("vulkan_submit_on_primary_buffer_end", "true");
    rex::cvar::SetFlagByName("vulkan_pipeline_creation_threads", "2");
    rex::cvar::SetFlagByName("vulkan_dynamic_rendering", "true");

    // Texture cache (shared)
    rex::cvar::SetFlagByName("texture_cache_memory_limit_hard", "4096");
    rex::cvar::SetFlagByName("texture_cache_memory_limit_soft", "2048");
    rex::cvar::SetFlagByName("texture_cache_memory_limit_soft_lifetime", "120");
    rex::cvar::SetFlagByName("texture_cache_memory_limit_render_to_texture", "256");

    // Fuzzy alpha + dither
    rex::cvar::SetFlagByName("use_fuzzy_alpha_epsilon", "true");
    rex::cvar::SetFlagByName("present_dither", "true");

    // Shader cache
    rex::cvar::SetFlagByName("store_shaders", "true");
  }
};
