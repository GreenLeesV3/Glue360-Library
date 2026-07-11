# Spider-Man 3 Recompiler — Architecture Design Document

**Status:** Design (brainstorm output, pre-implementation)
**Date:** 2026-07-07
**Scope:** Internal architecture for a desktop application that automates the
manual ISO → recompile → patch → build → deploy pipeline. Grounded in the
verified manual pipeline artifacts (`build.bat`, `spiderman3_manifest.toml`,
`spiderman3_app.h`, `xmp_bypass.cpp`, `rebuild_runtime_lto.bat`, SDK
`.gitmodules`, `DOCS/RUNTIME_FIXES.md`, `DOCS/BUILD_GUIDE.md`).

> This is an **internal** design doc. The user-facing spec (`DocSpec`) covers
> user-visible behavior; this doc covers the internal contract between modules.

---

## 0. Executive Summary

The application is a **hybrid GUI/CLI pipeline orchestrator** built in **C++**
with a **Qt6 (QML)** frontend. A single orchestrator core drives seven
sequential pipeline stages, each backed by a swappable module. Game-specific
knowledge (cvars, hooks, runtime patches, TOML template) is externalized into a
**game profile** (TOML + patch files) so Spider-Man 3 is one profile and other
Xbox 360 titles are additional profiles — not code changes.

Three guiding principles:

1. **Automate the manual pipeline exactly as performed** — do not invent a new
   process. The app wraps `rexglue.exe`, `cmake`, `ninja`, `clang-cl`, and file
   ops; it does not reimplement codegen or the runtime.
2. **Patches are data, not code** — cvar patches are declarative TOML; source
   patches are templated files copied into `src/`; runtime patches are
   flag-guarded overlay headers applied to the SDK source tree. Adding a game =
   adding a profile.
3. **Resumable, observable, recoverable** — every stage writes a state
   checkpoint; long-running stages stream progress; any failure can be retried
   from the last successful stage without redoing the whole pipeline.

**MVP → Ideal ladder** is defined in §8.

---

## 1. Application Architecture

### 1.1 GUI vs CLI vs Hybrid — Decision: Hybrid

| Option | Verdict | Rationale |
|--------|---------|-----------|
| Pure CLI + batch script | Rejected as sole form | The manual pipeline *is* already a batch script (`build.bat`). Wrapping it in another script adds little value and hides the 7-step structure from non-expert users. Keep CLI as a *mode*. |
| Pure GUI (Qt/ImGui/Win32) | Rejected as sole form | A GUI-only tool can't be scripted, CI'd, or chained. The pipeline is inherently scriptable. |
| **Hybrid (CLI core + GUI shell)** | **Chosen** | One orchestrator core, two frontends. GUI for end-users; `--headless` for power users, CI, and re-automation. |

### 1.2 Technology Stack

| Layer | Choice | Why |
|-------|--------|-----|
| **Language** | C++20/23 | The pipeline is C++-centric (CMake, clang-cl, C++23 codegen). C++ gives zero-friction FFI to SDK headers, native file/process ops, and the SDK itself is C++. Avoids a managed/native boundary. |
| **GUI framework** | **Qt6 + QML** | Mature, cross-platform, first-class process I/O (`QProcess`), signals/slots for progress, QML for a fluid multi-step wizard. ImGui is great for tools but weak for wizards/file pickers; Win32 is too low-level; a web UI (Tauri/Electron) adds a runtime + JS boundary for no gain. |
| **Build system (for the app itself)** | CMake + Ninja + clang-cl/MSVC | Same toolchain as the target — dogfooding reduces the dependency surface. |
| **Config format** | TOML (tomlplusplus, already an SDK dep) | Consistent with `spiderman3_manifest.toml`, `spiderman3.toml`. One parser, one mental model. |
| **Logging** | spdlog (already an SDK dep) | Consistent with the runtime's logging; structured logs for debugging. |
| **Process spawning** | `QProcess` (GUI) / `boost::process` or raw `CreateProcess` (headless core) | Captured stdout/stderr pipes → progress bus. |
| **Patch application** | Custom minimal patch engine (inja templates for game-project patches; CMake `-D` feature flags + overlay headers for SDK source patches) | Game-project patches = file copy + inja templating (SDK vendors `inja`); SDK source patches = flag-guarded code overlays, NOT diffs — see `DOCS/design_patch_system.md` (PatchSystem agent owns this). Unified diffs were rejected: fragile across SDK versions, opaque, non-composable. |
| **HTTP/SDK download** | libcurl or Qt NetworkModule | For optional SDK source fetch (see §5.3). |

### 1.3 Process Model

```
┌─────────────────────────────────────────────────────────────┐
│  SpiderMan3Recompiler.exe  (single process, Qt event loop)  │
│                                                             │
│  ┌──────────────┐   ┌────────────────────────────────────┐  │
│  │  QML Frontend │──▶│  Orchestrator Core (C++)           │  │
│  │  (wizard UI)  │   │  - PipelineState (checkpointed)    │  │
│  │  - progress   │◀──│  - StageRunner: runs stages serial │  │
│  │  - log view   │   │  - ProgressBus: signals/slots      │  │
│  │  - retry/cancel│  │  - ModuleRegistry: resolved deps   │  │
│  └──────────────┘   └────────────┬───────────────────────┘  │
│                                  │ spawns                    │
│              ┌───────────────────┼───────────────────┐      │
│              ▼                   ▼                   ▼      │
│        ┌──────────┐        ┌──────────┐        ┌──────────┐ │
│        │rexglue.exe│       │cmake+ninja│       │(file ops)│ │
│        │(codegen)  │       │(builds)   │       │(copy/lnk)│ │
│        └──────────┘        └──────────┘        └──────────┘ │
│              child processes, captured pipes                  │
└─────────────────────────────────────────────────────────────┘
```

- **One process** for the app; child processes for `rexglue.exe`, `cmake`,
  `ninja`. All child stdout/stderr is captured line-by-line on a worker thread
  and forwarded to the ProgressBus → QML.
- **No background daemon.** The pipeline is a single user-initiated run. Closing
  the app cancels in-flight child processes (graceful → SIGTERM → force after
  timeout).
- **Headless mode:** same core, no Qt event loop; a CLI driver reads a run spec
  TOML and writes progress to stdout as structured NDJSON.

### 1.4 Plugin / Extension Points

1. **GameProfile** (§6.2) — the primary extension. New game = new profile
   directory. No recompilation of the app.
2. **IsoExtractor** — interface; default impl shells to `extract-xiso`. A
   future impl could use the SDK's built-in extraction if/when exposed.
3. **PatchSource** — interface for where patches come from: bundled (shipped
   with app), local path, or remote (git URL). Enables community patch packs.
4. **RuntimeBuildStrategy** — interface: "use prebuilt SDK DLL" (fast path,
   no runtime patches) vs "build custom DLL from SDK source" (full path, with
   ThinLTO/save-fix patches). Chosen per-profile based on whether the profile
   declares runtime patches.

