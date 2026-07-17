// runtime_builder.cpp — Stage 5 implementation.
//
// Builds a custom rexruntime.dll from the (patched) SDK source tree, replacing
// the prebuilt DLL so the save-system + queueframes + ThinLTO patches
// take effect. SKIPPABLE: if the profile declares no runtime patches, the
// prebuilt rexruntime.dll from the SDK bin/ is used and this stage is a no-op.
//
// Build command (docs/04_build_automation.md §4, normalized from the buggy
// rebuild_runtime_lto.bat — explicit -G Ninja + clang-cl + consistent output):
//
//   cmake -G Ninja -S <SDK_SOURCE> -B <RUNTIME_BUILD_DIR> \
//       -DCMAKE_BUILD_TYPE=Release \
//       -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
//       -DCMAKE_C_COMPILER=clang-cl \
//       -DCMAKE_CXX_COMPILER=clang-cl \
//       -DREX_GAME_PROFILE=spiderman3 \
//       -D<each runtime flag>=1
//   cmake --build <RUNTIME_BUILD_DIR> --parallel --target rexruntime
//
// Both cmake invocations run inside the MSVC environment (ctx.build_env, the
// vcvarsall x64 env vars populated by main.cpp at startup) via ProcessRunner.
//
// Output: <RUNTIME_BUILD_DIR>/rexruntime.dll (Ninja single-config puts the
// binary at the build-dir root, NOT in a Release/ subfolder).

#include "stages/runtime_builder.h"

#include <filesystem>
#include <string>
#include <vector>

#include "core/pipeline_context.h"
#include "core/process_runner.h"
#include "core/types.h"

namespace fs = std::filesystem;

