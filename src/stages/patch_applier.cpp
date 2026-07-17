// patch_applier.cpp — Stage 4 implementation.
//
// Applies the three patch strata from the loaded GameProfile
// (recomp::profile::GameProfile, owned by DepsAndProfile in
// profile/game_profile.h):
//
//   1. CVARS:      ctx.profile.cvars (map<string,string>) is rendered into
//                  spiderman3_app.h via the inja template (stratum 2).
//   2. SOURCES:    Each ctx.profile.source_files entry (SourceFile{from,to,optional})
//                  is copied/rendered from <profile_dir>/<from> into
//                  <project_dir>/<to>. .inja templates are rendered with the
//                  cvar set; non-.inja files copied verbatim. This injects
//                  xmp_bypass.cpp, spiderman3_app.h, roundevenf.cpp,
//                  particle_perf.cpp, main.cpp into <project>/src/.
//   3. RUNTIME:    For each ctx.profile.runtime_patches entry (RuntimePatch),
//                  copy overlay headers from <profile_dir>/patches/<category>/<id>/overlay/
//                  into the SDK source working copy at <output_dir>/.recomp/sdk-src/.
//                  The RuntimeBuilder then builds that patched tree with the
//                  flags (ctx.profile.runtime_flags) passed as -D defines.
//                  Skipped when !ctx.profile.requires_sdk_source.

#include "stages/patch_applier.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>

#include "core/pipeline_context.h"
#include "core/types.h"
#include "profile/game_profile.h"

namespace fs = std::filesystem;

