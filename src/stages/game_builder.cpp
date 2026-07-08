// game_builder.cpp — Stage 6 implementation.
//
// Mirrors the manual build.bat (docs/04_build_automation.md §5):
//
//   cmake -G Ninja -S <project_dir> -B <build_dir> \
//       -DCMAKE_BUILD_TYPE=Release \
//       -DCMAKE_PREFIX_PATH=<sdk_path> \
//       -DCMAKE_C_COMPILER=clang-cl \
//       -DCMAKE_CXX_COMPILER=clang-cl
//   cmake --build <build_dir> --parallel
//
// Both cmake invocations run inside the MSVC environment (ctx.build_env, the
// vcvarsall x64 env vars populated by main.cpp at startup) via ProcessRunner.
//
// Output: <build_dir>/spiderman3.exe (~118MB). The game links the prebuilt
// rexruntime.lib from <sdk_path>/lib/; the custom DLL is swapped in at deploy.

#include "stages/game_builder.h"

#include <filesystem>
#include <string>
#include <vector>
#include <array>
#include <cstdio>

#include "core/pipeline_context.h"
#include "core/process_runner.h"
#include "core/types.h"

namespace fs = std::filesystem;

namespace recomp {

CheckResult GameBuilderStage::check_prereqs(const PipelineContext& ctx) const {
  CheckResult result;

  if (ctx.project_dir.empty() || !fs::exists(ctx.project_dir)) {
    result.ok = false;
    result.missing.push_back("project directory (run rexglue_init/codegen first)");
    result.message = "Project directory missing — earlier stages must run first.";
    return result;
  }

  // MSVC toolchain env must be available (vcvarsall resolved by main.cpp).
  if (ctx.build_env.empty()) {
    result.ok = false;
    result.missing.push_back("MSVC toolchain environment (vcvarsall.bat x64)");
    result.message =
        "MSVC environment unavailable — cannot build the game. "
        "Resolve vcvarsall via the dependency checker.";
    return result;
  }

  if (ctx.toolchain.cmake_exe.empty() || !fs::exists(ctx.toolchain.cmake_exe)) {
    result.ok = false;
    result.missing.push_back("cmake: " + ctx.toolchain.cmake_exe.string());
    result.message = "CMake not found.";
    return result;
  }

  // Prebuilt SDK (CMAKE_PREFIX_PATH) must exist.
  if (ctx.sdk_path.empty() || !fs::exists(ctx.sdk_path)) {
    result.ok = false;
    result.missing.push_back("prebuilt RexGlue SDK: " + ctx.sdk_path.string());
    result.message = "Prebuilt SDK root not found.";
    return result;
  }

  // Patches must have been applied (src/<name>_app.h is the canary).
  fs::path app_h = ctx.project_dir / "src" / (ctx.profile_name + "_app.h");
  std::error_code ec;
  if (!fs::exists(app_h, ec)) {
    result.ok = false;
    result.missing.push_back("src/" + ctx.profile_name + "_app.h (run apply_patches first)");
    result.message = "Game patches not applied — PatchApplier must run first.";
    return result;
  }

  result.ok = true;
  return result;
}

StageResult GameBuilderStage::run(PipelineContext& ctx, ProgressCallback progress) {
  fs::path build_dir = ctx.project_dir / "out" / "build" / "clang-release";
  ctx.game_build_dir = build_dir;

  std::error_code ec;
  if (ctx.clean && fs::exists(build_dir, ec)) {
    fs::remove_all(build_dir, ec);
  }
  fs::create_directories(build_dir, ec);

  // ---- CMake configure ----
  progress(0.1f, "Configuring game build (cmake)...");

  std::vector<std::string> configure_args = {
      "-G", "Ninja",
      "-S", ctx.project_dir.string(),
      "-B", build_dir.string(),
      "-DCMAKE_BUILD_TYPE=Release",
      "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF",  // clang-cl doesn't support C++23 module scanning
      "-DCMAKE_PREFIX_PATH=" + ctx.sdk_path.string(),
      "-DCMAKE_C_COMPILER=" + ctx.toolchain.clang_cl_exe.string(),
      "-DCMAKE_CXX_COMPILER=" + ctx.toolchain.clang_cl_exe.string(),
  };

  // Static MSVC runtime — only safe when the SDK is built from source so all
  // targets share the same /MT runtime. The prebuilt SDK was compiled with
  // /MD (dynamic CRT), so mixing /MT game code with /MD SDK libs causes LNK2038.
  // Skate3Recomp builds the SDK from source via add_subdirectory, making /MT
  // consistent. We gate on requires_sdk_source for the same reason.
  if (ctx.profile.build_options.static_msvc_runtime &&
      ctx.profile.requires_sdk_source) {
    configure_args.push_back("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>");
  }

  // LTO for Release builds only (UnleashedRecomp pattern).
  if (ctx.profile.build_options.enable_lto) {
    configure_args.push_back("-DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON");
  }

  // CPU target — "x86-64-v3" enables AVX2/BMI2 for better PPC SIMD matching.
  // Passed as CMAKE_CXX_FLAGS since the SDK has no REX_CPU_TARGET variable.
  if (!ctx.profile.build_options.cpu_target.empty()) {
    configure_args.push_back("-DCMAKE_CXX_FLAGS=-march=" + ctx.profile.build_options.cpu_target);
  }

  // ccache — if available on PATH, use it as the compiler launcher to speed
  // up incremental rebuilds (UnleashedRecomp CI pattern). Silently skipped
  // if ccache is not installed.
  if (ctx.toolchain.git_exe.empty() == false) {  // quick env sanity check
    // Use _popen to check for ccache (same pattern as dependency_checker.cpp).
    std::string ccache_out;
    FILE* pipe = _popen("where ccache 2>nul", "r");
    if (pipe) {
      std::array<char, 256> buf{};
      while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        ccache_out += buf.data();
      _pclose(pipe);
    }
    if (!ccache_out.empty()) {
      configure_args.push_back("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache");
      configure_args.push_back("-DCMAKE_C_COMPILER_LAUNCHER=ccache");
      progress(0.05f, "ccache detected — enabled for incremental rebuilds");
    }
  }
  // Explicitly point CMAKE_LINKER to MSVC's link.exe — MinGW's ld.exe (from
  // WinLibs Ninja distribution) shadows it in PATH and CMake's vs_link_exe
  // wrapper picks up the wrong linker.
  fs::path msvc_link = ctx.toolchain.vs_install_dir / "VC" / "Tools" /
                       "MSVC" / ctx.toolchain.msvc_toolset_version /
                       "bin" / "HostX64" / "x64" / "link.exe";
  if (fs::exists(msvc_link)) {
    configure_args.push_back("-DCMAKE_LINKER=" + msvc_link.string());
  }

  recomp::ProgressCallback on_line = [&progress](float, const std::string& line) {
    progress(0.3f, line);
  };

  // Run inside the MSVC env (ctx.build_env) so clang-cl/link.exe/Win SDK resolve.
  auto pr = recomp::ProcessRunner::run(ctx.toolchain.cmake_exe, configure_args,
                                       fs::path{}, ctx.build_env, on_line);
  if (pr.exit_code != 0) {
    return StageResult::fail(
        "Game cmake configure failed (exit " + std::to_string(pr.exit_code) + ").",
        pr.exit_code);
  }

  // ---- CMake build ----
  progress(0.4f, "Building game (cmake --build --parallel)...");

  std::vector<std::string> build_args = {
      "--build", build_dir.string(),
      "--parallel",
  };

  // ninja prints [n/total] progress lines; forward them. We estimate fraction
  // from [n/total] when present so the UI shows real progress through the
  // ~3-4min compile of 92 shards.
  recomp::ProgressCallback on_build_line = [&progress](float, const std::string& line) {
    auto lb = line.find('[');
    auto slash = line.find('/', lb == std::string::npos ? 0 : lb);
    auto rb = line.find(']', slash == std::string::npos ? 0 : slash);
    float frac = 0.7f;
    if (lb != std::string::npos && slash != std::string::npos && rb != std::string::npos) {
      try {
        int n = std::stoi(line.substr(lb + 1, slash - lb - 1));
        int total = std::stoi(line.substr(slash + 1, rb - slash - 1));
        if (total > 0) frac = 0.4f + 0.5f * (static_cast<float>(n) / total);
      } catch (...) {
      }
    }
    progress(frac, line);
  };

  pr = recomp::ProcessRunner::run(ctx.toolchain.cmake_exe, build_args,
                                  fs::path{}, ctx.build_env, on_build_line);
  if (pr.exit_code != 0) {
    return StageResult::fail(
        "Game build failed (exit " + std::to_string(pr.exit_code) + ").",
        pr.exit_code);
  }

  progress(0.95f, "Verifying game executable...");

  // Locate the built exe (Ninja single-config: build-dir root).
  fs::path exe = build_dir / (ctx.profile_name + ".exe");
  std::error_code ec2;
  if (!fs::exists(exe, ec2)) {
    return StageResult::fail(
        "Build succeeded but " + ctx.profile_name + ".exe not found in " +
        build_dir.string());
  }

  // Sanity: the exe should be substantial (~118MB). Reject <10MB (likely a
  // stub / failed link).
  if (fs::file_size(exe) < 10ull * 1024 * 1024) {
    return StageResult::fail(
        ctx.profile_name + ".exe is suspiciously small (<10MB) — build likely incomplete.");
  }

  ctx.built_exe = exe;

  progress(1.0f, "Game built: " + exe.string());
  return StageResult::ok("Built " + exe.string());
}

bool GameBuilderStage::is_complete(const PipelineContext& ctx) const {
  if (ctx.built_exe.empty()) return false;
  std::error_code ec;
  return fs::exists(ctx.built_exe, ec);
}

}  // namespace recomp