namespace recomp {

namespace {

bool profile_needs_runtime_build(const profile::GameProfile& profile) {
  return profile.requires_sdk_source || !profile.runtime_flags.empty() ||
         !profile.runtime_patches.empty();
}

}  // namespace

CheckResult RuntimeBuilderStage::check_prereqs(const PipelineContext& ctx) const {
  CheckResult result;

  // Profile-level custom DLL (e.g. SP3's 16.3MB FSR + save fix DLL) takes
  // precedence over building from source. If the profile declares a DLL,
  // it MUST exist — this is a requirement, not a hint. A missing profile
  // DLL is a hard failure (the game would ship without save/FSR fixes).
  if (!ctx.profile.custom_runtime_dll.empty()) {
    fs::path profile_dll = ctx.profile.profile_dir / ctx.profile.custom_runtime_dll;
    std::error_code ec;
    if (fs::exists(profile_dll, ec)) {
      result.ok = true;
      result.message = "Profile ships custom_runtime_dll — stage will skip.";
      return result;
    }
    result.ok = false;
    result.missing.push_back("Profile custom_runtime_dll: " + profile_dll.string());
    result.message =
        "Profile declares custom_runtime_dll but the file does not exist: " +
        profile_dll.string();
    return result;
  }

  bool needs_runtime_build = profile_needs_runtime_build(ctx.profile);
  if (!needs_runtime_build) {
    result.ok = true;
    result.message = "No runtime patches — stage will skip (use prebuilt DLL).";
    return result;
  }

  // MSVC toolchain env must be available (vcvarsall resolved by main.cpp).
  if (ctx.build_env.empty()) {
    result.ok = false;
    result.missing.push_back("MSVC toolchain environment (vcvarsall.bat x64)");
    result.message =
        "MSVC environment unavailable — cannot build the custom runtime. "
        "Resolve vcvarsall via the dependency checker.";
    return result;
  }

  // SDK source tree (patched working copy staged by PatchApplier, or the
  // pristine tree if PatchApplier didn't stage one).
  fs::path sdk_src = ctx.output_dir / ".recomp" / "sdk-src";
  if (!fs::exists(sdk_src)) sdk_src = ctx.sdk_source_path;
  if (sdk_src.empty() || !fs::exists(sdk_src)) {
    result.ok = false;
    result.missing.push_back("RexGlue SDK source tree: " + sdk_src.string());
    result.message = "SDK source tree not available for the runtime build.";
    return result;
  }

  // CMake from the toolchain.
  if (ctx.toolchain.cmake_exe.empty() || !fs::exists(ctx.toolchain.cmake_exe)) {
    result.ok = false;
    result.missing.push_back("cmake: " + ctx.toolchain.cmake_exe.string());
    result.message = "CMake not found.";
    return result;
  }

  result.ok = true;
  return result;
}

StageResult RuntimeBuilderStage::run(PipelineContext& ctx, ProgressCallback progress) {
  // Profile-level custom DLL takes precedence over building from source.
  // If the profile declares a DLL, it MUST exist — fail if missing.
  if (!ctx.profile.custom_runtime_dll.empty()) {
    fs::path profile_dll = ctx.profile.profile_dir / ctx.profile.custom_runtime_dll;
    std::error_code ec;
    if (fs::exists(profile_dll, ec)) {
      ctx.custom_runtime_dll = profile_dll;
      return StageResult::skip(
          "Using profile-provided custom_runtime_dll: " + profile_dll.string());
    }
    return StageResult::fail(
        "Profile declares custom_runtime_dll but file does not exist: " +
        profile_dll.string());
  }

  if (!profile_needs_runtime_build(ctx.profile)) {
    // The prebuilt rexruntime.dll lives at <sdk_path>/bin/rexruntime.dll.
    std::error_code ec;
    fs::path prebuilt = ctx.sdk_path / "bin" / "rexruntime.dll";
    if (fs::exists(prebuilt, ec)) {
      ctx.custom_runtime_dll = prebuilt;
    }
    return StageResult::skip(
        "No runtime patches — using prebuilt rexruntime.dll.");
  }

  fs::path sdk_src = ctx.output_dir / ".recomp" / "sdk-src";
  if (!fs::exists(sdk_src)) sdk_src = ctx.sdk_source_path;
  fs::path build_dir = ctx.output_dir / ".recomp" / "runtime-build";
  ctx.runtime_build_dir = build_dir;

  std::error_code ec;
  if (ctx.clean && fs::exists(build_dir, ec)) {
    fs::remove_all(build_dir, ec);
  }
  fs::create_directories(build_dir, ec);

  // ---- CMake configure ----
  progress(0.1f, "Configuring runtime build (cmake)...");

  std::vector<std::string> configure_args = {
      "-G", "Ninja",
      "-S", sdk_src.string(),
      "-B", build_dir.string(),
      "-DCMAKE_BUILD_TYPE=Release",
      "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON",
      "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF",  // clang-cl doesn't support C++23 module scanning (fmt-module)
      "-DFMT_MODULE=OFF",  // {fmt} C++23 module target incompatible with clang-cl
      "-DCMAKE_C_COMPILER=" + ctx.toolchain.clang_cl_exe.string(),
      "-DCMAKE_CXX_COMPILER=" + ctx.toolchain.clang_cl_exe.string(),
      "-DREX_GAME_PROFILE=" + ctx.profile_name,
  };
  // Static MSVC runtime — only safe when building SDK from source (all targets
  // share /MT). The runtime_builder only runs when requires_sdk_source=true,
  // but guard explicitly for consistency with game_builder.cpp.
  if (ctx.profile.build_options.static_msvc_runtime &&
      ctx.profile.requires_sdk_source) {
    configure_args.push_back("-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>");
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
  // Pass each runtime flag as a -D define (e.g. -DREX_GAME_SAVE_SYSTEM_FIX=1).
  for (const auto& flag : ctx.profile.runtime_flags) {
    configure_args.push_back("-D" + flag + "=1");
  }

  // Graphics backend — D3D12 only (bundled SDK is D3D12-only).
  configure_args.push_back("-DREXGLUE_USE_D3D12=ON");
  configure_args.push_back("-DREXGLUE_USE_VULKAN=OFF");

  recomp::ProgressCallback on_line = [&progress](float, const std::string& line) {
    progress(0.3f, line);
  };

  // Run inside the MSVC env (ctx.build_env) so clang-cl/link.exe/Win SDK resolve.
  auto pr = recomp::ProcessRunner::run(ctx.toolchain.cmake_exe, configure_args,
                                       fs::path{}, ctx.build_env, on_line);
  if (pr.exit_code != 0) {
    return StageResult::fail(
        "Runtime cmake configure failed (exit " + std::to_string(pr.exit_code) + ").",
        pr.exit_code);
  }

  // ---- CMake build ----
  progress(0.5f, "Building rexruntime (cmake --build --target rexruntime)...");

  std::vector<std::string> build_args = {
      "--build", build_dir.string(),
      "--parallel",
      "--target", "rexruntime",
  };

  recomp::ProgressCallback on_build_line = [&progress](float, const std::string& line) {
    progress(0.7f, line);
  };

  pr = recomp::ProcessRunner::run(ctx.toolchain.cmake_exe, build_args,
                                  fs::path{}, ctx.build_env, on_build_line);
  if (pr.exit_code != 0) {
    return StageResult::fail(
        "Runtime build failed (exit " + std::to_string(pr.exit_code) + ").",
        pr.exit_code);
  }

  progress(0.9f, "Verifying rexruntime.dll...");

  // The SDK's CMakeLists.txt sets CMAKE_RUNTIME_OUTPUT_DIRECTORY to
  // <source>/out/<platform>/ (e.g. out/win-amd64/), so the DLL ends up
  // there, not in the build dir. Check all possible locations.
  fs::path dll = build_dir / "rexruntime.dll";
  if (!fs::exists(dll, ec)) {
    dll = build_dir / "Release" / "rexruntime.dll";
  }
  if (!fs::exists(dll, ec)) {
    // SDK's CMAKE_RUNTIME_OUTPUT_DIRECTORY: <sdk-src>/out/win-amd64/
    dll = sdk_src / "out" / "win-amd64" / "rexruntime.dll";
  }
  if (!fs::exists(dll, ec)) {
    return StageResult::fail(
        "Build succeeded but rexruntime.dll not found under " + build_dir.string());
  }

  // Sanity: the DLL must be > 1MB (catches empty/truncated builds).
  if (fs::file_size(dll) < 1024 * 1024) {
    return StageResult::fail(
        "rexruntime.dll is suspiciously small (<1MB) — build likely incomplete.");
  }

  ctx.custom_runtime_dll = dll;

  progress(1.0f, "Custom runtime built: " + dll.string());
  return StageResult::ok("Built custom rexruntime.dll: " + dll.string());
}

bool RuntimeBuilderStage::is_complete(const PipelineContext& ctx) const {
  // Profile-level custom DLL: complete only if the declared file exists.
  // A declared-but-missing DLL is NOT complete — the pipeline must not
  // proceed to deploy without the required custom runtime.
  if (!ctx.profile.custom_runtime_dll.empty()) {
    fs::path profile_dll = ctx.profile.profile_dir / ctx.profile.custom_runtime_dll;
    std::error_code ec;
    return fs::exists(profile_dll, ec);
  }
  // A skipped prebuilt-runtime stage is complete once its DLL is recorded.
  if (!profile_needs_runtime_build(ctx.profile)) {
    return !ctx.custom_runtime_dll.empty();
  }
  // Built stage: custom DLL must exist.
  if (ctx.custom_runtime_dll.empty()) return false;
  const fs::path prebuilt = ctx.sdk_path / "bin" / "rexruntime.dll";
  std::error_code equivalent_ec;
  if (fs::equivalent(ctx.custom_runtime_dll, prebuilt, equivalent_ec)) {
    return false;
  }
  std::error_code ec;
  return fs::exists(ctx.custom_runtime_dll, ec);
}

}  // namespace recomp
