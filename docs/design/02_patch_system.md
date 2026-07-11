# Patch System Design — RexGlue360 Recompiler App

> **Status:** Design document for review. No code implemented.
> **Scope:** Programmatic application of all game-specific and general fixes
> to (a) RexGlue360-generated game projects and (b) the RexGlue SDK runtime
> source tree, producing a playable Windows executable from a user-supplied ISO.

---

## 0. The Foundational Decision: Two Patch Categories That Cannot Share a Format

Before specifying any patch format, we must confront the architectural split
that determines everything downstream. The patches we applied manually fall
into two categories that operate on **different trees, at different times, with
different coexistence constraints**:

| | **Game-Project Patches** | **SDK Source Patches** |
|---|---|---|
| **Target tree** | The `rexglue codegen` output (game-specific CMake project) | The RexGlue SDK 0.8.0 source tree (`rexruntime.dll` build) |
| **When applied** | After codegen, before game build | Before runtime build (one-time per SDK version) |
| **Produces** | `spiderman3.exe` (118 MB) | `rexruntime.dll` (custom runtime) |
| **Files** | `xmp_bypass.cpp`, `spiderman3_app.h`, `CMakeLists.txt`, `spiderman3.toml` | `xam_ui.cpp`, `xam_enum.cpp`, `xenumerator.cpp`, `render_target_cache.cpp`, `command_processor.h`, `src/system/CMakeLists.txt` |
| **Per-game?** | Yes — every game gets its own generated project | **The coexistence question** |
| **Regenerated?** | Yes — re-run on every `rexglue codegen` | No — applied once to a persistent SDK checkout |

### 0.1 The Coexistence Question (Answered First)

> *If spiderman3 patches `xam_ui.cpp` and another game needs a different patch
> to the same file, what happens?*

The `xmp_bypass.cpp` header comment itself documents the answer we already
chose in practice:

```
// Runtime fixes (in custom rexruntime.dll):
//   - xeXamEnumerate async path returns X_ERROR_IO_PENDING (not SUCCESS)
//   - WriteItems returns X_ERROR_SUCCESS for 0 items (not ERROR_NO_MORE_FILES)
//   - xeXamDispatchHeadless broadcasts XN_SYS_UI true/false via deferred path
```

The save-system fixes live in a **custom-built `rexruntime.dll`**, not in the
game project. This means the question is real and unavoidable: two games that
each need different `xam_ui.cpp` modifications cannot share one runtime DLL.

**Recommendation: Option (b) — fold SDK patches behind compile-time feature
flags, with a per-game runtime build matrix.** This is the only option that
avoids both fragility (diff-based patching across SDK versions) and divergence
(maintaining N forks of the SDK).

#### Why not (a) per-game SDK copies
- The SDK source tree is ~500 MB with 16+ git submodules. Copying it per game
  is wasteful and makes updating SDK versions a multi-copy migration.
- Most of the SDK is identical across games — only ~6 files differ.

#### Why not (c) per-game forks
- Forks diverge from upstream and become unmaintainable. Every SDK update
  requires rebasing N forks. We already saw this pain: the 3 build fixes
  (xxHash CMake path, libmspack symlinks, CMake 4.x module scan) were upstream
  issues that would need re-applying on every fork.

#### Why (b) wins
- The SDK source tree is shared (one tree, git-init'd and tagged by the
  recompiler app per §4.5 for version-pinning and free reversibility).
- Each game's patches are expressed as **CMake preprocessor defines** that
  guard the modified code paths. The runtime is rebuilt once per game-profile
  with that game's flag combination.
- General patches (kQueueFrames, ROV barrier skip) are **always-on flags** that
  benefit all games — they don't need per-game gating.
- Game-specific patches (save-system XAM fixes) are **opt-in flags** enabled
  only in that game's runtime build profile.

**The patch format for SDK source patches is therefore NOT diff-based.** It is
a **templated source overlay + CMake flag** system. This is the single most
important architectural decision in this document.

### 0.2 Patch Category Taxonomy

All patches are tagged with a `category` field in the patch manifest:

| Category | Description | Shared across games? | Example |
|---|---|---|---|
| `general-runtime` | SDK optimization/fix applicable to all games | Yes — always-on flag | `kQueueFrames = 2`, ROV barrier skip, ThinLTO/IPO |
| `game-runtime` | SDK fix required by a specific game's behavior | No — per-game flag | XAM save-system fixes (xam_ui, xam_enum, xenumerator) |
| `game-project` | Fix to the generated game project | No — per-game template | xmp_bypass.cpp, spiderman3_app.h cvars |
| `game-config` | Runtime configuration file | No — per-game template | spiderman3.toml |
| `game-build` | Build system modification for the game project | Partially — pattern reusable | CMakeLists.txt source additions |
| `sdk-build-fix` | Build system fix for the SDK source tree itself | Yes — always applied | xxHash CMake path, libmspack symlinks, CMake 4.x scan |

This taxonomy drives:
1. **Which patches go into the shared runtime flag set** vs. the per-game flag set.
2. **Which patches are applied to the game project** vs. the SDK source tree.
3. **Versioning scope** — general patches version with the SDK; game patches
   version with (SDK version + game version).

---

## 1. Patch Format Specification

### 1.1 Three Patch Sub-Formats

Because the two categories have fundamentally different application mechanics,
the patch system uses **three sub-formats**, each chosen for its target tree:

| Sub-format | Used for | Why |
|---|---|---|
| **A. Templated source files** | `xmp_bypass.cpp`, `spiderman3_app.h`, `spiderman3.toml` | These are entirely custom files added to the game project. No diff needed — they're generated from templates with game-specific parameters. |
| **B. Flag-guarded source overlays** | SDK source patches (xam_ui, xam_enum, xenumerator, render_target_cache, command_processor.h) | These modify existing SDK files. Rather than diffing (fragile across SDK versions), we ship overlay files that are `#include`-ed or `#ifdef`-gated behind CMake-defined flags. |
| **C. CMake property injections** | CMakeLists.txt modifications (ThinLTO/IPO, source list additions) | These are additive CMake properties applied via a generated `.cmake` fragment included by the target's CMakeLists.txt. |

### 1.2 Format A — Templated Source Files

**When to use:** The patch is a completely custom file that didn't exist in the
generated project (xmp_bypass.cpp) or replaces the generated app header
entirely (spiderman3_app.h).

**Format:** A Jinja2/Inja template (the SDK already vendors `inja` as a
submodule — reuse it) with a YAML/TOML parameter block.

```
patches/
  spiderman3/
    xmp_bypass.cpp.inja
    xmp_bypass.params.toml
    spiderman3_app.h.inja
    spiderman3_app.params.toml
```

**Template example (`xmp_bypass.cpp.inja`):**

```cpp
// {{ game_name }} - Save system fix: synchronous SUCCESS + async XN_SYS_UI lifecycle
//
// Device selector: returns SUCCESS with device_id={{ device_id }} ({{ device_name }})
//   synchronously, broadcasts XN_SYS_UI=true immediately, spawns a detached
//   thread that broadcasts XN_SYS_UI=false after {{ ui_false_delay_ms }}ms
//   (for the late-created listener).
//
// Runtime fixes (in custom rexruntime.dll, gated by {{ runtime_flag }}):
//   - xeXamEnumerate async path returns X_ERROR_IO_PENDING (not SUCCESS)
//   - WriteItems returns X_ERROR_SUCCESS for 0 items (not ERROR_NO_MORE_FILES)
//   - xeXamDispatchHeadless broadcasts XN_SYS_UI true/false via deferred path

#include "generated/default/{{ game_id }}_init.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <thread>
#include <chrono>

REX_HOOK_RAW(XMPGetStatus_Wrapper) {
  uint32_t status_ptr = ctx.r3.u32;
  REX_STORE_U32(status_ptr, 0);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(XamContentGetDeviceState_Wrapper) {
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(XamShowDeviceSelectorUI_Wrapper) {
  uint32_t device_id_ptr = ctx.r7.u32;
  uint32_t overlapped_ptr = ctx.r8.u32;

  if (device_id_ptr != 0) {
    REX_STORE_U32(device_id_ptr, {{ device_id }});
  }
  if (overlapped_ptr != 0) {
    REX_STORE_U32(overlapped_ptr + 0, 0);
  }

  REX_KERNEL_STATE()->BroadcastNotification(0x9, true);

  std::thread([]() {
    std::this_thread::sleep_for(std::chrono::milliseconds({{ ui_false_delay_ms }}));
    REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
  }).detach();

  ctx.r3.u64 = 0;
}
```

**Parameter file (`xmp_bypass.params.toml`):**

