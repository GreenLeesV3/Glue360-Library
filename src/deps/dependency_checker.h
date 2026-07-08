// dependency_checker.h — Phase 0 prerequisite checker for the Spider-Man 3
// Recompiler pipeline. Probes the build toolchain (CMake, Ninja, clang-cl,
// MSVC via vswhere, Windows SDK), the RexGlue SDK (prebuilt + source), and
// extract-xiso. Reports exact discovered versions, paths, and missing items
// with a severity model (BLOCKING / SOFT / INFO).
//
// Design grounded in docs/04_build_automation.md §2 and docs/01_architecture.md
// §5. No hardcoded C:\tmp paths — all discovery is dynamic (PATH, vswhere,
// registry, user-supplied SDK root).

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace recomp::deps {

namespace fs = std::filesystem;

// Severity of a missing/unmet dependency (docs/04_build_automation.md §2.3).
enum class Severity {
  Ok,        // found and version acceptable
  Info,      // optional tool missing (Tracy UI, dxcompiler.dll)
  Soft,      // degraded path exists (no Ninja → bundle it; no Git → archive SDK)
  Blocking,  // cannot proceed (no CMake/clang-cl/MSVC/Win SDK/prebuilt SDK)
};

// Version-drift warning categories (docs/04_build_automation.md §2.4).
// Empty string means no warning. Otherwise a human-readable remediation.
struct VersionWarning {
  bool        present = false;
  std::string message;  // e.g. "clang-cl < 16: no C++23 support"
};

// One checked dependency.
struct DepResult {
  std::string     id;           // "cmake", "ninja", "clang_cl", "msvc", ...
  std::string     display_name; // "CMake", "Ninja", "LLVM/clang-cl", ...
  Severity        severity = Severity::Blocking;
  std::string     version;      // discovered version string ("" if missing)
  fs::path        path;         // discovered path ("" if missing)
  std::string     remediation;  // install hint when missing/suboptimal
  VersionWarning  warning;      // version-drift note (non-fatal)
};

// Resolved toolchain — consumed by PipelineContext.toolchain (architecture
// §3.2). Lives here so the deps module owns the type it produces; core may
// re-export or alias it. Keep the field set stable.
struct ToolchainInfo {
  // Compilers / build tools
  fs::path cmake_exe;
  std::string cmake_version;
  fs::path ninja_exe;
  std::string ninja_version;
  fs::path clang_cl_exe;
  std::string clang_cl_version;     // e.g. "22.1.8"
  std::string clang_cl_target;      // e.g. "x86_64-pc-windows-msvc"

  // MSVC toolchain (resolved via vswhere, NOT hardcoded edition path)
  fs::path vs_install_dir;          // .../Microsoft Visual Studio/2022/Community
  fs::path vcvarsall_bat;           // <vs_install>/VC/Auxiliary/Build/vcvarsall.bat
  std::string msvc_toolset_version; // e.g. "14.44.35207"

  // Windows SDK
  fs::path    windows_sdk_root;     // .../Windows Kits/10
  std::string windows_sdk_version;  // e.g. "10.0.22621.0"

  // RexGlue SDK (prebuilt) — BLOCKING if absent
  fs::path    sdk_root;             // CMAKE_PREFIX_PATH
  std::string sdk_version;          // "0.8.0"

  // RexGlue SDK source (for custom runtime build; SOFT if absent)
  fs::path    sdk_source_dir;
  bool        sdk_source_is_git = false;

  // Optional / soft tools
  fs::path    git_exe;
  fs::path    extract_xiso_exe;

  // True when every BLOCKING dep was found.
  bool        blocking_ok = false;
};

// Full prerequisite report.
struct DepCheckReport {
  std::vector<DepResult> results;
  ToolchainInfo          toolchain;
  bool                   blocking_ok = false;  // all BLOCKING deps present
  bool                   soft_ok = false;      // all SOFT deps present
};

// Options for running the checker. All optional — defaults auto-discover.
struct DepCheckOptions {
  // User-supplied RexGlue prebuilt SDK root. If empty, the checker probes
  // candidate locations (env REXGLUE_SDK, sibling of the app, etc.).
  std::optional<fs::path> sdk_root;
  // User-supplied RexGlue SDK source dir. If empty, probes candidate paths.
  std::optional<fs::path> sdk_source_dir;
  // Optional vswhere override (default: the well-known Program Files (x86) path).
  std::optional<fs::path> vswhere_exe;
  // When true, skip the extract-xiso probe (it's bundle-able / optional).
  bool skip_extract_xiso = false;
  // App directory (where the exe lives + profiles/ + tools/ + sdk/ subdirs).
  // When set, probes <app_dir>/sdk/ for a bundled RexGlue SDK,
  // <app_dir>/tools/cmake/bin/cmake.exe, <app_dir>/tools/ninja.exe, and
  // <app_dir>/tools/extract-xiso.exe before falling back to PATH discovery.
  std::optional<fs::path> app_dir;
};

// Run the full prerequisite check and return a report. Never throws — on
// internal probe failure a DepResult is recorded with severity + remediation.
[[nodiscard]] DepCheckReport check_dependencies(const DepCheckOptions& opts = {});

// Format a report as a human-readable table for the CLI (`spiderman3-builder
// check`). Green/yellow/red coloring is left to the caller; this returns plain
// text with a [OK]/[SOFT]/[BLOCK]/[INFO] marker per row.
[[nodiscard]] std::string format_report_text(const DepCheckReport& report);

// Serialize a report to JSON for prereq_manifest.json (docs/04 §2.2).
// Includes exact versions + resolved paths so a "works on my machine" build
// can be diffed against a failing one.
[[nodiscard]] std::string format_report_json(const DepCheckReport& report);

// Convenience: true iff the report has no BLOCKING failures.
[[nodiscard]] inline bool can_proceed(const DepCheckReport& r) {
  return r.blocking_ok;
}

}  // namespace recomp::deps
