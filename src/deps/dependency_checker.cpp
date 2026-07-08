// dependency_checker.cpp — implementation of the Phase 0 prerequisite checker.
//
// Discovery strategy per docs/04_build_automation.md §2.1:
//   CMake / Ninja / clang-cl / git / extract-xiso → `where <tool>` then
//     `<tool> --version` parse.
//   MSVC → vswhere.exe -latest -products * -requires VC.Tools.x86.x64
//     -property installationPath → <path>/VC/Auxiliary/Build/vcvarsall.bat.
//   Windows SDK → vswhere -requires Microsoft.Windows.SDK.* OR enumerate
//     Windows Kits/10/Include/*.
//   RexGlue prebuilt SDK → probe bin/rexglue.exe, bin/rexruntime.dll,
//     cmake/SDL3Config.cmake, include/rex/rex_app.h.
//   RexGlue SDK source → probe CMakeLists.txt + src/ + thirdparty/ + .gitmodules.
//
// No hardcoded C:\tmp paths. All discovery is dynamic.

#include "dependency_checker.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace recomp::deps {

namespace {

// ---------------------------------------------------------------------------
// Process execution helpers (capture stdout)
// ---------------------------------------------------------------------------

// Run `cmd` via the shell, capture combined stdout (stderr discarded to keep
// version strings clean). Returns trimmed output on success, "" on failure.
// Uses _popen for portability across MSVC/clang-cl. Never throws.
std::string run_capture(const std::string& cmd) {
  std::string out;
#ifdef _WIN32
  // _Popen reads a pipe; on Windows use _popen with "r".
  FILE* pipe = _popen(cmd.c_str(), "r");
#else
  FILE* pipe = popen(cmd.c_str(), "r");
#endif
  if (!pipe) return out;
  std::array<char, 4096> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
    out += buf.data();
  }
#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  // Trim trailing whitespace/newlines.
  while (!out.empty() &&
         (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' ||
          out.back() == '\t')) {
    out.pop_back();
  }
  return out;
}

// Quote a path for shell inclusion.
std::string quote(const fs::path& p) {
  std::string s = p.string();
  if (s.empty()) return "\"\"";
  // Double any embedded backslashes are fine inside quotes for cmd.exe.
  return "\"" + s + "\"";
}

// `where <name>` on Windows / `which <name>` elsewhere. Returns the first hit.
fs::path where_tool(const std::string& name) {
  if (name.empty()) return {};
#ifdef _WIN32
  std::string cmd = "where " + name + " 2>nul";
#else
  std::string cmd = "which " + name + " 2>/dev/null";
#endif
  std::string out = run_capture(cmd);
  if (out.empty()) return {};
  // `where` may return multiple lines; take the first.
  auto nl = out.find('\n');
  std::string first = (nl == std::string::npos) ? out : out.substr(0, nl);
  // Trim spaces.
  while (!first.empty() && (first.back() == ' ' || first.back() == '\r'))
    first.pop_back();
  while (!first.empty() && first.front() == ' ') first.erase(0, 1);
  return fs::path(first);
}