> **Patch format clarification (cross-ref `DOCS/design_patch_system.md`):**
> SDK source patches (stratum 3, §6.3) use **flag-guarded overlay headers +
> inline `#ifdef` guards**, applied to a single shared, git-init'd SDK source
> tree. The format is **NOT** unified diff. This decision is owned by the
> PatchSystem design (`DOCS/design_patch_system.md` §0–§4); this doc
> references it and aligns its stratum-3 description accordingly. The two
> patch categories — **game-project patches** (templates, post-codegen, per
> game) and **SDK source patches** (overlay headers + CMake `-D` flags,
> persistent shared tree, per-game runtime build matrix) — cannot share a
> format and this doc treats them as distinct strata.

---

## 2. Module Breakdown

The orchestrator core is composed of seven stage modules, each implementing a
common `IStage` interface:

```cpp
struct IStage {
  std::string id;                     // "iso_extract", "rexglue_init", ...
  CheckResult check_prereqs(Context&); // deps present? inputs ready?
  StageResult run(Context&, ProgressSink&);  // do the work
  bool is_complete(Context&);         // idempotent completion check
};
```

`Context` is the shared pipeline state object (§4.1). Stages are registered in
a fixed order but each can be skipped (if already complete) or retried
independently.

### 2.1 Module List

| # | Module | ID | Inputs | Outputs | Wraps/Does |
|---|--------|----|--------|---------|-----------|
| 1 | **IsoExtractor** | `iso_extract` | user ISO path | `extracted/` dir with `default.xex` | shells to `extract-xiso` (XDVDFS parser); verifies `default.xex` exists |
| 2 | **RexglueInit** | `rexglue_init` | `extracted/default.xex`, project name | project dir: `CMakeLists.txt`, `CMakePresets.json`, `<name>_manifest.toml`, `generated/rexglue.cmake` | runs `rexglue init --name <name> --xex <path>` |
| 3 | **RexglueCodegen** | `rexglue_codegen` | manifest TOML | `generated/default/*.cpp` (92 shards), `sources.cmake`, `*_init.{cpp,h}`, `*_register.cpp` | runs `rexglue codegen <manifest>`; verifies shard count |
| 4 | **PatchApplier** | `apply_patches` | generated project + GameProfile | modified `src/*.cpp/.h`, modified `CMakeLists.txt`, (optional) patched SDK source tree | applies 3 patch strata (§6.3) |
| 5 | **RuntimeBuilder** | `build_runtime` | (patched) SDK source tree, toolchain | custom `rexruntime.dll` (+ `TracyClient.dll`) | runs `rebuild_runtime_lto.bat` equivalent; **skippable** if profile has no runtime patches (use prebuilt DLL) |
| 6 | **GameBuilder** | `build_game` | patched project, toolchain, runtime DLL | `<name>.exe` (118MB) | runs `build.bat` equivalent: vcvarsall + cmake configure + ninja build |
| 7 | **Deployer** | `deploy` | built exe + DLLs + GameProfile template | portable standalone folder: exe, DLLs, `<name>.toml`, `game/` junction, `user_data/` | copies artifacts, renders TOML template, creates junction, seeds dirs |

### 2.2 Module Boundary Rules

- Each module owns its on-disk outputs and cleans them on retry.
- Modules never mutate another module's outputs directly; they declare inputs
  and the orchestrator verifies they exist before running.
- A module's `is_complete()` makes it idempotent: re-running a completed stage
  is a no-op (or a verify-only pass), enabling **resume** (§4.3).
- Filesystem paths are resolved relative to a `Workspace` root configured by the
  user (not hardcoded `C:\tmp\...` — the manual paths are dev-machine
  conveniences, not portable defaults).

### 2.3 Cross-Cutting Modules (not stages)

| Module | Responsibility |
|--------|---------------|
| **DependencyChecker** (§5) | Detects CMake, LLVM/clang-cl, MSVC, Ninja, Windows SDK, RexGlue SDK; reports versions + missing items |
| **GameProfileLoader** (§6.2) | Loads + validates a game profile (TOML + patch files) |
| **PipelineStateStore** (§4.3) | Persists stage completion + artifact paths to `<workspace>/.recomp/state.json` |
| **ProgressBus** | Thread-safe signal/slot channel: stages emit progress %, log lines, errors → QML subscribers |
| **ToolchainEnv** | Sets up the MSVC env (vcvarsall) + PATH for child processes; encapsulates the `build_env.bat` logic |
| **Logger** | spdlog sink; mirrors to on-disk `<workspace>/.recomp/logs/run-<ts>.log` |

---

## 3. Data Flow Between Modules

### 3.1 End-to-End Data Flow (text diagram)

```
USER                     Workspace (on disk)                    Output
─────                    ─────────────────────                   ──────

[ISO file]──┐
            │  (1) IsoExtractor
            ▼
      ┌─────────────────┐
      │ extracted/      │── default.xex ──┐
      │  default.xex    │                 │
      │  *.toc, packs/  │                 │
      └─────────────────┘                 │
                                          │  (2) RexglueInit
                                          ▼
                                   ┌──────────────────┐
                                   │ <project>/       │
                                   │  CMakeLists.txt  │
                                   │  <name>_manifest │── manifest TOML ──┐
                                   │  generated/      │                  │
                                   │   rexglue.cmake  │                  │
                                   └──────────────────┘                  │
                                                                         │ (3) RexglueCodegen
                                                                         ▼
                                  ┌───────────────────────────────┐
                                  │ <project>/generated/default/  │
                                  │  sources.cmake                │
                                  │  <name>_recomp.{0..91}.cpp    │── 92 shards ──┐
                                  │  <name>_init.{cpp,h}          │               │
                                  │  <name>_register.cpp          │               │
                                  └───────────────────────────────┘               │
 GameProfile (TOML + patches)                                                     │
  - [cvars] table                                                                   │
  - src/*.inja          ──────────────┐                                           │
  - game-runtime/<flag>/ overlays      │ (4) PatchApplier                          │
  - deploy/*.inja                      ▼                                           │
                            ──────┐  ┌──────────────────────────────────┐            │
                            │  │ <project>/src/                   │            │
                            │  │  <name>_app.h      (templated)    │            │
                            │  │  xmp_bypass.cpp    (from profile) │            │
                            │  │  roundevenf.cpp    (from profile) │            │
                            │  │ CMakeLists.txt     (patched)      │            │
                            │  └──────────────────────────────────┘            │
                            │                                                    │
                            │  (4b) runtime patches → SDK source tree            │
                            ▼                                                    │
                  ┌─────────────────────────────┐                                │
                  │ sdk-src/ (patched)          │                                │
                  │  src/xam_ui.cpp  (patched)  │                                │
                  │  src/xam_enum.cpp(patched)  │                                │
                  │  ...                        │                                │
                  └─────────────┬───────────────┘                                │
                                │ (5) RuntimeBuilder                             │
                                ▼                                                │
                  ┌─────────────────────────────┐                                │
                  │ custom rexruntime.dll       │──── rexruntime.dll ──┐         │
                  │ (+ TracyClient.dll)         │---- TracyClient.dll──┤         │
                  └─────────────────────────────┘                     │         │
                                                                        │         │
                  Toolchain (vcvarsall + clang-cl + cmake + ninja)      │         │
                                                                        │ (6) GameBuilder
                                                                        ▼         │
                                                  ┌──────────────────────────────┐│
                                                  │ <project>/out/build/         ││
                                                  │   clang-release/<name>.exe   ││── exe ──┐
                                                  └──────────────────────────────┘│        │
                                                                                   │        │ (7) Deployer
                                                                                   │        ▼
                                                                                   │  ┌─────────────────────────┐
                                                                                   └─▶│ <deploy_dir>/           │
                                                                                      │  <name>.exe             │
                                                                                      │  rexruntime.dll         │
                                                                                      │  TracyClient.dll        │
                                                                                      │  <name>.toml (rendered) │
                                                                                      │  game/ → junction       │
                                                                                      │  user_data/             │
                                                                                      └─────────────────────────┘
                                                                                                  │
                                                                                                  ▼
                                                                                          [Playable game]
```

