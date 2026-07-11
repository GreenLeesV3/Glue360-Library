# UX Design — Spider-Man 3 Recompiler Application

> **Status:** Design document for review. No code has been written.
> **Audience:** The user (tomorrow's review), then the implementation team.
> **Scope:** The complete user-facing experience — from download to gameplay —
> for an application that automates the ISO → recompiled-exe pipeline documented
> in `BUILD_GUIDE.md`, `TROUBLESHOOTING.md`, `CVAR_REFERENCE.md`, and
> `RUNTIME_FIXES.md`.
>
> **Revised 2026-07-07** after verifying two pipeline facts that reshape the
> entire UX:
> 1. **Cvar tiering** — most user-facing settings (vsync, async shaders,
>    resolution, fullscreen, VRR, anisotropic, FXAA) are runtime TOML values
>    that apply on next launch with **no rebuild**. Only the "Forced" set in
>    `spiderman3_app.h::OnPreSetup` (ROV path, 120 Hz, the 4 city white/bloom
>    fixes, `d3d12_pipeline_creation_threads`, texture cache limits) is baked
>    into the exe and requires a rebuild. The Settings screen must reflect this
>    two-tier reality — forcing a 5–10 min rebuild for a vsync toggle would be a
>    major UX failure.
> 2. **The toolchain is not self-contained** — the compile step requires
>    VS 2022 Build Tools + LLVM/clang-cl + CMake + Ninja (~10 GB total). The
>    recompiled `spiderman3.exe` is derived game code (mechanical transpilation
>    of Activision/Treyarch's copyrighted XEX) and **cannot be distributed** —
>    every user must run the full recompilation locally. This is the same
>    friction every console recomp project faces (N64 recomp projects ship only
>    patches, not prebuilt exes). The app CAN ship: the orchestrator tool, the
>    patch database, game profiles, a prebuilt custom `rexruntime.dll` (SDK
>    code, not game code), and the prebuilt SDK static libs. The app CANNOT
>    ship: the game exe, the codegen output, or a pre-populated codegen cache.
>

---

## 0. Design Summary (TL;DR)

| Decision | Recommendation | Rationale |
|---|---|---|
| Interface paradigm | **Hybrid: native GUI primary, CLI mirror** | Non-technical users need a window; power users script/automate. Both drive the same pipeline engine. |
| Framework | **Dear ImGui + SDL3 backend** | The SDK already ships SDL3; ImGui is ~500 KB, no Python/Node dependency, and renders the kind of dense build-progress UI we need trivially. A Qt or WPF app would pull in a 50+ MB runtime. |
| Distribution | **Portable zip of the *app* (not the game)** | The app is ~15 MB (orchestrator + patch DB + game profile + bundled extract-xiso). No game content. The game exe is produced locally from the user's own ISO. |
| Settings tiering | **Two tiers: TOML (instant) vs Source (rebuild)** | Most cvars are TOML — apply on relaunch, zero rebuild. Only ~12 cvars are baked into `OnPreSetup` and need a rebuild. This is the single most important UX insight. |
| Toolchain strategy | **Auto-detect + guided install, not bundled** | VS 2022 Build Tools + LLVM + CMake + Ninja total ~10 GB and have their own licenses. The app detects them, links to official installers, and verifies. Cannot be bundled. |
| Process model | **Single .exe orchestrator** that shells out to bundled/detected tools | The orchestrator is self-contained; it finds or guides install of the toolchain. Users never touch a terminal. |
| Progress model | **Phase-based progress with per-phase sub-progress + live log tail** | The pipeline has 6 discrete phases with very different durations; a single indeterminate bar would be dishonest. |
| First run | **Guided wizard: legal → toolchain → SDK → ISO → build → play** | Gets a user from zero to playing. The toolchain install is the hardest step and must be presented honestly with clear progress. |
| Legal posture | **Click-through agreement + persistent reminder, no game content bundled** | The app is a tool; the user asserts they own their ISO. The recompiled exe is derived game code and stays on the user's machine. |

---

## 1. Personas

Two users, one app. Every screen must serve both.

### 1a. "Just Wants to Play" — Casual user
- Has a Spider-Man 3 ISO they dumped from their own disc.
- Does not know what a compiler, CMake, or a cvar is.
- Goal: install the tool, point at the ISO, wait, play the game.
- **Reality check:** this user must install ~10 GB of build tools (VS 2022 Build Tools + LLVM). This is unavoidable — the recompiled exe is derived game code and cannot be distributed. The app's job is to make this one-time install as painless as possible: auto-detect, link to official installers, verify, and never make the user understand what MSVC is.
- Will not read documentation unless something breaks.
- Tolerates a one-time setup (toolchain install + first build) if the app guides them through it and shows progress. Subsequent settings tweaks (resolution, vsync, AA) must NOT require another build — they're TOML changes that apply on relaunch.

### 1b. "Wants to Tweak" — Technical user
- Understands the recompilation pipeline, may have read the BUILD_GUIDE.
- Wants to change cvars — and knows which ones are TOML (instant) vs source-baked (rebuild).
- May want to rebuild with different optimization levels (Release vs RelWithDebInfo).
- Wants access to the raw build log and the generated project tree.
- May want to rebuild *only* the game (skip ISO extraction + codegen) after changing source-level cvars.
- May want to build a custom `rexruntime.dll` from SDK source with ThinLTO or specific runtime patches (power-user path; the app ships a prebuilt custom runtime as the default).

**Design principle:** The casual user's path is the default and is zero-config. Every technical option exists behind an "Advanced" disclosure — never in the critical path. **TOML-tier settings changes are instant (save + relaunch). Source-tier settings changes require a rebuild but skip extraction + codegen (≈3 min, not ≈20 min).**

---

## 2. User Journey Map

```
 ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
 │ 1. Get   │──▶│ 2. First │──▶│ 3. Point │──▶│ 4. Build │──▶│ 5. Play  │──▶│ 6. Tweak │──▶│ 7. Rebuild│
 │    the   │   │   run +  │   │ at ISO   │   │  (~20min │   │  Launch  │   │ (TOML:  │   │ (source: │
 │    app   │   │ toolchain│   │          │   │   first) │   │          │   │ instant) │   │ ~3min)   │
 └──────────┘   └──────────┘   └──────────┘   └──────────┘   └──────────┘   └──────────┘   └──────────┘
```

### Step 1 — Get the app
- User downloads a ~15 MB zip from GitHub Releases.
- The download page states clearly: *"You need your own Spider-Man 3 (Xbox 360) ISO. This tool does not provide game files. The recompiled executable is produced from YOUR ISO on YOUR machine — it is not distributed."*
- The app zip contains: the orchestrator exe, bundled `extract-xiso.exe`, the patch database (xmp_bypass.cpp template, spiderman3_app.h cvar template, runtime patch definitions), the game profile manifest, and a prebuilt custom `rexruntime.dll` (with the 3 save-system fixes + ThinLTO + kQueueFrames=2 — this is SDK code, redistributable). It does NOT contain: the game exe, the codegen output, the ReXGlue SDK (downloaded on first run), or the build toolchain.

### Step 2 — First run + toolchain + SDK setup
- User double-clicks `SpiderMan3Recompiler.exe`.
- **Legal screen** appears first (see §8). Must click "I agree" to proceed.
- **Toolchain check** runs automatically:
  - Scans for LLVM/clang-cl, VS 2022 Build Tools (MSVC + Windows SDK), CMake, Ninja.
  - Shows a checklist with green ✓ / red ✗ for each.
  - Missing tools get a one-click "Install" button that opens the official installer:
    - LLVM → official LLVM GitHub release installer
    - VS 2022 Build Tools → Microsoft Visual Studio download page (the "Build Tools for Visual Studio 2022" variant, not full VS — smaller, ~3 GB with the C++ workload)
    - CMake → official cmake.org download
    - Ninja → can be auto-installed via pip or direct download (small, ~200 KB)
  - **Honest framing:** "These are one-time installs. You won't need to do this again. Total download: ~5–10 GB. After install, the app will detect them automatically."
  - **ReXGlue SDK:** if not present, offers to download v0.8.0 prebuilt (~62 MB) from the official source and extract it. This is separate from the toolchain.
- Once all green, the wizard advances.

### Step 3 — Point at the ISO
- File picker: "Select your Spider-Man 3 (Xbox 360) ISO".
- The app validates the ISO: checks XDVDFS volume descriptor, confirms `default.xex` is present in the partition table, checks file size against expected (~6.8 GB).
- If the ISO is wrong (not an XGD2 Xbox 360 ISO, or not Spider-Man 3), shows an actionable error (see §6).
- User picks an output folder (default: a `SpiderMan3` folder next to the app).

### Step 4 — Build (the one-time ~20 minute phase)
- User clicks **"Build & Play"** (the single primary action).
- The app runs the full pipeline with live progress (see §5).
- **Honest time estimate:** "~15–25 minutes depending on your PC. This is a one-time cost — subsequent launches are instant, and most settings changes don't require a rebuild."
- The user can minimize the window; the app shows progress in the taskbar and sends a notification when done.
- If any phase fails, the app stops, shows an actionable error, and offers "Retry" or "View log".

### Step 5 — Play (first launch)
- When the build completes, the app shows a **"Play Spider-Man 3"** button.
- Clicking it launches `spiderman3.exe` from the output folder.
- The app stays resident (tray icon) so it can offer "change settings" later.
- First launch note: *"The first time you play, shaders will compile. This may take 20–30 seconds on the main menu. This only happens once — subsequent launches are fast."*

### Step 6 — Tweak settings (instant, no rebuild)
- Most settings (resolution, vsync, fullscreen, AA mode, async shaders, VRR, anisotropic level) are TOML-tier — they're written to `spiderman3.toml` and apply on the next game launch. **No rebuild needed.**
- User opens Settings, changes e.g. resolution to 1440p, clicks "Save & Apply", and the app relaunches the game with the new setting.
- This is the key UX win: the casual user never rebuilds for common tweaks.

### Step 7 — Rebuild (only for source-level cvar changes, ~3 min)
- A small set of cvars (ROV path, 120 Hz refresh rate, the 4 city white/bloom fixes, `d3d12_pipeline_creation_threads`, texture cache limits) are baked into `spiderman3_app.h::OnPreSetup` at compile time. Changing these requires a rebuild.
- The rebuild skips ISO extraction and codegen (both are cached and deterministic for a given XEX) — it only re-applies patches, recompiles, and redeploys. ~3 min, not ~20 min.
- The custom-runtime build (rexruntime.dll from SDK source) is a separate, power-user-only action. The app ships a prebuilt custom runtime as the default.

---

## 3. Interface Design — GUI (Primary)

### 3a. Window chrome
- Title: `Spider-Man 3 Recompiler`
- Fixed-size window, 900×640, resizable to a max of 1200×800. Centered on screen.
- Dark theme (the target audience is gamers; dark is expected and reduces eye strain during the long build).
- Single-window design — no multi-window dialogs except the native file picker.

### 3b. Home screen (post-setup, build exists)

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Spider-Man 3 Recompiler                                          [─][□][×]       ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   ┌────────────────────────────────────────────────────────────────────┐    ║
║   │                                                                    │    ║
║   │     SPIDER-MAN 3                                                    │    ║
║   │     Recompiled · Build #1 · 118 MB · Ready                          │    ║
║   │                                                                    │    ║
║   │     ┌──────────────────────────────────────────────────┐           │    ║
║   │     │   ▶  PLAY SPIDER-MAN 3                            │           │    ║
║   │     └──────────────────────────────────────────────────┘           │    ║
║   │                                                                    │    ║
║   │     Last played: never    Settings: 60 FPS · FXAA · 1080p           │    ║
║   │     Output: C:\Games\SpiderMan3\                                     │    ║
║   │                                                                    │    ║
║   └────────────────────────────────────────────────────────────────────┘    ║
║                                                                              ║
║   ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐              ║
║   │  Settings    │  │  Rebuild     │  │  Open Output Folder  │              ║
║   │  (instant)   │  │  (advanced)  │  │                      │              ║
║   └──────────────┘  └──────────────┘  └──────────────────────┘              ║
║                                                                              ║
║   [▼ Advanced]                                                               ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

Note the two-tier button labels: "Settings (instant)" vs "Rebuild (advanced)". This communicates the core UX insight: most changes are instant, rebuilding is the exception.

### 3c. First-run / no-build screen

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Spider-Man 3 Recompiler                                          [─][□][×] ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   Welcome. This tool recompiles YOUR Spider-Man 3 (Xbox 360) ISO into a      ║
║   native Windows executable — on your machine, from your own game.           ║
║                                                                              ║
║   You will need:                                                             ║
║     • Your own Spider-Man 3 (Xbox 360) ISO     ─  required                   ║
║     • Build tools (one-time install, ~5–10 GB) ─  VS 2022 + LLVM + CMake    ║
║     • ~2 GB free disk space                    ─  for the build output      ║
║     • ~20 minutes                              ─  first build time         ║
║                                                                              ║
║   This tool does NOT provide game files. The recompiled executable is        ║
║   produced from YOUR ISO on YOUR machine. It cannot be downloaded.           ║
║                                                                              ║
║   ┌──────────────────────────────────────────────────────────┐              ║
║   │   Get Started →                                           │              ║
║   └──────────────────────────────────────────────────────────┘              ║
║                                                                              ║
║   [I already have a build — point at existing folder...]                     ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

The first-run screen is honest about the toolchain cost. Hiding it would create a worse experience when the user hits the dependency check and feels deceived.

### 3d. Build progress screen

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Building Spider-Man 3 …                                [─][□][×]          ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   Overall  ████████████░░░░░░░░░░░░░░  45%  ~11 min remaining                ║
║                                                                              ║
║   ┌────────────────────────────────────────────────────────────────────┐    ║
║   │  ✓  1. Extract ISO             1.2 GB extracted          0:42      │    ║
║   │  ✓  2. Initialize project      43,676 functions found    0:08      │    ║
║   │  ⏳ 3. Codegen (PPC → C++)      shard 47 of 92            1:15      │    ║
║   │     ████████████░░░░░░░░░░░░░░░░░░░░░░░░  51%                       │    ║
║   │  ○  4. Apply game patches       3 hooks + 18 cvars       —         │    ║
║   │  ○  5. Compile + link           94 sources, clang-cl     —         │    ║
║   │  ○  6. Deploy (exe + DLLs)      copy + config + junction —         │    ║
║   └────────────────────────────────────────────────────────────────────┘    ║
║                                                                              ║
║   ┌────────────────────────────────────────────────────────────────────┐    ║
║   │ [live log tail]                                                     │    ║
║   │ [12:04:51] codegen: translating function 0x8297481C (sub_8297481C) │    ║
║   │ [12:04:51] codegen: shard 47/92 → spiderman3_recomp.47.cpp         │    ║
║   │ [12:04:52] codegen: 412 functions in shard 47                      │    ║
║   │ ...                                                                │    ║
║   └────────────────────────────────────────────────────────────────────┘    ║
║                                                                              ║
║   [Pause]  [Cancel]  [▼ Show full log]                                       ║
║                                                                              ║
║   ⏳ This is a one-time build. Future launches are instant, and most         ║
║      settings changes (resolution, vsync, AA) won't need a rebuild.          ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

**Key elements:**
- **Overall progress bar** with time estimate (updated from rolling average of phase durations). The estimate is honest: ~20 min for a first full build on a typical machine.
- **Phase list** with per-phase status icon (✓ done, ⏳ in progress, ○ pending, ✗ failed) and per-phase sub-progress bar for the long phases (codegen, compile).
- **Live log tail** — last 5–8 lines, auto-scrolling. Collapsible to a full log viewer.
- **Reassurance banner** at the bottom: reminds the user this is one-time. Reduces the perceived cost of the long wait.
- **Pause** — suspends before the next phase boundary (cannot pause mid-compile safely, so it queues).
- **Cancel** — confirms, then cleans up partial build artifacts.

### 3e. Build complete screen

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Build complete!                                              [─][□][×]     ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   ✓  Spider-Man 3 has been recompiled successfully.                          ║
║                                                                              ║
║      spiderman3.exe  ·  118 MB  ·  18 min 34 sec                             ║
║      Output: C:\Games\SpiderMan3\                                            ║
║                                                                              ║
║   ┌──────────────────────────────────────────────────────────┐              ║
║   │   ▶  PLAY SPIDER-MAN 3                                    │              ║
║   └──────────────────────────────────────────────────────────┘              ║
║                                                                              ║
║   First launch note: Shaders will compile on the main menu (20–30 sec).     ║
║   This is normal and only happens once.                                      ║
║                                                                              ║
║   Tip: You can change resolution, vsync, and anti-aliasing in Settings      ║
║   at any time — no rebuild needed. Just save and relaunch.                  ║
║                                                                              ║
║   [Open output folder]  [Create desktop shortcut]                           ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

The tip about instant settings is shown right after the long build — it reframes the build cost: "you paid 18 minutes once; from here, tweaks are free."

---

## 4. Interface Design — CLI (Mirror)

The same orchestrator exposes a CLI for automation, CI, and power users who prefer terminals.

```
$ SpiderMan3Recompiler.exe --help

Spider-Man 3 Recompiler — recompile your Xbox 360 ISO into a native Windows exe

USAGE:
  SpiderMan3Recompiler.exe <COMMAND> [OPTIONS]

COMMANDS:
  setup      First-run: legal + toolchain check + SDK download (interactive)
  build      Full pipeline: extract → codegen → patch → compile → deploy
  play       Launch the most recent build
  rebuild    Rebuild from cached extraction (skip ISO + codegen steps)
  status     Show toolchain status, SDK status, and current build state
  config     Get/set runtime TOML settings (instant, no rebuild)
  recompile  Rebuild only after source-level cvar changes (skip extraction+codegen)
  clean      Remove build artifacts (keeps extracted ISO cache)

OPTIONS:
  --iso <path>           Path to the Spider-Man 3 ISO
  --output <dir>         Output directory (default: .\SpiderMan3)
  --profile <name>       Settings preset: default | performance | quality | custom
  --cvar <name>=<val>    Override a TOML cvar (instant, repeatable)
  --source-cvar <n>=<v>  Override a source-baked cvar (triggers rebuild)
  --build-type <type>    Release | RelWithDebInfo | Debug (default: Release)
  --runtime <mode>       prebuilt | custom  (default: prebuilt — custom is power-user)
  --yes                  Skip all confirmations (non-interactive)
  --verbose              Show full build log (default: summary + errors)

EXAMPLES:
  SpiderMan3Recompiler.exe setup
  SpiderMan3Recompiler.exe build --iso "D:\Games\Spider-Man 3.iso"
  SpiderMan3Recompiler.exe config set vsync true        # instant, no rebuild
  SpiderMan3Recompiler.exe config set resolution 1440p  # instant, no rebuild
  SpiderMan3Recompiler.exe play
  SpiderMan3Recompiler.exe recompile --source-cvar video_mode_refresh_rate=90.0
```

Note the explicit separation: `config` (TOML, instant) vs `recompile` (source-baked cvars, rebuild). This mirrors the GUI's two-tier settings and prevents the user from accidentally triggering a 3-min rebuild when they meant to toggle vsync.

### CLI build output

```
$ SpiderMan3Recompiler.exe build --iso "Spider-Man 3.iso"

Spider-Man 3 Recompiler v1.0.0
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[1/6] Extracting ISO …          ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100%  0:42
      extracted 1.2 GB → .\SpiderMan3\extracted\

[2/6] Initializing project …    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100%  0:08
      43,676 functions, 92 shards, CMakeLists.txt generated

[3/6] Codegen (PPC → C++) …     ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100%  1:15
      shard 92/92 → spiderman3_recomp.91.cpp

[4/6] Applying patches …        ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100%  0:01
      ✓ xmp_bypass.cpp (3 hooks)
      ✓ spiderman3_app.h (18 cvars)

[5/6] Compiling (clang-cl) …    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100%  15:22
      94 sources → spiderman3.exe (118 MB)

[6/6] Deploying …               ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100%  0:04
      ✓ spiderman3.exe + rexruntime.dll + TracyClient.dll
      ✓ spiderman3.toml
      ✓ game\ junction → extracted\

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✓ Build complete in 18 min 34 sec
  Output: .\SpiderMan3\spiderman3.exe

  Run `SpiderMan3Recompiler.exe play` to launch.
  Run `SpiderMan3Recompiler.exe config set <cvar> <val>` to tweak settings
  (instant, no rebuild).
```

### CLI instant config (no rebuild)

```
$ SpiderMan3Recompiler.exe config set vsync false
✓ vsync = false  → written to .\SpiderMan3\spiderman3.toml
  Relaunch the game to apply: `SpiderMan3Recompiler.exe play`
```

### CLI error output

```
[5/6] Compiling (clang-cl) …    ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 100%  FAIL

✗ Build failed during compilation.

  Error: unresolved external symbol roundevenf
  In: spiderman3_recomp.43.cpp

  This is a known issue: the CRT math shim is missing.
  The app should have generated src/roundevenf.cpp automatically.

  What to try:
    1. Run `SpiderMan3Recompiler.exe status` to verify build integrity
    2. Run `SpiderMan3Recompiler.exe rebuild` to regenerate source files
    3. If the problem persists, report this with the full log:
       .\SpiderMan3\logs\build_2026-07-07_120451.log

  Full log: .\SpiderMan3\logs\build_2026-07-07_120451.log
```

---

## 5. Progress Feedback Design

### 5a. Phase model

The pipeline has 6 phases. Each has a known duration profile, which drives the progress UX:

| # | Phase | Typical duration | Progress type | Sub-progress source |
|---|---|---|---|---|
| 1 | Extract ISO | 30–60 sec | Indeterminate → determinate | extract-xiso reports file count |
| 2 | Init project (rexglue init) | 5–10 sec | Indeterminate (fast) | — |
| 3 | Codegen | 60–90 sec | **Determinate** | shard N/92 (rexglue codegen outputs progress) |
| 4 | Apply patches | 1–2 sec | Instant (fast) | — |
| 5 | Compile + link | **10–15 min** | **Determinate** | Ninja build progress (compiled X/Y objects) |
| 6 | Deploy | 3–5 sec | Instant (fast) | — |

The compile phase dominates (>75% of total time). This is the 92 generated shards (377 MB of C++, 43K functions) compiled by clang-cl and linked into a 118 MB exe. There is no legal way to precompute this for the user.

### 5b. Progress bar semantics
- **Overall bar**: weighted by phase duration estimates. The compile phase gets ~75% of the bar weight. If a phase overruns by >50%, the estimate switches to "calculating…" rather than showing a wrong ETA.
- **Phase sub-bars**: only for codegen (shard-based) and compile (Ninja object-count-based). Other phases show a spinner.
- **Taskbar progress**: Windows taskbar shows the overall percentage as a green overlay on the app icon.
- **System notification** on completion: "Spider-Man 3 build complete — click to play."

### 5c. Log output
- The live log tail shows the last N lines of the active subprocess output.
- Log lines are timestamped and tagged by phase: `[12:04:51] [codegen] ...`.
- The full log is written to `logs/build_YYYY-MM-DD_HHMMSS.log` in the output folder.
- A "Show full log" button opens the complete log in a scrollable viewer with search and filter-by-phase.
- **Verbose mode** (CLI `--verbose`, GUI checkbox) shows every line in the main view instead of just the tail.

### 5d. Handling the long compile phase
The compile phase (10–15 min) is where users are most likely to think the app froze. Mitigations:
- Ninja outputs `[X/Y] Compiling spiderman3_recomp.NN.cpp` — we parse this for the sub-progress bar.
- When the linker runs (`link.exe`), there's no progress output for ~30–60 sec. We show "Linking … (this takes ~1 min, no progress bar available)" so the user doesn't think it's stuck.
- The live log keeps scrolling with clang-cl's own output, proving the process is alive.
- **The reassurance banner** (§3d) stays visible: "This is a one-time build. Future launches are instant, and most settings changes won't need a rebuild."

### 5e. Rebuild progress (shorter)
When rebuilding after source-level cvar changes, phases 1–3 are cached/skipped:

```
   ✓  1. Extract ISO             [cached — skipped]            —
   ✓  2. Initialize project      [cached — skipped]            —
   ✓  3. Codegen                 [cached — skipped]            —
   ✓  4. Apply patches           regenerated cvar block        0:01
   ⏳ 5. Compile + link           94 sources, clang-cl          ~2:30
      ████████████░░░░░░░░░░░░░░░░░░░░  45%
   ○  6. Deploy                   copy + config + junction     —
```

Overall bar starts at ~40% (phases 1–3 done). The compile phase is the only significant wait.

---

## 6. Error Presentation

### 6a. Error design principles
1. **Actionable above all.** Every error tells the user what happened, why, and what to do next — in that order.
2. **No raw stack traces in the primary view.** Technical detail goes behind a "Show details" disclosure.
3. **Categorize by user-actionability:**
   - **Fixable by the user** (missing ISO, missing toolchain) → steps to fix + links.
   - **Fixable by the app** (missing source shim, patch not applied) → "Retry" or "Repair" button.
   - **Unknown / bug** → "Report this" button that packages the log + system info.
4. **Never blame the user.** "The ISO couldn't be read" not "You provided an invalid ISO."

### 6b. Error screen layout

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  ⚠  Build failed at: Compile + link                              [─][□][×] ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   What happened:                                                             ║
║   The compiler ran out of heap space while compiling a generated shard.      ║
║                                                                              ║
║   Why:                                                                       ║
║   One of the 92 generated source files is too large for the available        ║
║   memory. This can happen if other programs are using RAM.                   ║
║                                                                              ║
║   What to try:                                                               ║
║     1. Close other applications (browsers, IDEs, etc.)                       ║
║     2. Click Retry below — the build will resume                             ║
║     3. If it fails again, try a Debug build (smaller memory footprint)       ║
║                                                                              ║
║   ┌──────────┐  ┌──────────┐  ┌──────────────────────┐                      ║
║   │  Retry   │  │  Cancel  │  │  Open build log      │                      ║
║   └──────────┘  └──────────┘  └──────────────────────┘                      ║
║                                                                              ║
║   [▼ Show technical details]                                                 ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

### 6c. Known error catalog (from BUILD_GUIDE + TROUBLESHOOTING)

The app ships with a built-in error catalog that pattern-matches build output and presents curated messages. Each maps a raw error to the human-readable form above.

| Raw error pattern | Category | User message | Fix action |
|---|---|---|---|
| `vcvarsall failed` / `FAILED: vcvarsall` | Toolchain | "Visual Studio 2022 C++ Build Tools not found" | Link to VS Build Tools installer with "Desktop development with C++" workload |
| `clang-cl not found` / `where clang-cl` fails | Toolchain | "LLVM/clang-cl compiler not found" | Link to LLVM installer, or auto-install |
| `CMake not found` | Toolchain | "CMake build system not found" | Link to cmake.org download |
| `Ninja not found` | Toolchain | "Ninja build tool not found" | Auto-install (small, ~200 KB) |
| `ReXGlue SDK not found` | SDK | "ReXGlue SDK not installed" | Auto-download v0.8.0 prebuilt, or point at existing |
| `unresolved external symbol roundevenf` | App bug | "CRT math shim missing — regenerating source files" | Auto-repair: regenerate `src/roundevenf.cpp` and retry |
| `unresolved external symbol __imp__Xam*` | App bug | "System-call hooks missing — regenerating `xmp_bypass.cpp`" | Auto-repair: regenerate hooks and retry |
| `Permission denied` on `spiderman3.exe` | User state | "The game is still running. Close it and retry." | Detect running `spiderman3.exe`, offer to close it gracefully |
| `C1060: compiler out of heap space` | Resource | "Compiler ran out of memory" (see §6b mockup) | Retry; suggest closing apps or Debug build |
| `unknown argument '/EHa'` | Config | "Wrong compiler — need clang-cl, not clang++" | Auto-fix: set `CMAKE_CXX_COMPILER=clang-cl` and retry |
| Build OK but black screen | Runtime | "ROV render path not enabled" | Auto-repair: force `render_target_path_d3d12=rov` in TOML + app.h, rebuild |
| Build OK but "Invalid UTF-8" crash | Runtime | "Device selector hook not active — save system fix missing" | Auto-repair: verify `xmp_bypass.cpp` hooks, rebuild |
| ISO not XDVDFS / not Xbox 360 | Input | "This doesn't look like an Xbox 360 ISO" | Re-prompt for ISO file |
| ISO doesn't contain `default.xex` | Input | "This ISO doesn't contain a game executable" | Re-prompt; suggest extracting from a different partition |
| Shader cache corruption (20s hang) | Runtime | "Shader cache corrupted — clearing and rebuilding" | Auto-repair: delete `user_data\cache\shaders\`, warn about first-launch slowness |
| Custom runtime build: xxHash/mspack/CMake4 fix fails | SDK source | "SDK source tree layout differs from expected" | Fallback to prebuilt runtime; offer "report this" |

### 6d. Error recovery model
- **Auto-repairable errors**: the app silently fixes and retries (e.g., regenerating a missing source file). It shows a brief "Repairing …" state, then continues. The user is informed post-hoc: "Note: `roundevenf.cpp` was missing and has been regenerated."
- **User-fixable errors**: the app stops and presents the error with steps. The "Retry" button re-runs from the failed phase (not from scratch).
- **Fatal errors** (SDK incompatible, toolchain too old): the app stops and presents the error with a "Report this" button that zips the build log + system diagnostics.

---

## 7. Settings / Configuration Design

### 7a. The two-tier insight

This is the most important UX decision in the document. Cvars fall into two tiers with fundamentally different UX:

**Tier 1 — Runtime TOML (instant, no rebuild):**
These are written to `spiderman3.toml` and read at game launch. Changing them requires only saving the file and relaunching the game. Zero compile time.

| Cvar | Type | Default | Effect |
|---|---|---|---|
| `vsync` | bool | `false` | Host vsync on/off. Disable for uncapped FPS. |
| `async_shader_compilation` | bool | `true` | Background PSO builds; reduces first-play stutter. |
| `resolution` | string | `"720p"` | Host window scale preset (720p/1080p/1440p/4k). |
| `window_width` / `window_height` | int32 | 1280/720 | Explicit window dimensions. |
| `fullscreen` | bool | `false` | Borderless fullscreen. |
| `monitor` | int32 | `0` | Output monitor index. |
| `d3d12_allow_variable_refresh_rate_and_tearing` | bool | `true` | FreeSync/G-Sync support. |
| `d3d12_adapter` | int32 | `0` | GPU selection (multi-GPU systems). |
| `audio_mute` | bool | `false` | Mute audio. |
| `input_backend` | string | `"sdl"` | Input backend (sdl/xinput). |
| `game_data_root` | string | path | Game data location. |
| `resolution_scale` | int32 | `1` | Internal render-target scaling factor (1×, 2×, 3×, 4×). |
| `draw_resolution_scale_x` / `_y` | int32 | `1` | Per-axis 3D draw scaling. |
| `present_effect` | string | `"none"` | Presenter post effect: `none` / `cas` / `bilinear` / FSR. |
| `present_cas_additional_sharpness` | double | `0.0` | CAS sharpness (0–1) when `present_effect=cas`. |
| `log_level` | string | `"info"` | Log verbosity. |

**Tier 2 — Source-baked (`OnPreSetup`, requires rebuild):**
These are compiled into `spiderman3_app.h::OnPreSetup` via `rex::cvar::SetFlagByName`. They override TOML at runtime. To change them, the app regenerates `spiderman3_app.h`, recompiles, and redeploys — but skips ISO extraction and codegen (both cached). ~3 min rebuild.

| Cvar | Type | Default | Effect | Why it's source-baked |
|---|---|---|---|---|
| `render_target_path_d3d12` | string | `"rov"` | ROV EDRAM emulation; fixes black menu | Game-specific critical; TOML can be stale/missing |
| `video_mode_refresh_rate` | double | `120.0` | 60 FPS unlock (120 Hz vblank × 2) | Overrides TOML; belt-and-braces |
| `gamma_render_target_as_unorm16` | bool | `true` | City white/bloom fix (1/4) | Linked set — must change together |
| `snorm16_render_target_full_range` | bool | `true` | City white/bloom fix (2/4) | Linked set |
| `mrt_edram_used_range_clamp_to_min` | bool | `true` | City white/bloom fix (3/4) | Linked set |
| `readback_resolve` | string | `"fast"` | City white/bloom fix (4/4) | Linked set |
| `anisotropic_override` | int32 | `5` | Force anisotropic level (max effective 5) | Game-specific |
| `swap_post_effect` | string | `"fxaa"` | CP-level post-processing AA | Game-specific |
| `d3d12_pipeline_creation_threads` | int32 | `2` | PSO build thread count (avoids lock contention) | Perf-critical |
| `host_present_from_non_ui_thread` | bool | `true` | Decouple presentation from UI thread | Perf |
| `d3d12_bindless` | bool | `true` | Bindless descriptor heaps | Perf |
| `d3d12_tiled_shared_memory` | bool | `true` | Tiled 512 MB shared memory | Perf |
| `d3d12_submit_on_primary_buffer_end` | bool | `false` | Batch ECLs to reduce barriers | Perf |
| `readback_memexport` / `_fast` | bool | `true` | memexport readback path | Perf |
| `texture_cache_memory_limit_hard` | int32 | `4096` | Hard eviction budget (MB) | Perf |
| `texture_cache_memory_limit_soft` | int32 | `2048` | Soft eviction budget | Perf |
| `texture_cache_memory_limit_soft_lifetime` | int32 | `120` | Soft eviction lifetime | Perf |
| `texture_cache_memory_limit_render_to_texture` | int32 | `256` | RT texture cache budget | Perf |
| `use_fuzzy_alpha_epsilon` | bool | `true` | Fuzzy alpha test | Visual fix |
| `present_dither` | bool | `true` | Ordered dither on 8 bpc output | Visual |
| `store_shaders` | bool | `true` | Persist shader cache | Perf (first-launch) |

### 7b. Settings screen — two-tier design

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Settings                                              [← Back] [─][□][×]  ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   ┌─ Quick Settings (instant — no rebuild needed) ──────────────────────┐   ║
║   │                                                                      │   ║
║   │  These take effect on the next game launch. Just save and play.      │   ║
║   │                                                                      │   ║
║   │  Resolution       [1080p ▼]  (720p, 1080p, 1440p, 4K, Custom)       │   ║
║   │  Render scale     [1× (native) ▼]  (1×, 2×, 3×, 4×)  ← supersample  │   ║
║   │  Post-processing  [Bilinear ▼]  (None, Bilinear, CAS, FSR)          │   ║
║   │  CAS sharpness    [0.30 ▼]  (0.0–1.0, only if post=CAS)             │   ║
║   │  Fullscreen       [ ] Enabled                                        │   ║
║   │  VSync            [ ] Enabled     ← disable for uncapped FPS         │   ║
║   │  Async shaders    [✓] Enabled     ← reduces first-play stutter       │   ║
║   │  VRR / Tearing    [✓] Enabled     ← FreeSync/G-Sync friendly         │   ║
║   │  Audio            [✓] Enabled                                        │   ║
║   │  GPU adapter      [GPU 0: AMD Radeon ▼]                              │   ║
║   │  Monitor          [Display 1 ▼]                                      │   ║
║   │                                                                      │   ║
║   │  ┌──────────────────────────────────────┐                            │   ║
║   │  │   Save & Apply (relaunch game)        │                            │   ║
║   │  └──────────────────────────────────────┘                            │   ║
║   └──────────────────────────────────────────────────────────────────────┘   ║
║                                                                              ║
║   ┌─ Advanced Settings (requires rebuild — ~3 min) ─────────────────────┐   ║
║   │                                                                      │   ║
║   │  These cvars are compiled into the game executable. Changing them    │   ║
║   │  triggers a rebuild (skips ISO extraction + codegen — only the       │   ║
║   │  compile + link step runs).                                          │   ║
║   │                                                                      │   ║
║   │  ┌─ Presets ────────────────────────────────────────────────────┐   │   ║
║   │  │  (•) Default (recommended)   60 FPS, FXAA, fast readback     │   │   ║
║   │  │  ( ) Performance             60 FPS, no AA, fast readback    │   │   ║
║   │  │  ( ) Quality                 60 FPS, FXAA, 2× resolution*    │   │   ║
║   │  │  ( ) Custom                   configure below                 │   │   ║
║   │  └──────────────────────────────────────────────────────────────┘   │   ║
║   │                                                                      │   ║
║   │  Frame rate       [60 FPS (120 Hz vblank) ▼]  ← unlocks from 30     │   ║
║   │                    (30 FPS, 60 FPS, 90 FPS, 120 FPS, Custom)        │   ║
║   │  Anti-aliasing    [FXAA  ▼]  (None, FXAA, FXAA Extreme)             │   ║
║   │  Anisotropic      [5× (max for this game) ▼]                        │   ║
║   │                                                                      │   ║
║   │  ┌─ Rendering Fixes (game-specific — do not change unless ─────┐   │   ║
║   │  │  you know what you're doing)                                  │   │   ║
║   │  │  ✓ ROV render target path        ← fixes black menu           │   │   ║
║   │  │  ✓ Gamma unorm16                  ← fixes city white/bloom    │   │   ║
║   │  │  ✓ SNORM16 full range             ← fixes city white/bloom    │   │   ║
║   │  │  ✓ MRT EDRAM clamp to min         ← fixes city white/bloom    │   │   ║
║   │  │  ✓ Fast readback resolve          ← fixes city bloom          │   │   ║
║   │  │  ⚠ These are linked — changing one requires changing all.     │   │   ║
║   │  └───────────────────────────────────────────────────────────────┘   │   ║
║   │                                                                      │   ║
║   │  ┌─ Performance ────────────────────────────────────────────────┐   │   ║
║   │  │  ✓ Host present from non-UI thread                             │   │   ║
║   │  │  ✓ D3D12 bindless                                               │   │   ║
║   │  │  ✓ D3D12 tiled shared memory                                    │   │   ║
║   │  │  ✓ Batch ECLs (submit_on_primary_buffer_end = false)           │   │   ║
║   │  │  ✓ Readback memexport fast path                                 │   │   ║
║   │  │  Pipeline creation threads  [2 ▼]  (auto causes 10× slowdown)  │   │   ║
║   │  │  Texture cache hard limit  [4096 MB ▼]                         │   │   ║
║   │  │  Texture cache soft limit  [2048 MB ▼]                         │   │   ║
║   │  └───────────────────────────────────────────────────────────────┘   │   ║
║   │                                                                      │   ║
║   │  ┌─ Build ──────────────────────────────────────────────────────┐   │   ║
║   │  │  Build type      [Release ▼]  (Release, RelWithDebInfo, Debug)│   │   ║
║   │  │  Optimization    [ThinLTO  ▼]  (ThinLTO, None)                │   │   ║
║   │  │  Runtime         [Prebuilt (recommended) ▼]                   │   │   ║
║   │  │    (•) Prebuilt custom runtime (save fixes + ThinLTO, shipped)│   │   ║
║   │  │    ( ) Build runtime from SDK source (power-user, ~25 min)    │   │   ║
║   │  └───────────────────────────────────────────────────────────────┘   │   ║
║   │                                                                      │   ║
║   │  ┌─ Raw cvar editor ────────────────────────────────────────────┐   │   ║
║   │  │  [▼ Advanced cvar overrides]                                  │   │   ║
║   │  │  name                          = value                          │   │   ║
║   │  │  texture_cache_memory_limit_hard= 4096                          │   │   ║
║   │  │  [+ Add cvar]                                                  │   │   ║
║   │  │  ⚠ These are source-baked. See CVAR_REFERENCE.md.             │   │   ║
║   │  └───────────────────────────────────────────────────────────────┘   │   ║
║   │                                                                      │   ║
║   │  ┌──────────────────────────────────────┐                            │   ║
║   │  │   Save & Rebuild (~3 min)             │                            │   ║
║   │  └──────────────────────────────────────┘                            │   ║
║   └──────────────────────────────────────────────────────────────────────┘   ║
║                                                                              ║
║  * Resolution scale and CAS are Tier 1 cvars (resolution_scale, present_effect) —
║    the "Quality" preset applies instantly, no rebuild needed.
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

### 7c. Visual distinction between tiers
- **Tier 1 (Quick Settings)**: light blue background panel, "instant" badge, "Save & Apply" button that just writes TOML and offers to relaunch.
- **Tier 2 (Advanced Settings)**: darker panel, "rebuild" badge, "Save & Rebuild (~3 min)" button that triggers the rebuild flow.
- The button labels are the key communication: "Save & Apply (relaunch game)" vs "Save & Rebuild (~3 min)". The user can never accidentally trigger a rebuild when they meant to toggle vsync.

### 7d. Presets (Tier 2)

| Preset | Key cvars | Target user |
|---|---|---|
| **Default** | 60 FPS, FXAA, AF=5, fast readback, ROV, all perf flags on, ThinLTO prebuilt runtime | Everyone — the known-good config from the manual pipeline |
| **Performance** | Same as Default but `swap_post_effect=none` (no AA) | Lower-end GPUs |
| **Quality** | Same + `resolution_scale=2`, `present_effect=cas` (Tier 1 — instant, no rebuild) | High-end GPUs |
| **Custom** | Everything user-configurable | Technical users |

### 7e. What is NOT user-configurable (locked for safety)
The four "city white/bloom" cvars are locked as a set — the UI warns that they are linked and changing one requires changing all four. A power user can still override via the raw cvar editor, but the UI does not make it easy. This matches the TROUBLESHOOTING guidance: *"They must be changed together or not at all."*

### 7f. Settings storage architecture

Settings are stored in a single `settings.toml` next to the app. They map to two outputs:
1. **`spiderman3.toml`** (Tier 1 — runtime config) — written instantly on "Save & Apply".
2. **`spiderman3_app.h`** `OnPreSetup` cvar block (Tier 2 — source-baked) — regenerated on "Save & Rebuild", then compiled.

**Layering (highest precedence wins, at game runtime):**
1. App defaults (preset)
2. `settings.toml` (user-saved settings)
3. CLI `--cvar` / `--source-cvar` overrides (ephemeral)
4. Raw cvar editor entries in the Advanced panel
5. `OnPreSetup` forced values (Tier 2 — always win at runtime, overriding TOML)

Note: Tier 2 cvars **always override** Tier 1 at runtime because `OnPreSetup` runs after TOML parse. This is by design — the forced values are the proven-working set. If a user sets `video_mode_refresh_rate=60.0` in TOML but `OnPreSetup` forces `120.0`, the game runs at 60 FPS. The UI explains this: "These values override Quick Settings at runtime — they are the proven-working configuration."

---

## 8. First-Run Experience (Wizard)

### 8a. Wizard flow

```
 ┌─────────┐   ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
 │ Step 1  │──▶│ Step 2   │───▶│ Step 3   │───▶│ Step 4   │───▶│ Step 5   │───▶│ Step 6   │
 │ Legal   │   │ Toolchain│    │ SDK      │    │ ISO      │    │ Settings │    │ Build +  │
 │         │   │ check    │    │ download │    │ select   │    │ review   │    │ Play     │
 └─────────┘   └──────────┘    └──────────┘    └──────────┘    └──────────┘    └──────────┘
```

### 8b. Step 1 — Legal agreement

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Before we begin …                                    [─][□][×]             ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   Spider-Man 3 Recompiler is a tool that recompiles YOUR legally-owned       ║
║   Spider-Man 3 (Xbox 360) game into a native Windows executable.             ║
║                                                                              ║
║   By continuing, you confirm that:                                           ║
║                                                                              ║
║     • You own a legitimate copy of Spider-Man 3 (Xbox 360)                   ║
║     • The ISO you provide is from your own disc dump                         ║
║     • You will not distribute the resulting executable or game files         ║
║     • This tool does not provide, host, or distribute any game content       ║
║       — the recompiled executable is derived game code and stays on          ║
║       your machine                                                           ║
║                                                                              ║
║   Spider-Man 3 is a trademark of Activision / Marvel. This tool is not       ║
║   affiliated with or endorsed by them. It is a fan-made recompilation        ║
║   tool provided for preservation and personal use.                           ║
║                                                                              ║
║   ┌──────────────────────────────────────────────────────────┐              ║
║   │   ☐  I have read and agree                               │              ║
║   └──────────────────────────────────────────────────────────┘              ║
║                                                                              ║
║   ┌──────────────┐  ┌────────────────────────┐                              ║
║   │   Continue → │  │   Exit (I don't agree) │                              ║
║   └──────────────┘  └────────────────────────┘                              ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

The checkbox is required. "Continue" is disabled until checked. This is a one-time gate — subsequent launches skip it (stored in `settings.toml`: `legal.agreed = true` with a timestamp).

### 8c. Step 2 — Toolchain check

This is the hardest step for a non-technical user. The app must make it as painless as possible while being honest about the cost.

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Build tools check                                     [← Back] [─][□][×]  ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   The recompiler needs these build tools to compile your game.               ║
║   This is a ONE-TIME install — you won't need to do this again.              ║
║   Total download: ~5–10 GB (mostly Visual Studio Build Tools).              ║
║                                                                              ║
║   ✓  LLVM / clang-cl 22.1.8        C:\Program Files\LLVM\                   ║
║   ⏳ VS 2022 Build Tools            downloading installer …                  ║
║       ┌──────────────────────────┐                                          ║
║       │  Downloading … 45%       │                                          ║
║       │  ██████████░░░░░░░░░░░░  │                                          ║
║       └──────────────────────────┘                                          ║
║       The installer will open. Check "Desktop development with C++".        ║
║   ✗  CMake 4.2.1                    not found                               ║
║       ┌──────────────────┐                                                  ║
║       │  Install CMake → │  ← downloads official installer                  ║
║       └──────────────────┘                                                  ║
║   ✓  Ninja 1.13.2                   on PATH                                 ║
║   ✓  Windows SDK                   10.0.26100                                ║
║                                                                              ║
║   ┌──────────────────────────────────────┐                                  ║
║   │   Continue →                         │  ← enabled when all green        ║
║   └──────────────────────────────────────┘                                  ║
║                                                                              ║
║   [Why does this need Visual Studio?]  [Skip — I'll install manually]       ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

**"Why does this need Visual Studio?" disclosure:**
```
  The recompilation translates your game's Xbox 360 code into C++ and
  compiles it into a Windows executable. This needs:
    • clang-cl (LLVM) — the compiler that handles the generated C++23 code
    • MSVC linker + Windows SDK — clang-cl uses Microsoft's linker and
      system libraries to produce the .exe
    • CMake + Ninja — the build system that orchestrates compilation

  These are the same tools used by professional C++ developers. The app
  can't bundle them (they're large and have their own licenses), but it
  can install them for you and verify they work.
```

**Missing tool — install flow:**
```
   ✗  LLVM / clang-cl               not found
       ┌──────────────────┐
       │  Install LLVM →  │  ← opens official installer in browser
       └──────────────────┘
       After installing, click [Re-check] to verify.
       or install manually from https://github.com/llvm/llvm-project/releases
```

### 8d. Step 3 — SDK download

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  ReXGlue SDK                                           [← Back] [─][□][×]  ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   The app needs the ReXGlue SDK (v0.8.0) — the recompilation toolkit.       ║
║   This is separate from the build tools. (~62 MB download)                  ║
║                                                                              ║
║   ⏳ Downloading ReXGlue SDK v0.8.0 …         34 MB / 62 MB                 ║
║       ████████████░░░░░░░░  55%                                             ║
║                                                                              ║
║   Extracting to: C:\SpiderMan3Recompiler\sdk\                                ║
║                                                                              ║
║   [Browse for existing SDK install…]  [Re-check]                             ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

### 8e. Step 4 — ISO selection

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Select your game ISO                                 [← Back] [─][□][×]   ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   Choose your Spider-Man 3 (Xbox 360) ISO file.                              ║
║                                                                              ║
║   ┌──────────────────────────────────────────────────────────┐  ┌──────┐   ║
║   │ D:\Games\Spider-Man 3 (USA, Europe).iso                  │  │Browse│   ║
║   └──────────────────────────────────────────────────────────┘  └──────┘   ║
║                                                                              ║
║   ✓  Valid Xbox 360 ISO (XDVDFS format)                                      ║
║   ✓  Game partition found: contains default.xex                              ║
║   ✓  ISO size: 6.8 GB (matches expected)                                     ║
║                                                                              ║
║   Output folder:                                                             ║
║   ┌──────────────────────────────────────────────────────────┐  ┌──────┐   ║
║   │ C:\Games\SpiderMan3                                       │  │Browse│   ║
║   └──────────────────────────────────────────────────────────┘  └──────┘   ║
║                                                                              ║
║   ┌──────────────────────────────────────┐                                  ║
║   │   Continue →                         │                                  ║
║   └──────────────────────────────────────┘                                  ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

**Invalid ISO:**
```
   ✗  This file is not an Xbox 360 ISO.
      Expected XDVDFS format. Got: UDF (DVD video disc).

      Make sure you're selecting the .iso file dumped from your
      Spider-Man 3 Xbox 360 disc, not a video DVD or other format.
```

### 8f. Step 5 — Settings review (simplified)

For first run, only the essential choices are shown. Full settings are available later.

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Ready to build                                       [← Back] [─][□][×]    ║
║──────────────────────────────────────────────────────────────────────────────║
║                                                                              ║
║   Everything is set up. Here's what will happen:                             ║
║                                                                              ║
║     ISO:      D:\Games\Spider-Man 3 (USA, Europe).iso                       ║
║     Output:   C:\Games\SpiderMan3\                                           ║
║     Preset:   Default (60 FPS, FXAA, recommended)                           ║
║     Runtime:  Prebuilt custom runtime (save fixes + ThinLTO)                ║
║     Time:     ~15–25 minutes (one-time)                                     ║
║                                                                              ║
║   After the build, you can change resolution, vsync, and other settings     ║
║   instantly — no rebuild needed. Just save and relaunch.                    ║
║                                                                              ║
║   [▼ Change settings]  ← opens full settings panel                          ║
║                                                                              ║
║   ┌──────────────────────────────────────────────────────────┐              ║
║   │   ▶  Build & Play                                        │              ║
║   └──────────────────────────────────────────────────────────┘              ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

### 8g. Step 6 — Build + Play
Transitions to the build progress screen (§3d) and then the build complete screen (§3e).

---

## 9. Rebuild vs. Instant-Apply Flows

### 9a. The two flows

```
 ┌─────────────────────┐                    ┌─────────────────────────┐
 │ TIER 1: Instant     │                    │ TIER 2: Rebuild          │
 │ (TOML settings)     │                    │ (source-baked cvars)     │
 │                     │                    │                          │
 │ Settings → Save     │                    │ Settings → Save & Rebuild│
 │    ↓                │                    │    ↓                     │
 │ Write spiderman3.toml│                   │ Regenerate app.h cvars   │
 │    ↓                │                    │    ↓                     │
 │ Relaunch game       │                    │ Re-apply patches         │
 │    ↓                │                    │    ↓                     │
 │ DONE (0 sec)        │                    │ Recompile + link (~3 min)│
 │                     │                    │    ↓                     │
 │                     │                    │ Deploy                   │
 │                     │                    │    ↓                     │
 │                     │                    │ DONE                     │
 └─────────────────────┘                    └─────────────────────────┘
```

### 9b. Cache structure (enables fast rebuild)

```
<output_folder>\
├── spiderman3.exe              ← current build
├── rexruntime.dll              ← prebuilt custom runtime (shipped with app)
├── TracyClient.dll
├── spiderman3.toml             ← Tier 1 settings (instant-editable)
├── game\                       ← junction/symlink to extracted\
├── user_data\                  ← saves + shader cache (preserved across rebuilds)
├── _recompiler\                ← internal cache (hidden from user)
│   ├── extracted\              ← cached ISO extraction (default.xex + assets)
│   ├── project\                ← cached CMake project (generated/ + src/)
│   ├── sdk\                    ← ReXGlue SDK (prebuilt, downloaded on first run)
│   └── state.json              ← build state: ISO hash, SDK version, settings hash
└── logs\
```

### 9c. Rebuild decision logic

```
User clicks "Save & Rebuild" (Tier 2 settings changed):
  1. Read state.json → get ISO hash, settings hash, SDK version
  2. Compute new settings hash from current Tier 2 settings
  3. Did Tier 2 cvars change? → regenerate spiderman3_app.h OnPreSetup block
  4. Did ISO change? → NO (user didn't provide a new ISO) → skip extraction
  5. Did generated code change? → NO (codegen depends on XEX, not cvars) → skip codegen
  6. Re-apply patches (xmp_bypass.cpp, spiderman3_app.h) — instant
  7. Recompile game project (compile + link) — ~2–3 min (the only real wait)
  8. Deploy (copy exe + DLLs + config)
```

This turns a ~20 min full build into a ~3 min rebuild.

### 9d. Tier 1 instant-apply flow

```
User clicks "Save & Apply" (Tier 1 settings changed):
  1. Write new values to spiderman3.toml
  2. If game is running: offer to relaunch
  3. If game is not running: done — next launch will use new settings
  No rebuild. No compilation. Zero wait.
```

### 9e. Rebuild progress (Tier 2)

The rebuild uses the same progress screen (§3d) but phases 1–3 show "✓ (cached — skipped)" and the overall bar starts at ~40%.

```
   ✓  1. Extract ISO             [cached — skipped]            —
   ✓  2. Initialize project      [cached — skipped]            —
   ✓  3. Codegen                 [cached — skipped]            —
   ✓  4. Apply patches           regenerated cvar block        0:01
   ⏳ 5. Compile + link           94 sources, clang-cl          ~2:30
      ████████████░░░░░░░░░░░░░░░░░░░░  45%
   ○  6. Deploy                   copy + config + junction     —
```

---

## 10. Advanced: Custom Runtime Build (Power-User Only)

Building the custom `rexruntime.dll` from SDK *source* (with the 3 save-system patches + ThinLTO + `kQueueFrames=2`) is the most complex part of the pipeline. It requires the SDK source tree with 16 git submodules, 3 source-tree fixes, and a separate CMake build (~25 min).

### 10a. UX approach
- **Default (recommended)**: the app ships a **prebuilt custom `rexruntime.dll`** (with all fixes + ThinLTO). This is SDK code, not game code — redistributable. The user's build links against it. No SDK source build needed for 95% of users.
- **Power-user opt-in**: in Advanced Settings → Runtime, the user can select "Build runtime from SDK source." This downloads the SDK source tree + submodules, applies the 3 source-tree fixes + 3 runtime patches, and builds. ~25 min. Exposed behind a clear warning.
- **Expert**: a "Build custom runtime only" action in the Advanced panel, for users who want to rebuild just the runtime (e.g., to try PGO, or change `kQueueFrames`).

### 10b. Runtime build progress (only if power-user opts in)

```
  ┌─ Custom Runtime Build (from SDK source) ─────────────────────────────┐
  │  ⚠  This is a power-user action (~25 min). The prebuilt runtime      │
  │      already includes all fixes. Only do this if you need to change  │
  │      runtime source (kQueueFrames, PGO, etc.).                       │
  │                                                                      │
  │  ✓  Download SDK source (16 submodules)           180 MB   2:15     │
  │  ✓  Apply 3 source-tree fixes (xxHash, mspack, CMake4)        0:02   │
  │  ✓  Apply 3 runtime patches (xam_ui, xam_enum, xenumerator)   0:01   │
  │  ✓  Apply perf patches (kQueueFrames=2, ThinLTO)              0:01   │
  │  ⏳ CMake configure + build (clang-cl, ThinLTO)              ~20:00  │
  │     ████████████░░░░░░░░░░░░░░░░░  62%                               │
  │  ○  Copy rexruntime.dll to output                              —     │
  └──────────────────────────────────────────────────────────────────────┘
```

### 10c. The 3 source-tree fixes (presented if they fail)
If the source-tree fixes (xxHash CMake path, libmspack symlinks, CMake 4.x module scan) fail to apply — e.g., because the SDK source version differs — the app shows:

```
  ⚠  Could not apply source-tree fix: xxHash CMake path

  The SDK source tree layout differs from what this tool expects.
  This can happen if the SDK source version is newer than v0.8.0.

  Options:
    1. [Use prebuilt runtime] — revert to the shipped prebuilt rexruntime.dll
       (already has all fixes — recommended)
    2. [Report this] — package the error + SDK version for the developers
    3. [Apply manually] — open the source tree and follow BUILD_GUIDE.md §7b
```

---

## 11. Tray Icon + Background Behavior

### 11a. System tray
- After a build, the app minimizes to the system tray (optional, configurable).
- Tray icon right-click menu:
  ```
  ▶ Play Spider-Man 3
  ─────────────────
  Settings (instant)
  Rebuild (advanced)
  Open output folder
  ─────────────────
  Exit
  ```
- Tray tooltip: `Spider-Man 3 Recompiler — Ready (last build: today 12:04 PM)`

### 11b. Notifications
- Build complete: `"Spider-Man 3 build complete. Click to play."` → click launches the game.
- Build failed: `"Spider-Man 3 build failed. Click to see details."` → click restores the window to the error screen.
- Play launched: `"Spider-Man 3 is running."` (dismissable)

---

## 12. CLI ↔ GUI Parity

Both interfaces drive the same pipeline engine. Every action available in the GUI has a CLI equivalent:

| GUI action | CLI equivalent |
|---|---|
| First-run wizard | `SpiderMan3Recompiler.exe setup` (interactive) |
| Build & Play | `SpiderMan3Recompiler.exe build && SpiderMan3Recompiler.exe play` |
| Quick Settings → Save & Apply | `SpiderMan3Recompiler.exe config set <cvar> <val>` (instant) |
| Advanced Settings → Save & Rebuild | `SpiderMan3Recompiler.exe recompile --source-cvar <n>=<v>` |
| Settings → Preset | `SpiderMan3Recompiler.exe config set preset performance` |
| Dependency check | `SpiderMan3Recompiler.exe status` |
| Open output folder | `SpiderMan3Recompiler.exe open` |
| Clean build | `SpiderMan3Recompiler.exe clean` |

**`--gui` flag**: `SpiderMan3Recompiler.exe build --gui` launches the GUI progress window even when invoked from the CLI. Useful for scripts that want visual feedback.

**`--yes` flag**: CLI non-interactive mode. Skips all confirmations, uses saved settings. Returns exit code 0 on success, non-zero on failure with the error message on stderr. Suitable for CI.

---

## 13. Self-Containment & Toolchain Strategy

### 13a. What the app ships vs. what the user must install

The prompt says "no Python/Node.js runtime dependency" — that's satisfied. But the app **cannot** be fully self-contained because the compile step requires a C++ toolchain that is too large to bundle and has its own redistribution restrictions.

| Component | Size | Shipped? | How it gets to the user |
|---|---|---|---|
| Orchestrator exe (ImGui + SDL3 + pipeline logic) | ~8 MB | **Yes** — in the app zip | Download from GitHub Releases |
| extract-xiso.exe | ~200 KB | **Yes** — bundled | In the app zip |
| Patch database (xmp_bypass.cpp, app.h templates, runtime patch defs) | ~50 KB | **Yes** — bundled | In the app zip |
| Game profile manifest | ~5 KB | **Yes** — bundled | In the app zip |
| Prebuilt custom rexruntime.dll (save fixes + ThinLTO + kQueueFrames=2) | ~12 MB | **Yes** — bundled | In the app zip (SDK code, redistributable) |
| TracyClient.dll | ~240 KB | **Yes** — bundled | In the app zip |
| ReXGlue SDK (prebuilt: rexglue.exe + libs + headers) | ~62 MB | **No** — downloaded on first run | App downloads v0.8.0 from official source |
| LLVM / clang-cl | ~500 MB | **No** — user installs | App links to official installer, verifies |
| VS 2022 Build Tools (MSVC linker + Windows SDK + CRT) | ~3–5 GB | **No** — user installs | App links to VS Build Tools installer |
| CMake | ~50 MB | **No** — user installs | App links to cmake.org, or auto-installs |
| Ninja | ~200 KB | **No** — auto-installs | App downloads directly (tiny) |
| **Total app download** | **~21 MB** | | |
| **Total after toolchain setup** | **~6–10 GB** | (mostly VS Build Tools) | |

### 13b. Why not bundle the toolchain?
- **Size**: VS 2022 Build Tools alone is 3–5 GB. Bundling would make the app download 5+ GB.
- **Licensing**: Microsoft's Visual Studio license does not allow redistribution of the build tools. LLVM's license allows it, but the size is still prohibitive.
- **Updates**: the toolchain updates independently. Bundling a specific version would become stale.
- **The N64 recomp precedent**: every console recomp project (SM64 PC, OoT PC) requires the user to install build tools. This is expected friction in this ecosystem.

### 13c. Mitigating the toolchain friction
The app makes the one-time install as painless as possible:
1. **Auto-detect**: scans registry, PATH, and known install paths for each tool.
2. **One-click install**: each missing tool has an "Install →" button that opens the official installer in the user's browser. For Ninja (tiny), the app downloads and installs it directly.
3. **Post-install re-check**: after the user returns from an installer, the app re-scans and updates the checklist.
4. **VS Build Tools, not full VS**: the app links to the "Build Tools for Visual Studio 2022" download (command-line build tools, no IDE) which is smaller than full Visual Studio. The user only needs the "Desktop development with C++" workload.
5. **Persistent detection**: once tools are found, they're cached in `settings.toml` so the check is instant on subsequent launches.

### 13d. The "I just want to play and I can't install 10 GB" user
This is a real persona that the app cannot fully serve. The recompiled exe is derived game code and cannot be distributed. The honest answer is: this tool requires a one-time development toolchain install. If the user cannot install build tools, they cannot use this tool. The first-run screen states this clearly upfront rather than hiding it.

Future mitigations (not v1):
- **Cloud build service**: user uploads their XEX, server runs codegen+build, returns exe. Legally identical to local build (the server processes the user's own game code and returns it). Would require infrastructure and has bandwidth costs. Not in scope.
- **Pre-compiled object files**: ship `.obj`/`.lib` files instead of source. Still derived game code — same legal problem. Not viable.

---

## 14. Edge Cases & Graceful Handling

| Scenario | UX response |
|---|---|
| User closes the app mid-build | Confirm dialog: "Building is in progress. Cancel and discard?" → cleans up `_recompiler/` partial artifacts |
| Build output folder is on a slow/network drive | Warn: "Output folder appears to be on a network drive. Build may be very slow. Continue?" |
| ISO is on a DVD drive (not ripped) | Error: "Cannot read directly from disc. Please rip the ISO to your hard drive first using a tool like wxPirs or Xbox Backup Creator." |
| User provides a PS2/GameCube Spider-Man ISO | Error: "This is not an Xbox 360 ISO. This tool only supports the Xbox 360 version of Spider-Man 3." |
| Disk space < 2 GB | Error: "Not enough free space. Need 2 GB, have X GB. Free up space or choose a different output folder." |
| Antivirus quarantines the built exe | Warning: "Your antivirus flagged spiderman3.exe. This is a false positive (recompiled code is unfamiliar to AV heuristics). You may need to add an exception. [How to add an exception]" |
| Build succeeds but game crashes on launch | Diagnostic mode: "The game crashed. Would you like to run the built-in diagnostic?" → checks for known runtime issues (ROV path, shader cache, save hooks) and offers auto-repair |
| User wants to move the output folder | "Move output folder" action that updates all junctions and config paths. The app tracks the output folder in `settings.toml`. |
| Multiple builds (different cvar sets) | Future: "Build profiles" — save/load named settings sets. V1 keeps a single active build; the user can rebuild to switch. |
| VS Build Tools installed but wrong workload | "Visual Studio is installed but the C++ workload is missing. Open VS Installer and add 'Desktop development with C++'." with a button that launches the VS Installer. |
| LLVM installed but too old | "LLVM version 18.x found, but version 22+ is required (the generated code uses clang-specific intrinsics). Please update LLVM." |
| User tries to change a Tier 2 cvar via Tier 1 TOML | The app detects the conflict and warns: "This setting is overridden by the compiled game code (OnPreSetup). To change it, use Advanced Settings and rebuild." |
| Toolchain install interrupted | "Setup was interrupted. Click Resume to continue from where you left off." — the wizard is resumable. |

---

## 15. Accessibility

- **Keyboard navigation**: every button and control is reachable via Tab. Enter activates. Esc closes dialogs.
- **High contrast**: the dark theme has a high-contrast variant (toggle in settings).
- **Font size**: scalable via a slider in settings (ImGui handles this natively).
- **Screen reader**: ImGui is not screen-reader-friendly by default. The CLI mode (`--help`, text output) is the accessible fallback. Documented in the help screen.
- **No flashing**: progress bars and animations are static or smooth — no strobing.

---

## 16. Visual Identity (Lightweight)

- **Color palette** (dark theme):
  - Background: `#1a1a2e` (deep navy)
  - Surface: `#16213e`
  - Tier 1 panel (instant settings): `#1a2a3e` (slight blue tint)
  - Tier 2 panel (rebuild settings): `#2a1a1e` (slight red tint — signals "cost")
  - Primary accent: `#e94560` (Spider-Man red)
  - Secondary accent: `#0f3460` (Spider-Man blue)
  - Success: `#2ecc71`
  - Warning: `#f39c12`
  - Error: `#e74c3c`
  - Text: `#ecf0f1`
  - Muted text: `#95a5a6`
- **Typography**: system sans-serif (Segoe UI on Windows). ImGui default font is fine; load Segoe UI at 16px for the body.
- **Iconography**: minimal. Status icons (✓ ✗ ⏳ ○ ⚠) are Unicode glyphs. No custom icon set needed.
- **Tier badges**: "⚡ Instant" (green) for Tier 1, "🔧 Rebuild" (amber) for Tier 2. These appear on section headers and buttons to reinforce the two-tier model at a glance.

---

## 17. Alignment with Architecture (from Architect peer)

The Architect peer is landing on:
- **C++ core engine** (pipeline orchestrator + module DLLs) with a **Qt6 frontend** (QML for wizard, C++ widgets optional).
- **Process model**: single GUI process spawning child processes (rexglue.exe, cmake, ninja) via captured stdout/stderr pipes feeding a progress bus.
- **Plugin/extension points**: (1) game profile abstraction (TOML manifest: cvars, hooks, runtime patches, toml template), (2) patch layer with three strata (cvar patches = TOML, source patches = templated .cpp/.h, runtime patches = SDK source), (3) pluggable ISO extractor.
- **GUI flow**: 7-step wizard mirroring the pipeline. CLI mode via `--headless`.

**UX alignment notes:**
- The Architect's three-strata patch model maps cleanly to our two-tier settings: cvar patches (TOML) = Tier 1, source patches (.cpp/.h) = Tier 2. Runtime patches (SDK source) = the power-user custom-runtime build (§10).
- The game profile abstraction is excellent for UX — it means the same app can support other Xbox 360 titles by loading a different profile. The UI should present "Game: Spider-Man 3" as a profile selector (even if v1 only ships one profile), making the architecture visible and extensible.
- **Framework note**: I recommended ImGui; Architect recommended Qt6/QML. This is an implementation decision, not a UX decision — both can render the screens described here. Qt6/QML would produce a more polished wizard (better animations, native file dialogs, accessibility) at the cost of a larger dependency. ImGui is lighter but rougher. **Recommendation: defer to the implementation team; the UX design is framework-agnostic.**
- **Cancel/resume**: Architect correctly flags that long-running steps need cancel/resume and must not freeze the UI. The progress screen (§3d) supports cancel (with cleanup). Resume is supported at phase boundaries — the wizard is resumable if interrupted during toolchain setup.

---

## 18. Open Questions for Tomorrow's Review

These are decisions I've made provisionally but flag for your input:

1. **Toolchain install: auto-install vs. guide-only?** I recommend auto-install where possible (Ninja, CMake — small, direct download) and guide+verify for large tools (VS Build Tools, LLVM — open official installer in browser). Trade-off: auto-install of small tools is smooth; large tools need interactive installers we can't silently run. Resolved: this is the only viable approach.

2. **Prebuilt custom runtime: bundle in the app zip, or download on first run?** I recommend bundle (12 MB — it's SDK code, redistributable, and saves a download step). The app zip goes from ~8 MB to ~21 MB. Trade-off: slightly larger initial download vs. smoother first run.

3. **Tier 2 settings: how much to expose?** I recommend exposing the common ones (frame rate, AA, anisotropic, presets) with the rendering-fixes block locked/warned, and a raw cvar editor for everything else. Trade-off: too many exposed options confuses casual users; too few frustrates power users.

4. **GUI framework: ImGui vs Qt6/QML?** Deferred to implementation. Both can render this design. Qt6 is more polished; ImGui is lighter. The UX design is framework-agnostic.

5. **Cloud build service (future)?** Not in scope for v1. Would eliminate the toolchain install for non-technical users but requires server infrastructure. Flagged as a v2+ opportunity.

6. **Build profiles (multiple saved cvar sets)?** Deferred to v2. V1 keeps a single active build. The `settings.toml` format is designed to support profiles later.

7. **Should the app manage the extracted ISO files, or let the user point at an existing extraction?** I recommend the app manages extraction (caches in `_recompiler/extracted/`), but offers "I already have extracted files" as an advanced option for users who ran extract-xiso themselves.

8. **Auto-update for the app itself?** Not in scope for v1. The app checks for updates on launch and shows a non-intrusive banner: "Version 1.1 available — download from GitHub."

9. **Repo/app naming: `spiderman3-recompiler` vs `xbox360-recompiler`?** The distribution design doc recommends `xbox360-recompiler` as the repo name for reduced trademark exposure ("Spider-Man" is an Activision/M Marvel trademark) and to avoid BSD-3 clause-3 issues (which restrict using the SDK author's project name, not ours — but a generic name is safer overall). The trade-off: a generic name ("Xbox 360 Recompiler" with Spider-Man 3 as a game profile) reduces takedown risk and is extensible to other titles, while a specific name ("Spider-Man 3 Recompiler") is clearer for SEO and the immediate audience. This is an owner decision — the UX design works either way. The game-profile architecture (from the Architect) supports both: the app shell is generic, the profile is specific.

---

## 19. Summary: What the User Sees, End to End

### Casual user (first time, full journey):
```
Download (~21 MB zip)
  → Extract + run SpiderMan3Recompiler.exe
    → Legal agreement (one-time, click-through)
      → Toolchain check (one-time: install VS Build Tools + LLVM + CMake — ~10 GB, guided)
        → SDK download (one-time: ~62 MB, automatic)
          → Select ISO (file picker + validation)
            → Settings review (default preset)
              → Build & Play (~20 min one-time progress screen)
                → Play (launches spiderman3.exe)
                  → Later: reopen app → Play (instant)
                    → Tweak resolution/vsync/AA → Save & Apply → relaunch (instant, NO rebuild)
```

### Technical user (subsequent visits):
```
Open app → Home screen → Play (instant)
  OR
Open app → Settings → Advanced → change source cvar → Save & Rebuild (~3 min, cached)
  OR
CLI: SpiderMan3Recompiler.exe config set vsync false && SpiderMan3Recompiler.exe play
  OR
CLI: SpiderMan3Recompiler.exe recompile --source-cvar video_mode_refresh_rate=90.0
```

**The casual user sees 6 clicks, one ~20-minute one-time build, and then instant settings tweaks forever.** The technical user gets full cvar control, CLI automation, raw log access, and ~3-minute incremental rebuilds. Both get actionable errors and never see a terminal unless they choose to.

The two-tier settings model is the design's core insight: it transforms the user's mental model from "every change costs 20 minutes" to "most changes are instant; only deep rendering changes need a 3-minute rebuild."