// Read a file's first line, or "" on failure.
std::string read_first_line(const fs::path& p) {
  std::error_code ec;
  if (!fs::exists(p, ec)) return {};
  FILE* f = nullptr;
#ifdef _WIN32
  fopen_s(&f, p.string().c_str(), "r");
#else
  f = std::fopen(p.string().c_str(), "r");
#endif
  if (!f) return {};
  std::array<char, 1024> buf{};
  if (!std::fgets(buf.data(), static_cast<int>(buf.size()), f)) {
    std::fclose(f);
    return {};
  }
  std::fclose(f);
  std::string s = buf.data();
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

// Extract first match of a regex from a string.
std::string regex_extract(const std::string& s, const std::string& re,
                          int group = 1) {
  try {
    std::regex r(re);
    std::smatch m;
    if (std::regex_search(s, m, r) && group < (int)m.size()) {
      return m[group].str();
    }
  } catch (...) {
  }
  return "";
}

// ---------------------------------------------------------------------------
// Known-location helpers (NOT hardcoded paths — probed, fallback only)
// ---------------------------------------------------------------------------

fs::path default_vswhere_path() {
#ifdef _WIN32
  // vswhere ships with the VS Installer in Program Files (x86).
  char base[MAX_PATH] = {0};
  if (SHGetFolderPathA(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, base) ==
      S_OK) {
    fs::path p = fs::path(base) / "Microsoft Visual Studio" / "Installer" /
                 "vswhere.exe";
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
  }
  // Common fallback.
  fs::path p = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/"
               "vswhere.exe";
  std::error_code ec;
  return fs::exists(p, ec) ? p : fs::path{};
#else
  return {};
#endif
}

// Enumerate Windows Kits/10/Include/<ver> dirs, return the highest version.
std::string highest_windows_sdk_version() {
#ifdef _WIN32
  char base[MAX_PATH] = {0};
  if (SHGetFolderPathA(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, base) !=
      S_OK)
    return "";
  fs::path inc = fs::path(base) / "Windows Kits" / "10" / "Include";
  std::error_code ec;
  if (!fs::exists(inc, ec)) return "";
  std::string best;
  for (const auto& e : fs::directory_iterator(inc, ec)) {
    if (!e.is_directory()) continue;
    std::string v = e.path().filename().string();
    // Expect "10.0.<build>.0".
    if (v.rfind("10.0.", 0) == 0) {
      if (v > best) best = v;
    }
  }
  return best;
#else
  return "";
#endif
}

fs::path windows_sdk_root_for(const std::string& version) {
#ifdef _WIN32
  char base[MAX_PATH] = {0};
  if (SHGetFolderPathA(nullptr, CSIDL_PROGRAM_FILESX86, nullptr, 0, base) !=
      S_OK)
    return {};
  return fs::path(base) / "Windows Kits" / "10";
#else
  (void)version;
  return {};
#endif
}

// ---------------------------------------------------------------------------
// Per-tool probes
// ---------------------------------------------------------------------------

DepResult probe_cmake(std::optional<fs::path> app_dir) {
  DepResult r{"cmake", "CMake", Severity::Blocking};
  r.remediation = "Install CMake >= 3.25 (4.x recommended): winget Kitware.CMake";
  fs::path exe = where_tool("cmake");
  std::error_code ec;
  // Bundled fallback: <app_dir>/tools/cmake/bin/cmake.exe
  if ((exe.empty() || !fs::exists(exe, ec)) && app_dir && !app_dir->empty()) {
    fs::path bundled = *app_dir / "tools" / "cmake" / "bin" / "cmake.exe";
    if (fs::exists(bundled, ec)) exe = bundled;
  }
  if (exe.empty() || !fs::exists(exe, ec)) {
    r.severity = Severity::Blocking;
    return r;
  }
  r.path = exe;
  std::string ver = run_capture(quote(exe) + " --version");
  // "cmake version 4.2.1"
  r.version = regex_extract(ver, R"(cmake version\s+([0-9]+\.[0-9]+\.[0-9]+))");
  if (r.version.empty()) r.version = regex_extract(ver, R"(([0-9]+\.[0-9]+\.[0-9]+))");

  // Drift warning: < 3.25 is BLOCKING (game CMakeLists minimum).
  auto major = std::atoi(r.version.c_str());
  auto minor = [] (const std::string& s) {
    auto dot = s.find('.');
    if (dot == std::string::npos) return 0;
    return std::atoi(s.c_str() + dot + 1);
  }(r.version);
  if (major < 3 || (major == 3 && minor < 25)) {
    r.warning.present = true;
    r.warning.message =
        "CMake < 3.25: game CMakeLists.txt requires >= 3.25 (BLOCKING).";
    r.severity = Severity::Blocking;
  } else {
    r.severity = Severity::Ok;
  }
  return r;
}

DepResult probe_ninja(std::optional<fs::path> app_dir) {
  DepResult r{"ninja", "Ninja", Severity::Soft};
  r.remediation =
      "Install Ninja: winget Ninja-build.Ninja (or the app can bundle it).";
  fs::path exe = where_tool("ninja");
  std::error_code ec;
  // Bundled fallback: <app_dir>/tools/ninja.exe
  if ((exe.empty() || !fs::exists(exe, ec)) && app_dir && !app_dir->empty()) {
    fs::path bundled = *app_dir / "tools" / "ninja.exe";
    if (fs::exists(bundled, ec)) exe = bundled;
  }
  if (exe.empty() || !fs::exists(exe, ec)) {
    r.severity = Severity::Soft;  // bundle-able
    return r;
  }
  r.path = exe;
  std::string ver = run_capture(quote(exe) + " --version");
  r.version = regex_extract(ver, R"(([0-9]+\.[0-9]+\.[0-9]+))");
  if (r.version.empty()) r.version = ver;
  r.severity = Severity::Ok;
  return r;
}
DepResult probe_clang_cl() {
  DepResult r{"clang_cl", "LLVM/clang-cl", Severity::Blocking};
  r.remediation = "Install LLVM: winget LLVM.LLVM (need clang-cl with MSVC target).";
  fs::path exe = where_tool("clang-cl");
  std::error_code ec;
  if (exe.empty() || !fs::exists(exe, ec)) {
    // Try the well-known LLVM install dir as a fallback (probed, not assumed).
#ifdef _WIN32
    char base[MAX_PATH] = {0};
    if (SHGetFolderPathA(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, base) ==
        S_OK) {
      fs::path cand = fs::path(base) / "LLVM" / "bin" / "clang-cl.exe";
      if (fs::exists(cand, ec)) exe = cand;
    }
#endif
    if (exe.empty()) return r;
  }
  r.path = exe;
  std::string out = run_capture(quote(exe) + " --version");
  // "clang version 22.1.8 ... Target: x86_64-pc-windows-msvc"
  r.version = regex_extract(out, R"(clang version\s+([0-9]+\.[0-9]+\.[0-9]+))");
  // clang-cl target string is re-extracted by clang_cl_target_of() below
  // (kept off DepResult to avoid schema cruft).
  if (r.version.empty()) r.version = out;

  // clang-cl < 16 → no C++23 → BLOCKING.
  int major = std::atoi(r.version.c_str());
  if (major > 0 && major < 16) {
    r.warning.present = true;
    r.warning.message = "clang-cl < 16: no C++23 support (BLOCKING).";
    r.severity = Severity::Blocking;
  } else {
    r.severity = Severity::Ok;
  }
  return r;
}

DepResult probe_msvc(const fs::path& vswhere) {
  DepResult r{"msvc", "MSVC build tools (VS 2022)", Severity::Blocking};
  r.remediation =
      "Install Visual Studio 2022 with the 'Desktop development with C++' "
      "workload (includes VC.Tools.x86.x64 + Windows SDK).";
  std::error_code ec;
  if (vswhere.empty() || !fs::exists(vswhere, ec)) {
    return r;
  }
  // Resolve the latest VS with the x86/x64 VC tools.
  std::string cmd = quote(vswhere) +
                    " -latest -products * "
                    "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
                    " -property installationPath";
  std::string install = run_capture(cmd);
  if (install.empty()) {
    return r;
  }
  // vswhere may emit multiple lines; take the first.
  auto nl = install.find('\n');
  if (nl != std::string::npos) install = install.substr(0, nl);
  while (!install.empty() && install.back() == '\r') install.pop_back();
  fs::path vs_dir = install;
  fs::path vcvarsall = vs_dir / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat";
  if (!fs::exists(vcvarsall, ec)) {
    return r;
  }
  r.path = vcvarsall;

  // Toolset version from the VC/Tools/MSVC/<ver> directories.
  fs::path tools_dir = vs_dir / "VC" / "Tools" / "MSVC";
  std::string best;
  if (fs::exists(tools_dir, ec)) {
    for (const auto& e : fs::directory_iterator(tools_dir, ec)) {
      if (!e.is_directory()) continue;
      std::string v = e.path().filename().string();
      if (v > best) best = v;
    }
  }
  r.version = best;
  if (best.empty()) r.version = "unknown";

  // MSVC < 14.40 (VS 2022 17.10+) → warn.
  if (!best.empty()) {
    int major = std::atoi(best.c_str());
    if (major > 0 && major < 14) {
      r.warning.present = true;
      r.warning.message = "MSVC < 14.40: older toolchain may lack needed CRT/SDK.";
    } else if (major == 14) {
      int minor = [] (const std::string& s) {
        auto a = s.find('.'); auto b = (a == std::string::npos) ? std::string::npos : s.find('.', a + 1);
        if (b == std::string::npos) return 0;
        return std::atoi(s.c_str() + b + 1);
      }(best);
      if (minor < 40) {
        r.warning.present = true;
        r.warning.message =
            "MSVC < 14.40 (VS 2022 17.10): older toolchain may lack needed CRT/SDK.";
      }
    }
  }
  r.severity = Severity::Ok;
  return r;
}

DepResult probe_windows_sdk(const fs::path& vswhere) {
  DepResult r{"windows_sdk", "Windows SDK", Severity::Blocking};
  r.remediation = "Install the Windows 10/11 SDK via the Visual Studio Installer.";
  std::string ver;
  // Try vswhere -requires Microsoft.Windows.SDK.* first.
  std::error_code ec;
  if (!vswhere.empty() && fs::exists(vswhere, ec)) {
    std::string cmd = quote(vswhere) +
                      " -latest -products * "
                      "-requires Microsoft.Windows.SDK.* "
                      "-property version";
    ver = run_capture(cmd);
    auto nl = ver.find('\n');
    if (nl != std::string::npos) ver = ver.substr(0, nl);
    while (!ver.empty() && ver.back() == '\r') ver.pop_back();
  }
  // Fallback: enumerate Windows Kits/10/Include/*.
  if (ver.empty()) ver = highest_windows_sdk_version();
  if (ver.empty()) {
    return r;
  }
  r.version = ver;
  r.path = windows_sdk_root_for(ver);
  // 10.0.19041+ required.
  auto pos = ver.find("10.0.");
  if (pos == 0) {
    long build = std::atol(ver.c_str() + 5);  // "19041..."
    if (build > 0 && build < 19041) {
      r.warning.present = true;
      r.warning.message = "Windows SDK < 10.0.19041: too old for this toolchain.";
    }
  }
  r.severity = Severity::Ok;
  return r;
}

// Probe a candidate RexGlue prebuilt SDK root. Returns true if it looks like
// the SDK (probes per docs §2.1): bin/rexglue.exe, bin/rexruntime.dll,
// cmake/SDL3Config.cmake, include/rex/rex_app.h.
bool looks_like_sdk_root(const fs::path& root) {
  std::error_code ec;
  if (root.empty() || !fs::is_directory(root, ec)) return false;
  const auto check = [&](const fs::path& rel) {
    return fs::exists(root / rel, ec);
  };
  return check("bin/rexglue.exe") && check("bin/rexruntime.dll") &&
         check("cmake/SDL3Config.cmake") && check("include/rex/rex_app.h");
}

// Read the SDK version from include/rex/version.h (#define REX_VERSION_*).
std::string read_sdk_version(const fs::path& root) {
  fs::path vh = root / "include" / "rex" / "version.h";
  std::error_code ec;
  if (!fs::exists(vh, ec)) return "";
  std::string first = read_first_line(vh);  // not robust; do a real scan.
  (void)first;
  FILE* f = nullptr;
#ifdef _WIN32
  fopen_s(&f, vh.string().c_str(), "r");
#else
  f = std::fopen(vh.string().c_str(), "r");
#endif
  if (!f) return "";
  std::string maj, min, pat;
  std::array<char, 512> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), f)) {
    std::string line = buf.data();
    if (line.find("REX_VERSION_MAJOR") != std::string::npos)
      maj = regex_extract(line, R"((\d+))");
    else if (line.find("REX_VERSION_MINOR") != std::string::npos)
      min = regex_extract(line, R"((\d+))");
    else if (line.find("REX_VERSION_PATCH") != std::string::npos)
      pat = regex_extract(line, R"((\d+))");
  }
  std::fclose(f);
  if (maj.empty()) return "";
  std::string v = maj + "." + (min.empty() ? "0" : min) + "." +
                  (pat.empty() ? "0" : pat);
  return v;
}

