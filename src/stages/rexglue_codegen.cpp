// rexglue_codegen.cpp — Stage 3 implementation.
//
// Command (docs/03_codegen_automation.md §3.1):
//   rexglue codegen spiderman3_manifest.toml
//
// Output (docs/01_architecture.md §2.1 row 3):
//   <project>/generated/default/
//     sources.cmake                 <- shard source list (we parse this)
//     <name>_recomp.{0..N}.cpp      <- N shards (92 for Spider-Man 3, but
//                                       NOT hardcoded — we count sources.cmake)
//     <name>_init.{cpp,h}
//     <name>_register.cpp
//
// Shard count is read from sources.cmake, not hardcoded to 92, so the stage
// survives SDK version drift.

#include "stages/rexglue_codegen.h"

#include <cctype>
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

namespace {

// Count shard .cpp references in sources.cmake. Robust to both set(...) and
// list(APPEND ...) forms — counts lines containing <name>_recomp. and .cpp.
int count_shards_from_sources_cmake(const fs::path& sources_cmake,
                                    const std::string& project_name) {
  std::ifstream f(sources_cmake);
  if (!f) return 0;
  std::string needle = project_name + "_recomp.";
  int count = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (line.find(needle) != std::string::npos &&
        line.find(".cpp") != std::string::npos) {
      ++count;
    }
  }
  return count;
}

// Fallback: count shard files on disk.
int count_shard_files_on_disk(const fs::path& generated_dir,
                              const std::string& project_name) {
  int count = 0;
  std::string prefix = project_name + "_recomp.";
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(generated_dir, ec)) {
    if (ec) break;
    std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0 && name.size() > 4 &&
        name.compare(name.size() - 4, 4, ".cpp") == 0) {
      ++count;
    }
  }
  return count;
}

}  // namespace

CheckResult RexglueCodegenStage::check_prereqs(const PipelineContext& ctx) const {
  CheckResult result;

  if (ctx.manifest_path.empty() || !fs::exists(ctx.manifest_path)) {
    result.ok = false;
    result.missing.push_back("manifest TOML: " + ctx.manifest_path.string());
    result.message = "Manifest missing — rexglue init must run first.";
    return result;
  }

  fs::path rexglue = ctx.toolchain.sdk_root / "bin" / "rexglue.exe";
  if (rexglue.empty() || !fs::exists(rexglue)) {
    result.ok = false;
    result.missing.push_back("rexglue.exe: " + rexglue.string());
    result.message = "rexglue.exe not found in the prebuilt SDK.";
    return result;
  }

  result.ok = true;
  return result;
}

StageResult RexglueCodegenStage::run(PipelineContext& ctx, ProgressCallback progress) {
  const std::string& project_name = ctx.profile_name;

  progress(0.05f, "Running rexglue codegen on " + ctx.manifest_path.string() +
                      "...");

  // Run from the project directory so the manifest's relative paths resolve.
  // Entrypoint function hints are injected by rexglue_init from the game
  // profile (profile.toml [entrypoint_functions]); with them present, codegen
  // validation passes without --force.
  std::vector<std::string> args = {"codegen", ctx.manifest_path.string()};

  fs::path rexglue = ctx.toolchain.sdk_root / "bin" / "rexglue.exe";

  // rexglue codegen prints progress like "[42/92] Emitting shard...".
  recomp::ProgressCallback on_line =
      [&progress](float /*frac*/, const std::string& line) {
        progress(0.5f, line);
      };

  auto pr = recomp::ProcessRunner::run(rexglue, args, ctx.project_dir, {}, on_line);

  if (pr.exit_code != 0) {
    return StageResult::fail(
        "rexglue codegen failed (exit " + std::to_string(pr.exit_code) + ").",
        pr.exit_code);
  }

  progress(0.85f, "Verifying generated shards...");

  fs::path generated = ctx.project_dir / "generated" / "default";
  fs::path sources_cmake = generated / "sources.cmake";
  std::error_code ec;
  if (!fs::exists(sources_cmake, ec)) {
    return StageResult::fail(
        "rexglue codegen succeeded but sources.cmake missing: " +
        sources_cmake.string());
  }

  int shards = count_shards_from_sources_cmake(sources_cmake, project_name);
  if (shards == 0) {
    shards = count_shard_files_on_disk(generated, project_name);
  }
  if (shards == 0) {
    return StageResult::fail("rexglue codegen produced 0 shards — check the manifest.");
  }

  ctx.generated_dir = generated;
  ctx.generated_shard_count = shards;

  progress(1.0f, "Codegen complete: " + std::to_string(shards) + " shards.");
  return StageResult::ok("Generated " + std::to_string(shards) + " shards.");
}

bool RexglueCodegenStage::is_complete(const PipelineContext& ctx) const {
  if (ctx.generated_dir.empty()) return false;
  std::error_code ec;
  if (!fs::exists(ctx.generated_dir / "sources.cmake", ec)) return false;
  return ctx.generated_shard_count > 0;
}

}  // namespace recomp
