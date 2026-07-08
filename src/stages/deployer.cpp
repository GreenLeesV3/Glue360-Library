// deployer.cpp — Stage 7 implementation.
//
// Stages a portable standalone folder (docs/04_build_automation.md §9,
// docs/01_architecture.md §6.4):
//
//   <deploy_dir>/
//     spiderman3.exe             <- from game build dir
//     rexruntime.dll             <- custom (from runtime build) over prebuilt
//     TracyClient.dll            <- from prebuilt SDK bin/ (0.13.1, not rebuilt)
//     spiderman3.toml            <- rendered from profile deploy template
//     game/                      <- junction -> extracted ISO files
//     user_data/                 <- seeded (empty cache builds on first launch)
//       cache/
//
// The TOML is rendered from the profile's deploy template
// (ctx.profile.toml_template) with {{GAME_DATA_ROOT}} filled at deploy (the
// absolute extracted path, or "game" if a junction is created).
//
// Atomic deploy: stage into <deploy_dir>.new/, then swap. The previous
// <deploy_dir> is kept as <deploy_dir>.bak/ for rollback.

#include "stages/deployer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>

#include "core/pipeline_context.h"
#include "core/types.h"
#include "profile/game_profile.h"
#include "profile/template_renderer.h"

namespace fs = std::filesystem;