DepResult probe_sdk_prebuilt(std::optional<fs::path> user_root,
                             std::optional<fs::path> app_dir) {
  DepResult r{"rexglue_sdk", "RexGlue SDK (prebuilt)", Severity::Blocking};
  r.remediation =
      "Supply the RexGlue360 prebuilt SDK root (must contain bin/rexglue.exe, "
      "bin/rexruntime.dll, cmake/, include/rex/), or bundle it at <app>/sdk/.";
  fs::path root;
  if (user_root && looks_like_sdk_root(*user_root)) {
    root = *user_root;
  } else {
    // Probe environment variable.
    if (const char* env = std::getenv("REXGLUE_SDK")) {
      fs::path cand = env;
      if (looks_like_sdk_root(cand)) root = cand;
    }
    // Probe sibling-of-exe: <app_dir>/sdk/
    if (root.empty() && app_dir && !app_dir->empty()) {
      fs::path cand = *app_dir / "sdk";
      if (looks_like_sdk_root(cand)) root = cand;
    }
  }
  if (root.empty()) {
    return r;
  }
  r.path = root;
  r.version = read_sdk_version(root);
  if (r.version.empty()) r.version = "0.8.0 (unversioned headers)";
  r.severity = Severity::Ok;
  return r;
}

// Probe a candidate RexGlue SDK source tree: CMakeLists.txt + src/ +
// thirdparty/ + .gitmodules.
bool looks_like_sdk_source(const fs::path& root) {
  std::error_code ec;
  if (root.empty() || !fs::is_directory(root, ec)) return false;
  const auto check = [&](const fs::path& rel) {
    return fs::exists(root / rel, ec);
  };
  return check("CMakeLists.txt") && check("src") && check("thirdparty") &&
         check(".gitmodules");
}

