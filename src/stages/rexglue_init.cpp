// rexglue_init.cpp — Stage 2 implementation.
//
// Command (docs/03_codegen_automation.md §2.1 — verified against
// `rexglue init --help`; the BUILD_GUIDE's --name/--xex are STALE):
//
//   rexglue init \
//     --project-name spiderman3 \
//     --xex-path   "../extracted/default.xex" \
//     --game-root  "../extracted" \
//     --project-root "<output_dir>/spiderman3"
//
// The xex-path and game-root are RELATIVE to the project-root (so the manifest
// stores "../extracted/default.xex", which resolves correctly when the project
// lives at <output_dir>/spiderman3 and the extracted dir is <output_dir>/extracted).
// For Spider-Man 3: NO --scan-dll (single default.xex entrypoint, no DLL modules).

#include "stages/rexglue_init.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/pipeline_context.h"
#include "core/process_runner.h"
#include "core/types.h"

namespace fs = std::filesystem;

namespace recomp {

CheckResult RexglueInitStage::check_prereqs(const PipelineContext& ctx) const {
  CheckResult result;

  if (ctx.extracted_dir.empty() ||
      !fs::exists(ctx.extracted_dir / "default.xex")) {
    result.ok = false;
    result.missing.push_back("extracted/default.xex (run iso_extract first)");
    result.message = "default.xex not found — ISO extraction must run first.";
    return result;
  }

  // rexglue.exe lives in the prebuilt SDK bin/ directory.
  fs::path rexglue = ctx.toolchain.sdk_root / "bin" / "rexglue.exe";
  if (rexglue.empty() || !fs::exists(rexglue)) {
    result.ok = false;
    result.missing.push_back("rexglue.exe: " + rexglue.string());
    result.message = "rexglue.exe not found in the prebuilt SDK.";
    return result;
  }

  if (ctx.output_dir.empty()) {
    result.ok = false;
    result.missing.push_back("Workspace output directory");
    result.message = "Workspace root not configured.";
    return result;
  }

  if (ctx.profile_name.empty()) {
    result.ok = false;
    result.missing.push_back("Project/profile name");
    result.message = "Project name not set.";
    return result;
  }

  result.ok = true;
  return result;
}

StageResult RexglueInitStage::run(PipelineContext& ctx, ProgressCallback progress) {
  const std::string& project_name = ctx.profile_name;
  fs::path project_root = ctx.recomp_dir() / project_name;
  std::error_code ec;
  if (fs::exists(project_root, ec)) {
    progress(0.05f, "Removing previous project directory...");
    fs::remove_all(project_root, ec);
  }

  // rexglue resolves --xex-path/--game-root relative to CWD, not project-root
  // (despite some docs). Passing absolute paths is robust: rexglue auto-
  // relativizes them against the project-root when writing the manifest
  // (e.g. "../extracted/<sub>/default.xex").
  fs::path xex_abs = ctx.extracted_dir / "default.xex";
  fs::path game_root_abs = ctx.extracted_dir;

  if (!fs::exists(xex_abs, ec)) {
    return StageResult::fail("default.xex missing: " + xex_abs.string());
  }

  progress(0.2f, "Running rexglue init --project-name " + project_name + " ...");

  std::vector<std::string> args = {
      "init",
      "--project-name", project_name,
      "--xex-path", xex_abs.string(),
      "--game-root", game_root_abs.string(),
      "--project-root", project_root.string(),
  };
  // NOTE: deliberately NOT passing --scan-dll (Spider-Man 3 has no DLL modules).

  fs::path rexglue = ctx.toolchain.sdk_root / "bin" / "rexglue.exe";

  recomp::ProgressCallback on_line =
      [&progress](float /*frac*/, const std::string& line) {
        progress(0.5f, line);
      };

  auto pr = recomp::ProcessRunner::run(rexglue, args, ctx.output_dir, {}, on_line);

  if (pr.exit_code != 0) {
    return StageResult::fail(
        "rexglue init failed (exit " + std::to_string(pr.exit_code) + ").",
        pr.exit_code);
  }

  progress(0.8f, "Verifying generated manifest + CMakeLists.txt...");

  fs::path manifest = project_root / (project_name + "_manifest.toml");
  fs::path cmakelists = project_root / "CMakeLists.txt";

  if (!fs::exists(manifest, ec)) {
    return StageResult::fail("rexglue init succeeded but manifest missing: " +
                             manifest.string());
  }
  // Inject game-profile extras into the manifest.
  //
  // Three injection mechanisms:
  //   1. setjmp/longjmp — spliced INSIDE [entrypoint] (code path:
  //      ManifestConfig::Load -> LoadBinaryConfig passes [entrypoint] subtable
  //      -> ApplyToml reads toml["setjmp_address"]/["longjmp_address"]).
  //   2. Function boundary overrides — written to a separate functions.toml
  //      file and added to [entrypoint].includes (Skate3Recomp pattern).
  //      The codegen's ApplyTableWithIncludes (config.cpp:428-446) processes
  //      includes depth-first, so the [functions] table in the included file
  //      is merged into the RecompilerConfig. This is cleaner than splicing
  //      171 [entrypoint.functions.<addr>] sections into the manifest text.
  //   3. [[entrypoint.switch_tables]] — appended at end (array-of-tables
  //      headers open their own scope regardless of context).
  // Legacy [entrypoint_functions] (addr -> name string) is still supported
  // for backward compatibility with the Spider-Man 3 profile.
  bool needs_setjmp = ctx.profile.setjmp_address != 0;
  bool needs_longjmp = ctx.profile.longjmp_address != 0;
  bool has_legacy_functions = !ctx.profile.entrypoint_functions.empty();
  bool has_function_bounds = !ctx.profile.functions.empty();
  bool has_switch_tables = !ctx.profile.switch_tables.empty();
  bool has_invalid_instructions = !ctx.profile.invalid_instructions.empty();
  bool has_extras = needs_setjmp || needs_longjmp ||
                    has_legacy_functions || has_function_bounds ||
                    has_switch_tables || has_invalid_instructions;

  if (has_extras) {
    progress(0.85f, "Injecting manifest extras...");

    // Read the whole manifest for splicing.
    std::ifstream in(manifest);
    if (!in) {
      return StageResult::fail("Failed to read manifest for injection: " +
                               manifest.string());
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    in.close();

    // --- 1. Splice setjmp/longjmp into [entrypoint] ---
    std::string entrypoint_extras;
    if (needs_setjmp) {
      entrypoint_extras += "\n# Xenon CRT _setjmp hook-stub fix (from game profile)\n";
      entrypoint_extras += "setjmp_address = 0x" +
          std::format("{:X}", ctx.profile.setjmp_address) + "\n";
    }
    if (needs_longjmp) {
      entrypoint_extras += "longjmp_address = 0x" +
          std::format("{:X}", ctx.profile.longjmp_address) + "\n";
    }
    if (!entrypoint_extras.empty()) {
      auto insert_pos = content.find("\n[entrypoint.");
      if (insert_pos == std::string::npos) {
        content += entrypoint_extras;
      } else {
        content.insert(insert_pos, entrypoint_extras);
      }
    }

    // --- 2. Write function boundary overrides to functions.toml and add
    //        to [entrypoint].includes (Skate3Recomp pattern) ---
    if (has_function_bounds) {
      fs::path func_file = project_root / (project_name + "_functions.toml");
      progress(0.87f, "Writing " + std::to_string(ctx.profile.functions.size()) +
                  " function boundaries to " + func_file.filename().string() + "...");
      std::ofstream ff(func_file);
      if (!ff) {
        return StageResult::fail("Failed to write functions.toml: " +
                                 func_file.string());
      }
      ff << "# Function boundary overrides for " << project_name << "\n";
      ff << "# Generated by xbox360-recompiler from profile.toml [functions]\n";
      ff << "# Explicit end addresses prevent the function scanner from truncating\n";
      ff << "# at conditional branches. Parent relationships handle overlaps.\n\n";
      ff << "[functions]\n\n";
      for (const auto& [addr, fe] : ctx.profile.functions) {
        ff << "\"" << addr << "\" = {";
        bool first = true;
        if (fe.end != 0) {
          ff << "end = 0x" << std::format("{:X}", fe.end);
          first = false;
        }
        if (fe.parent != 0) {
          if (!first) ff << ", ";
          ff << "parent = 0x" << std::format("{:X}", fe.parent);
          first = false;
        }
        if (!fe.name.empty()) {
          if (!first) ff << ", ";
          ff << "name = \"" << fe.name << "\"";
        }
        ff << " }\n";
      }
      ff.close();

      // Add the functions file to [entrypoint].includes in the manifest.
      // The codegen's ApplyTableWithIncludes processes includes before the
      // table's own values, so the function boundaries are merged in.
      std::string include_line = "includes = [\"" +
          func_file.filename().string() + "\"]\n";
      // Check if the manifest already has an includes key in [entrypoint].
      auto ep_start = content.find("[entrypoint]\n");
      if (ep_start != std::string::npos) {
        auto ep_includes = content.find("includes", ep_start);
        auto ep_subtable = content.find("\n[entrypoint.", ep_start);
        if (ep_includes != std::string::npos &&
            (ep_subtable == std::string::npos || ep_includes < ep_subtable)) {
          // Existing includes — append our file to the array.
          // Find the closing ] of the includes array.
          auto close = content.find(']', ep_includes);
          if (close != std::string::npos) {
            // If the array is empty ([]), insert without leading comma.
            if (close > 0 && content[close - 1] == '[') {
              content.insert(close, "\"" + func_file.filename().string() + "\"");
            } else {
              content.insert(close, ", \"" + func_file.filename().string() + "\"");
            }
          }
        } else {
          // No includes — add one before the first subtable or at end of [entrypoint].
          std::string inc_block = "includes = [\"" +
              func_file.filename().string() + "\"]\n";
          if (ep_subtable != std::string::npos) {
            content.insert(ep_subtable, "\n" + inc_block);
          } else {
            content += "\n" + inc_block;
          }
        }
      }
    }

    // --- 3. Append legacy [entrypoint.functions.<addr>] and switch tables ---
    std::string tail;
    for (const auto& [addr, name] : ctx.profile.entrypoint_functions) {
      tail += "\n[entrypoint.functions." + addr + "]\n";
      tail += "name = \"" + name + "\"\n";
    }
    for (const auto& sw : ctx.profile.switch_tables) {
      tail += "\n[[entrypoint.switch_tables]]\n";
      tail += "address = 0x" + std::format("{:X}", sw.address) + "\n";
      tail += "register = " + std::to_string(sw.register_index) + "\n";
      tail += "labels = [";
      for (size_t i = 0; i < sw.labels.size(); ++i) {
        if (i > 0) tail += ", ";
        tail += "0x" + std::format("{:X}", sw.labels[i]);
      }
      tail += "]\n";
    }
    for (const auto& [data, size] : ctx.profile.invalid_instructions) {
      tail += "\n[[entrypoint.invalid_instructions]]\n";
      tail += "data = 0x" + std::format("{:X}", data) + "\n";
      tail += "size = " + std::to_string(size) + "\n";
    }
    content += tail;

    std::ofstream out(manifest, std::ios::trunc);
    if (!out) {
      return StageResult::fail("Failed to write manifest after injection: " +
                               manifest.string());
    }
    out << content;
  }
  if (!fs::exists(cmakelists, ec)) {
    return StageResult::fail(
        "rexglue init succeeded but CMakeLists.txt missing: " +
        cmakelists.string());
  }

  ctx.project_dir = project_root;
  ctx.manifest_path = manifest;

  progress(1.0f, "Project initialized at " + project_root.string());
  return StageResult::ok("rexglue init created " + project_root.string());
}

bool RexglueInitStage::is_complete(const PipelineContext& ctx) const {
  if (ctx.project_dir.empty() || ctx.manifest_path.empty()) return false;
  std::error_code ec;
  return fs::exists(ctx.manifest_path, ec) &&
         fs::exists(ctx.project_dir / "CMakeLists.txt", ec);
}

}  // namespace recomp