namespace recomp {

namespace {

bool copy_file_with_dirs(const fs::path& src, const fs::path& dst) {
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

// Create an NTFS directory junction (mklink /J). Returns true on success.
// Junctions don't require admin privileges and survive reboots.
bool create_junction(const fs::path& link, const fs::path& target) {
  std::error_code ec;
  // std::filesystem::create_directory_symlink creates a reparse point; on
  // Windows this works for junctions in practice. Fall back to mklink /J if
  // it fails (e.g. insufficient privilege for a true symlink).
  fs::create_directory_symlink(target, link, ec);
  if (!ec) return true;
  std::string cmd = "cmd.exe /C mklink /J \"" + link.string() + "\" \"" +
                    target.string() + "\"";
  return std::system(cmd.c_str()) == 0;
}

}  // namespace

CheckResult DeployerStage::check_prereqs(const PipelineContext& ctx) const {
  CheckResult result;

  if (ctx.built_exe.empty() || !fs::exists(ctx.built_exe)) {
    result.ok = false;
    result.missing.push_back("built game exe (run build_game first)");
    result.message = "Game executable missing — build_game must run first.";
    return result;
  }

  // Runtime DLL: custom if built, else prebuilt.
  std::error_code ec;
  fs::path runtime_dll = ctx.custom_runtime_dll;
  if (runtime_dll.empty() || !fs::exists(runtime_dll, ec)) {
    runtime_dll = ctx.sdk_path / "bin" / "rexruntime.dll";
  }
  if (!fs::exists(runtime_dll, ec)) {
    result.ok = false;
    result.missing.push_back("rexruntime.dll (custom or prebuilt)");
    result.message = "rexruntime.dll not found (neither custom nor prebuilt).";
    return result;
  }

  if (ctx.profile.toml_template.empty() ||
      !fs::exists(ctx.profile.toml_template, ec)) {
    result.ok = false;
    result.missing.push_back("deploy TOML template: " +
                              ctx.profile.toml_template.string());
    result.message = "Profile deploy TOML template not found.";
    return result;
  }

  if (ctx.extracted_dir.empty() || !fs::exists(ctx.extracted_dir, ec)) {
    result.ok = false;
    result.missing.push_back("extracted game data (run iso_extract first)");
    result.message = "Extracted game data missing — cannot create game junction.";
    return result;
  }

  if (ctx.output_dir.empty()) {
    result.ok = false;
    result.missing.push_back("workspace output directory");
    result.message = "Workspace root not configured.";
    return result;
  }

  result.ok = true;
  return result;
}

StageResult DeployerStage::run(PipelineContext& ctx, ProgressCallback progress) {
  // Stage into <deploy_dir>.new/, then swap atomically.
  fs::path deploy_new = ctx.output_dir / "standalone.new";
  fs::path deploy_final = ctx.output_dir / "standalone";
  ctx.deploy_dir = deploy_final;

  std::error_code ec;
  if (fs::exists(deploy_new, ec)) fs::remove_all(deploy_new, ec);
  fs::create_directories(deploy_new, ec);

  // 1. Copy the game exe.
  progress(0.1f, "Copying " + ctx.built_exe.filename().string() + "...");
  fs::path exe_dst = deploy_new / ctx.built_exe.filename();
  if (!copy_file_with_dirs(ctx.built_exe, exe_dst)) {
    return StageResult::fail("Failed to copy game exe to " + exe_dst.string());
  }

  // 2. Copy rexruntime.dll (custom over prebuilt).
  progress(0.2f, "Copying rexruntime.dll...");
  fs::path runtime_dll = ctx.custom_runtime_dll;
  if (runtime_dll.empty() || !fs::exists(runtime_dll, ec)) {
    runtime_dll = ctx.sdk_path / "bin" / "rexruntime.dll";
  }
  if (!copy_file_with_dirs(runtime_dll, deploy_new / "rexruntime.dll")) {
    return StageResult::fail("Failed to copy rexruntime.dll.");
  }

  // 3. Copy the DLLs declared in the profile (e.g. TracyClient.dll from SDK bin/).
  for (const auto& dll : ctx.profile.copy_dlls) {
    if (dll == "rexruntime.dll") continue;  // already copied
    progress(0.3f, "Copying " + dll + "...");
    fs::path src = ctx.sdk_path / "bin" / dll;
    if (!fs::exists(src, ec)) {
      progress(0.3f, "Skipping missing DLL: " + dll);
      continue;
    }
    if (!copy_file_with_dirs(src, deploy_new / dll)) {
      return StageResult::fail("Failed to copy " + dll);
    }
  }

  // 4. Render the deploy TOML via DepsAndProfile's template renderer
  // (single {{GAME_DATA_ROOT}} placeholder).
  progress(0.5f, "Rendering " + ctx.profile_name + ".toml...");
  std::string game_data_root = ctx.profile.create_game_junction
                                   ? std::string("game")
                                   : ctx.extracted_dir.string();
  fs::path toml_dst = deploy_new / (ctx.profile_name + ".toml");
  std::unordered_map<std::string, std::string> toml_vars = {
      {"GAME_DATA_ROOT", game_data_root},
      {"PROJECT_NAME", ctx.profile_name},
      {"PROFILE_ID", ctx.profile.id},
  };
  if (!recomp::profile::render_template(ctx.profile.toml_template, toml_dst,
                                        toml_vars)) {
    return StageResult::fail("Failed to render " + toml_dst.string());
  }

  // 5. Create the game/ junction -> extracted ISO files.
  if (ctx.profile.create_game_junction) {
    progress(0.7f, "Creating game/ junction -> " + ctx.extracted_dir.string());
    fs::path game_link = deploy_new / "game";
    if (!create_junction(game_link, ctx.extracted_dir)) {
      // Non-fatal: re-render the TOML with the absolute path since the
      // junction is absent, so the exe can still find game data.
      progress(0.7f,
               "WARNING: junction creation failed; game will use the absolute "
               "path in the TOML.");
      toml_vars["GAME_DATA_ROOT"] = ctx.extracted_dir.string();
      recomp::profile::render_template(ctx.profile.toml_template, toml_dst,
                                       toml_vars);
    } else {
      // Verify the junction resolves default.xex.
      if (!fs::exists(game_link / "default.xex", ec)) {
        progress(0.7f,
                 "WARNING: game/default.xex not reachable via junction.");
      }
    }
  }

  // 6. Seed user_data/ (empty cache builds on first launch).
  if (ctx.profile.create_user_data) {
    progress(0.85f, "Creating user_data/...");
    fs::create_directories(deploy_new / "user_data" / "cache", ec);
    // Do NOT pre-populate cache/shaders/ — it builds on first launch.
    // Do NOT create a Content/ subtree (triggers a UTF-8 crash without patches).
  }

  // 7. Atomic swap: rename existing standalone/ -> standalone.bak/, then
  // standalone.new/ -> standalone/.
  progress(0.95f, "Swapping deploy directory...");
  fs::path deploy_bak = ctx.output_dir / "standalone.bak";
  if (fs::exists(deploy_bak, ec)) fs::remove_all(deploy_bak, ec);
  if (fs::exists(deploy_final, ec)) {
    fs::rename(deploy_final, deploy_bak, ec);
    if (ec) {
      return StageResult::fail(
          "Failed to rename existing standalone/ to standalone.bak/: " +
          ec.message());
    }
  }
  fs::rename(deploy_new, deploy_final, ec);
  if (ec) {
    // Roll back: restore the backup if the final rename failed.
    if (fs::exists(deploy_bak, ec)) fs::rename(deploy_bak, deploy_final, ec);
    return StageResult::fail(
        "Failed to finalize deploy directory: " + ec.message());
  }

  progress(1.0f, "Deployed to " + deploy_final.string());
  return StageResult::ok("Deployed standalone game to " + deploy_final.string());
}

bool DeployerStage::is_complete(const PipelineContext& ctx) const {
  if (ctx.deploy_dir.empty()) return false;
  std::error_code ec;
  if (!fs::exists(ctx.deploy_dir, ec)) return false;
  if (ctx.built_exe.empty()) return false;
  return fs::exists(ctx.deploy_dir / ctx.built_exe.filename(), ec);
}

}  // namespace recomp