### 3.2 Data Contracts (the `Context` object)

All inter-module data flows through a single `Context` struct held by the
orchestrator and persisted as JSON:

```cpp
struct PipelineContext {
  // user inputs
  fs::path iso_path;
  std::string game_profile_id;     // "spiderman3", or custom
  fs::path workspace_root;         // all outputs live under here

  // resolved by stages
  fs::path extracted_dir;          // after iso_extract
  fs::path project_dir;            // after rexglue_init
  fs::path manifest_path;          // after rexglue_init
  fs::path generated_dir;          // after rexglue_codegen
  int      generated_shard_count;  // after rexglue_codegen (e.g. 92)
  fs::path sdk_source_dir;         // resolved for runtime build
  fs::path custom_runtime_dll;     // after build_runtime (empty if skipped)
  fs::path built_exe;              // after build_game
  fs::path deploy_dir;             // after deploy

  // toolchain (resolved by DependencyChecker)
  ToolchainInfo toolchain;
  fs::path sdk_root;               // prebuilt SDK (CMAKE_PREFIX_PATH)

  // profile (loaded by GameProfileLoader)
  GameProfile profile;
};
```

Each stage reads what it needs from `Context` and writes its outputs back. The
state store (§4.3) persists `Context` to disk so a resumed run reconstructs it.

### 3.3 Key Data Transforms

| Stage | Transform | Mechanism |
|-------|-----------|-----------|
| iso_extract | XDVDFS ISO → file tree | external `extract-xiso` |
| rexglue_init | XEX + name → project skeleton | external `rexglue init` |
| rexglue_codegen | XEX PPC → 92 C++ shards | external `rexglue codegen` |
| apply_patches (cvar stratum) | profile cvars → `OnPreSetup` body | render `app.h.inja` template with cvar list |
| apply_patches (source stratum) | profile templates → `src/*.cpp/.h` | copy + inja render with profile vars |
| apply_patches (runtime stratum) | profile flags → flag-guarded SDK source overlays | inline `#ifdef FLAG` guards + overlay headers `#include`'d behind `#ifdef`; CMake `-D` defines per profile (see `DOCS/design_patch_system.md`) |
| build_runtime | patched SDK src → custom DLL | cmake + ninja on SDK source tree |
| build_game | patched project + toolchain → exe | cmake + ninja on project dir |
| deploy | exe + DLLs + template → portable folder | file copy + inja render of `deploy.toml.inja` + junction creation |

---

## 4. State Management

### 4.1 Pipeline State Model

The pipeline is a **linear state machine** with 7 stages. State transitions:

```
[INIT] →iso_extract→ [EXTRACTED] →rexglue_init→ [INITED]
  →rexglue_codegen→ [CODEGENED] →apply_patches→ [PATCHED]
  →build_runtime→ [RT_BUILT] (or [RT_SKIPPED]) →build_game→ [BUILT]
  →deploy→ [DEPLOYED] → [DONE]

any stage ──fail──→ [FAILED:<stage>] ──retry──→ re-run stage
any stage ──cancel──→ [CANCELLED] ──resume──→ re-run from last incomplete
```

Each state is persisted to `<workspace>/.recomp/state.json`:

```json
{
  "version": 1,
  "game_profile": "spiderman3",
  "iso_path": "D:/Games/Spider-Man 3.iso",
  "iso_sha256": "abc123...",
  "started_at": "2026-07-07T12:00:00Z",
  "current_stage": "build_game",
  "stages": {
    "iso_extract":     {"status": "complete", "artifacts": ["extracted/default.xex"], "finished_at": "..."},
    "rexglue_init":    {"status": "complete", "artifacts": ["spiderman3/CMakeLists.txt"], ...},
    "rexglue_codegen": {"status": "complete", "shard_count": 92, ...},
    "apply_patches":   {"status": "complete", "patches_applied": ["cvars","src","runtime"], ...},
    "build_runtime":   {"status": "skipped", "reason": "profile has no runtime patches"},
    "build_game":      {"status": "in_progress", "progress": 0.62, ...},
    "deploy":          {"status": "pending"}
  },
  "context": { /* full PipelineContext, see §3.2 */ }
}
```

### 4.2 Progress Tracking

- **Per-stage progress:** stages emit progress in `[0.0, 1.0]` via the
  `ProgressSink` interface. For shell-out stages (codegen, build), progress is
  estimated from output line counts / known totals (e.g., 92 shards → %
  = compiled_shards/92) or from cmake/ninja progress lines (`[42/94]`).
- **Global progress:** weighted average across stages; weights are configurable
  (codegen and game build dominate — see §4.4).
- **Log streaming:** all child stdout/stderr lines are timestamped, classified
  (info/warn/error via regex on known patterns), and forwarded to the GUI log
  view and the on-disk run log.
- **Cancellation:** `ProgressSink` carries a cancellation token; stages poll it
  and terminate child processes gracefully (close stdin → wait → kill).

### 4.3 Error Recovery & Resumable Builds

Three recovery primitives:

1. **Retry stage** — re-run the current failed stage after the user fixes the
   issue (e.g., installs a missing dep). The stage cleans its own outputs first.
2. **Resume from checkpoint** — on app restart or explicit "Resume", the
   orchestrator loads `state.json`, finds the last incomplete stage, and runs
   forward from there. Completed stages are verified via `is_complete()` (e.g.,
   check `extracted/default.xex` exists, check 92 shards exist, check exe
   exists) and re-run only if verification fails.
3. **Reset from stage** — discard a stage's outputs and re-run it (and all
   downstream stages). Useful when a profile change should propagate.