DepResult probe_sdk_source(std::optional<fs::path> user_root, bool& is_git) {
  DepResult r{"rexglue_sdk_source", "RexGlue SDK source", Severity::Soft};
  r.remediation =
      "Supply the RexGlue SDK source tree (for the custom runtime build). "
      "Required only if the profile declares runtime patches.";
  fs::path root;
  if (user_root && looks_like_sdk_source(*user_root)) {
    root = *user_root;
  } else {
    if (const char* env = std::getenv("REXGLUE_SDK_SOURCE")) {
      fs::path cand = env;
      if (looks_like_sdk_source(cand)) root = cand;
    }
  }
  if (root.empty()) {
    r.severity = Severity::Soft;
    return r;
  }
  r.path = root;
  // Version from the top-level CMakeLists project() if present.
  std::string cml = read_first_line(root / "CMakeLists.txt");
  (void)cml;
  // Cheap: look for a VERSION marker in the first ~4KB.
  FILE* f = nullptr;
#ifdef _WIN32
  fopen_s(&f, (root / "CMakeLists.txt").string().c_str(), "r");
#else
  f = std::fopen((root / "CMakeLists.txt").string().c_str(), "r");
#endif
  if (f) {
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), f)) {
      std::string line = buf.data();
      auto pos = line.find("VERSION");
      if (pos != std::string::npos) {
        std::string v = regex_extract(line, R"(VERSION\s+([0-9]+\.[0-9]+\.[0-9]+))");
        if (!v.empty()) { r.version = v; break; }
      }
    }
    std::fclose(f);
  }
  // Is it a git repo?
  std::error_code ec;
  is_git = fs::exists(root / ".git", ec);
  r.severity = Severity::Ok;
  return r;
}

