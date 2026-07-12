// game_profile.h — GameProfile: the declarative heart of the recompiler.
//
// A game profile is a directory (shipped with the app or user-added) that
// contains all game-specific knowledge: cvars, source files to copy into the
// project, runtime patch descriptors, and the deploy TOML template. Spider-Man
// 3 is one profile; another Xbox 360 title is another profile — no code
// changes required (docs/01_architecture.md §6.2).
//
// The struct below is the stable contract consumed by PipelineContext.profile
// and by every pipeline stage. Field names and types are final.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace recomp::profile {

namespace fs = std::filesystem;

// A source file copied from the profile into the generated project's src/.
// `from` is relative to profile_dir; `to` is relative to the project dir.
struct SourceFile {
  std::string from;             // e.g. "src/main.cpp"
  std::string to;               // e.g. "src/main.cpp"
  bool        optional = false; // not required in CMakeLists (e.g. DRS orphan)
};

// A runtime patch descriptor. Patches are flag-guarded source overlays applied
// to the SDK source tree (NOT unified diffs — see docs/02_patch_system.md).
// `flag` is the CMake -D define that gates the overlay; "" for build-config-only.
struct RuntimePatch {
  std::string id;               // "xam_enum", "xenumerator", "xam_ui", ...
  std::string flag;             // "REX_XAM_ENUM_IO_PENDING", "REX_THINLTO", ""
  std::string target;           // SDK source path touched, e.g.
                                // "src/kernel/xam/xam_enum.cpp" or
                                // "system/CMakeLists" for build-config patches
  std::string category;         // "game-runtime" | "general-runtime" | "game-build"
  bool        required = true;  // false = optional perf patch
};

// The loaded game profile. Produced by load_profile()/load_profile_from_dir().
struct GameProfile {
  // --- Metadata ---
  std::string id;               // "spiderman3"
  std::string name;             // "Spider-Man 3 (Xbox 360)"
  std::string title_id;         // "415607E2" (Xbox 360 title ID)
  std::string sdk_version;      // "0.8.0"
  std::string xex_entrypoint = "default.xex";
  std::string project_name;     // "spiderman3" (build/project dir name)

  // --- Patches as data ---
  // cvar name -> value (rendered into <name>_app.h::OnPreSetup).
  std::map<std::string, std::string> cvars;

  // Source files copied into the project src/ after codegen.
  std::vector<SourceFile> source_files;

  // Runtime patch descriptors (applied to the SDK source tree).
  std::vector<RuntimePatch> runtime_patches;

  // CMake -D defines passed to the runtime build; each maps to an overlay dir.
  std::vector<std::string> runtime_flags;

  // True when the profile declares runtime patches → build_runtime runs.
  bool requires_sdk_source = false;
  // --- Build options (from [build_options] in profile.toml) ---
  struct BuildOptions {
    bool static_msvc_runtime = true;   // static CRT (no VC++ redistributable)
    bool enable_lto = true;            // LTO for Release builds only
    std::string cpu_target;            // "" = default, "x86-64-v3" = AVX2/BMI2
  } build_options;

  // Legacy entrypoint function hints: PPC address (hex string key) -> symbol
  // name. Injected into the manifest as [entrypoint.functions.<addr>] sections
  // so rexglue codegen treats them as analysis roots. Superseded by the richer
  // [functions] table below (which supports end/parent boundaries), but kept
  // for backward compatibility with the Spider-Man 3 profile.
  std::map<std::string, std::string> entrypoint_functions;

  // Function boundary overrides: PPC address (hex string key) -> {end, parent,
  // name}. The codegen reads these from the manifest's [functions] table
  // (config.cpp:183-227) — explicit end addresses prevent the function scanner
  // from truncating at conditional branches (e.g. _setjmp's bnectr), and parent
  // relationships help with overlapping functions. Inspired by Skate3Recomp's
  // config/skate3_functions.toml (1777 entries with end/parent).
  struct FunctionEntry {
    uint64_t end = 0;        // exclusive end address (0 = auto-detect)
    uint64_t parent = 0;     // parent function address (0 = none)
    std::string name;        // symbol name (empty = auto-generate)
  };
  std::map<std::string, FunctionEntry> functions;

  // Invalid instruction hints: 32-bit data value -> skip size. The codegen
  // skips over these in the instruction stream (exception data, padding,
  // frame handlers that the disassembler would misinterpret as instructions).
  // XenonRecomp pattern (recompiler_config.cpp:61-70).
  std::map<uint64_t, uint64_t> invalid_instructions;

  // Setjmp/longjmp addresses for Xenon CRT _setjmp hook-stub fix.
  // The function scanner truncates _setjmp at a conditional bnectr; the
  // recompiled version returns garbage, breaking setjmp-protected code
  // (e.g. Lua's luaD_rawrunprotected). 0 = not needed.
  uint64_t setjmp_address = 0;
  uint64_t longjmp_address = 0;

  // Switch table overrides for misdetected jump tables.
  struct SwitchTable {
    uint64_t address;              // bctr instruction address
    int register_index;            // register holding case index
    std::vector<uint64_t> labels;  // case targets
  };
  std::vector<SwitchTable> switch_tables;

  // --- Paths ---
  fs::path profile_dir;         // .../profiles/spiderman3
  fs::path toml_template;       // profile_dir / "spiderman3.toml.template"

  // --- Deploy ---
  std::vector<std::string> copy_dlls = {"rexruntime.dll", "TracyClient.dll"};
  bool create_game_junction = true;
  bool create_user_data     = true;
};

// Load + validate a profile from <app_dir>/profiles/<id>/profile.toml.
// Throws std::runtime_error on missing file or schema violation.
[[nodiscard]] GameProfile load_profile(const fs::path& app_dir,
                                       const std::string& profile_id);

// Load + validate a profile from an explicit profile directory.
// Throws std::runtime_error on missing file or schema violation.
[[nodiscard]] GameProfile load_profile_from_dir(const fs::path& profile_dir);

// Cross-check that every non-optional .cpp in source_files appears in the
// project's CMakeLists source list. The spiderman3 profile's CMakeLists is
// not known at load time, so this is a helper the PatchApplier stage can call
// once the generated CMakeLists is in hand. Returns the list of orphans.
[[nodiscard]] std::vector<std::string> find_source_orphans(
    const GameProfile& profile,
    const std::vector<std::string>& cmakelists_sources);

}  // namespace recomp::profile