**Idempotency contracts** (each stage's `is_complete`):
- `iso_extract`: `extracted/default.xex` exists AND ISO SHA matches state.
- `rexglue_init`: `project_dir/CMakeLists.txt` + manifest exist.
- `rexglue_codegen`: `sources.cmake` lists N files AND all N exist on disk.
- `apply_patches`: a `.applied` marker file per stratum with patch hashes.
- `build_runtime`: `custom_runtime_dll` exists AND newer than its source inputs.
- `build_game`: `built_exe` exists AND newer than all project sources.
- `deploy`: `deploy_dir/<name>.exe` + DLLs + toml all exist.

### 4.4 Stage Cost Estimates (for progress weighting)

| Stage | Typical time | Weight | Notes |
|-------|-------------|--------|-------|
| iso_extract | 1–3 min | 5% | I/O bound |
| rexglue_init | <10s | 1% | trivial |
| rexglue_codegen | 2–8 min | 15% | PPC disasm + C++ emit; 43,676 funcs |
| apply_patches | <5s | 1% | file copy + template render + diff apply |
| build_runtime | 10–20 min | 30% | full SDK rebuild with ThinLTO; **skipped** if no runtime patches |
| build_game | 5–15 min | 40% | 92 shards + link → 118MB exe |
| deploy | <30s | 2% | copy + junction |

Weights shift when runtime build is skipped (redistribute its 30% across
codegen/build_game).

---

## 5. Dependency Management

### 5.1 Required Dependencies (the toolchain)

Sourced from `DOCS/BUILD_GUIDE.md` §1 verified versions:

| Dependency | Min version | Detection method | Used by stage(s) |
|-----------|-------------|------------------|------------------|
| **LLVM / clang-cl** | 22.x | `where clang-cl` + `clang-cl --version`; check `Target: x86_64-pc-windows-msvc` | game build, runtime build |
| **MSVC (VS 2022)** | 14.4x | look for `vcvarsall.bat` under `VS install dir\VC\Auxiliary\Build\`; parse `cl /?` version | game build, runtime build (link.exe, CRT, Win SDK) |
| **CMake** | 3.25+ (4.x ok) | `cmake --version` | init, codegen (via cmake target), game build, runtime build |
| **Ninja** | 1.13+ | `where ninja` + `ninja --version` | game build, runtime build |
| **Windows SDK** | (via VS) | present after vcvarsall; check `%WindowsSdkDir%` | game build, runtime build |
| **RexGlue SDK (prebuilt)** | 0.8.0 | check `share/rexglue/` CMake package under SDK root; verify `bin/rexglue.exe` + `bin/rexruntime.dll` | init, codegen, game build (CMAKE_PREFIX_PATH) |
| **RexGlue SDK source** | 0.8.0 | check `.gitmodules` + `CMakeLists.txt` at SDK source root; verify submodules checked out (`thirdparty/` non-empty) | runtime build (only if profile has runtime patches) |
| **extract-xiso** | any | `where extract-xiso` (or bundled) | iso_extract |

### 5.2 DependencyChecker Module

Runs at pipeline start (and on-demand from GUI "Check Dependencies" button).

```
DependencyChecker
  ├─ probes each dep via: registry lookup, PATH search, version flag parse
  ├─ for MSVC: enumerate installed VS instances via `vswhere.exe`
  ├─ for RexGlue SDK: user-configured path OR auto-discover
  ├─ for SDK source: user-configured path; verify submodules with `git submodule status`
  └─ produces DependencyReport:
       { dep: {found, version, path, status: OK|WRONG_VERSION|MISSING|BROKEN} }
```

GUI renders the report as a checklist with "Install" hints (link to VS
Installer, LLVM download, etc.) and blocks pipeline start only on hard
missing deps (clang-cl, MSVC, CMake, Ninja, RexGlue SDK). SDK source is
**soft** — only required if the profile has runtime patches; otherwise the
prebuilt DLL path is used.

### 5.3 SDK Source Tree & Submodules (the hard part)

The custom runtime build requires the SDK source tree with **16 git
submodules** (per `.gitmodules`: libmspack, glslang, FFmpeg, tomlplusplus,
simde, xxHash, spdlog, fmt, catch2, snappy, utfcpp, volk, vulkan-headers,
vulkan-memory-allocator, imgui, spirv-tools, spirv-headers, cli11, o1heap,
sdl3, inja, tracy). Three strategies, in order of preference:

1. **Bundled SDK source snapshot (preferred for distribution).** The app
   ships a prepared SDK source archive (`rexglue-sdk-0.8.0-src.tar.zst`,
   ~few hundred MB after submodule init) with the 3 known build fixes
   (xxHash CMake path, libmspack symlinks→copies, CMake 4.x module scan)
   **pre-applied**. On first runtime build, the app extracts this to
   `<workspace>/.recomp/sdk-src/`. Users don't need git. **Trade-off:**
   larger download; fixes are frozen at app release time.
2. **User-provided SDK source path.** User points the app at their existing
   `rexglue-sdk-0.8.0 Source code` checkout. The app runs `git submodule
   update --init --recursive` if needed, then applies the 3 build-fix
   patches (from `patches/sdk-build-fix/` in the app's profile) via the
   overlay-header + `#ifdef` mechanism (see `DOCS/design_patch_system.md`),
   NOT via `git apply` — the SDK source ships as a tarball, not a git repo.
3. **Clone from remote (future).** App clones the SDK source repo + submodules
   from upstream. Requires network + git. Not MVP.

### 5.3.1 git-init for Version Anchoring (not patch application)

Regardless of which strategy above supplied the SDK source tree, the app
**git-initializes** the extracted tree on first use:

```bash
cd <workspace>/.recomp/sdk-src/
git init
git add -A
git commit -m "sdk-0.8.0 pristine" 
git tag sdk-0.8.0-pristine
```

This is **not** for `git apply`. Patch application uses the flag-guarded
overlay-header mechanism (§6.3 Stratum 3, `DOCS/design_patch_system.md`).
git-init serves two purposes:

- **Version anchoring** — the `sdk-0.8.0-pristine` tag is a stable reference
  point. The app records the tag in `state.json` so a resumed run can verify
  the SDK source hasn't drifted (e.g., `git diff sdk-0.8.0-pristine -- .`).
  Submodule commits are also pinned and verified (§5.3.2).
- **Free reversibility** — to re-apply patches cleanly (e.g., after a profile
  flag change), the app does `git checkout sdk-0.8.0-pristine -- .` to restore
  the pristine tree, then re-applies overlays. This avoids accumulating
  half-applied state across runs.

### 5.3.2 Submodule Integrity

The app records expected submodule commits in the profile and verifies them
after init. A mismatch warns the user (submodule versions affect runtime ABI).
For the bundled-snapshot strategy, submodules are pre-initialized at known
commits and frozen in the archive, so this check is a tamper guard.

### 5.4 The 3 SDK Source Build Fixes

These are documented in `BUILD_GUIDE.md` §7b and must be applied before the
first runtime configure. Per `DOCS/design_patch_system.md` they belong to the
**`sdk-build-fix`** category — general patches required for the SDK source to
build at all on the current toolchain, shared across all games. They are
**not** game-specific and **not** diff-based:

```
patches/sdk-build-fix/           # shared across all profiles (general)
  01-xxhash-cmake-path/          # one fix per directory
    overlay/                     # overlay headers + #ifdef'd source snippets
    flag.toml                    # flag name, files touched, apply order
  02-libmspack-symlinks-to-copies/
  03-cmake4-module-scan/
```

Applied by the `apply_patches` stage's runtime stratum (before `build_runtime`)
via the same overlay-header mechanism as game-runtime patches. The app applies
all `sdk-build-fix` patches first (always-on), then the profile's `game-runtime`
patches (per-game flags). Idempotency: the `sdk-0.8.0-pristine` tag + a per-fix
`.applied` marker (flag name + hash) detect re-application.

---

## 6. Configuration Management

### 6.1 User Settings (app-level)

`<user_config>/SpiderMan3Recompiler/settings.toml` (e.g., `%APPDATA%/...`):

```toml
[paths]
workspace_root = "D:/RecompWorkspaces"      # default for new runs
sdk_root       = "C:/tmp/Workspace 1/RexGlue360Recomp"  # prebuilt SDK
sdk_source_root = "C:/tmp/Workspace 1/rexglue-sdk-0.8.0 Source code"

[toolchain]
# auto-detected if empty; user can override
vcvarsall = "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvarsall.bat"
llvm_bin  = "C:/Program Files/LLVM/bin"
cmake     = ""   # from PATH if empty
ninja     = ""   # from PATH if empty

[ui]
theme = "dark"
log_level = "info"
open_deploy_folder_on_done = true

[build]
parallel_jobs = 0   # 0 = auto (CPU count)
runtime_lto = "thin"  # "thin" | "off" | "full"
profile_guest_functions = false
```

### 6.2 Game Profile (the key abstraction for reusability)

A game profile is a **directory** (shipped with the app or user-added) that
contains all game-specific knowledge. Spider-Man 3 is one profile; another
Xbox 360 game is another profile. The app loads profiles from
`<app_dir>/profiles/<id>/` and `<user_config>/profiles/<id>/`.

```
profiles/spiderman3/
  profile.toml          # metadata + cvars + sources + runtime flags (§6.2)
  src/                  # game-project source patches (inja templates)
    main.cpp.inja
    spiderman3_app.h.inja
    xmp_bypass.cpp.inja
    roundevenf.cpp.inja
    particle_perf.cpp.inja       # placeholder, in CMakeLists (see §6.3 S2)
    dynamic_resolution.cpp.inja  # DRS hook, optional (see §6.3 S2 orphan note)
  cmake/
    CMakeLists.txt.fragment      # SPIDERMAN3_SOURCES + options, merged into generated CMakeLists
  patches/
    sdk-build-fix/               # general, always-on (shared, §5.4): xxHash/libmspack/CMake4
    game-runtime/                # game-specific runtime flags + overlay headers
      rex_game_save_system_fix/  # one directory per flag
        overlay/                 # overlay headers (.h) + #ifdef'd source snippets
        flag.toml                # flag name, files touched, apply order
      rex_rov_barrier_skip/
      rex_queueframes_2/
      rex_xam_dispatch_headless/
      rex_xam_enum_io_pending/
      rex_xenumerator_writeitems_zero/
      rex_thinlto/               # general-runtime (always-on), lives here or in sdk-build-fix
  deploy/
    spiderman3.toml.inja         # runtime TOML template
  manifest-overrides.toml        # optional: entrypoint hints, game_root, etc.
```

**`profile.toml`** (the declarative heart):

```toml
[profile]
id = "spiderman3"
name = "Spider-Man 3 (Xbox 360)"
sdk_version = "0.8.0"
xex_entrypoint = "default.xex"

[build]
project_name = "spiderman3"
[patches.runtime]
# if no flags → build_runtime stage is SKIPPED, prebuilt DLL used (fast path)
requires_sdk_source = true
# CMake -D defines passed to the runtime build; each maps to an overlay dir
# under patches/game-runtime/<flag_name>/ (see DOCS/design_patch_system.md)
flags = [
  "REX_GAME_SAVE_SYSTEM_FIX",    # xam_ui + xam_enum + xenumerator (3 files)
  "REX_ROV_BARRIER_SKIP",        # render_target_cache.cpp EDRAM barrier skip
  "REX_QUEUEFRAMES_2",           # command_processor.h kQueueFrames 3→2
  "REX_XAM_DISPATCH_HEADLESS",   # xam_ui.cpp deferred pre/post (part of save fix)
  "REX_XAM_ENUM_IO_PENDING",     # xam_enum.cpp async returns IO_PENDING
  "REX_XENUMERATOR_WRITEITEMS_ZERO",  # xenumerator.cpp 0-items → SUCCESS
  "REX_THINLTO",                 # system CMakeLists ThinLTO (general-runtime)
]

[sources]
# Self-validating source surface (see §6.3 Stratum 2). The profile declares
# BOTH what gets copied into src/ AND what CMakeLists compiles, so the
# GameProfileLoader can cross-check them and catch silent-exclusion bugs
# like the dynamic_resolution.cpp orphan (see §6.3 S2 orphan note).
files = [
  { from = "src/main.cpp.inja",               to = "src/main.cpp" },
  { from = "src/spiderman3_app.h.inja",       to = "src/spiderman3_app.h" },
  { from = "src/xmp_bypass.cpp.inja",         to = "src/xmp_bypass.cpp" },
  { from = "src/roundevenf.cpp.inja",         to = "src/roundevenf.cpp" },
  { from = "src/particle_perf.cpp.inja",      to = "src/particle_perf.cpp" },
]
# .cpp files present in src/ but NOT compiled by CMakeLists. Declared so the
# loader knows they're intentional opt-outs, not drift. Empty = every .cpp
# in [sources].files must appear in cmakelists (the strict default).
optional = [
  "src/dynamic_resolution.cpp",  # DRS hook exists but isn't wired into the build
]
# The CMake source list the fragment contributes. GameProfileLoader asserts
# every non-optional .cpp in [sources].files appears here.
cmakelists = [
  "src/main.cpp",
  "src/roundevenf.cpp",
  "src/xmp_bypass.cpp",
  "src/particle_perf.cpp",
]

[cvars]  # rendered into spiderman3_app.h::OnPreSetup via the template
render_target_path_d3d12 = "rov"
video_mode_refresh_rate = "120.0"
gamma_render_target_as_unorm16 = "true"
snorm16_render_target_full_range = "true"
mrt_edram_used_range_clamp_to_min = "true"
readback_resolve = "fast"
anisotropic_override = "5"
swap_post_effect = "fxaa"
host_present_from_non_ui_thread = "true"
d3d12_bindless = "true"
d3d12_tiled_shared_memory = "true"
d3d12_submit_on_primary_buffer_end = "false"
d3d12_pipeline_creation_threads = "2"
readback_memexport = "true"
readback_memexport_fast = "true"
texture_cache_memory_limit_hard = "4096"
texture_cache_memory_limit_soft = "2048"
texture_cache_memory_limit_soft_lifetime = "120"
texture_cache_memory_limit_render_to_texture = "256"
use_fuzzy_alpha_epsilon = "true"
present_dither = "true"
store_shaders = "true"

[paths]
# OnConfigurePaths behavior
game_data_subdir = "game"        # look for <exe>/game/default.xex
fallback_game_data_root = ""     # empty = no hardcoded fallback (portable)
user_data_subdir = "user_data"
cache_subdir = "user_data/cache"

[deploy]
toml_template = "deploy/spiderman3.toml.inja"
# files to copy into deploy dir alongside exe
copy_dlls = ["rexruntime.dll", "TracyClient.dll"]
create_game_junction = true
create_user_data = true
```

### 6.3 Patch Application — Three Strata

The `PatchApplier` stage applies patches in three independent strata. This is
the core of the "patches are data" principle and what makes the app reusable.

#### Stratum 1: Cvar patches (declarative, no codegen)

- **Source:** `[cvars]` table in `profile.toml`.
- **Mechanism:** the `spiderman3_app.h.inja` template has an `OnPreSetup` body
  generated by iterating the cvar table:
  ```cpp
  // rendered from [cvars]
  rex::cvar::SetFlagByName("render_target_path_d3d12", "rov");
  rex::cvar::SetFlagByName("video_mode_refresh_rate", "120.0");
  // ... one line per cvar
  ```
- **Adding a cvar** = adding a line to `[cvars]`. No C++ knowledge required.
- **Idempotency:** hash of the cvar table stored in a `.applied` marker; re-render
  if changed.

#### Stratum 2: Source patches (templated files → `src/`)

- **Source:** `[sources]` table in `profile.toml` (§6.2) + `src/*.inja` files.
- **Mechanism:** each `*.inja` template is rendered with the profile's variables
  (cvar list, project name, paths) and written to `<project>/src/<target>`.
  This is how `xmp_bypass.cpp`, `spiderman3_app.h`, `main.cpp`,
  `roundevenf.cpp`, `particle_perf.cpp` are produced. The manual pipeline had
  these as hand-written files; the app templates them so a profile can
  parameterize hook lists, fallback paths, etc.
- **Why templates, not static files?** Because some values are profile-relative
  (project name in `REX_DEFINE_APP(<name>, ...)`, class name `<Name>App`, path
  fallbacks). Templates keep one source of truth in `profile.toml`.
- **The CMakeLists.txt patch:** the generated `CMakeLists.txt` lists
  `SPIDERMAN3_SOURCES`. The profile's `cmake/CMakeLists.txt.fragment` is
  merged in (source list + `target_compile_options(... /EHa)` +
  `rexglue_setup_target`). The current source of truth is **4 sources**
  (`main.cpp`, `roundevenf.cpp`, `xmp_bypass.cpp`, `particle_perf.cpp`), not
  the 3 listed in `BUILD_GUIDE.md` (which is stale — see §11 doc-drift).

- **Self-validating source surface (design lesson from the DRS orphan):**
  The manual pipeline kept "what gets copied into `src/`" and "what
  `CMakeLists.txt` compiles" as **independent** pieces of data, and that
  independence is exactly how `dynamic_resolution.cpp` got orphaned. The
  profile schema (§6.2 `[sources]`) is therefore **self-validating**:
  `GameProfileLoader` cross-checks that every `.cpp` in `[sources].files`
  appears in `[sources].cmakelists` unless it is listed in
  `[sources].optional`. This catches the silent-exclusion class of bug at
  profile-load time rather than at "why isn't my DRS hook in the build" time.
  - **Strict default:** if `[sources].optional` is empty/absent, every `.cpp`
    in `files` MUST appear in `cmakelists` — otherwise the loader errors.
  - **Declared opt-outs:** `[sources].optional` lists `.cpp` files present in
    `src/` but intentionally not compiled. The loader treats these as
    "known orphans" and surfaces them as warnings, not errors.

- **The `dynamic_resolution.cpp` orphan (confirmed against source):**
  `src/dynamic_resolution.cpp` is a **real Dynamic Resolution Scaling hook**
  (`REX_HOOK_RAW(FrameDelta_Compute)` at `sub_8284E6B8`, chain-through to
  `__imp__FrameDelta_Compute`, scales `draw_resolution_scale_x/y` between 1
  and 3, pairs with `present_effect = "cas"`). It compiles to a `.obj` but is
  **NOT** in `CMakeLists.txt`'s `SPIDERMAN3_SOURCES` — so the shipped
  `spiderman3.exe` has **no DRS**. This directly affects the 60 FPS / perf
  story (DRS trades resolution for frame-time stability) and is a genuine
  discrepancy, not a doc error. The profile declares it in
  `[sources].optional` so:
    1. The loader doesn't error on the orphan.
    2. The GUI surfaces it to the user as "DRS hook exists but is disabled —
       add to CMakeLists to enable?"
    3. A future profile revision can move it from `optional` to `cmakelists`
       to enable DRS without any other change.
  This is the canonical example of why the source stratum must reconcile
  CMakeLists sources against on-disk sources rather than treating them as
  independent.

#### Stratum 3: Runtime patches (flag-guarded overlays → SDK source tree)

- **Format (owned by `DOCS/design_patch_system.md`):** **NOT** unified diff.
  Runtime patches are **flag-guarded overlay headers + inline `#ifdef` guards**,
  applied to a single shared, git-init'd SDK source tree (§5.3.1). Unified
  diffs were rejected: (a) fragile across SDK versions — context-line changes
  break hunks; (b) opaque — overlay headers are readable C++; (c) not
  composable — two diffs on the same file need manual merge, two `#ifdef`
  flags coexist cleanly; (d) the SDK source ships as a tarball with no diff
  baseline. This doc owns the *contract* (stratum 3 exists, is flag-guarded,
  triggered by profile flags, skipped when no flags → prebuilt DLL);
  `DOCS/design_patch_system.md` owns the *application engine*.

- **Two sub-patterns** (per `DOCS/design_patch_system.md`):
  - **Single-token changes** (e.g. `xenumerator.cpp` return value,
    `kQueueFrames` 3→2): inline `#if defined(FLAG) / new_value / #else /
    old_value / #endif` directly in the SDK source file. No overlay header.
  - **Behavioral changes** (e.g. `xam_ui.cpp` pre/post callbacks,
    `xam_enum.cpp` IO_PENDING): an **overlay header** with the extracted
    patched function is `#include`'d behind `#ifdef FLAG` in the SDK source
    file; the original code is preserved in `#else`. One SDK tree, rebuilt
    per game-profile with different flag combos.

- **Source:** `[patches.runtime].flags` in `profile.toml` (§6.2). Each flag
  maps to a directory `patches/game-runtime/<flag_name>/` containing the
  overlay headers + `flag.toml` (files touched, apply order). Flags are
  passed as CMake `-D` defines to the runtime build
  (`-DREX_GAME_SAVE_SYSTEM_FIX=1`, `-DREX_ROV_BARRIER_SKIP=1`, etc.).

- **Categorization** (per `DOCS/design_patch_system.md` §0.2, 6 categories):
  - **general-runtime** (shared, always-on across all games):
    `REX_ROV_BARRIER_SKIP`, `REX_QUEUEFRAMES_2`, `REX_THINLTO`.
  - **game-runtime** (per-game, opt-in): `REX_GAME_SAVE_SYSTEM_FIX`,
    `REX_XAM_DISPATCH_HEADLESS`, `REX_XAM_ENUM_IO_PENDING`,
    `REX_XENUMERATOR_WRITEITEMS_ZERO`.
  - **sdk-build-fix** (general, always-on, §5.4): xxHash/libmspack/CMake4.
  - **game-project** (this stratum-2, templates), **game-config** (cvars/TOML),
    **game-build** (CMake injection) are the other strata.
  The app applies `sdk-build-fix` + `general-runtime` always, then the
  profile's `game-runtime` flags.

- **Skip condition:** if `[patches.runtime].flags` is empty, the
  `build_runtime` stage is **skipped entirely** and the prebuilt
  `rexruntime.dll` from the SDK `bin/` is used. This is the fast path for
  games that need no runtime changes.

- **The Spider-Man 3 runtime patches** (from `RUNTIME_FIXES.md`, now as flags):
  1. `REX_XAM_DISPATCH_HEADLESS` — `xam_ui.cpp` `xeXamDispatchHeadless`
     deferred path with XN_SYS_UI true/false pre/post. (overlay header)
  2. `REX_XAM_ENUM_IO_PENDING` — `xam_enum.cpp` `xeXamEnumerate` async
     returns `X_ERROR_IO_PENDING`. (overlay header)
  3. `REX_XENUMERATOR_WRITEITEMS_ZERO` — `xenumerator.cpp` `WriteItems`
     returns `X_ERROR_SUCCESS` for 0 items. (inline #ifdef)
  4. `REX_ROV_BARRIER_SKIP` — `render_target_cache.cpp` EDRAM barrier skip
     (skip `MarkEdramBufferModified` when draw writes no color/depth/stencil).
     (inline #ifdef, general-runtime)
  5. `REX_QUEUEFRAMES_2` — `command_processor.h` `kQueueFrames = 2` (was 3).
     (inline #ifdef, general-runtime)
  6. `REX_THINLTO` — `CMakeLists.txt` (system) ThinLTO via
     `INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE`. (CMake flag, general-runtime)
  `REX_GAME_SAVE_SYSTEM_FIX` is a composite flag that enables 1+2+3 together
  (the save-system fix is the three XAM/Xenumerator patches as a unit).

- **Idempotency / re-application:** to re-apply cleanly (e.g. after a profile
  flag change), the app does `git checkout sdk-0.8.0-pristine -- .` (§5.3.1)
  to restore the pristine tree, then re-applies overlays. A per-flag
  `.applied` marker (flag name + hash) records what's currently applied.

### 6.4 Generated TOML (deploy config)

The `deploy/spiderman3.toml.inja` template renders the runtime TOML from
profile data. It mirrors the manual `spiderman3.toml`:

```toml
[GPU]
render_target_path_d3d12 = "{{ cvars.render_target_path_d3d12 }}"
async_shader_compilation = true
vsync = false
d3d12_allow_variable_refresh_rate_and_tearing = true
video_mode_refresh_rate = {{ cvars.video_mode_refresh_rate }}
gamma_render_target_as_unorm16 = {{ cvars.gamma_render_target_as_unorm16 }}
# ... etc, pulled from [cvars]

[Core]
user_data_root = "{{ paths.user_data_subdir }}"
cache_path = "{{ paths.cache_subdir }}"
```

The TOML duplicates some cvars (by design — the manual pipeline has the same
redundancy: `OnPreSetup` overrides at startup, TOML provides defaults). The
profile is the single source; both `app.h` and `deploy.toml` render from it.

---

## 7. Reusability for Other Xbox 360 Games

The architecture is game-agnostic by construction. To support a new title:

1. **Create a profile directory** `profiles/<newgame>/` with:
   - `profile.toml` (cvars, paths, build config)
   - `src/*.inja` templates (at minimum `main.cpp.inja` + an `app.h.inja`)
   - `deploy/<newgame>.toml.inja`
   - optionally `patches/game-runtime/<flag>/` overlay dirs if runtime changes are needed
2. **The app discovers it** via a profile loader that scans
   `<app_dir>/profiles/` and `<user_config>/profiles/`.
3. **The pipeline runs unchanged** — the 7 stages are generic; only the profile
   data differs.

**What's game-specific vs generic:**

| Aspect | Generic (app-wide) | Game-specific (profile) |
|--------|-------------------|------------------------|
| ISO extraction (XDVDFS) | ✓ | — |
| rexglue init/codegen | ✓ | — (project name from profile) |
| Cvar set | — | ✓ (`[cvars]`) |
| Source hooks (XAM/XMP) | — | ✓ (`src/*.inja`) |
| Runtime patches | — | ✓ (`patches/game-runtime/<flag>/` overlays + `[patches.runtime].flags`) |
| CMake fragments | — | ✓ (`cmake/`) |
| Deploy TOML | — | ✓ (`deploy/*.inja`) |
| Paths/portability | ✓ (mechanism) | ✓ (values in `[paths]`) |
| Toolchain | ✓ | — |

**Limits of reusability (honest):**
- Cvar *discovery* is manual — finding the right cvar stack for a new game is
  the hard reverse-engineering work (see `LESSONS_LEARNED.md` §1.2: "city
  rendering required 4 cvars flipped together"). The app automates
  *applying* known cvars, not *finding* them. A future "cvar experimentation"
  mode (live-tweak + relaunch) could help but is out of MVP scope.
- Hook *authoring* is manual C++ (the `xmp_bypass.cpp` hooks require
  understanding the game's XAM call sites). Templates parameterize *values*
  but not *which functions to hook*. A new game with different XAM usage needs
  new hook source.
- Runtime patches are flag-guarded overlays tied to specific SDK source files;
  an SDK version bump may require re-basing the overlay headers against the
  new source (the `git checkout sdk-<ver>-pristine -- .` + re-apply flow in
  §5.3.1 makes this mechanical). The `#ifdef` approach is far more resilient
  than diffs because only the overlay header content needs updating, not
  fragile context lines.

---

## 8. MVP vs Ideal Architecture

### 8.1 MVP (Minimal Viable) — "automate the manual pipeline for Spider-Man 3"

**Scope:** one game (Spider-Man 3), GUI wizard, all 7 stages, resume/retry,
bundled SDK source snapshot with build fixes pre-applied.

| Feature | MVP | How |
|---------|-----|-----|
| Frontend | Qt6 QML wizard, 7 steps | one QML file per stage + log view |
| Profiles | spiderman3 only (one bundled profile) | profile loader still present, just one entry |
| Patches | all 3 strata for spiderman3 | templates + overlay-header flags shipped |
| SDK source | bundled snapshot (strategy 1, §5.3) | extract on first runtime build |
| Dependency check | full (§5.2) | blocks start on missing hard deps |
| Resume/retry | yes | state.json + is_complete() per stage |
| Headless mode | yes (minimal) | `--headless --profile spiderman3 --iso <path>` |
| Custom runtime | yes (ThinLTO) | rebuild_runtime_lto equivalent |
| Game profile editor | **no** | profiles are files, edited by hand |
| Cvar experimentation | **no** | — |
| Multi-game | **no** (architecture supports it, UI doesn't expose) | — |
| Remote patch packs | **no** | — |
| Vulkan backend | **no** | untested per `RUNTIME_FIXES.md` §13 |
| PGO | **no** | incompatible with ThinLTO, experimental |
| Tracy profiler build | **no** | use bundled DLL; profiler UI out of scope |

**MVP deliverable:** an installer/zip containing the app exe + Qt DLLs + bundled
SDK source snapshot + spiderman3 profile. User supplies ISO + has toolchain
installed. Run → point at ISO → wait → get playable folder.

### 8.2 Ideal (full architecture) — "reusable Xbox 360 recompilation workbench"

Everything in MVP plus:

- **Multi-game profile management UI:** browse/install/edit profiles; profile
  marketplace (community-submitted, signed).
- **Cvar workbench:** live cvar editing with hot-reload (relaunch game with
  changed TOML, no rebuild) — for the experimentation phase that's currently
  manual edit-rebuild cycles.
- **Hook authoring assistant:** a template library of common XAM/XMP hook
  patterns (device selector, content enumerator, video status) with a UI to
  compose them, reducing the C++ burden for new games.
- **Runtime patch rebasing:** when SDK source updates, assist rebasing
  overlay headers + `#ifdef` guards against new source with conflict
  resolution UI (the `git checkout pristine + re-apply` flow makes this
  mechanical; the UI surfaces which overlays need content updates).
- **CI/headless-first mode:** NDJSON progress, exit codes per stage, usable in
  GitHub Actions for automated game-profile validation.
- **Vulkan backend toggle** (experimental, per `RUNTIME_FIXES.md` §13 — future
  work, untested).
- **PGO orchestration** (the two-build dance from `BUILD_GUIDE.md` §7d):
  instrumented run → profile merge → optimized rebuild, automated.
- **Tracy profiler UI** build + launch integration.
- **Differential rebuilds:** detect which profile stratum changed and only
  redo downstream stages (e.g., cvar-only change → re-deploy, no rebuild).

### 8.3 MVP → Ideal Migration Path

The architecture is designed so MVP is a strict subset — no rework needed:

1. **MVP** ships with one profile and the profile *loader* (not a profile
   *editor*). Adding games later = adding profile directories + a management UI.
2. The patch *engine* (3 strata) is fully built in MVP; the ideal just adds a
   UI to *author* patches.
3. Headless mode in MVP uses a fixed schema; the ideal adds richer CI flags.
4. The `IStage` interface and `PipelineContext` are unchanged; new stages (PGO,
   Tracy) slot in as additional optional stages.

---

## 9. Key Design Decisions & Trade-offs

| Decision | Chosen | Rejected | Why |
|----------|--------|----------|-----|
| Language | C++ | Python, C# | Native FFI to SDK, no managed boundary, same toolchain as target. Python would shell out to everything anyway; C# adds .NET runtime dep. |
| GUI | Qt6/QML | ImGui, Win32, Web | Wizard + file pickers + log view + cross-platform. ImGui is for dev tools; Win32 too low-level; web adds a JS boundary + runtime. |
| Patch format | inja templates (game-project) + flag-guarded overlay headers + `#ifdef` (SDK source) | custom DSL, full C++ recompile, unified diffs | Reuse vendored `inja` for templates; overlays are readable C++, composable across games, and resilient to SDK version drift (unlike diffs). See `DOCS/design_patch_system.md`. |
| SDK source | bundled snapshot (MVP) | always-clone, always-user | Bundled avoids git/network deps and pre-applies the 3 fixes. User-provided supported as fallback. |
| State | JSON checkpoint per stage | single opaque blob, in-memory only | Enables resume + inspection + debugging. |
| Process model | child processes, captured pipes | in-process codegen/build | The SDK tools are external exes; wrapping (not reimplementing) keeps us correct by construction. |
| Game abstraction | profile = directory of TOML + templates + overlay flags | hardcoded per-game modules | New game = new directory, no recompile. This is the single most important reusability lever. |
| Runtime build | optional (skipped if no runtime patches) | always build | Fast path for games that work with prebuilt DLL; full path only when needed. |


## 10. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| SDK version drift breaks runtime overlays | Med | Med (overlay header content stale) | Pin SDK source commit in profile + `sdk-0.8.0-pristine` tag (§5.3.1); `git checkout pristine + re-apply` flow; rebase tooling in ideal. Overlays are more resilient than diffs (only overlay content updates, not context lines). |
| MSVC env setup is fragile (vcvarsall paths vary) | Med | Med | `vswhere.exe` enumeration; allow user override in settings; clear error if env setup fails. |
| Codegen produces different shard counts across SDK versions | Low | Low | `is_complete` checks shard count from `sources.cmake`, not hardcoded 92. |
| ThinLTO + PGO incompatibility | Known | Med | Default to ThinLTO (as shipped); PGO is ideal-only and explicitly excludes ThinLTO. |
| Large SDK source bundle (~GBs with FFmpeg submodule) | Med | Med | Strip unneeded submodules from bundle (e.g., FFmpeg for non-video games) or use shallow clones. |
| Profile authoring is still expert work | High | Med (limits community growth) | Ideal-mode hook assistant + cvar workbench lower the bar; MVP targets Spider-Man 3 only. |
| Force-kill corrupts shader cache (LESSONS_LEARNED §1.3) | Med | High (broken game) | Deployer warns users not to force-kill the game; documents cache recovery. App itself always graceful-cancels child processes. |

---

## 11. File Layout (the app itself)

```
SpiderMan3Recompiler/
├── CMakeLists.txt
├── src/
│   ├── main.cpp                  # Qt app entry / headless dispatch
│   ├── core/
│   │   ├── orchestrator.cpp      # stage runner, state machine
│   │   ├── pipeline_context.cpp  # Context struct + JSON ser/de
│   │   ├── state_store.cpp       # state.json persistence
│   │   ├── progress_bus.cpp      # signals/slots channel
│   │   └── toolchain_env.cpp     # vcvarsall + PATH setup
│   ├── stages/
│   │   ├── istage.h
│   │   ├── iso_extractor.cpp
│   │   ├── rexglue_init.cpp
│   │   ├── rexglue_codegen.cpp
│   │   ├── patch_applier.cpp     # 3 strata
│   │   ├── runtime_builder.cpp
│   │   ├── game_builder.cpp
│   │   └── deployer.cpp
│   ├── deps/
│   │   └── dependency_checker.cpp
│   ├── profile/
│   │   ├── game_profile.cpp      # load + validate profile.toml
│   │   └── template_renderer.cpp # inja wrapper
│   ├── patch/
│   │   ├── cvar_patch.cpp        # stratum 1
│   │   ├── source_patch.cpp      # stratum 2
│   │   └── runtime_patch.cpp     # stratum 3 (overlay headers + #ifdef flags; see DOCS/design_patch_system.md)
│   └── headless/
│       └── cli_driver.cpp        # --headless mode
├── ui/
│   ├── main.qml
│   ├── wizard/
│   │   ├── StageExtract.qml
│   │   ├── StageInit.qml
│   │   ├── StageCodegen.qml
│   │   ├── StagePatch.qml
│   │   ├── StageRuntime.qml
│   │   ├── StageBuild.qml
│   │   └── StageDeploy.qml
│   ├── LogView.qml
│   └── DepCheckView.qml
├── profiles/
│   └── spiderman3/               # the bundled profile (§6.2)
├── patches/                      # shared SDK build fixes (§5.4) + general-runtime overlays
└── third_party/                  # Qt, tomlplusplus, inja, spdlog
```

---

## 12. Summary

The Spider-Man 3 Recompiler is a **C++/Qt6 hybrid GUI+CLI pipeline orchestrator**
that wraps the existing manual toolchain (rexglue, cmake, ninja, clang-cl)
without reimplementing it. Its seven stage modules (`iso_extract → rexglue_init
→ rexglue_codegen → apply_patches → build_runtime → build_game → deploy`) share
a checkpointed `PipelineContext`, enabling resume/retry from any stage. All
game-specific knowledge lives in a **GameProfile** (TOML + inja templates +
flag-guarded overlay headers), making the architecture reusable for other
Xbox 360 titles: Spider-Man 3 is profile `spiderman3`; a new game is a new
profile directory, no recompile. Patches are applied in three strata —
declarative cvars, templated source files, and runtime overlay-header flags —
so most game adjustments require no C++ knowledge. The MVP targets Spider-Man 3
only with a bundled SDK source snapshot; the ideal expands to a multi-game
recompilation workbench with a cvar workbench, hook assistant, and CI
integration. The architecture is designed so MVP is a strict subset of ideal —
no rework on the path from one to the other.