DepResult probe_git() {
  DepResult r{"git", "Git", Severity::Soft};
  r.remediation = "Install Git: winget Git.Git (only needed for SDK source fetch).";
  fs::path exe = where_tool("git");
  std::error_code ec;
  if (exe.empty() || !fs::exists(exe, ec)) {
    r.severity = Severity::Soft;
    return r;
  }
  r.path = exe;
  std::string ver = run_capture(quote(exe) + " --version");
  r.version = regex_extract(ver, R"(git version\s+([0-9]+\.[0-9]+\.[0-9]+))");
  if (r.version.empty()) r.version = ver;
  r.severity = Severity::Ok;
  return r;
}

DepResult probe_extract_xiso(std::optional<fs::path> app_dir) {
  DepResult r{"extract_xiso", "extract-xiso", Severity::Soft};
  r.remediation = "extract-xiso is MIT-licensed and can be bundled with the app.";
  fs::path exe = where_tool("extract-xiso");
  std::error_code ec;
  // Bundled fallback: <app_dir>/tools/extract-xiso.exe
  if ((exe.empty() || !fs::exists(exe, ec)) && app_dir && !app_dir->empty()) {
    fs::path bundled = *app_dir / "tools" / "extract-xiso.exe";
    if (fs::exists(bundled, ec)) exe = bundled;
  }
  if (exe.empty() || !fs::exists(exe, ec)) {
    r.severity = Severity::Soft;
    return r;
  }
  r.path = exe;
  // extract-xiso has no stable --version; capture --help first line.
  std::string out = run_capture(quote(exe) + " --version 2>&1");
  r.version = regex_extract(out, R"(([0-9]+\.[0-9]+\.[0-9]+))");
  if (r.version.empty()) r.version = "found";
  r.severity = Severity::Ok;
  return r;
}