```toml
# Parameters for xmp_bypass.cpp template
# These are the game-specific tunables for the save-system device selector hook.
game_id = "spiderman3"
game_name = "Spider-Man 3"
device_id = 1            # Xbox 360 HDD device ID
device_name = "HDD"
ui_false_delay_ms = 200  # Delay for late-created notification listener
runtime_flag = "REX_GAME_SPIDERMAN3_SAVE_FIX"
```

**Why templating, not diff:** `xmp_bypass.cpp` is a net-new file. There is no
"original" to diff against. The template captures the exact 3-hook structure
while parameterizing the values that could differ per game (device ID, delay
timing, the runtime flag name that must match the SDK overlay's `#ifdef`).

### 1.3 Format B — Flag-Guarded Source Overlays (SDK Source Patches)

**When to use:** Modifying existing SDK source files (xam_ui.cpp, xam_enum.cpp,
xenumerator.cpp, render_target_cache.cpp, command_processor.h).

**The problem with diffs here:** The SDK source was distributed as a tarball
(no `.git` directory — verified). There is no baseline to `git diff` against.
Even if there were, unified diffs are fragile: any upstream change to context
lines breaks application. With 6 patches across 5 files and a 500 MB tree with
16 submodules, re-applying diffs on every SDK update is a maintenance
nightmare.

**The solution:** Ship the patched code as **overlay fragments** that are
compiled into the SDK via CMake-defined preprocessor flags. The SDK source
files are modified once (manually, as we did) to add `#ifdef` guards around
the patched sections, and the overlay is enabled by passing
`-DREX_GAME_<GAME>_SAVE_FIX=ON` (or similar) at CMake configure time.

**Design: Two-layer overlay system**

```
patches/
  sdk-overlays/                    # Ships with the recompiler app
    rexglue-sdk-0.8.0/
      save_system/
        xam_ui_headless_overlay.h       # The patched xeXamDispatchHeadless body
        xam_enum_overlay.h             # The patched xeXamEnumerate body
        # xenumerator.cpp: inline #ifdef (single-token change, no overlay header)
      general/
        rov_barrier_skip.h             # The EDRAM barrier skip logic
        queue_frames.h                 # kQueueFrames = 2 constant
    CMakeLists.txt.patch              # CMake flag definitions + overlay includes
```

**How a single patch is expressed — example: `xam_ui.cpp` headless dispatch**

The original SDK code (lines 161-180 of `xam_ui.cpp`) was:

```cpp
X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, uint32_t overlapped) {
  if (!overlapped) {
    auto result = run_callback();
    return result;
  } else {
    // [original: just CompleteOverlappedDeferred with no pre/post callbacks]
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run_callback, overlapped);
    return X_ERROR_IO_PENDING;
  }
}
```

Our patch added XN_SYS_UI pre/post notification callbacks to the async path.
Instead of shipping this as a diff, the overlay system works as follows:

**Step 1 (one-time SDK modification):** The SDK source file is modified to
include an overlay header behind a flag:

```cpp
// In xam_ui.cpp (one-time edit, shipped as part of the recompiler's SDK prep):
#if defined(REX_GAME_SAVE_SYSTEM_FIX)
#include "patches/sdk-overlays/rexglue-sdk-0.8.0/save_system/xam_ui_headless_overlay.h"
#endif

X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, uint32_t overlapped) {
#if defined(REX_GAME_SAVE_SYSTEM_FIX)
  return xeXamDispatchHeadless_Patched(run_callback, overlapped);
#else
  if (!overlapped) {
    auto result = run_callback();
    return result;
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run_callback, overlapped);
    return X_ERROR_IO_PENDING;
  }
#endif
}
```

**Step 2 (overlay file):** `xam_ui_headless_overlay.h` contains the patched
implementation as a standalone function:

```cpp
// patches/sdk-overlays/rexglue-sdk-0.8.0/save_system/xam_ui_headless_overlay.h
// Patched xeXamDispatchHeadless: async path with XN_SYS_UI pre/post callbacks.
// Enabled by -DREX_GAME_SAVE_SYSTEM_FIX=ON at CMake configure time.
#pragma once
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/thread.h>

namespace rex::kernel::xam {

inline X_RESULT xeXamDispatchHeadless_Patched(
    std::function<X_RESULT()> run_callback, uint32_t overlapped) {
  REXKRNL_DEBUG("xeXamDispatchHeadless: called, overlapped=0x{:08X}", overlapped);
  if (!overlapped) {
    REXKRNL_DEBUG("xeXamDispatchHeadless: sync path (no overlapped)");
    auto result = run_callback();
    return result;
  } else {
    REXKRNL_DEBUG("xeXamDispatchHeadless: async path, deferring with XN_SYS_UI notifications");
    auto pre = []() {
      REX_KERNEL_STATE()->BroadcastNotification(0x9, true);
    };
    auto post = []() {
      rex::thread::Sleep(std::chrono::milliseconds(100));
      REX_KERNEL_STATE()->BroadcastNotification(0x9, false);
    };
    REX_KERNEL_STATE()->CompleteOverlappedDeferred(run_callback, overlapped, pre, post);
    REXKRNL_DEBUG("xeXamDispatchHeadless: deferred queued, returning IO_PENDING");
    return X_ERROR_IO_PENDING;
  }
}

}  // namespace rex::kernel::xam
```

**Why this format:**
- **Reversible:** Don't define the flag → original code compiles. The `#else`
  branch is the unmodified SDK code.
- **Version-tolerant:** The overlay is a self-contained function. If the SDK
  updates `xeXamDispatchHeadless`'s signature, we update the overlay header —
  no context-line matching, no diff conflicts.
- **Composable:** Multiple games can enable the same flag (e.g.,
  `REX_GAME_SAVE_SYSTEM_FIX` if another game has the same save-system issue).
  Different games can define different flags for different fixes.
- **Inspectable:** The overlay file is readable C++ code, not a diff. A
  maintainer can read the patched logic directly.

**Full mapping of all 6 SDK source patches to overlay files:**

| Patch | File | Flag | Category | Overlay file |
|---|---|---|---|---|
| Headless dispatch XN_SYS_UI | `xam_ui.cpp` L161-180 | `REX_GAME_SAVE_SYSTEM_FIX` | game-runtime | `save_system/xam_ui_headless_overlay.h` |
| Enumerate async returns IO_PENDING | `xam_enum.cpp` L65-76 | `REX_GAME_SAVE_SYSTEM_FIX` | game-runtime | `save_system/xam_enum_overlay.h` |
| WriteItems returns SUCCESS for 0 items | `xenumerator.cpp` L55-76 | `REX_GAME_SAVE_SYSTEM_FIX` | game-runtime | inline `#ifdef` (no overlay header) |
| EDRAM barrier skip for read-only draws | `render_target_cache.cpp` L1081-1095 | `REX_ROV_BARRIER_SKIP` | general-runtime | `general/rov_barrier_skip.h` |
| kQueueFrames = 2 | `command_processor.h` L231 | `REX_QUEUE_FRAMES_2` | general-runtime | `general/queue_frames.h` |
| ~~XNotifyGetNext DEBUG logging~~ | `xam_notify.cpp` | — | — | **NOT A PATCH** — `REXKRNL_DEBUG` is standard logging; `@modified` header is on all SDK files. Not in user's 6-modification scope. Excluded from patch set. |

### 1.4 Format C — CMake Property Injections

**When to use:** Modifying CMakeLists.txt files (both game project and SDK
source) to add build properties, source files, or link flags.

**The problem:** The game project's `CMakeLists.txt` is generated by
`rexglue codegen`. Any manual edits are lost on re-codegen. The SDK's
`src/system/CMakeLists.txt` is part of the SDK tree and shouldn't be
permanently modified.

**The solution:** Inject a generated `.cmake` fragment that is `include()`-ed
by the target CMakeLists.txt. The patch system adds a single `include()` line
to the generated CMakeLists.txt (via a codegen post-step or a sed-like
injection), and all patch properties live in the fragment.

**Game project CMake injection (`patches.cmake`):**

```cmake
# Auto-generated by patch system — included by CMakeLists.txt
# Do not edit manually; regenerate with: recomp apply-patches --game spiderman3

# === Source additions ===
# xmp_bypass.cpp — save system device selector hooks
target_sources(${PROJECT_NAME} PRIVATE src/xmp_bypass.cpp)

# === Compile options ===
# /EHa — async exception handling (required by RexGlue hook infrastructure)
target_compile_options(${PROJECT_NAME} PRIVATE /EHa)

# === ThinLTO / IPO for Release builds ===
# NOTE: This applies to the GAME EXE, not the runtime DLL.
# The runtime DLL's IPO is handled by the SDK source CMake injection below.
set_target_properties(${PROJECT_NAME} PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
)
```

**SDK source CMake injection (`sdk_patches.cmake`):**

This fragment is included by the SDK's `src/system/CMakeLists.txt` (via a
one-time `include()` addition, same pattern as the game project):