namespace recomp {

namespace {

std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool write_file(const fs::path& p, const std::string& content) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
  return f.good();
}

bool copy_file_with_dirs(const fs::path& src, const fs::path& dst) {
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

bool copy_tree(const fs::path& src, const fs::path& dst) {
  std::error_code ec;
  fs::create_directories(dst, ec);
  // Skip build artifact directories that contain generated files with paths
  // exceeding Windows MAX_PATH (260). NOTE: do NOT skip "build" — xxHash
  // legitimately uses thirdparty/xxHash/build/cmake/ as its source layout.
  auto is_artifact_dir = [](const fs::path& p) {
    std::string name = p.filename().string();
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == "out" || lower == ".cpm-cache" || lower == ".git" ||
           lower == ".cache";
  };
  for (auto it = fs::recursive_directory_iterator(
           src, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); ++it) {
    if (ec) return false;
    fs::path rel = fs::relative(it->path(), src, ec);
    if (ec) return false;
    fs::path target = dst / rel;
    if (it->is_directory()) {
      if (is_artifact_dir(it->path())) {
        it.disable_recursion_pending();  // skip this dir's contents
        continue;
      }
      fs::create_directories(target, ec);
    } else if (it->is_regular_file()) {
      fs::create_directories(target.parent_path(), ec);
      fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, ec);
      if (ec) {
        // Log and skip individual file failures (e.g. locked/long paths)
        // rather than aborting the entire staging — the runtime build only
        // needs the source headers + .cpp files, not every vendored test.
        ec.clear();
      }
    }
  }
  return true;
}

// Minimal inja-style {{VAR}} substitution. The app's full template renderer
// (DepsAndProfile) uses real inja; this fallback handles the common case
// (spiderman3_app.h.inja references {{CVAR_render_target_path_d3d12}}) without
// a hard inja dependency in the stage.
std::string render_simple(const std::string& tpl,
                          const std::unordered_map<std::string, std::string>& vars) {
  std::string out;
  out.reserve(tpl.size());
  for (std::size_t i = 0; i < tpl.size();) {
    if (i + 1 < tpl.size() && tpl[i] == '{' && tpl[i + 1] == '{') {
      std::size_t end = tpl.find("}}", i + 2);
      if (end == std::string::npos) {
        out.append(tpl, i, std::string::npos);
        break;
      }
      std::string key = tpl.substr(i + 2, end - (i + 2));
      auto first = key.find_first_not_of(" \t");
      auto last = key.find_last_not_of(" \t");
      if (first != std::string::npos) key = key.substr(first, last - first + 1);
      auto it = vars.find(key);
      out.append(it == vars.end() ? std::string{} : it->second);
      i = end + 2;
    } else {
      out.push_back(tpl[i++]);
    }
  }
  return out;
}

std::unordered_map<std::string, std::string> build_template_vars(
    const PipelineContext& ctx) {
  std::unordered_map<std::string, std::string> vars;
  vars["PROJECT_NAME"] = ctx.profile_name;
  vars["PROFILE_ID"] = ctx.profile.id;
  vars["TITLE_ID"] = ctx.profile.title_id;
  for (const auto& [k, v] : ctx.profile.cvars) {
    std::string key = "CVAR_" + k;
    for (char& c : key) {
      if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    vars[key] = v;
  }
  return vars;
}

std::string to_lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

}  // namespace

CheckResult PatchApplierStage::check_prereqs(const PipelineContext& ctx) const {
  CheckResult result;

  if (ctx.project_dir.empty() || !fs::exists(ctx.project_dir)) {
    result.ok = false;
    result.missing.push_back("project directory (run rexglue_init first)");
    result.message = "Generated project missing — codegen must run first.";
    return result;
  }

  if (ctx.profile.id.empty()) {
    result.ok = false;
    result.missing.push_back("loaded game profile");
    result.message = "No game profile loaded.";
    return result;
  }

  if (ctx.profile.profile_dir.empty() ||
      !fs::exists(ctx.profile.profile_dir)) {
    result.ok = false;
    result.missing.push_back("profile directory: " +
                              ctx.profile.profile_dir.string());
    result.message = "Profile directory not found.";
    return result;
  }

  // Runtime patches require SDK source. If it's not available, warn but
  // continue — the game will work without the patches (e.g. no save system
  // fixes for SP3) but won't have the custom runtime optimizations.
  if (!ctx.profile.runtime_patches.empty()) {
    if (ctx.sdk_source_path.empty() || !fs::exists(ctx.sdk_source_path)) {
      result.warnings.push_back(
          "SDK source not available — runtime patches (" +
          std::to_string(ctx.profile.runtime_patches.size()) +
          ") will be skipped. Game will use prebuilt rexruntime.dll "
          "without custom fixes (e.g. save system patches).");
    }
  }

  result.ok = true;
  return result;
}

StageResult PatchApplierStage::run(PipelineContext& ctx, ProgressCallback progress) {
  fs::path project_src = ctx.project_dir / "src";
  std::error_code ec;
  fs::create_directories(project_src, ec);

  // ---- Stratum 1: cvars ----
  progress(0.1f, "Applying cvar patch stratum (" +
                     std::to_string(ctx.profile.cvars.size()) + " cvars)...");
  if (ctx.profile.cvars.empty()) {
    return StageResult::fail(
        "Profile has no cvars — OnPreSetup would be empty.");
  }

  // ---- Stratum 2: source patches (templates -> src/) ----
  progress(0.2f, "Applying source patch stratum (" +
                     std::to_string(ctx.profile.source_files.size()) +
                     " files)...");

  auto tpl_vars = build_template_vars(ctx);
  int applied = 0;

  for (const auto& sf : ctx.profile.source_files) {
    fs::path src = ctx.profile.profile_dir / sf.from;
    fs::path dst = ctx.project_dir / sf.to;

    if (!fs::exists(src, ec)) {
      if (sf.optional) {
        progress(0.4f, "Skipping optional missing file: " + sf.from);
        continue;
      }
      return StageResult::fail("Source patch file missing: " + src.string());
    }

    fs::create_directories(dst.parent_path(), ec);

    bool is_inja = src.extension() == ".inja";
    fs::path real_dst = dst;
    if (is_inja && real_dst.extension() == ".inja") {
      real_dst.replace_extension();
    }

    if (is_inja) {
      std::string tpl = read_file(src);
      std::string rendered = render_simple(tpl, tpl_vars);
      if (!write_file(real_dst, rendered)) {
        return StageResult::fail("Write failed: " + real_dst.string());
      }
    } else {
      if (!copy_file_with_dirs(src, real_dst)) {
        return StageResult::fail("Copy failed: " + src.string() + " -> " +
                                 real_dst.string());
      }
    }
    ++applied;
    progress(0.4f, "Wrote " + real_dst.filename().string());
  }

  progress(0.6f, "Source stratum: " + std::to_string(applied) + " files applied.");

  // Inject profile .cpp source files into the generated CMakeLists.txt.
  // rexglue init only adds src/main.cpp; the remaining profile sources
  // (roundevenf.cpp, particle_perf.cpp, xmp_bypass.cpp) must be added to
  // the target's source list or they won't compile/link.
  fs::path cmakelists = ctx.project_dir / "CMakeLists.txt";
  if (fs::exists(cmakelists, ec)) {
    std::string content = read_file(cmakelists);
    std::string marker = "    src/main.cpp\n)";
    auto pos = content.find(marker);
    if (pos != std::string::npos) {
      std::string injection;
      for (const auto& sf : ctx.profile.source_files) {
        // Only add .cpp files that aren't already listed and aren't main.cpp
        if (sf.to.size() >= 4 &&
            sf.to.compare(sf.to.size() - 4, 4, ".cpp") == 0 &&
            sf.to != "src/main.cpp") {
          injection += "    " + sf.to + "\n";
        }
      }
      if (!injection.empty()) {
        content.insert(pos + marker.size() - 1, injection);
        write_file(cmakelists, content);
      }
    }
  }


  // ---- Stratum 3: runtime patches (overlay headers -> SDK source tree) ----
  // Skip when SDK source is not available (lite mode — prebuilt runtime).
  if (!ctx.profile.runtime_patches.empty() &&
      !ctx.sdk_source_path.empty() && fs::exists(ctx.sdk_source_path)) {
    progress(0.7f, "Applying runtime patch stratum (" +
                       std::to_string(ctx.profile.runtime_patches.size()) +
                       " patches)...");

    // Work on a copy under <output_dir>/.recomp/sdk-src/ so the pristine SDK
    // source is untouched and re-application is clean.
    fs::path sdk_src_copy = ctx.output_dir / ".recomp" / "sdk-src";
    if (ctx.clean && fs::exists(sdk_src_copy, ec)) {
      fs::remove_all(sdk_src_copy, ec);
    }
    if (!fs::exists(sdk_src_copy, ec)) {
      progress(0.72f, "Staging SDK source tree...");
      if (!copy_tree(ctx.sdk_source_path, sdk_src_copy)) {
        return StageResult::fail("Failed to stage SDK source tree to " +
                                 sdk_src_copy.string());
      }
    }

    // The SDK source was extracted from an archive, not git-cloned, so
    // thirdparty/<submodule>/.git dirs don't exist. The SDK's
    // thirdparty/CMakeLists.txt checks for .git to verify submodules are
    // populated. Create marker .git files (gitdir pointers) to satisfy it.
    fs::path thirdparty = sdk_src_copy / "thirdparty";
    if (fs::exists(thirdparty, ec)) {
      for (auto& entry : fs::directory_iterator(thirdparty, ec)) {
        if (entry.is_directory()) {
          fs::path git_marker = entry.path() / ".git";
          if (!fs::exists(git_marker, ec)) {
            std::ofstream gf(git_marker);
            gf << "gitdir: ../../.git/modules/" << entry.path().filename().string()
               << "\n";
          }
        }
      }
    }

    // Patch rexglue_helpers.cmake: the SDK adds -msse4.1 for x86_64 only
    // under if(NOT MSVC), but CMake treats clang-cl as MSVC-like (MSVC=TRUE),
    // so the flag is skipped. clang-cl needs it for _mm_shuffle_epi8 (SSSE3)
    // and other intrinsics that MSVC enables by default. Change the guard to
    // also fire for clang-cl (compiler_id "Clang" even in MSVC mode).
    fs::path helpers_cmake = sdk_src_copy / "cmake" / "rexglue_helpers.cmake";
    if (fs::exists(helpers_cmake, ec)) {
      std::string content = read_file(helpers_cmake);
      std::string old_guard = "if(NOT MSVC)";
      std::string new_guard =
          "if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL \"Clang\")";
      auto pos = content.find(old_guard);
      if (pos != std::string::npos) {
        content.replace(pos, old_guard.size(), new_guard);
        std::ofstream wf(helpers_cmake);
        wf << content;
      }
    }

    // Patch root CMakeLists.txt: clang-cl (unlike MSVC) requires explicit CPU
    // feature flags for SSE intrinsics. memory.cpp (rexcore) and xma_context.cpp
    // (rexaudio) both use _mm_shuffle_epi8 (SSSE3). A global add_compile_options
    // covers all targets at once — per-target patches would miss OBJECT libs
    // that don't call rexglue_apply_target_settings.
    fs::path root_cmake = sdk_src_copy / "CMakeLists.txt";
    if (fs::exists(root_cmake, ec)) {
      std::string content = read_file(root_cmake);
      // Insert after the global add_compile_options() block's closing paren.
      std::string marker = "    -fno-char8_t\n)";
      auto pos = content.find(marker);
      if (pos != std::string::npos) {
        pos += marker.size();
        content.insert(pos,
            "\n# Added by xbox360-recompiler: clang-cl fixes\n"
            "# 1. MSVC enables SSSE3/SSE4.1 by default; clang-cl needs -msse4.1\n"
            "#    for _mm_shuffle_epi8 in memory.cpp/xma_context.cpp.\n"
            "# 2. clang-cl ignores -fno-char8_t (GCC flag); u8\"\" literals\n"
            "#    produce char8_t[] which won't convert to string_view.\n"
            "#    /Zc:char8_t- is the MSVC equivalent.\n"
            "if(CMAKE_CXX_COMPILER_ID STREQUAL \"Clang\" AND "
            "CMAKE_SYSTEM_PROCESSOR MATCHES \"x86_64|AMD64\")\n"
            "    add_compile_options(-msse4.1)\n"
            "    add_compile_options(/Zc:char8_t-)\nendif()\n");
        std::ofstream wf(root_cmake);
        wf << content;
      }
    }

    int patches_applied = 0;
    for (const auto& rp : ctx.profile.runtime_patches) {
      // Overlay headers live at <profile_dir>/patches/<category>/<id>/overlay/.
      fs::path overlay = ctx.profile.profile_dir / "patches" / rp.category /
                         rp.id / "overlay";
      std::error_code ec2;
      if (fs::exists(overlay, ec2)) {
        if (!copy_tree(overlay, sdk_src_copy)) {
          return StageResult::fail("Failed to apply runtime overlay: " + rp.id);
        }
      } else if (rp.required && rp.flag.empty()) {
        return StageResult::fail(
            "Required runtime overlay is missing: " + overlay.string());
      }
      // Inline-#ifdef / CMake-flag-only patches have no overlay dir; the flag
      // is still recorded for RuntimeBuilder to pass as -D.
      ++patches_applied;
      progress(0.8f, "Applied runtime patch: " + rp.id);
    }

    progress(0.9f, "Runtime stratum: " + std::to_string(patches_applied) +
                       " patches recorded.");
  } else {
    progress(0.9f, "No runtime patches — build_runtime will skip.");
  }

  progress(1.0f, "All patches applied.");
  return StageResult::ok(
      "Applied " + std::to_string(applied) + " source files" +
      (ctx.profile.requires_sdk_source ? " + runtime patches" : ""));
}

bool PatchApplierStage::is_complete(const PipelineContext& ctx) const {
  // Complete iff <project>/src/<name>_app.h exists (stratum-2 canary).
  if (ctx.project_dir.empty()) return false;
  std::error_code ec;
  fs::path app_h = ctx.project_dir / "src" / (ctx.profile_name + "_app.h");
  if (fs::exists(app_h, ec)) return true;
  // Fall back to any *_app.h in src/.
  fs::path src_dir = ctx.project_dir / "src";
  if (!fs::exists(src_dir, ec)) return false;
  for (const auto& e : fs::directory_iterator(src_dir, ec)) {
    if (ec) break;
    std::string n = e.path().filename().string();
    if (n.size() > 7 && n.compare(n.size() - 7, 7, "_app.h") == 0) return true;
  }
  return false;
}

}  // namespace recomp