// Re-extract clang-cl target string from a --version run (kept separate to
// avoid stashing non-schema fields on DepResult).
std::string clang_cl_target_of(const DepResult& d) {
  if (d.path.empty()) return "";
  std::string out = run_capture(quote(d.path) + " --version");
  return regex_extract(out, R"(Target:\s+(\S+))");
}

const char* severity_tag(Severity s) {
  switch (s) {
    case Severity::Ok:       return "[OK]";
    case Severity::Info:     return "[INFO]";
    case Severity::Soft:     return "[SOFT]";
    case Severity::Blocking: return "[BLOCK]";
  }
  return "[?]";
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

DepCheckReport check_dependencies(const DepCheckOptions& opts) {
  DepCheckReport report;
  ToolchainInfo& tc = report.toolchain;

  fs::path vswhere = opts.vswhere_exe ? *opts.vswhere_exe
                                      : default_vswhere_path();

  // Compilers / build tools.
  auto r_cmake   = probe_cmake(opts.app_dir);
  auto r_ninja   = probe_ninja(opts.app_dir);
  auto r_clang   = probe_clang_cl();
  auto r_msvc    = probe_msvc(vswhere);
  auto r_winsdk  = probe_windows_sdk(vswhere);
  auto r_sdk     = probe_sdk_prebuilt(opts.sdk_root, opts.app_dir);
  bool sdk_source_is_git = false;
  auto r_sdk_src = probe_sdk_source(opts.sdk_source_dir, sdk_source_is_git);
  auto r_git     = probe_git();
  DepResult r_xiso;
  if (!opts.skip_extract_xiso) r_xiso = probe_extract_xiso(opts.app_dir);

  // Populate ToolchainInfo.
  tc.cmake_exe       = r_cmake.path;
  tc.cmake_version   = r_cmake.version;
  tc.ninja_exe       = r_ninja.path;
  tc.ninja_version   = r_ninja.version;
  tc.clang_cl_exe    = r_clang.path;
  tc.clang_cl_version= r_clang.version;
  tc.clang_cl_target = clang_cl_target_of(r_clang);
  if (r_msvc.severity == Severity::Ok) {
    tc.vs_install_dir        = r_msvc.path.parent_path().parent_path()
                                   .parent_path();  // .../VC/Auxiliary/Build → VS root
    tc.vcvarsall_bat         = r_msvc.path;
    tc.msvc_toolset_version  = r_msvc.version;
  }
  tc.windows_sdk_root    = r_winsdk.path;
  tc.windows_sdk_version = r_winsdk.version;
  tc.sdk_root            = r_sdk.path;
  tc.sdk_version         = r_sdk.version;
  tc.sdk_source_dir      = r_sdk_src.path;
  tc.sdk_source_is_git   = sdk_source_is_git;
  tc.git_exe             = r_git.path;
  tc.extract_xiso_exe    = r_xiso.path;

  // Assemble report rows in display order.
  report.results.push_back(r_cmake);
  report.results.push_back(r_ninja);
  report.results.push_back(r_clang);
  report.results.push_back(r_msvc);
  report.results.push_back(r_winsdk);
  report.results.push_back(r_sdk);
  report.results.push_back(r_sdk_src);
  report.results.push_back(r_git);
  if (!opts.skip_extract_xiso) report.results.push_back(r_xiso);

  // Severity rollup.
  report.blocking_ok = true;
  report.soft_ok = true;
  for (const auto& d : report.results) {
    if (d.severity == Severity::Blocking) report.blocking_ok = false;
    if (d.severity == Severity::Soft)     report.soft_ok = false;
  }
  tc.blocking_ok = report.blocking_ok;
  return report;
}

std::string format_report_text(const DepCheckReport& report) {
  std::ostringstream os;
  os << "Prerequisite check\n";
  os << "==================\n\n";
  // Column widths.
  constexpr int kNameW = 26;
  constexpr int kVerW  = 18;
  auto pad = [](const std::string& s, int w) {
    std::string out = s;
    if ((int)out.size() > w) out.resize(w);
    while ((int)out.size() < w) out.push_back(' ');
    return out;
  };
  os << pad("Tool", kNameW) << pad("Status", 8) << pad("Version", kVerW)
     << "Path\n";
  os << std::string(kNameW + 8 + kVerW + 4, '-') << "\n";
  for (const auto& d : report.results) {
    os << pad(d.display_name, kNameW)
       << pad(severity_tag(d.severity), 8)
       << pad(d.version.empty() ? "(missing)" : d.version, kVerW)
       << (d.path.empty() ? std::string("(not found)") : d.path.string())
       << "\n";
    if (d.warning.present) {
      os << "    ! " << d.warning.message << "\n";
    }
    if (d.severity != Severity::Ok && !d.remediation.empty()) {
      os << "    -> " << d.remediation << "\n";
    }
  }
  os << "\n";
  if (report.blocking_ok) {
    os << "All blocking prerequisites satisfied.";
    if (!report.soft_ok) os << " (some optional tools missing — see [SOFT] above)";
    os << "\n";
  } else {
    os << "BLOCKING prerequisites missing — resolve the [BLOCK] rows above "
          "before continuing.\n";
  }
  return os.str();
}

namespace {

// Minimal JSON string escape (no external dep).
std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out.push_back(c);
    }
  }
  return out;
}

