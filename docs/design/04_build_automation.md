# Spider-Man 3 Recompiler — Build Automation Design

**Author:** BuildAutomation (plan agent)
**Date:** 2026-07-07
**Status:** Design — awaiting review
**Grounded in:** `DOCS/BUILD_GUIDE.md`, `DOCS/TROUBLESHOOTING.md`, `DOCS/RUNTIME_FIXES.md`, `spiderman3/build.bat`, `rexglue-sdk-0.8.0 Source code/rebuild_runtime_lto.bat`, `.gitmodules`, `spiderman3/src/`, `spiderman3/CMakeLists.txt`

---

## 0. Grounding Notes (from reading the actual workspace + DOCS)

These observations constrain the design. They come from the real files, not the brief alone:

### 0.1 Verified tool versions (BUILD_GUIDE §1, "verified on this machine")

| Tool | Version | Path |
|---|---|---|
| LLVM / clang-cl | **22.1.8** | `C:\Program Files\LLVM\bin\` |
| MSVC (VS 2022 Community) | **14.44.35207** | `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\` |
| CMake | **4.2.1** | on `PATH` |
| Ninja | **1.13.2** | on `PATH` |
| ReXGlue SDK | **v0.8.0** | `C:\tmp\Workspace 1\RexGlue360Recomp\` |

**Why clang-cl + MSVC together:** clang-cl is the compiler (codegen emits C++23 with `__builtin_rintf` and clang-specific intrinsics), but clang-cl on Windows reuses the MSVC linker (`link.exe`), Windows SDK headers, and CRT libs. `vcvarsall.bat x64` sets up `INCLUDE`/`LIB`/`PATH` for MSVC; then `clang-cl` is put on `PATH` as the compiler.

### 0.2 The exact command sequence (BUILD_GUIDE §2, §5, §7c)

- **Init:** `rexglue init --name spiderman3 --xex extracted\default.xex` (NOT just `rexglue init` — the `--name` and `--xex` flags are required)
- **Codegen:** `rexglue codegen spiderman3_manifest.toml` (or `cmake --build out\build\clang-release --target spiderman3_codegen`)
- **Game configure (Ninja + clang-cl):** `cmake -G Ninja -S <SRC> -B <BUILD> -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<SDK_ROOT> -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`
- **Game build:** `cmake --build <BUILD> --parallel`
- **Runtime configure (BUILD_GUIDE §7c — the *correct* form):** `cmake -G Ninja -S <SDK_SOURCE> -B <BUILD> -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`
- **Runtime build:** `cmake --build <BUILD> --parallel --target rexruntime`

### 0.3 ⚠️ The hand-script `rebuild_runtime_lto.bat` is INCONSISTENT with BUILD_GUIDE §7c — the automation must NORMALIZE, not replicate

The actual `rebuild_runtime_lto.bat` on disk:
- **Omits `-G Ninja`** → falls back to the default VS generator (multi-config), which is why it uses `--config Release`.
- **Omits `-DCMAKE_C/CXX_COMPILER=clang-cl`** → may pick MSVC `cl.exe` as compiler instead of clang-cl. This is almost certainly a latent bug in the hand-script; the BUILD_GUIDE §7c summary explicitly shows both flags.
- **Copy-path mismatch:** it configures into `out/build/win-amd64` but copies from `out\win-amd64\Release\rexruntime.dll` (VS generator nests `<build>/Release/`, not `<build>/`). The script "works" because the VS generator produces `out/build/win-amd64/Release/rexruntime.dll` and the `if exist` check happens to find it — but the path in the script (`out\win-amd64\Release\`) is wrong relative to the configured build dir and only resolves by accident from the wrong CWD.

**The automation will use the BUILD_GUIDE §7c normalized form:** explicit `-G Ninja`, explicit `clang-cl` compilers, single-config `Release`, and a consistent output path `<build_dir>/rexruntime.dll` (Ninja single-config puts the binary at the build-dir root, not in a `Release/` subfolder). This removes the multi-config/`--config` confusion and matches the game build's toolchain. **Do not blindly reproduce the hand-script's quirks.**

### 0.4 Patches: 3 build-fixes + 3 required runtime patches + 2 optional perf patches + 1 build-config flag

Per BUILD_GUIDE §7b + RUNTIME_FIXES §5/§12, and cross-checked against the PatchSystem agent's design (`DOCS/design_patch_system.md`), the actual patch set splits into **three categories** with distinct criticality. Only the first (3 behavioral save-system patches) is load-bearing for a working build; the rest are optional perf or build-config.

**3 source-tree build fixes (one-time, `sdk-build-fix` category, BUILD_GUIDE §7b):**
1. xxHash CMake path — bundled xxHash CMakeLists references a non-existent path; point at vendored xxHash dir.
2. libmspack symlinks → copies — Unix symlinks don't resolve on Windows; replace with copies.
3. CMake 4.x module scan — CMake 4.x removed deprecated modules the source tree scans for; patch the scan to current names.

**(a) 3 REQUIRED runtime behavioral patches (`game-runtime` category, RUNTIME_FIXES §5) — load-bearing for save system:**
1. `xeXamEnumerate` async return → `X_ERROR_IO_PENDING` (`src/kernel/xam/xam_enum.cpp`) — without this the game's async poll loop never enters and it hangs.
2. `WriteItems` 0-item result → `X_ERROR_SUCCESS` (`src/system/xenumerator.cpp`) — the game treats `ERROR_NO_MORE_FILES` as fatal; SUCCESS with 0 items means "no saves, create new."
3. `xeXamDispatchHeadless` notifications — XN_SYS_UI=true/false pre/post callbacks (`src/kernel/xam/xam_ui.cpp`) — for headless UI dialogs (MessageBox etc.).

**(b) 2 OPTIONAL perf patches (`general-runtime` category, RUNTIME_FIXES §12) — always-on flags, benefit all games:**
4. `kQueueFrames` 3 → 2 (`src/graphics/d3d12/command_processor.h` — compile-time constant, not a cvar) — more stable FPS: minimums 23→30+, avg 46-48.
5. EDRAM barrier skip — skip `MarkEdramBufferModified` when draw writes neither color nor depth/stencil (`src/graphics/d3d12/render_target_cache.cpp:1088-1094`) — no measurable gain on Spider-Man 3 but no-cost correctness-preserving; kept.

**(c) 1 BUILD-CONFIG flag (`game-build` category) — NOT a source patch, a CMake include() injection:**
6. ThinLTO/IPO — applied via `sdk_patches.cmake` (runtime) and `patches.cmake` (game) include() fragments setting `INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE` on the target. Pairs with `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` at runtime configure as belt-and-suspenders. See §4.2. This is NOT a source-tree edit and does NOT belong in the runtime patch table.

> **Correction note:** an earlier draft listed `xam_notify.cpp` (XNotifyGetNext DEBUG logging) as a patch. It appears in the brief's manual-process list but is NOT in BUILD_GUIDE/RUNTIME_FIXES as a required patch, and the PatchSystem design does not include it in the load-bearing set. It is **dropped** from the required patch list. (The PatchSystem design's `patches/sdk-overlays/` dir does contain an `xam_notify_overlay.h` as a DEBUG-logging overlay, but it is optional profiling instrumentation, not a correctness fix.)
>
> **Patch format note (from PatchSystem agent):** SDK source patches are NOT `.patch` files. They are **flag-guarded source overlays** — `#ifdef`-gated code behind CMake-defined flags (e.g. `REX_GAME_SAVE_SYSTEM_FIX`) + inline `#ifdef` guards in SDK source + overlay headers `#include`-ed behind the flag + CMake `include()` injections. The SDK prep step git-inits the tarball (no `.git` exists) and tags `sdk-0.8.0-pristine` for free git checkout reversibility. Game-project patches are **Jinja-templated source files** (`xmp_bypass.cpp.inja`, `spiderman3_app.h.inja`, `spiderman3.toml.inja`) + a generated `patches.cmake` fragment included by `CMakeLists.txt`. Stable patch IDs (used verbatim below): `runtime/xam-ui-headless`, `runtime/xam-enum-pending`, `runtime/xenumerator-success`, `runtime/queue-frames`, `runtime/rov-barrier-skip`, `runtime/thinlto-ipo`, `game/xmp-bypass`, `game/app-header`, `game/cvar-preset`, `game/runtime-toml`, `game/cmake-injection`.

