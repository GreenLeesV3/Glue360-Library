// core/pipeline_context.h — shared pipeline state.
//
// GameProfile and ToolchainInfo are owned by the profile/deps layers
// (src/profile/game_profile.h and src/deps/dependency_checker.h) and included
// here by reference so the whole app agrees on a single definition (ODR).
//
// Field names below match DepsAndProfile's confirmed contract:
//   - toolchain uses clang_cl_exe / vcvarsall_bat / sdk_root / blocking_ok
//     (NOT llvm_bin/rexglue_exe/resolved).
//   - profile uses recomp::profile::GameProfile (rich schema).
#pragma once

#include "core/types.h"            // recomp::GraphicsBackend

#include "deps/dependency_checker.h"   // recomp::deps::ToolchainInfo
#include "profile/game_profile.h"      // recomp::profile::GameProfile

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace recomp {

namespace fs = std::filesystem;

struct PipelineContext {
    // --- user inputs ---
    fs::path iso_path;
    fs::path output_dir;
    fs::path app_dir;
    fs::path sdk_path;          // prebuilt SDK root (also toolchain.sdk_root)
    fs::path sdk_source_path;   // SDK source tree (for custom runtime build)
    std::string profile_name = "spiderman3";
    GraphicsBackend graphics_backend = GraphicsBackend::D3D12;

    // --- resolved by stages ---
    fs::path extracted_dir;
    fs::path project_dir;
    fs::path manifest_path;
    fs::path generated_dir;
    int      generated_shard_count = 0;
    fs::path runtime_build_dir;
    fs::path custom_runtime_dll;
    fs::path tracy_dll_path;
    fs::path game_build_dir;
    fs::path built_exe;
    fs::path deploy_dir;

    // --- toolchain (resolved by dependency checker) ---
    deps::ToolchainInfo toolchain;

    // --- captured MSVC build env (vcvarsall x64 output), for ProcessRunner ---
    std::map<std::string, std::string> build_env;

    // --- profile (loaded by GameProfileLoader; not serialized — reloaded) ---
    profile::GameProfile profile;

    // --- transient flags (not serialized) ---
    bool clean  = false;
    bool resume = false;

    // JSON serialize/deserialize for state.json. Serializes paths + toolchain;
    // does NOT serialize build_env, profile, or transient flags.
    std::string to_json() const;
    bool from_json(const std::string& text);

    // Derived paths under output_dir.
    fs::path recomp_dir() const;
    fs::path log_dir() const;
    fs::path state_path() const;
};

} // namespace recomp