void emit_json_field(std::ostringstream& os, const char* key,
                     const std::string& val, bool last = false) {
  os << "    \"" << key << "\": \"" << json_escape(val) << "\"";
  if (!last) os << ",";
  os << "\n";
}

}  // namespace

std::string format_report_json(const DepCheckReport& report) {
  std::ostringstream os;
  os << "{\n";
  os << "  \"blocking_ok\": " << (report.blocking_ok ? "true" : "false")
     << ",\n";
  os << "  \"soft_ok\": " << (report.soft_ok ? "true" : "false") << ",\n";
  os << "  \"toolchain\": {\n";
  const ToolchainInfo& tc = report.toolchain;
  emit_json_field(os, "cmake_version", tc.cmake_version);
  emit_json_field(os, "cmake_exe", tc.cmake_exe.string());
  emit_json_field(os, "ninja_version", tc.ninja_version);
  emit_json_field(os, "ninja_exe", tc.ninja_exe.string());
  emit_json_field(os, "clang_cl_version", tc.clang_cl_version);
  emit_json_field(os, "clang_cl_target", tc.clang_cl_target);
  emit_json_field(os, "clang_cl_exe", tc.clang_cl_exe.string());
  emit_json_field(os, "msvc_toolset_version", tc.msvc_toolset_version);
  emit_json_field(os, "vcvarsall_bat", tc.vcvarsall_bat.string());
  emit_json_field(os, "vs_install_dir", tc.vs_install_dir.string());
  emit_json_field(os, "windows_sdk_version", tc.windows_sdk_version);
  emit_json_field(os, "windows_sdk_root", tc.windows_sdk_root.string());
  emit_json_field(os, "sdk_version", tc.sdk_version);
  emit_json_field(os, "sdk_root", tc.sdk_root.string());
  emit_json_field(os, "sdk_source_dir", tc.sdk_source_dir.string());
  os << "    \"sdk_source_is_git\": "
     << (tc.sdk_source_is_git ? "true" : "false") << ",\n";
  emit_json_field(os, "git_exe", tc.git_exe.string(), true);
  os << "  },\n";
  os << "  \"dependencies\": [\n";
  for (size_t i = 0; i < report.results.size(); ++i) {
    const DepResult& d = report.results[i];
    os << "    {\n";
    os << "      \"id\": \"" << json_escape(d.id) << "\",\n";
    os << "      \"name\": \"" << json_escape(d.display_name) << "\",\n";
    const char* sev = "ok";
    switch (d.severity) {
      case Severity::Ok: sev = "ok"; break;
      case Severity::Info: sev = "info"; break;
      case Severity::Soft: sev = "soft"; break;
      case Severity::Blocking: sev = "blocking"; break;
    }
    os << "      \"severity\": \"" << sev << "\",\n";
    os << "      \"version\": \"" << json_escape(d.version) << "\",\n";
    os << "      \"path\": \"" << json_escape(d.path.string()) << "\",\n";
    os << "      \"remediation\": \"" << json_escape(d.remediation) << "\",\n";
    os << "      \"warning\": "
       << (d.warning.present
               ? "\"" + json_escape(d.warning.message) + "\""
               : "null");
    os << "\n    }";
    if (i + 1 < report.results.size()) os << ",";
    os << "\n";
  }
  os << "  ]\n";
  os << "}\n";
  return os.str();
}

}  // namespace recomp::deps