### 0.5 ThinLTO + PGO are mutually exclusive (critical constraint)

Per BUILD_GUIDE §7c/§7d and RUNTIME_FIXES §12.1: clang rejects `-flto=thin` together with `-fprofile-instr-generate`. The shipped custom runtime uses **ThinLTO** (5-15% gain). PGO is a documented but **unshipped** experimental path requiring a two-build collection dance. **The automation's default is ThinLTO; PGO is an opt-in mode that disables ThinLTO.** The error catalog must catch any attempt to enable both and surface the specific remediation.

### 0.6 Parallelization insight (confirmed by docs)

Per BUILD_GUIDE §7: the custom `rexruntime.dll` is built from the SDK **source** tree, then **drop-in copied** into the game build output (and portable folder), replacing the prebuilt one. The game build itself links against the **prebuilt** `rexruntime.lib` from the installed SDK (`CMAKE_PREFIX_PATH` = SDK root with `lib\`). Therefore:

- The **runtime build (SDK source, ~90s)** is **independent** of the **game chain (ISO extract → codegen → game build)**.
- They can run **in parallel** and **join at deployment**, where the custom DLL overwrites the prebuilt one.
- Critical path = `max(runtime chain, game chain)` = `max(~110s, ~4min)` = **~4min**, vs serial ~6.5min. **~90s saved.**

### 0.7 Submodules: the brief says 16, `.gitmodules` has 22

The actual `.gitmodules` in the SDK source tree lists exactly **22 submodules** (verified by `grep -c "^\[submodule" .gitmodules`): libmspack, glslang, FFmpeg, tomlplusplus, simde, xxHash, spdlog, fmt, catch2, snappy, utfcpp, volk, vulkan-headers, vulkan-memory-allocator, imgui, spirv-tools, spirv-headers, cli11, o1heap, sdl3, inja, tracy. (The `thirdparty/` dir contains additional non-submodule vendored folders like `aes_128`, `crypto`, `disasm`, `dxbc`, `dxc`, `pe`, `picosha2`, `renderdoc`, `tiny-aes` — these are NOT `.gitmodules` entries; don't conflate the directory listing with the submodule count.) **The app must NOT hardcode 16.** It reads `.gitmodules` dynamically and reports the real count (22). (Doc bug in the brief to surface to the user; not a blocker.)

### 0.8 The SDK source tree on this machine is NOT a git repo

`git rev-parse --show-toplevel` failed in `C:\tmp\Workspace 1\rexglue-sdk-0.8.0 Source code`. It's an extracted "Source code" archive (GitHub release tarball style). This breaks a naive "clone + `git submodule update --init`" plan. The automation must handle both git-clone and pre-extracted-archive strategies (§3).

### 0.9 Game source files (user-authored, not generated)

From `spiderman3/src/` + BUILD_GUIDE §4: the hand-written files are `main.cpp`, `spiderman3_app.h`, `roundevenf.cpp` (CRT math shim for `__builtin_rintf`→`roundevenf`), `xmp_bypass.cpp` (3 hooks), `particle_perf.cpp`, `dynamic_resolution.cpp`. The `CMakeLists.txt` lists `main.cpp`, `roundevenf.cpp`, `xmp_bypass.cpp` (the working tree has extra files not yet wired into CMakeLists — a discrepancy to note). `generated/` is codegen-owned and must never be patched.

---

## 1. Architecture Overview

```
┌──────────────────────────────────────────────────────────────┐
│                  BuildOrchestrator (core)                     │
│  - step graph, dependency resolution, resume, logging         │
└───────┬──────────────────────────────────────────────────────┘
        │
   PrereqCheck (Phase 0)
        │
        ├────────────────────────┬──────────────────────────────┐
        ▼ (SDK source)           ▼ (prebuilt SDK + ISO)         │
   SDKSourceSetup (Phase 1)  GameSetup (Phase 2)                │
        │                       │                               │
        ▼                       ▼                               │
   RuntimeBuild (Phase 3)   GameBuild (Phase 4)                 │
        │                       │                               │
        └───────────┬───────────┘                               │
                    ▼                                            │
              Deployment (Phase 5) ◄────────────────────────────┘
                    │
                    ▼
              Verify (Phase 6)
```

**Key dependency insight (§0.6):** Phase 1→3 (runtime chain) and Phase 2→4 (game chain) are **independent branches** that fork after prereqs and join at deployment. The game build links the **prebuilt** SDK runtime; the custom `rexruntime.dll` is only swapped in at deploy. This enables true parallelism.

**Principle:** each phase is an idempotent, resumable unit with a declared input→output contract. The orchestrator records phase completion in a state file so a failed run resumes from the last incomplete phase (unless `--clean`).

---

## 2. Prerequisite Checker (Phase 0) — Spec

### 2.1 Required tools, verified versions, and discovery

| Tool | Verified version | Discovery method | On failure |
|---|---|---|---|
| **CMake** | ≥ 4.2.1 (game `CMakeLists.txt` requires ≥ 3.25; runtime needs CMake 4.x for the module-scan fix) | `where cmake` → `cmake --version` parse | winget `Kitware.CMake` |
| **Ninja** | ≥ 1.13.2 | `where ninja` | winget `Ninja-build.Ninja`; **or bundle** (§11) |
| **LLVM/clang-cl** | **22.1.8** (`x86_64-pc-windows-msvc`) | `where clang-cl`; verify `C:\Program Files\LLVM\bin\clang-cl.exe --version` reports `clang version 22.1.8 ... Target: x86_64-pc-windows-msvc` | winget `LLVM.LLVM` |
| **MSVC build tools** | **14.44.35207** (VS 2022) | **`vswhere.exe`** at `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath` → `<path>\VC\Auxiliary\Build\vcvarsall.bat` | Prompt: install VS 2022 with "Desktop development with C++" workload |
| **Windows SDK** | 10.0.19041+ | via vswhere `-requires Microsoft.Windows.SDK.*` OR enumerate `C:\Program Files (x86)\Windows Kits\10\Include\*` | Bundled with VS installer |
| **Git** | 2.20+ (only if SDK source fetched via git) | `where git` | winget `Git.Git` (SOFT if archive strategy used) |
| **RexGlue prebuilt SDK** | **v0.8.0** | Probe `bin\rexglue.exe`, `bin\rexruntime.dll`, `cmake\SDL3Config.cmake`, `share\rexglue\` (CMake package), `include\rex\rex_app.h` | **BLOCKING** — user must supply path |
| **RexGlue SDK source** | v0.8.0 | Probe for `CMakeLists.txt` + `src\` + `thirdparty\` + `.gitmodules` | See §3 (fetch or extract) |
| **extract-xiso** | any (XDVDFS-aware) | `where extract-xiso` | **Bundle** (MIT-licensed; §11) |

**vcvarsall resolution (replaces the hardcoded `C:\Program Files\Microsoft Visual Studio\2022\Community\...` path):** the hand-scripts hardcode the Community edition path. Real installs may be Enterprise/Professional/BuildTools or under `Program Files (x86)`. The automation resolves via `vswhere` and caches the resolved path in `prereq_manifest.json` so subsequent phases don't re-probe.

### 2.2 Reproducibility manifest

The checker writes `prereq_manifest.json` capturing exact versions (clang-cl `22.1.8`, MSVC `14.44.35207`, Win SDK build number, CMake `4.2.1`, Ninja `1.13.2`, SDK `v0.8.0`, plus the resolved vcvarsall path). Stored with build logs so a "works on my machine" build can be diffed against a failing one. This manifest is also a **cache key input** (§8) — a compiler upgrade invalidates build caches.

### 2.3 Severity model

- **BLOCKING** (cannot proceed): no CMake, no clang-cl, no MSVC, no Win SDK, no prebuilt RexGlue SDK.
- **SOFT** (degraded path): no Ninja → the automation should install/bundle it (Ninja is required for the game build's `-G Ninja`); no Git → require pre-extracted SDK source archive.
- **INFO**: missing optional tools (Tracy profiler UI, dxcompiler.dll which is non-fatal per TROUBLESHOOTING §6).

### 2.4 Version drift warnings

The automation treats the verified versions as **recommended**, not hard-required (the toolchain will work with newer CMake/clang-cl). It warns on:
- clang-cl < 16 (no C++23) — BLOCKING.
- CMake < 3.25 (game CMakeLists `cmake_minimum_required(VERSION 3.25)`) — BLOCKING.
- CMake 4.x without the module-scan build-fix applied to SDK source — the runtime build will fail; the fix is in §3.
- MSVC < 14.40 (VS 2022 17.10+) — warn; older may lack needed CRT/SDK.

### 2.5 UX

- **CLI:** `spiderman3-builder check` prints a table with green/yellow/red per item and exact discovered versions.
- **GUI (the larger app):** "System Check" panel, per-item status, "How to fix" tooltip, "Install via winget" buttons for CMake/LLVM/Ninja/Git. VS + Win SDK install is heavier; deep-link to the VS Installer.

---

## 3. SDK Source Setup (Phase 1)

### 3.1 The real problem: the working copy is NOT a git repo (§0.8)

`git rev-parse` failed in the SDK source tree. It's an extracted archive. Two strategies:

**Strategy A — Git clone (preferred, when upstream is public + network available):**
1. Clone the RexGlue SDK repo at tag `v0.8.0`.
2. `git submodule update --init --recursive --jobs 8` (parallel submodule fetch).
3. **Read submodule count from `.gitmodules` dynamically** — do NOT hardcode 16 (§0.7). Report mismatches as a warning.
4. Verify each `thirdparty/<name>` dir is non-empty (common failure: shallow clone with broken submodule).

**Strategy A' — SDK source supplied as archive (the actual current state):**
The user supplies the same "Source code" zip. The app unpacks it. Submodules are absent in a tarball (no `.git`), so:
- If `.gitmodules` is present: `git init` the dir, register submodule URLs from `.gitmodules`, `git submodule update --init --recursive`. This converts the extracted tree into a working repo.
- If `thirdparty/<name>` dirs are already populated (the zip vendor pre-populated them): verify each is non-empty and skip fetch (this matches the current working state — `thirdparty/xxHash/xxhash.h` etc. exist).

**Strategy B — Pre-populated archive (fallback, zero network):**
Detect `thirdparty/xxHash/xxhash.h`, `thirdparty/libmspack/libmspack/...`, etc. If all submodule dirs have content, skip fetch entirely.

### 3.2 The 3 build fixes (BUILD_GUIDE §7b) — idempotent, sentinel-tracked

| Fix ID | Target file(s) | Nature |
|---|---|---|
| `sdk-build/xxhash-cmake-path` | `thirdparty/xxHash/CMakeLists.txt` (or `cmake/*.cmake` referencing xxHash) | Point vendored xxHash CMake at the correct path |
| `sdk-build/libmspack-symlinks` | `thirdparty/libmspack/**` | Replace Unix symlinks with copies of target files |
| `sdk-build/cmake4x-module-scan` | Top-level `CMakeLists.txt` or `cmake/*.cmake` module scan | Patch scan to use current CMake 4.x module names |

**Application mechanism (owned by the automation; patch contents owned by PatchSystem agent):**
- Each fix ships as a `.patch` file in the app's `patches/sdk-build/` resources, with a **sentinel comment** (e.g. `// SPIDERMAN3-BUILDER-PATCH: xxhash-cmake-path`).
- The applier checks for the sentinel before applying; if present, skip (idempotent).
- For non-git trees: use a custom applier (not `git apply`) that tolerates the extracted-archive state.
- State recorded in `sdk_setup.state` (`{fixes_applied: [...], source_hash, applied_at}`).
- On resume: skip fixes already recorded; re-verify via sentinel.

### 3.3 Source verification

After setup, hash the patched source tree (manifest of key files) and store it. If a re-run's hash matches, skip the entire Phase 1. If it differs, warn and offer to re-setup (with a backup of the prior state).

---

## 4. Custom Runtime Build (Phase 3)

### 4.1 Apply runtime patches (RUNTIME_FIXES §5, §12; patch format per PatchSystem design)

The runtime patches split into **three categories** with distinct criticality (see §0.4). The patch format is **flag-guarded source overlays** (not `.patch` files): `#ifdef`-gated code behind CMake-defined flags + overlay headers + CMake `include()` injections. The SDK prep step git-inits the tarball and tags `sdk-0.8.0-pristine` for reversibility (§3.2).

**(a) 3 REQUIRED behavioral save-system patches (`game-runtime`, gated by `-DREX_GAME_PROFILE=spiderman3` → `REX_GAME_SAVE_SYSTEM_FIX`):**

| Patch ID | File | Purpose |
|---|---|---|
| `runtime/xam-enum-pending` | `src/kernel/xam/xam_enum.cpp` | `xeXamEnumerate` async path returns `X_ERROR_IO_PENDING` (not SUCCESS) — without this the game's async poll loop never enters and it hangs |
| `runtime/xenumerator-success` | `src/system/xenumerator.cpp` | `WriteItems` returns `X_ERROR_SUCCESS` for 0 items (not `ERROR_NO_MORE_FILES`) — game treats NO_MORE_FILES as fatal |
| `runtime/xam-ui-headless` | `src/kernel/xam/xam_ui.cpp` | `xeXamDispatchHeadless` deferred path with XN_SYS_UI=true/false pre/post callbacks (headless UI dialogs) |

**(b) 2 OPTIONAL perf patches (`general-runtime`, always-on flags — benefit all games):**

| Patch ID | File | Purpose |
|---|---|---|
| `runtime/queue-frames` | `src/graphics/d3d12/command_processor.h` | `kQueueFrames` 3 → 2 (more stable FPS: minimums 23→30+, avg 46-48) |
| `runtime/rov-barrier-skip` | `src/graphics/d3d12/render_target_cache.cpp:1088-1094` | Skip `MarkEdramBufferModified` when draw writes neither color nor depth/stencil (no-cost correctness-preserving) |

**(c) 1 BUILD-CONFIG flag (`game-build`) — NOT a source patch, a CMake `include()` injection:**

| Patch ID | Mechanism | Purpose |
|---|---|---|
| `runtime/thinlto-ipo` | `sdk_patches.cmake` include() in `src/system/CMakeLists.txt` sets `INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE` on the rexruntime target | ThinLTO 5-15% gain via cross-module inlining. Pairs with `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` at configure as belt-and-suspenders. **This is a CMake property injection, not a source-tree edit.** |

Application is idempotent (the git-init + tag + overlay-insert + commit sequence is re-runnable; sentinels are the `#ifdef` guards themselves). State recorded in `runtime_patches.state` (patch IDs + overlay commit hash). The §3.2 build fixes and these runtime patches are independent (xxHash CMake fix doesn't conflict with xam_enum.cpp overlay). Apply both sets before configuring. **Only the 3 category-(a) patches are load-bearing for a working save system; (b) and (c) are perf/build-config and a build without them still produces a functional (if slower) runtime.**

### 4.2 Configure + build (NORMALIZED per BUILD_GUIDE §7c — §0.3)

The automation uses the **normalized** command, NOT the hand-script's buggy form:

```
vcvarsall.bat x64   (resolved via vswhere in Phase 0)
set PATH=<LLVM>\bin;%PATH%
cmake -G Ninja -S <SDK_SOURCE> -B <RUNTIME_BUILD_DIR> \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DREX_GAME_PROFILE=spiderman3 \
    -DCMAKE_C_COMPILER=clang-cl \
    -DCMAKE_CXX_COMPILER=clang-cl
cmake --build <RUNTIME_BUILD_DIR> --parallel --target rexruntime
```

- **Generator: Ninja** (single-config; matches BUILD_GUIDE §7c and the game build). This fixes the hand-script's missing `-G Ninja` and eliminates the `--config Release` multi-config confusion.
- **Compilers: explicit `clang-cl`** (fixes the hand-script's missing compiler flags — without these CMake may pick `cl.exe`).
- **Output path: `<RUNTIME_BUILD_DIR>/rexruntime.dll`** (Ninja single-config puts the binary at the build-dir root, NOT in a `Release/` subfolder). This fixes the hand-script's mismatched `out\win-amd64\Release\` copy path.
- **ThinLTO/IPO:** `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` is belt-and-suspenders alongside the `sdk_patches.cmake` include() injection (patch `runtime/thinlto-ipo`) which sets `INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE` on the rexruntime target via `set_target_properties`. Both agree; the include() is the authoritative mechanism (version-independent), the `-D` flag is redundant safety.
- **`-DREX_GAME_PROFILE=spiderman3`:** selects this game's overlay `#ifdef` flags (e.g. `REX_GAME_SAVE_SYSTEM_FIX`) via `target_compile_definitions` in `sdk_patches.cmake`. **Runtime-only flag** — it has nothing to do with ThinLTO. Without it the category-(a) save-system overlays are compiled out and the runtime is unpatched.
- **Parallelism:** `--parallel` (all cores). Runtime ~90s on Z1 Extreme (8 cores).

### 4.3 Output verification

Assert `<RUNTIME_BUILD_DIR>/rexruntime.dll` exists, is > 1 MB, and has a newer timestamp than the prebuilt `bin\rexruntime.dll`. If missing, run `dir /s /b <RUNTIME_BUILD_DIR>\*rexruntime.dll` (as the hand-script does) to locate it and report the actual path — sometimes generator nesting differs.

### 4.4 PGO mode (opt-in, experimental — BUILD_GUIDE §7d)

PGO is a documented but **unshipped** experimental path. The automation exposes it as `--pgo` but **defaults to ThinLTO**. Selecting `--pgo`:
1. **Disables ThinLTO** (mutual exclusion — §0.5).
2. Instrumented build with `-DCMAKE_CXX_FLAGS="-fprofile-instr-generate"` (no ThinLTO).
3. Profile collection run (launch game, play representative session, exit cleanly — force-kill loses the profile).
4. `llvm-profdata merge -o rexruntime.profdata default.profraw`.
5. Optimized rebuild with `-DCMAKE_CXX_FLAGS="-fprofile-use=rexruntime.profdata"` (still no ThinLTO).

**The error catalog (§7) must catch any attempt to enable both `-flto=thin` and `-fprofile-instr-generate` and surface the specific remediation:** "ThinLTO and PGO are mutually exclusive; choose one. The shipped config uses ThinLTO."

### 4.5 Caching the runtime build (incremental)

- The CMake build dir is **persistent** across runs. No source changes → no-op configure + incremental link (~2-5s, not 90s).
- Cache key = hash of all patched runtime source files (3 required + 2 perf overlays) + `sdk_patches.cmake` + `CMakeLists.txt` chain + `prereq_manifest.json` (compiler version matters — a clang-cl upgrade invalidates).
- Cache key matches last successful build → **skip the build entirely**, reuse existing `rexruntime.dll` (re-verify it exists; optional `LoadLibrary` load-test).
- Cache key differs → incremental build (CMake/Ninja dependency tracking). Full rebuild only on `--clean` or compiler change.

---

## 5. Game Project Setup (Phase 2) + Build (Phase 4)

### 5.1 Setup sub-phase (BUILD_GUIDE §2, §3, §4)

```
rexglue init --name spiderman3 --xex <SDK_ROOT>\extracted\default.xex
rexglue codegen spiderman3_manifest.toml
```

- **Init** creates `spiderman3\` with `CMakeLists.txt`, `CMakePresets.json`, `spiderman3_manifest.toml`, `generated/rexglue.cmake`.
- **Codegen** translates **43,676 functions** → **92 generated `.cpp` files** (`spiderman3_recomp.0.cpp` … `spiderman3_recomp.91.cpp`) + `spiderman3_init.cpp` + `spiderman3_register.cpp`. ~30s.
- **Idempotency:** `rexglue init` likely refuses to overwrite. Detect existing project (`generated/` + `CMakeLists.txt`) and skip init, or offer `--force`.
- **Codegen cache key** = hash of `extracted\default.xex` + `rexglue.exe` version. If unchanged, skip codegen (saves ~30s + 92-file churn).
- **Apply game source files after codegen** (BUILD_GUIDE §4): the hand-written files (`main.cpp`, `spiderman3_app.h`, `roundevenf.cpp`, `xmp_bypass.cpp`, `particle_perf.cpp`, `dynamic_resolution.cpp`, `CMakeLists.txt`) are **templates** the app ships. Codegen only writes `generated/`; it does not touch `src/` (per BUILD_GUIDE §2 note: "Do NOT edit files in `generated/`"). So the app writes its templates into `src/` after init, before/after codegen (order doesn't matter since they're disjoint dirs).
- **Template model (recommended):** the app ships template versions of the `src/` files. After init, write templates. Reproducible; doesn't depend on patch application order.

### 5.2 Configure + build (build.bat, BUILD_GUIDE §5)

```
vcvarsall.bat x64
set PATH=<LLVM>\bin;%PATH%
set SDK_ROOT=<prebuilt SDK>
set SRC_DIR=<project>
set BUILD_DIR=<project>\out\build\clang-release
cmake -G Ninja -S "%SRC_DIR%" -B "%BUILD_DIR%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%SDK_ROOT%" ^
    -DCMAKE_C_COMPILER=clang-cl ^
    -DCMAKE_CXX_COMPILER=clang-cl
cmake --build "%BUILD_DIR%" --parallel
```

- **Generator: Ninja** (single-config, fastest for 92-file parallel compile).
- **`CMAKE_PREFIX_PATH` = prebuilt SDK root** (`C:\tmp\Workspace 1\RexGlue360Recomp`), NOT the SDK source. The game links `rexruntime.lib` (prebuilt) + static deps (SDL3, fmt, spdlog, snappy, xxhash, libavcodec, etc.) from the SDK `lib\`. This is what makes Phase 3 and Phase 4 independent (§0.6).
- **`/EHa`** required (SEH with structured exceptions for PPC exception translation) — already in `CMakeLists.txt` via `target_compile_options(spiderman3 PRIVATE /EHa)`.
- **ThinLTO/IPO for the game:** handled by the `patches.cmake` include() fragment (patch `game/cmake-injection`) which sets `INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE` on the spiderman3 target. **No `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` flag is passed at game configure** — the include() handles it. (Contrast with the runtime build §4.2, which passes the `-D` flag as belt-and-suspenders alongside `sdk_patches.cmake`.) This matches the hand-script `build.bat`, which never passed the IPO flag.
- **Output:** `out\build\clang-release\spiderman3.exe`, ~118 MB. Verify PE header + size.

### 5.3 Caching the game build

- Build dir persistent (Ninja incremental).
- Cache key = hash of `default.xex` (codegen input) + all template files + `generated/` manifest + `prereq_manifest.json`.
- Incremental rebuild on small template changes: seconds to ~1 min. Full rebuild only on codegen change or `--clean`.

---

## 6. Process Model & Parallelism (§0.6)

### 6.1 The fork/join graph

```
Phase 0 (prereq)
   │
   ├──────────────────────┬───────────────────────────┐
   ▼ (SDK source)         ▼ (prebuilt SDK + ISO)      │
Phase 1: SDKSourceSetup   Phase 2: GameSetup           │
   │  (submodules +       │  (extract ISO →            │
   │   3 build fixes)     │   rexglue init → codegen   │
   ▼                      ▼   → apply src templates)   │
Phase 3: RuntimeBuild     Phase 4: GameBuild           │
   │  (3 req + 2 perf       │
   │   ThinLTO build,     │   ninja build, ~3-4min)    │
   │   ~90s)              │                            │
   └──────────┬───────────┘                            │
              ▼                                        │
        Phase 5: Deployment ◄──────────────────────────┘
              │  (copy exe + custom DLL + TracyClient.dll
              │   + TOML + game junction + user_data)
              ▼
        Phase 6: Verify (launch + 5s + kill)
```

**Critical path = max(Phase 1+3, Phase 2+4) = max(~110s, ~4min) = ~4min.** Serial would be ~6.5min. **~90s saved.**

### 6.2 CPU contention management

Two `--parallel` builds simultaneously oversubscribe the 8-core Z1 Extreme. Policy:
- **≥8 cores:** run both branches in parallel, cap each: `runtime --parallel 4`, `game --parallel 4` (total 8). Slightly slows each but avoids thrash.
- **<8 cores:** run serially (runtime first, then game).
- Detect core count in Phase 0; decide automatically. User can override with `--jobs`.

### 6.3 Process spawning

- Each build phase spawns a `cmd.exe` child with a generated temp `.bat` that calls `vcvarsall.bat x64` then runs cmake. Don't replicate vcvarsall's env in-app (fragile).
- Capture stdout+stderr to a rotating log file (§7.4).
- Stream lines to the UI via a line-buffered pipe.
- On non-zero exit: parse log for likely error (§7.2), mark phase failed, preserve build dir for resume.

---

## 7. Error Handling Strategy

### 7.1 Failure classification + catalog (from BUILD_GUIDE "Common Build Errors" + TROUBLESHOOTING)

| Class | Error signature | Remediation |
|---|---|---|
| **Env** | `vcvarsall failed` / `FAILED: vcvarsall` | VS 2022 not installed or not at expected path. Re-resolve via vswhere; if not found, prompt to install "Desktop development with C++" workload. |
| **Env** | `clang-cl not found` / `where clang-cl` fails | LLVM not installed. Install LLVM 22.x; verify `C:\Program Files\LLVM\bin\clang-cl.exe --version` reports `clang version 22.1.8 ... Target: x86_64-pc-windows-msvc`. |
| **Configure** | `CMake Error: ReXGlue SDK not found` | `-DCMAKE_PREFIX_PATH` not pointing at SDK root, or SDK missing `share\rexglue\` package config. Verify SDK path. |
| **Compile** | `unresolved external symbol roundevenf` | `src/roundevenf.cpp` missing or not in `CMakeLists.txt` `SPIDERMAN3_SOURCES`. The CRT shim for PPC `frin/frip/frim/friz` → `__builtin_rintf` → `roundevenf` (MSVC CRT lacks it). Ensure the template writes this file and CMakeLists lists it. |
| **Compile** | `unresolved external symbol __imp__XamEnumerate` (or other XAM imports) | Expected — `xmp_bypass.cpp` hooks the wrapper functions and provides chain-through to `__imp__XamEnumerate`. Ensure `xmp_bypass.cpp` is in the build. |
| **Link** | `Permission denied` / `cannot open spiderman3.exe for writing` | The exe is currently running. Close `spiderman3.exe` (Task Manager); **do not `taskkill /F`** — a forced kill during shader cache write corrupts `.xpso`. Clean close only. |
| **Compile** | `fatal error C1060: compiler is out of heap space` | A generated shard too large. Ensure `-G Ninja` (parallelizes across shards). 92-shard split is designed to keep each file compilable. |
| **Compile** | `error: unknown argument '/EHa'` | Building with vanilla `clang++` instead of `clang-cl`. Ensure `-DCMAKE_CXX_COMPILER=clang-cl` (not `clang++`). `/EHa` is MSVC-compatible only. |
| **Link (runtime)** | ThinLTO + PGO conflict | **Specific catalog entry:** clang rejects `-flto=thin` + `-fprofile-instr-generate`. Choose one. Shipped config = ThinLTO. Disable PGO or disable ThinLTO. |
| **Runtime (launch)** | Black screen / no UI | `render_target_path_d3d12 = "rov"` missing from TOML + `OnPreSetup`. ROV required for menu/UI. (Not a build error but post-build verify catches it.) |
| **Runtime (launch)** | "Invalid UTF-8" crash / storage loop | `XamShowDeviceSelectorUI_Wrapper` hook not active, or custom runtime missing the IO_PENDING + WriteItems-SUCCESS patches. Verify hooks compiled + custom DLL deployed. |
| **Runtime (launch)** | Crash on launch | Corrupted shader cache `.xpso`. Delete `user_data\cache\shaders\shareable\*.xpso`, relaunch (expect ~20s recompile). **Do not delete the whole `shaders/` dir** — forces full 407-PSO recompile every time. |
| **Runtime (launch)** | Custom RelWithDebInfo runtime won't start | Runtime DLL config mismatch. Project built Release → imports `rexruntime.dll`; shipping a RelWithDebInfo runtime renamed to `rexruntime.dll` causes ABI mismatch. Match configs: Release project ↔ Release (`rexruntime.dll`); RelWithDebInfo project ↔ `rexruntimerd.dll`. |
| **Runtime (profiling)** | Tracy profiler won't connect / garbled | Profiler UI version ≠ client version. SDK bundles Tracy **0.13.1** client. Build profiler UI from `thirdparty/tracy/profiler/` (same 0.13.1 source). Don't mix releases. |
| **Runtime (profiling)** | `REXGLUE_PROFILE_GUEST_FUNCTIONS` link error / no zones | `TracyClient.dll` ABI mismatch. The DLL next to the profiled exe must be the same build the project linked against. Don't swap a different Tracy release's client. |

### 7.2 Error detection heuristics (log post-processing)

The orchestrator runs lightweight log parsing after each phase:
- `error:` / `error :` (clang-cl, MSVC) → extract `file:line:col: message`.
- `CMake Error` → extract the error block.
- `FAILED: ` (Ninja) → marks a compile failure; following lines name the command + source.
- `lld-link: error:` / `LNK2019` / `LNK1120` → unresolved externals; list symbols. Special-case `profile` / `PGO` / `llvm-profdata` → surface the ThinLTO+PGO mutual-exclusion remediation.
- `Permission denied` / `cannot open file for writing` → surface "close running spiderman3.exe" remediation.

Each detected error maps to a **remediation hint** from a bundled knowledge base (the table above).

### 7.3 Resume semantics

State file `build_state.json`:
```json
{
  "schema": 1,
  "started_at": "...",
  "prereq_manifest_hash": "...",
  "phases": {
    "prereq":         {"status": "ok", "hash": "..."},
    "sdk_setup":      {"status": "ok", "source_hash": "...", "fixes": ["xxhash","libmspack","cmake4x"]},
    "runtime_patches":{"status": "ok", "overlay_commit": "<sha>", "required": ["runtime/xam-enum-pending","runtime/xenumerator-success","runtime/xam-ui-headless"], "perf": ["runtime/queue-frames","runtime/rov-barrier-skip"], "build_config": ["runtime/thinlto-ipo"]},
    "runtime_build":  {"status": "failed", "last_error": "...", "cache_key": "..."},
    "game_setup":     {"status": "ok", "codegen_hash": "..."},
    "game_build":     {"status": "pending"},
    "deploy":         {"status": "pending"},
    "verify":         {"status": "pending"}
  }
}
```

- On restart: load state, skip `ok` phases (after a quick re-verify — e.g., dll still exists, hash matches), resume at the first non-`ok` phase.
- `--clean`: wipe build dirs + state, full rebuild.
- `--from <phase>`: force resume from a named phase downstream.
- A `failed` phase is re-run; its build dir is kept unless the failure was a **configure** error (in which case `CMakeCache.txt` is deleted first — stale cache is the usual culprit).

### 7.4 Logging

- One log per phase per run: `logs/<timestamp>-<phase>.log`.
- Combined `logs/<timestamp>-full.log` with phase banners.
- `logs/latest/` copies of the most recent run for easy access.
- Each phase log begins with: env (compiler versions, resolved paths), command, full stdout+stderr, exit code, duration.
- UI: live tail + post-failure "Open log" button.

---

## 8. Caching Design (Consolidated)

### 8.1 Cache layers

1. **Prereq manifest** (`prereq_manifest.json`) — compiler/SDK versions. Cheap to recompute; cached to skip re-probing.
2. **SDK source cache** — the patched SDK source tree. Keyed by upstream version + applied patches. Survives across game builds.
3. **Runtime build cache** — `<runtime_build_dir>/` (CMake + object files + dll). Keyed by patched source hashes (3 required + 2 perf) + `sdk_patches.cmake` + `prereq_manifest.json`. Incremental on small changes.
4. **Codegen cache** — `generated/` dir. Keyed by `default.xex` hash + `rexglue.exe` version. Skip the 30s codegen on re-runs with same ISO.
5. **Game build cache** — `<game_build_dir>/` (Ninja). Keyed by codegen hash + template hash + `prereq_manifest.json`.
6. **Deployment cache** — keep last-good `standalone/` as `standalone.bak/` so a failed redeploy can roll back.

### 8.2 Cache invalidation rules

- **Compiler version change** → invalidate both build caches (object files are ABI-specific). This is why `prereq_manifest_hash` is a cache key input.
- **`default.xex` change** (different ISO) → invalidate codegen + game build (runtime build unaffected — runtime doesn't depend on the ISO).
- **Patch/template change** → invalidate the relevant build (runtime patches → runtime cache; game templates → game cache).
- **`--clean`** → wipe all build caches (keep source cache + prereq manifest).
- **Disk space guard:** if free space on the work drive < 5 GB before a build, warn. Full build artifacts: runtime ~2-4 GB, game ~2-3 GB (92 objects + 118MB exe + intermediate). Recommend 10 GB free.

### 8.3 Where caches live

Default: alongside the manual layout (`C:\tmp\Workspace 1\...`) for back-compat. Configurable via `--workdir`. The app must **not** assume `C:\tmp` — make it relocatable from day one (the manual paths are a dev-machine artifact).

---

## 9. Deployment (Phase 5)

After both build branches succeed:

1. **Stage** into `<deploy>.new/`:
   - `spiderman3.exe` (from game build dir)
   - `rexruntime.dll` (from **runtime build dir** — the custom one, over the prebuilt)
   - `TracyClient.dll` (from prebuilt SDK `bin/` — 0.13.1, do not rebuild unless profiling)
   - `spiderman3.toml` (generated from template; §10)
   - `tracy-profiler.exe` (optional, only if user built it from `thirdparty/tracy/profiler/`)
2. **Create `game/` junction** → extracted ISO files (`mklink /J game <SDK_ROOT>\extracted`). Verify `game\default.xex` resolves. (Per `OnConfigurePaths`, the exe checks `exe_dir\game\default.xex` first, then the hardcoded fallback.)
3. **Create `user_data/` and `user_data\cache\`** directories. **Do NOT pre-populate `cache/shaders/`** — it builds on first launch. **Do NOT create a `Content/` subtree** (TROUBLESHOOTING §3: triggers the UTF-8 crash if runtime patches aren't applied; with patches it's unnecessary).
4. **Verify (Phase 6):** load-test `spiderman3.exe` briefly (launch + 5s + clean kill). A ~20s "Not Responding" window is **normal** on first launch (407-PSO pipeline compile, TROUBLESHOOTING §7) — don't flag it as a failure. Catch only hard crashes within the first 2s (missing DLL, ABI mismatch).
5. **Swap:** rename existing `standalone/` → `standalone.bak/`, rename `standalone.new/` → `standalone/`. Delete `.bak` after configurable delay or on next successful deploy.
6. **Report:** paths, file sizes, junction target, total time.

**Config-mismatch guard (TROUBLESHOOTING §13):** the project is built **Release** → imports `rexruntime.dll`. The custom runtime must also be **Release** (ThinLTO+Release, per BUILD_GUIDE §7c). The automation must NOT deploy a RelWithDebInfo runtime into a Release project (ABI mismatch). Enforce this in deploy: assert the runtime build config matches the game build config.

---

## 10. TOML Configuration Generation

The TOML is **generated** (not copied) so paths are relocatable. Template, grounded in BUILD_GUIDE §6b + RUNTIME_FIXES §6:

```toml
# Spider-Man 3 Recompiled - Runtime Configuration

[GPU]
# ROV render target path — required for menu/UI rendering
render_target_path_d3d12 = "rov"

# Async shader compilation — reduces stutter for new shaders
async_shader_compilation = true

# Disable VSync for uncapped frame rate (60 FPS cap is from guest vblank-wait-2, not VSync)
vsync = false

# Allow variable refresh rate / tearing (FreeSync/G-Sync)
d3d12_allow_variable_refresh_rate_and_tearing = true

# Guest video mode refresh rate (TOML default; OnPreSetup overrides to 120.0 for 60 FPS)
video_mode_refresh_rate = 60.0

[Core]
# Default game data path — allows launching without CLI args
# Overridden at runtime by OnConfigurePaths if exe_dir\game\default.xex exists
game_data_root = "{{GAME_DATA_ROOT}}"
```

`{{GAME_DATA_ROOT}}` filled at deploy (absolute extracted path, or `game\` if junction used). 

**Important design note (RUNTIME_FIXES §6):** the 4 city-rendering cvars (`gamma_render_target_as_unorm16`, `snorm16_render_target_full_range`, `mrt_edram_used_range_clamp_to_min`, `readback_resolve`) + `anisotropic_override` + `swap_post_effect` + `video_mode_refresh_rate=120.0` are **NOT in the TOML** — they are forced only by `OnPreSetup` in `spiderman3_app.h`, deliberately, so users can't break correctness via config. The TOML holds only tunables. The generated TOML must respect this split. Keep the TOML cvar list in sync with `spiderman3_app.h::OnPreSetup` via a test (single source of truth).

---

## 11. Bundle vs. System-Installed Tools

| Tool | Decision | Rationale |
|---|---|---|
| CMake | **System-installed** (winget helper) | Large, version-sensitive, users likely have it |
| LLVM/clang-cl | **System-installed** | Large (~2GB), version-sensitive (need 22.1.8) |
| VS build tools + Win SDK | **System-installed** | Very large (~6GB+), licensing, must match user's VS |
| Ninja | **Bundle** (~200KB) | Tiny, no licensing issue, eliminates a common missing-tool failure (the game build requires `-G Ninja`) |
| RexGlue prebuilt SDK | **User-supplied** (or downloaded if public) | License/availability unclear; app accepts a path or download URL |
| RexGlue SDK source | **User-supplied** or git clone | Same |
| `extract-xiso` | **Bundle** (MIT) | Small, single binary, eliminates a prereq |
| Patch files / src templates | **Bundled in app** | App-owned, versioned with the app |
| Tracy profiler UI | **Optional, build from SDK source** | Only if user opts into profiling; not required to run |
| `TracyClient.dll` | **From prebuilt SDK `bin/`** | Already 0.13.1; don't rebuild unless profiling |

**Conclusion:** bundle the small, unambiguous tools (Ninja, extract-xiso, patches, templates); rely on system installs for the big compiler/SDK stack, with guided `winget` install buttons. Keeps the app download small (~5-10 MB) while removing the most common failure modes (missing Ninja, missing extract-xiso). A "portable VS" bundle (~8GB) is **not recommended** for public release — document the VS Build Tools requirement instead.

---

## 12. Estimated Total Build Time

### 12.1 Fresh build (cold cache), Z1 Extreme (8 cores), parallel runtime‖game

| Phase | Time | Notes |
|---|---|---|
| 0 — Prereq check | ~5s | version probes + vswhere |
| 1 — SDK source setup (clone + submodules + 3 fixes) | ~60-180s | dominated by submodule fetch; varies with network |
| 1 — SDK source setup (from pre-extracted archive + fixes) | ~10s | just apply 3 fixes |
| 3 — Runtime build (6 patches + ThinLTO, ~parallel-4) | ~90-110s | parallel with Phase 2+4 |
| 2 — Game setup (ISO extract ~1min + init + codegen ~30s + templates) | ~90s | parallel with Phase 1+3 |
| 4 — Game build (92 files, clang-cl, Ninja, ~parallel-4) | ~3-4 min | parallel with Phase 3 |
| 5 — Deploy | ~5s | copy + junction + toml + user_data |
| 6 — Verify | ~10s | launch + 5s + kill |
| **Total (clone path, parallel)** | **~4.5-6 min** | critical path = Phase 2+4 |
| **Total (archive path, parallel)** | **~4-5 min** | Phase 1 negligible |
| **Total (serial, for comparison)** | **~6.5-8 min** | |

### 12.2 Incremental rebuild (warm cache, no source changes)

| Phase | Time |
|---|---|
| 0 — Prereq | ~3s |
| 1 — SDK setup | ~1s (hash check) |
| 3 — Runtime | ~3s (CMake no-op + link check) |
| 2 — Game setup | ~1s (codegen cache hit) |
| 4 — Game | ~3s (Ninja no-op) |
| 5 — Deploy | ~5s |
| 6 — Verify | ~10s |
| **Total** | **~25s** |

### 12.3 Partial rebuild scenarios

- **Game template change only** (e.g., tweak a cvar in `spiderman3_app.h`): Ninja recompiles affected file + relinks → **~30-60s**. No codegen, no runtime rebuild.
- **Runtime patch change**: runtime incremental → **~20-40s** + deploy swap.
- **Different ISO** (new `default.xex`): codegen (~30s) + full game build (~3-4min) → **~4-4.5min**. Runtime unaffected (doesn't depend on ISO).

---

## 13. Open Questions for Review

1. **Is the RexGlue SDK public on GitHub?** Determines whether Phase 1 can `git clone` or must accept a user-supplied archive. The working copy isn't a git repo, suggesting archive distribution.
2. **Submodule count discrepancy (brief: 16, `.gitmodules`: 22-32).** Confirm; the app reads `.gitmodules` dynamically so either is fine, but the brief should be corrected.
3. **Does `rexglue codegen` overwrite `src/`?** BUILD_GUIDE says codegen only writes `generated/`, so the template model (write `src/` after init) is safe. Confirm at first integration.
4. **PGO support depth:** expose `--pgo` as a full two-build workflow, or leave as "experimental, not automated"? Recommend: ship ThinLTO default; PGO as a documented-but-manual advanced path for v1.
5. **Vulkan backend:** untested for this game (RUNTIME_FIXES §13). Build system should not preclude it, but no automation for Vulkan cvar retuning.
6. **Distribution:** GitHub Releases (bundled Ninja + extract-xiso + patches + templates) vs. self-extracting installer. Affects how prereq failures surface to non-developer users.

---

## 14. Summary

- **6 phases** (prereq → fork: [SDK setup → runtime build] ‖ [game setup → game build] → join: deploy → verify) with an explicit fork/join graph enabling **parallel runtime+game builds** (~90s saved, critical path ~4min vs ~6.5min serial).
- **Prerequisite checker** uses `vswhere` (not hardcoded paths), verifies the exact toolchain (clang-cl 22.1.8, MSVC 14.44.35207, CMake 4.2.1, Ninja 1.13.2, SDK v0.8.0), writes a reproducibility manifest, offers guided `winget` installs.
- **SDK source setup** handles both git-clone and pre-extracted-archive strategies (the working copy is NOT a git repo — a real constraint), reads submodule count dynamically from `.gitmodules` (not hardcoded), applies 3 build fixes as idempotent sentinel-tracked patches.
- **Runtime build NORMALIZES the buggy hand-script** (`rebuild_runtime_lto.bat` omits `-G Ninja` + `clang-cl` flags and has a mismatched copy path): uses explicit Ninja + clang-cl + `-DREX_GAME_PROFILE=spiderman3` + consistent output path per BUILD_GUIDE §7c. Applies 3 required save-system patches + 2 optional perf patches via flag-guarded overlays; ThinLTO via `sdk_patches.cmake` include() (not a source patch). ThinLTO default; PGO opt-in with mutual-exclusion enforcement.
- **Game build** mirrors `build.bat` (Ninja + clang-cl, `CMAKE_PREFIX_PATH`=prebuilt SDK). Game links prebuilt `rexruntime.lib`; custom DLL swapped at deploy — the key to parallelism.
- **Caching** at 6 layers with explicit invalidation (compiler version, ISO hash, patch/template hash); incremental rebuilds drop from ~6min to ~25s.
- **Error handling** with a 14-entry failure catalog grounded in BUILD_GUIDE + TROUBLESHOOTING (vcvarsall, clang-cl, SDK-not-found, roundevenf, XAM imports, permission-denied, C1060 heap, /EHa, ThinLTO+PGO conflict, black screen, UTF-8 crash, shader cache corruption, RelWithDebInfo mismatch, Tracy version mismatch), log heuristics mapping errors to remediation hints, resumable state file, atomic deploy with rollback.
- **Bundling:** small tools bundled (Ninja, extract-xiso, patches, templates); big stack system-installed with guided setup.
- **Estimated fresh build: ~4.5-6 min (parallel, clone) / ~4-5 min (archive). Incremental: ~25s.**