```cmake
# Auto-generated by patch system — SDK runtime build modifications
# Included by src/system/CMakeLists.txt

# === ThinLTO / IPO for rexruntime.dll ===
# Already present in the SDK at lines 54-57, but we ensure it's set
# regardless of SDK version by re-applying here.
set_target_properties(rexruntime PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
)

# === Overlay compile definitions ===
# These flags enable the #ifdef-guarded overlay code in SDK source files.
# General patches (always-on for all games):
target_compile_definitions(rexruntime PRIVATE
    REX_ROV_BARRIER_SKIP=1
    REX_QUEUE_FRAMES_2=1
)

# Game-specific patches (enabled per game-profile):
if(REX_GAME_PROFILE STREQUAL "spiderman3")
    target_compile_definitions(rexruntime PRIVATE
        REX_GAME_SAVE_SYSTEM_FIX=1
    )
endif()

# === Overlay include paths ===
# So #include "patches/sdk-overlays/..." resolves from the SDK source tree.
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/../patches/sdk-overlays
)
```

**Why injection, not diff:** The generated `CMakeLists.txt` is only 29 lines
and is fully regenerated on every `rexglue codegen`. Injecting an `include()`
line is a one-line sed/regex post-step. All patch logic lives in the
fragment, which is version-controlled separately and survives re-codegen.

**The generated CMakeLists.txt after patch injection:**

```cmake
# spiderman3 - ReXGlue Recompiled Project
# ... (generated boilerplate) ...
rexglue_setup_target(spiderman3)

# === Patch system injection ===
include(${CMAKE_CURRENT_SOURCE_DIR}/patches.cmake)
# === End patch injection ===
```

That single `include()` line is the only modification to the generated file.
Everything else lives in `patches.cmake`.

---

## 2. The Cvar Database / Preset System

### 2.1 Problem Statement

`spiderman3_app.h`'s `OnPreSetup()` method calls `rex::cvar::SetFlagByName()`
for 17 cvars. These are not arbitrary — each was root-caused through SDK source
analysis and falls into a specific category:

| Cvar group | Cvars | Purpose | Game-specific? |
|---|---|---|---|
| Render path | `render_target_path_d3d12 = "rov"` | Fixes black menu | Likely general for ROV games |
| FPS unlock | `video_mode_refresh_rate = "120.0"` | 60 FPS via 120Hz vblank × 2 | Game-specific (depends on game's frame timing) |
| City rendering | `gamma_render_target_as_unorm16`, `snorm16_render_target_full_range`, `mrt_edram_used_range_clamp_to_min`, `readback_resolve = "fast"` | Fixes city rendering artifacts | Game-specific (depends on game's render target formats) |
| Visual quality | `anisotropic_override = "5"`, `swap_post_effect = "fxaa"` | Texture + AA enhancements | User-tunable, not game-required |
| Performance | `host_present_from_non_ui_thread`, `d3d12_bindless`, `d3d12_tiled_shared_memory`, `d3d12_submit_on_primary_buffer_end = "false"`, `d3d12_pipeline_creation_threads = "2"` | Performance optimizations | General (informed by SDK source, safe for all games) |
| Readback | `readback_memexport`, `readback_memexport_fast` | EDRAM readback fast path | Likely general |
| Texture cache | `texture_cache_memory_limit_hard = "4096"`, soft = "2048", lifetime = "120", RT = "256" | Prevents mid-frame paging on APU | Hardware-dependent, with sensible defaults |
| Output quality | `use_fuzzy_alpha_epsilon`, `present_dither`, `store_shaders` | Visual + shader cache | General |

### 2.2 Cvar Preset Format

A cvar preset is a TOML file that maps cvar names to values, grouped by
category, with metadata for each entry:

```toml
# patches/spiderman3/cvar_preset.toml
# Cvar preset for Spider-Man 3 (Xbox 360) recompilation.
# Applied in OnPreSetup() via rex::cvar::SetFlagByName().

[preset]
id = "spiderman3"
name = "Spider-Man 3 Cvar Preset"
sdk_version = "0.8.0"
game_version = "1.0"
description = "Performance-optimized configuration with SDK source-informed cvars"

# === Render path ===
[cvars.render_target_path_d3d12]
value = "rov"
category = "render-path"
required = true          # Game is broken without this
description = "ROV render target path — fixes black menu"
incompatible = ["kHostRenderTargets"]  # Mutually exclusive alternatives

# === FPS unlock ===
[cvars.video_mode_refresh_rate]
value = "120.0"
category = "fps"
required = false
description = "60 FPS unlock via 120Hz vblank × 2"
depends_on = []           # No dependencies

# === City rendering fixes ===
[cvars.gamma_render_target_as_unorm16]
value = "true"
category = "render-fix"
required = true
description = "City rendering fix: gamma as unorm16"

[cvars.snorm16_render_target_full_range]
value = "true"
category = "render-fix"
required = true
description = "City rendering fix: snorm16 full range"

[cvars.mrt_edram_used_range_clamp_to_min]
value = "true"
category = "render-fix"
required = true
description = "City rendering fix: MRT EDRAM range clamp"

[cvars.readback_resolve]
value = "fast"
category = "render-fix"
required = false
description = "EDRAM readback — proven good, some visual artifacts"

# === Visual enhancements ===
[cvars.anisotropic_override]
value = "5"
category = "visual-quality"
required = false
description = "Texture anisotropic filter override"
user_tunable = true       # Exposed in UI for user adjustment

[cvars.swap_post_effect]
value = "fxaa"
category = "visual-quality"
required = false
description = "Anti-aliasing post-effect"
user_tunable = true

# === Performance optimizations ===
[cvars.host_present_from_non_ui_thread]
value = "true"
category = "performance"
required = false
description = "Decouple presentation from UI thread"

[cvars.d3d12_bindless]
value = "true"
category = "performance"
required = false
description = "Bindless resource binding (confirmed safe)"

[cvars.d3d12_tiled_shared_memory]
value = "true"
category = "performance"
required = false
description = "Tiled shared memory (confirmed safe)"

[cvars.d3d12_submit_on_primary_buffer_end]
value = "false"
category = "performance"
required = false
description = "Batch ECLs — reduces UAV barrier count on EDRAM buffer"

[cvars.d3d12_pipeline_creation_threads]
value = "2"
category = "performance"
required = false
description = "Avoid PSO lock contention (default 6 threads contends on driver lock)"

# === Readback memexport ===
[cvars.readback_memexport]
value = "true"
category = "performance"
required = false

[cvars.readback_memexport_fast]
value = "true"
category = "performance"
required = false

# === Texture cache ===
[cvars.texture_cache_memory_limit_hard]
value = "4096"
category = "texture-cache"
required = false
description = "Keep city texture set resident (MB). 0=unlimited causes mid-frame paging"

[cvars.texture_cache_memory_limit_soft]
value = "2048"
category = "texture-cache"

[cvars.texture_cache_memory_limit_soft_lifetime]
value = "120"
category = "texture-cache"

[cvars.texture_cache_memory_limit_render_to_texture]
value = "256"
category = "texture-cache"

# === Output quality ===
[cvars.use_fuzzy_alpha_epsilon]
value = "true"
category = "output-quality"

[cvars.present_dither]
value = "true"
category = "output-quality"

[cvars.store_shaders]
value = "true"
category = "output-quality"
description = "Store PSOs to .xpso file for zero stutter on future launches"
```

### 2.3 Cvar Preset → App Header Generation

The app header (`spiderman3_app.h`) is generated from two inputs:
1. A **base template** (`app.h.inja`) — the class skeleton with `OnConfigurePaths`
   and `OnPreSetup` method stubs.
2. The **cvar preset TOML** (above).

**Template (`app.h.inja`):**

```cpp
// {{ game_id }} - ReXGlue Recompiled Project
// {{ preset_description }}

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <filesystem>

class {{ app_class_name }} : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<{{ app_class_name }}>(new {{ app_class_name }}(ctx, "{{ game_id }}",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    auto exe_dir = rex::filesystem::GetExecutableFolder();

    if (paths.game_data_root.empty()) {
      auto game_subdir = exe_dir / "game";
      if (std::filesystem::exists(game_subdir / "default.xex")) {
        paths.game_data_root = game_subdir;
      } else {
        paths.game_data_root = "{{ fallback_game_data_path }}";
      }
    }

    paths.user_data_root = exe_dir / "user_data";
    paths.cache_root = exe_dir / "user_data" / "cache";
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
{% for cvar in cvars %}
    // === {{ cvar.category }} ===
    rex::cvar::SetFlagByName("{{ cvar.name }}", "{{ cvar.value }}");
{% endfor %}
  }
};
```

**Why a preset database, not hardcoded cvars:**
- **Extensibility:** Adding a cvar for a new game is a TOML edit, not a C++ edit.
- **User tunability:** Cvars tagged `user_tunable = true` can be exposed in the
  recompiler app's UI as sliders/toggles, letting users adjust visual quality
  without editing source.
- **Validation:** The preset loader can check `depends_on` and `incompatible`
  constraints before generating the header, catching configuration errors
  early.
- **Documentation:** Each cvar entry carries a `description` and `category`,
  auto-generating a cvar reference doc.
- **Cross-game reuse:** Performance cvars (bindless, tiled shared memory,
  pipeline threads) can be shared as a "general performance" preset that
  multiple games include.

### 2.4 Path Configuration

`OnConfigurePaths` in `spiderman3_app.h` has a hardcoded fallback path:
```cpp
paths.game_data_root = "C:\\tmp\\Workspace 1\\RexGlue360Recomp\\extracted";
```

This is a **dev-machine path** that must be parameterized. The template uses
`{{ fallback_game_data_path }}`, which the recompiler app sets to a sensible
default (e.g., empty string, causing the app to error with "game data not
found" rather than silently using a wrong path). For distribution, the
`game/` junction approach is the correct portable solution — the fallback
exists only for development.

---

## 3. TOML Configuration Generator

### 3.1 The Runtime TOML

The `spiderman3.toml` file is the **runtime** configuration read by the built
executable at launch. It differs from the cvar preset (which is a
**build-time** configuration baked into the app header). The TOML contains
cvars that can be changed without rebuilding — the user can edit the TOML to
override build-time defaults.

### 3.2 Template (`runtime.toml.inja`)

```toml
# {{ game_name }} Recompiled - Runtime Configuration
# Generated by RexGlue360 Recompiler App
# This file overrides build-time cvar defaults. Edit freely.

[GPU]
{% for cvar in gpu_cvars %}
# {{ cvar.description }}
{{ cvar.name }} = {{ cvar.toml_value }}
{% endfor %}

[Core]
# User data and cache paths — relative to exe location for portability
user_data_root = "user_data"
cache_path = "user_data\\cache"

[Paths]
# Game data root — set to the path where your extracted ISO files are.
# The recompiler app sets this automatically. Override if you move the game.
game_data_root = "{{ game_data_root }}"
```

### 3.3 Parameter File (`runtime_toml.params.toml`)

```toml
game_name = "Spider-Man 3"
game_data_root = ""  # Set by recompiler app at deployment time

[[gpu_cvars]]
name = "render_target_path_d3d12"
toml_value = '"rov"'
description = "ROV render target path — required for menu/UI rendering"

[[gpu_cvars]]
name = "video_mode_refresh_rate"
toml_value = 120.0
description = "60 FPS unlock: present waits 2 vblanks, 120Hz = 60 FPS"

[[gpu_cvars]]
name = "async_shader_compilation"
toml_value = true
description = "Async shader compilation — reduces stutter for new shaders"

[[gpu_cvars]]
name = "vsync"
toml_value = false
description = "Disable VSync for uncapped frame rate"

[[gpu_cvars]]
name = "d3d12_allow_variable_refresh_rate_and_tearing"
toml_value = true
description = "Allow variable refresh rate / tearing"

[[gpu_cvars]]
name = "gamma_render_target_as_unorm16"
toml_value = true
description = "City rendering fix: gamma"

[[gpu_cvars]]
name = "snorm16_render_target_full_range"
toml_value = true
description = "City rendering fix: snorm16"

[[gpu_cvars]]
name = "mrt_edram_used_range_clamp_to_min"
toml_value = true
description = "City rendering fix: MRT EDRAM"

[[gpu_cvars]]
name = "readback_resolve"
toml_value = '"fast"'
description = "EDRAM readback"

[[gpu_cvars]]
name = "swap_post_effect"
toml_value = '"fxaa"'
description = "FXAA anti-aliasing"
```

### 3.4 TOML vs Cvar Preset — Relationship

The TOML and the cvar preset serve different roles:

| | Cvar Preset (build-time) | Runtime TOML (launch-time) |
|---|---|---|
| Applied in | `OnPreSetup()` — compiled into the exe | Read by rexruntime at launch |
| Can be changed | Only by rebuilding | By editing the TOML file |
| Contains | All 17 cvars (including non-GPU) | GPU + Core + Paths sections only |
| Purpose | Guaranteed-correct defaults | User-overridable runtime config |

The TOML generator should produce a **subset** of the cvar preset — only the
cvars that make sense to override at runtime (GPU, paths, core). The build-time
preset contains everything (including internal performance cvars that
shouldn't be user-tunable).

---

## 4. Versioning Strategy

### 4.1 Three Version Axes

Patches are versioned across three axes:

```
patch = f(sdk_version, game_version, patch_revision)
```

| Axis | What it tracks | Example |
|---|---|---|
| `sdk_version` | Which RexGlue SDK the patch targets | `0.8.0` |
| `game_version` | Which game build (XEX version) the patch targets | `1.0` (Spider-Man 3 NTSC) |
| `patch_revision` | The patch itself — incremented when the patch logic changes | `3` |

### 4.2 Version Constraints in the Patch Manifest

Each patch file carries a version constraint block:

```toml
# In the patch manifest (patches/spiderman3/manifest.toml)

[versions]
sdk = ">=0.8.0, <0.9.0"     # SDK major.minor must match; patch level flexible
game = "1.0"                  # Specific game version (XEX hash or version string)
patch_revision = 3            # This patch set's revision

[compatibility]
# If the SDK updates and a patch is no longer needed (fixed upstream),
# express that here so the patch system skips it with a warning.
min_sdk_where_obsolete = "0.9.0"
obsolete_message = "XN_SYS_UI dispatch fixed in SDK 0.9.0 — patch no longer needed"
```

### 4.3 How Version Checks Work at Apply Time

```
1. Read patch manifest
2. Check sdk_version constraint against installed SDK version
   → If SDK too old: ERROR "Patch requires SDK >= 0.8.0, found 0.7.x"
   → If SDK too new: WARN "Patch targets SDK <0.9.0, found 0.9.x — may not apply cleanly"
   → If SDK in obsolescence range: SKIP with message
3. Check game_version against the XEX's reported version
   → If mismatch: ERROR "Patch targets game version 1.0, found 1.1"
4. Record applied patch revision in a stamp file (see §5)
```

### 4.4 Why Three Axes

- **SDK version:** General-runtime patches (kQueueFrames, ROV barrier skip)
  are tightly coupled to SDK internals. An SDK update might rename
  `kQueueFrames` or refactor `MarkEdramBufferModified`. The overlay must match.
- **Game version:** The save-system hooks target specific function signatures
  from the XEX. A different game version (e.g., GOTY edition, region variant)
  might have different export names or addresses.
- **Patch revision:** If we discover a better fix for the save system (e.g.,
  300ms delay instead of 200ms), we bump the revision. The stamp file (§5)
  tracks which revision was applied so re-runs know whether to re-apply.

### 4.5 SDK Source Tree Versioning — Git-Init the Pristine Tarball

**The tarball reality:** The SDK source on disk (`rexglue-sdk-0.8.0 Source code`)
is an extracted release tarball. It has `.gitmodules` and `.gitattributes` but
**no `.git` directory** (verified — `git log` fails with "not a git
repository"). The submodules were populated (their content is present) but the
top-level tree itself has no version-control history. This means:

- There is no baseline commit to `git diff` against.
- There is no `git apply` / `git stash` / `git revert` for free.
- "SDK version" cannot be anchored to a git commit hash.

**Decision: Step one of SDK prep is `git init` the pristine tarball.**

Before any patches touch the SDK source tree, the recompiler app runs:

```bash
cd "rexglue-sdk-0.8.0 Source code"
git init
git add -A
git commit -m "rexglue-sdk 0.8.0 pristine (release tarball)"
git tag sdk-0.8.0-pristine
```

This creates a baseline commit tagged `sdk-0.8.0-pristine`. All subsequent SDK
prep modifications (the `#ifdef` guard insertions, CMake include injections,
overlay header copies) are committed on top:

```bash
git checkout -b recomp-prep
# ... apply #ifdef guards, copy overlays, inject CMake includes ...
git add -A
git commit -m "recomp SDK prep: overlays + flags for spiderman3 (rev 1)"
git tag sdk-0.8.0-recomp-r1
```

**Why git-init, not content-hash anchoring:**

| Approach | Pros | Cons |
|---|---|---|
| **(a) Git-init + tag** | Free `git diff`/`git apply`/`git stash`/`git revert`; `git tag` gives human-readable version anchors; `git log` shows prep history; reversibility is `git checkout sdk-0.8.0-pristine -- .` | Adds ~50 MB `.git` to the ~500 MB tree; git-init on a large tree takes ~30 seconds |
| **(b) Content hash only** | No git overhead; stamp file tracks hashes | No free reversibility; must implement custom find-and-revert logic; no diff visualization; no history |

**We choose (a).** The 50 MB `.git` overhead is negligible relative to the
500 MB tree. The free `git stash`/`git checkout` reversibility directly
satisfies the "reversible" acceptance criterion without custom revert logic.
The tag `sdk-0.8.0-pristine` is the immutable baseline; the tag
`sdk-0.8.0-recomp-rN` marks each prep revision.

**The stamp file supplements the git tags, not replaces them.** The git tags
are the primary version anchor; the stamp file records which overlays were
applied (for the patch manifest's apply/skip logic and for detecting manual
edits to the prepped tree).

```toml
# rexglue-sdk-0.8.0/.recomp_sdk_prep
sdk_version = "0.8.0"
git_baseline_tag = "sdk-0.8.0-pristine"
git_prep_tag = "sdk-0.8.0-recomp-r1"
prep_date = "2026-07-07T12:00:00Z"
prep_revision = 1
applied_overlays = [
    "save_system/xam_ui_headless_overlay.h",
    "save_system/xam_enum_overlay.h",
    # xenumerator.cpp: inline #ifdef (no overlay header)
    "general/rov_barrier_skip.h",
    "general/queue_frames.h",
]
applied_cmake_injections = [
    "src/system/CMakeLists.txt",
]
```

**Version check at apply time** reads the git tag:
```bash
git tag --list "sdk-*-pristine"   # → which SDK version is this tree?
git tag --list "sdk-*-recomp-r*"  # → which prep revisions exist?
```

If the pristine tag doesn't exist, the SDK tree hasn't been git-init'd yet —
the recompiler app runs the git-init step first. If a recomp-rN tag exists
with the same prep_revision as the manifest, prep is already done — skip.

**Re-running prep with a new patch revision:**
```bash
git checkout sdk-0.8.0-pristine -- .   # Reset to pristine
# ... apply new overlay set ...
git add -A
git commit -m "recomp SDK prep: overlays + flags for spiderman3 (rev 2)"
git tag sdk-0.8.0-recomp-r2
```

This is clean, atomic, and uses git's own versioning rather than a parallel
stamp-file scheme. The stamp file remains for the patch manifest's consumption
(which overlays are applied), but git is the source of truth for tree state.

---

## 5. Reversibility Strategy

### 5.1 Game-Project Patches (Formats A, C)

Game-project patches are **fully reversible by design**: the entire game
project is regenerated by `rexglue codegen`. To "revert" patches:

1. Re-run `rexglue codegen` → regenerates pristine `CMakeLists.txt` and
   `spiderman3_app.h` (without patches).
2. Delete the generated `patches.cmake` and `xmp_bypass.cpp`.
3. Re-apply patches with `recomp apply-patches --game spiderman3`.

The patch system records what it applied in a stamp file:

```toml
# spiderman3/.recomp_patches_applied
game = "spiderman3"
sdk_version = "0.8.0"
game_version = "1.0"
patch_revision = 3
applied_at = "2026-07-07T12:30:00Z"
applied_files = [
    "src/xmp_bypass.cpp",        # created (Format A)
    "src/spiderman3_app.h",      # generated (Format A)
    "CMakeLists.txt",            # modified (include line added, Format C)
    "patches.cmake",             # created (Format C)
    "spiderman3.toml",           # generated (Format A)
]
original_cmakelists_hash = "sha256:..."  # For detecting manual edits
```

**Revert procedure:**
```
recomp revert-patches --game spiderman3
  → Delete files in applied_files that were "created"
  → Restore CMakeLists.txt from hash (or re-codegen)
  → Delete the stamp file
```

### 5.2 SDK Source Patches (Format B) — Git-Anchored Reversibility

With the git-init decision from §4.5, SDK source patch reversibility is a
single git command. The `sdk-0.8.0-pristine` tag is the immutable baseline;
the `sdk-0.8.0-recomp-rN` tag marks each prep revision.

**Revert to pristine (full rollback):**
```bash
cd "rexglue-sdk-0.8.0 Source code"
git checkout sdk-0.8.0-pristine -- .
# Tree is now exactly the release tarball state. No #ifdef guards, no overlays.
```

**Revert to a specific prep revision:**
```bash
git checkout sdk-0.8.0-recomp-r1 -- .
```

**Re-apply prep from scratch (new patch revision):**
```bash
git checkout sdk-0.8.0-pristine -- .   # Reset to pristine
# ... apply new overlay set ...
git add -A
git commit -m "recomp SDK prep: overlays + flags for spiderman3 (rev 2)"
git tag sdk-0.8.0-recomp-r2
```

**Detect manual edits to the prepped tree:**
```bash
git status --porcelain
# If any files show as modified, someone edited the prepped tree outside
# the patch system. git stash or git checkout restores the tagged state.
```

This is strictly better than the stamp-file-only approach: git gives us
atomic rollback, diff visualization (`git diff sdk-0.8.0-pristine`), edit
detection (`git status`), and history (`git log`) for free. The stamp file
remains only for the patch manifest's apply/skip logic (it records *which*
overlays are applied, not the tree state — git owns tree state).

**Important:** The one-time SDK modification (adding `#ifdef` guards) is
applied by a **find-and-wrap** operation (see §7.3) that searches for a
unique function signature and wraps the body in `#ifdef`/`#else`/`#endif`.
This is robust against minor upstream changes because it anchors on the
signature, not context lines. If the anchor is not found, the patch system
reports an error prompting a wrap-spec update for the new SDK version.

### 5.3 Why Not Pure Diffs (unified `.patch` files)

We considered unified diff format for all patches. We rejected it for SDK
source patches because:

1. **Git baseline now exists (§4.5):** The git-init step creates
   `sdk-0.8.0-pristine`, so `git diff` could technically produce unified
   diffs. But diffs remain the wrong format for the *content* of these
   patches — see points 2–4 below. We use git for *reversibility* (§5.2) and
   *versioning* (§4.5), not for *patch content representation*.
2. **Fragility:** Unified diffs fail on any context-line change. SDK updates
   frequently refactor function bodies. A 3-line context change breaks the hunk.
3. **Opacity:** Diffs are hard to read. An overlay header is readable C++.
4. **No composability:** Two diffs touching the same file require manual merge.
   Two overlay flags can coexist if they guard different code paths.

We **do** use a diff-like specification for the one-time `#ifdef` guard
insertion (Format B Step 1), but this is a "find-and-wrap" operation, not a
context-line diff — it searches for a unique function signature and wraps the
body in `#ifdef`/`#else`/`#endif`. This is robust against minor upstream
changes to the function body because it anchors on the signature, not context
lines.

---

## 6. The Patch Manifest

### 6.1 Top-Level Manifest (`patches/spiderman3/manifest.toml`)

```toml
# Spider-Man 3 Patch Manifest
# Defines all patches applied to the game project and SDK runtime for
# Spider-Man 3 (Xbox 360) recompilation.

[game]
id = "spiderman3"
name = "Spider-Man 3"
platform = "xbox360"
xex_version = "1.0"
title_id = "415607E2"          # From the XEX header

[versions]
sdk = ">=0.8.0, <0.9.0"
game = "1.0"
patch_revision = 3

[compatibility]
min_sdk_where_obsolete = ""
obsolete_message = ""

# === Game-project patches (Format A: templated source) ===
[[patches]]
id = "xmp_bypass"
category = "game-project"
format = "codegen-overlay"           # Owned by codegen's src/ overlay, NOT generated by patch system
ref = "codegen:src/xmp_bypass.cpp"   # Pre-authored with post-funcid-rename symbols
owner = "codegen-automation"         # CodeGenAutomation agent owns this file
description = "Save system device selector hooks (XMP + XAM bypass). Pre-authored static file with post-rename symbol names (XMPGetStatus_Wrapper etc.). Hook targets depend on funcid rename step being complete."
reversible = true                    # Reverted by re-codegen (src/ overlay restores it)

# NOTE: The xmp_bypass.cpp.inja template + xmp_bypass.params.toml parameter
# file remain in the patch system as a RECIPE for future games that need
# save-system bypass with different parameters (device_id, delay_ms). For
# Spider-Man 3, the pre-authored static file in codegen's src/ overlay takes
# precedence — the patch system does NOT generate xmp_bypass.cpp for this game.

[[patches]]
id = "app_header"
category = "game-project"
format = "templated-source"
template = "app.h.inja"
params = ["app.params.toml", "cvar_preset.toml"]  # Merged
target = "src/spiderman3_app.h"
action = "create"  # Overwrites generated header
description = "App class with OnPreSetup cvars and OnConfigurePaths"
reversible = true

[[patches]]
id = "runtime_toml"
category = "game-config"
format = "templated-source"
template = "runtime.toml.inja"
params = "runtime_toml.params.toml"
target = "spiderman3.toml"
action = "create"
description = "Runtime TOML configuration"
reversible = true

# === Game-project CMake (Format C: CMake injection) ===
[[patches]]
id = "cmake_injection"
category = "game-build"
format = "cmake-injection"
fragment = "patches.cmake"
target = "CMakeLists.txt"
injection_point = "after:rexglue_setup_target"
injection_line = 'include(${CMAKE_CURRENT_SOURCE_DIR}/patches.cmake)'
description = "CMake source additions, compile options, ThinLTO for game exe"
reversible = true

# === SDK source patches (Format B: flag-guarded overlays) ===
[[patches]]
id = "sdk_save_system"
category = "game-runtime"
format = "sdk-overlay"
overlay_dir = "sdk-overlays/rexglue-sdk-0.8.0/save_system"
flag = "REX_GAME_SAVE_SYSTEM_FIX"
files = [
    "xam_ui_headless_overlay.h",
    "xam_enum_overlay.h",
    # xenumerator.cpp: inline #ifdef (no overlay file)
sdk_files_modified = [
    "src/kernel/xam/xam_ui.cpp",
    "src/kernel/xam/xam_enum.cpp",
    "src/system/xenumerator.cpp",
]
description = "XAM save-system fixes: headless dispatch XN_SYS_UI, enumerate IO_PENDING, WriteItems 0-item SUCCESS"
reversible = true
game_specific = true

[[patches]]
id = "sdk_rov_barrier_skip"
category = "general-runtime"
format = "sdk-overlay"
overlay_dir = "sdk-overlays/rexglue-sdk-0.8.0/general"
flag = "REX_ROV_BARRIER_SKIP"
files = ["rov_barrier_skip.h"]
sdk_files_modified = ["src/graphics/d3d12/render_target_cache.cpp"]
description = "Skip EDRAM barrier for read-only draws (color/depth/stencil not written)"
reversible = true
game_specific = false

[[patches]]
id = "sdk_queue_frames"
category = "general-runtime"
format = "sdk-overlay"
overlay_dir = "sdk-overlays/rexglue-sdk-0.8.0/general"
flag = "REX_QUEUE_FRAMES_2"
files = ["queue_frames.h"]
sdk_files_modified = ["include/rex/graphics/d3d12/command_processor.h"]
description = "kQueueFrames = 2 (was 3) — reduces frame latency"
reversible = true
game_specific = false

[[patches]]
id = "sdk_runtime_ipo"
category = "general-runtime"
format = "cmake-injection"
fragment = "sdk_patches.cmake"
target = "src/system/CMakeLists.txt"
injection_point = "after:add_library(rexruntime"
description = "ThinLTO/IPO for rexruntime.dll Release builds"
reversible = true
game_specific = false
```

### 6.2 General Patch Library (`patches/general/`)

General patches (category `general-runtime`) are shared across all games.
They live in a separate directory and are referenced by any game manifest:

```
patches/
  general/                          # Shared across all games
    sdk-overlays/
      rexglue-sdk-0.8.0/
        general/
          rov_barrier_skip.h
          queue_frames.h
    sdk_patches.cmake               # General CMake injection (IPO, flags)
  spiderman3/                       # Game-specific
    manifest.toml
    xmp_bypass.cpp.inja
    xmp_bypass.params.toml
    app.h.inja
    app.params.toml
    cvar_preset.toml
    runtime.toml.inja
    runtime_toml.params.toml
    patches.cmake                   # Game-specific CMake injection
    sdk-overlays/
      rexglue-sdk-0.8.0/
        save_system/
          xam_ui_headless_overlay.h
          xam_enum_overlay.h
          # xenumerator.cpp: inline #ifdef (no overlay header)
  another_game/                     # Future game
    manifest.toml
    ...
```

A game manifest includes general patches by reference:

```toml
# In spiderman3/manifest.toml
[[patches]]
id = "sdk_rov_barrier_skip"
ref = "general/sdk-overlays/rexglue-sdk-0.8.0/general/rov_barrier_skip.h"
# ... (same fields as inline, but the overlay file is shared)
```

---

## 7. Patch Application Pipeline

### 7.1 Apply Order

Patches must be applied in a specific order because of dependencies:

```
┌─────────────────────────────────────────────────────────────┐
│  1. ISO EXTRACT                                              │
│     extract-xiso → game files (default.xex + assets)        │
└──────────────────────────┬──────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  2. REXGLUE CODEGEN                                          │
│  3. APPLY GAME-PROJECT PATCHES                               │
│     a. xmp_bypass.cpp: owned by codegen src/ overlay (skip) │
│     b. Generate spiderman3_app.h from cvar template (A)     │
│     c. Generate spiderman3.toml from template (Format A)    │
│     d. Generate patches.cmake (Format C)                    │
│     e. Inject include() line into CMakeLists.txt (Format C) │
│     f. Write .recomp_patches_applied stamp                  │
└──────────────────────────┬──────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  4. SDK PREP (one-time per SDK version)                      │
│     a. Git-init pristine tarball + tag sdk-0.8.0-pristine    │
│     b. Check git tag sdk-0.8.0-recomp-rN — skip if matches   │
│     c. Apply #ifdef guards to 5 SDK source files (Format B)  │
│     d. Inject include() into src/system/CMakeLists.txt       │
│     e. Copy overlay headers to SDK source tree               │
│     f. git commit + tag sdk-0.8.0-recomp-rN + write stamp   │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  5. BUILD RUNTIME                                            │
│     cmake configure with -DREX_GAME_PROFILE=spiderman3      │
│       → enables REX_GAME_SAVE_SYSTEM_FIX + general flags    │
│     cmake --build --target rexruntime --config Release      │
│       → rexruntime.dll (with ThinLTO)                       │
└──────────────────────────┬──────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  6. BUILD GAME                                               │
│     cmake configure + build with clang-cl                   │
│       → spiderman3.exe (118 MB, with patches.cmake + LTO)   │
└──────────────────────────┬──────────────────────────────────┘
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  7. DEPLOY                                                   │
│     Copy spiderman3.exe + rexruntime.dll + TracyClient.dll  │
│     Create game/ junction to extracted ISO files            │
│     Create user_data/ directory                             │
│     spiderman3.toml already generated in step 3c            │
└─────────────────────────────────────────────────────────────┘
```

### 7.2 The SDK Prep Step — Detailed

This is the most delicate step because it modifies the SDK source tree. The
recompiler app must:

1. **Verify SDK version** matches the manifest's `sdk` constraint.
2. **Check for existing stamp** — if `.recomp_sdk_prep` exists with the same
   SDK version and prep_revision, skip (SDK already prepared).
3. **Apply `#ifdef` guards** to each SDK source file listed in
   `sdk_files_modified`. This is done by a **find-and-wrap** operation:
   - Search for a unique anchor string (e.g., the function signature
     `X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, uint32_t overlapped)`)
   - Wrap the function body in:
     ```cpp
     #if defined(REX_GAME_SAVE_SYSTEM_FIX)
     #include "...overlay.h"
     #endif
     // ... function with #ifdef/#else/#endif ...
     ```
   - The find-and-wrap spec is part of the overlay's metadata (see §7.3).
4. **Inject CMake include** into `src/system/CMakeLists.txt`.
5. **Copy overlay headers** into the SDK source tree (under a
   `patches/sdk-overlays/` directory relative to the SDK root).
6. **Write the stamp file.**

### 7.3 Find-and-Wrap Specification

Each SDK overlay declares how to find and wrap the target code:

```toml
# In the overlay's metadata (embedded in manifest or separate .meta.toml)
[wrap]
# xam_ui.cpp — xeXamDispatchHeadless
file = "src/kernel/xam/xam_ui.cpp"
anchor = "X_RESULT xeXamDispatchHeadless(std::function<X_RESULT()> run_callback, uint32_t overlapped) {"
# The anchor is a unique string. The wrapper finds the line, then wraps
# the function body (up to the matching closing brace).
flag = "REX_GAME_SAVE_SYSTEM_FIX"
include = "patches/sdk-overlays/rexglue-sdk-0.8.0/save_system/xam_ui_headless_overlay.h"
wrapped_function = "xeXamDispatchHeadless_Patched"
# The wrapper generates:
#   #if defined(flag)
#   #include "include"
#   #endif
#   anchor
#   #if defined(flag)
#     return wrapped_function(run_callback, overlapped);
#   #else
#     ... original body ...
#   #endif
#   }
```

This specification is robust because:
- The anchor is a **function signature** — stable across patch-level SDK
  updates (function signatures change less frequently than bodies).
- If the anchor is not found, the patch system reports an error with the
  expected string, prompting a manual update of the wrap spec for the new SDK
  version.
- The `#else` branch preserves the original code, so the SDK remains
  buildable without any flags defined.

---

## 8. Example: Complete Spider-Man 3 Patch Set

### 8.1 Directory Structure

```
patches/
├── general/
│   ├── sdk-overlays/
│   │   └── rexglue-sdk-0.8.0/
│   │       └── general/
│   │           ├── rov_barrier_skip.h
│   │           └── queue_frames.h
│   └── sdk_patches.cmake
└── spiderman3/
    ├── manifest.toml
    ├── xmp_bypass.cpp.inja
    ├── xmp_bypass.params.toml
    ├── app.h.inja
    ├── app.params.toml
    ├── cvar_preset.toml
    ├── runtime.toml.inja
    ├── runtime_toml.params.toml
    ├── patches.cmake
    └── sdk-overlays/
        └── rexglue-sdk-0.8.0/
            └── save_system/
                ├── xam_ui_headless_overlay.h
                ├── xam_enum_overlay.h
                # xenumerator.cpp: inline #ifdef (no overlay header)

```

### 8.2 Overlay: `rov_barrier_skip.h` (General)

```cpp
// patches/general/sdk-overlays/rexglue-sdk-0.8.0/general/rov_barrier_skip.h
// Skip EDRAM barrier (MarkEdramBufferModified) when the draw call doesn't
// write color or depth/stencil. Prevents useless UAV barriers on read-only
// draws (depth-only passes, occlusion queries with no color output).
//
// Enabled by -DREX_ROV_BARRIER_SKIP=1 at CMake configure time.
// Applied to: src/graphics/d3d12/render_target_cache.cpp
//   Path::kPixelShaderInterlock case in D3D12RenderTargetCache::Update()
#pragma once

namespace rex::graphics::d3d12 {

// Inline helper used by the #ifdef-guarded code in render_target_cache.cpp
inline bool ShouldMarkEdramBufferModified(uint32_t normalized_color_mask,
                                          const DepthRenderControl& normalized_depth_control) {
    return normalized_color_mask != 0 ||
           normalized_depth_control.z_write_enable ||
           normalized_depth_control.stencil_enable;
}

}  // namespace rex::graphics::d3d12
```

**The `#ifdef` guard in `render_target_cache.cpp` (after SDK prep):**

```cpp
// In D3D12RenderTargetCache::Update(), case Path::kPixelShaderInterlock:
case Path::kPixelShaderInterlock: {
    TransitionEdramBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CommitEdramBufferUAVWrites(EdramBufferModificationStatus::kAsUAV);
#if defined(REX_ROV_BARRIER_SKIP)
    if (ShouldMarkEdramBufferModified(normalized_color_mask, normalized_depth_control)) {
        MarkEdramBufferModified(EdramBufferModificationStatus::kAsROV);
    }
#else
    MarkEdramBufferModified(EdramBufferModificationStatus::kAsROV);
#endif
} break;
```

### 8.3 Overlay: `queue_frames.h` (General)

```cpp
// patches/general/sdk-overlays/rexglue-sdk-0.8.0/general/queue_frames.h
// Reduces frame queue depth from 3 to 2 to lower latency.
// Safe for all games — 2 frames is sufficient for the D3D12 command processor.
//
// Enabled by -DREX_QUEUE_FRAMES_2=1 at CMake configure time.
// Applied to: include/rex/graphics/d3d12/command_processor.h
#pragma once

namespace rex::graphics::d3d12 {

// Used by the #ifdef guard in command_processor.h:
//   #if defined(REX_QUEUE_FRAMES_2)
//   static constexpr uint32_t kQueueFrames = 2;
//   #else
//   static constexpr uint32_t kQueueFrames = 3;
//   #endif

}  // namespace rex::graphics::d3d12
```

**The `#ifdef` guard in `command_processor.h` (after SDK prep):**

```cpp
private:
#if defined(REX_QUEUE_FRAMES_2)
  static constexpr uint32_t kQueueFrames = 2;
#else
  static constexpr uint32_t kQueueFrames = 3;
#endif
```

### 8.4 Overlay: `xam_enum_overlay.h` (Game-Specific)

```cpp
// patches/spiderman3/sdk-overlays/rexglue-sdk-0.8.0/save_system/xam_enum_overlay.h
// Patched xeXamEnumerate: async path returns X_ERROR_IO_PENDING (not SUCCESS).
// The original code returned the result directly from CompleteOverlappedImmediateEx,
// which caused the game's overlapped polling to see a completed operation before
// the enumerator was ready. Returning IO_PENDING + completing overlapped immediately
// matches Xbox 360 behavior.
//
// Enabled by -DREX_GAME_SAVE_SYSTEM_FIX=1 at CMake configure time.
// Applied to: src/kernel/xam/xam_enum.cpp
#pragma once
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>

namespace rex::kernel::xam {

inline uint32_t xeXamEnumerate_Patched(uint32_t handle, uint32_t flags, mapped_void buffer_ptr,
                                       uint32_t buffer_size, uint32_t* items_returned,
                                       uint32_t overlapped_ptr) {
  REXKRNL_DEBUG("xeXamEnumerate: handle=0x{:08X} flags={} overlapped=0x{:08X}", handle, flags, overlapped_ptr);
  assert_true(flags == 0);

  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [e, buffer_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->WriteItems(buffer_ptr.guest_address(), buffer_ptr.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    uint32_t extended_error, length;
    auto result = run(extended_error, length);
    REXKRNL_DEBUG("xeXamEnumerate: result=0x{:08X} items={}", result, length);
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr, result, extended_error, length);
    return X_ERROR_IO_PENDING;  // <-- THE FIX: was returning result directly
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

}  // namespace rex::kernel::xam
```

### 8.5 Inline `#ifdef` — `xenumerator.cpp` (Game-Specific, Single-Token)

The `xenumerator.cpp` patch is a **single-token change**: the `!count` branch
returns `X_ERROR_SUCCESS` instead of `X_ERROR_NO_MORE_FILES`. This does not
need a full overlay-header function extraction — that would require access to
private members (`buffer_`, `item_count_`, `current_item_`) that don't have
public accessors. Instead, use the same inline `#ifdef` pattern as
`kQueueFrames` (§8.3): wrap just the changed return statement.

**No overlay header file needed.** The find-and-wrap spec for this patch
anchors on the return statement in the `!count` branch, not a full function
signature.

**The `#ifdef` guard in `xenumerator.cpp` (after SDK prep):**

```cpp
// In XStaticUntypedEnumerator::WriteItems(), the !count branch:
if (!count) {
    if (written_count) {
      *written_count = 0;
    }
#if defined(REX_GAME_SAVE_SYSTEM_FIX)
    return X_ERROR_SUCCESS;    // Spider-Man 3 expects SUCCESS, not NO_MORE_FILES
#else
    return X_ERROR_NO_MORE_FILES;
#endif
}
```

**Find-and-wrap spec:**

```toml
[wrap]
file = "src/system/xenumerator.cpp"
# Anchor: the return statement in the !count branch
anchor = "return X_ERROR_NO_MORE_FILES;"
# Context guard: only wrap the occurrence inside the !count branch
# (there is only one X_ERROR_NO_MORE_FILES in this file, so no ambiguity)
flag = "REX_GAME_SAVE_SYSTEM_FIX"
replacement_before = "return X_ERROR_SUCCESS;"
replacement_after = "return X_ERROR_NO_MORE_FILES;"
# Generates: #if defined(flag) / return SUCCESS / #else / return NO_MORE_FILES / #endif
```

**Why inline `#ifdef`, not overlay extraction:**
- The change is one token (`X_ERROR_NO_MORE_FILES` → `X_ERROR_SUCCESS`).
- No behavioral logic to extract — no pre/post callbacks, no control-flow change.
- No private-member access issue — the `#ifdef` wraps a return statement
  *inside* the existing member function, so all members are already in scope.
- The find-and-wrap anchor is a return statement (simpler, more stable than a
  full function signature).

**When to use overlay-header extraction vs inline `#ifdef`:**

| Patch type | Pattern | Examples |
|---|---|---|
| **Single-token or few-line change** | Inline `#ifdef` around the changed lines | `xenumerator.cpp` (return value), `command_processor.h` (kQueueFrames), `render_target_cache.cpp` (barrier skip condition) |
| **Real behavioral change** (new control flow, callbacks, function call swap) | Overlay header with extracted function | `xam_ui.cpp` (pre/post notification callbacks), `xam_enum.cpp` (CompleteOverlappedImmediateEx + return IO_PENDING) |

The `render_target_cache.cpp` ROV barrier skip (§8.2) is a boundary case: it
adds a conditional around `MarkEdramBufferModified()`. The inline `#ifdef`
pattern works there too (and is what §8.2 already shows), but the overlay
header provides the `ShouldMarkEdramBufferModified()` helper. This is a
hybrid: the `#ifdef` is inline in `render_target_cache.cpp`, and the overlay
header just provides the helper function. No function extraction needed.

---

## 9. Extensibility for Other Games

### 9.1 Adding a New Game

To add patches for a new game (e.g., "Another Game"):

1. Create `patches/another_game/` directory.
2. Write `manifest.toml` with the game's metadata and version constraints.
3. Identify which patches are needed:
   - **General patches** (ThinLTO, kQueueFrames, ROV barrier skip): Reference
     them from `patches/general/` — no new files needed.
   - **Game-specific project patches**: Write new templates
     (`app.h.inja`, `xmp_bypass.cpp.inja` or equivalent, `cvar_preset.toml`).
   - **Game-specific SDK patches**: Write new overlay headers under
     `patches/another_game/sdk-overlays/` with new CMake flags.
4. Run `recomp apply-patches --game another_game`.

### 9.2 Sharing General Patches

General patches are referenced by multiple game manifests. The CMake flag
definitions for general patches live in `patches/general/sdk_patches.cmake`,
which is included by every game's SDK build. A game manifest declares:

```toml
[[patches]]
id = "sdk_general"
ref = "general/sdk_patches.cmake"
# Always applied — no game-specific gating
```

### 9.3 Game-Specific SDK Patches — Coexistence Revisited

If two games need different patches to the same SDK file (e.g., both patch
`xam_ui.cpp` but in different ways), the overlay system handles this via
**different flags**:

```cmake
# In sdk_patches.cmake (game-profile-dependent section):
if(REX_GAME_PROFILE STREQUAL "spiderman3")
    target_compile_definitions(rexruntime PRIVATE REX_GAME_SAVE_SYSTEM_FIX=1)
elseif(REX_GAME_PROFILE STREQUAL "another_game")
    target_compile_definitions(rexruntime PRIVATE REX_GAME_ANOTHER_FIX=1)
endif()
```

The SDK source file has both `#ifdef` guards:

```cpp
X_RESULT xeXamDispatchHeadless(...) {
#if defined(REX_GAME_SAVE_SYSTEM_FIX)
    return xeXamDispatchHeadless_SaveFix(run_callback, overlapped);
#elif defined(REX_GAME_ANOTHER_FIX)
    return xeXamDispatchHeadless_AnotherFix(run_callback, overlapped);
#else
    // ... original code ...
#endif
}
```

Each game's runtime build enables only its own flag. The SDK source tree is
shared — only the CMake `-DREX_GAME_PROFILE=...` differs per build. This is
the core of the Option (b) answer from §0.1.

### 9.4 The Per-Game Runtime Build Matrix

Since each game may need a different set of SDK flags, the recompiler app
builds a separate `rexruntime.dll` per game profile:

```
out/
  rexruntime-spiderman3.dll    # Built with REX_GAME_SAVE_SYSTEM_FIX + general flags
  rexruntime-another_game.dll  # Built with REX_GAME_ANOTHER_FIX + general flags
```

At deployment, the appropriate DLL is copied as `rexruntime.dll` alongside the
game's exe. This is the tradeoff of Option (b): one SDK source tree, but N
runtime DLL builds (one per game profile). The build is fast (~2 minutes with
ThinLTO) and the DLLs are ~30 MB each — manageable.

---

## 10. Summary of Architectural Decisions

| Decision | Choice | Rationale |
|---|---|---|
| SDK patch coexistence | Option (b): flag-guarded overlays, one shared SDK tree | Avoids per-game copies/forks; overlays are readable and composable; one tree rebuilt per game-profile with different flag combos |
| SDK patch format | `#ifdef`-guarded overlay headers + inline `#ifdef` (Format B) | Overlays are readable C++ and composable across games; inline `#ifdef` for single-token changes; `#else` preserves original code so SDK builds without flags defined |
| Game-project patch format | Jinja templates with TOML params (Format A) | Net-new files; parameterized for reuse |
| CMake modification format | Generated `.cmake` fragment + `include()` injection (Format C) | Survives re-codegen; additive; reversible |
| Cvar system | TOML preset database + app header template generation | Extensible, documented, user-tunable cvars identifiable |
| TOML generation | Separate template, subset of cvar preset | Runtime-overridable config vs. build-time defaults |
| Versioning | Three axes: SDK version + game version + patch revision | SDK patches are SDK-version-coupled; game patches are game-version-coupled; patch revision tracks patch evolution |
| Reversibility | git-init + tags for SDK tree (§4.5); regenerate-from-template for game patches | SDK patches revert via `git checkout sdk-0.8.0-pristine -- .`; game patches revert by re-codegen; both tracked by stamp files for apply/skip logic |
| General vs. game-specific | Category tags in manifest; general patches shared in `patches/general/` | General patches (ThinLTO, kQueueFrames, ROV barrier) benefit all games; game-specific patches (save system) are opt-in per game |

---

## 11. Open Questions for Review

1. **~~Overlay header access to private members~~ (RESOLVED):** The
   `xenumerator.cpp` patch is a single-token change (`X_ERROR_NO_MORE_FILES` →
   `X_ERROR_SUCCESS`) and uses inline `#ifdef` (§8.5), not overlay-header
   function extraction. No private-member access issue exists — the `#ifdef`
   wraps a return statement *inside* the existing member function. Overlay
   extraction is reserved for patches with real behavioral changes
   (`xam_ui.cpp`, `xam_enum.cpp`).

2. **`xam_notify.cpp` logging patch (RESOLVED — not a confirmed patch):**
   The `REXKRNL_DEBUG` calls at lines 39-41 and 92-93 may have been part of
   the original RexGlue port rather than a Spider-Man 3-specific patch. The
   `@modified Tom Clay, 2026` header is on ALL SDK files (standard RexGlue
   adaptation marker), so it doesn't distinguish our patches from the port.
   DEBUG logging compiles out in Release builds — not a behavioral change.
  **Decision:** xam_notify.cpp is removed from the patch set, manifest,
  stamp file, and directory structures entirely. The user's explicit scope
  was "6 modifications across 5 files" — xam_notify.cpp was not among them.
  If a future investigation reveals a real `REXKRNL_LOG` → `REXKRNL_DEBUG`
  change, it can be added back as a confirmed patch.

3. **General patch sharing mechanism:** Should general patches be
   auto-applied to all games, or explicitly referenced in each game manifest?
   Auto-applied is simpler but risks unexpected behavior for games that don't
   need ROV (e.g., a game using `kHostRenderTargets` wouldn't benefit from
   `REX_ROV_BARRIER_SKIP`). **Recommendation:** General patches are referenced
   explicitly but via a one-line `ref = "general/..."` — simple but opt-in.

4. **SDK prep idempotency:** If the recompiler app re-prep an already-prepped
   SDK (e.g., after updating to a new patch revision), it must detect existing
   `#ifdef` guards and update them rather than double-wrapping. The stamp file
   tracks this, but the find-and-wrap logic must be idempotent (check for
   existing `#if defined(flag)` before wrapping).

5. **Template engine choice:** The SDK already vendors `inja` (a C++
   template engine). Should the recompiler app use `inja` for consistency, or
   a Python-native engine (Jinja2) if the app is written in Python?
   **Recommendation:** If the recompiler app is Python-based, use Jinja2
   (more mature, better error messages). If C++, use `inja`. The template
   syntax is nearly identical between the two.

6. **Fallback game data path:** `spiderman3_app.h` has a hardcoded dev path
   (`C:\tmp\Workspace 1\RexGlue360Recomp\extracted`). The template
   parameterizes this, but what should the default be? **Recommendation:**
   Empty string — the app should fail with a clear error ("game data not
   found, please set game_data_root in the TOML") rather than silently using
   a wrong path.
