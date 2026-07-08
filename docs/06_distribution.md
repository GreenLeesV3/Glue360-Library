# Spider-Man 3 Recompiler — Distribution & Packaging Design

> **Status:** Design document (brainstorm output, 2026-07-07). Awaits owner
> review before any implementation.
>
> **Scope:** How to package, publish, and update the "Spider-Man 3 Recompiler"
> application — the orchestration tool + patch database + game profiles that
> turn a user's own legally-owned Spider-Man 3 (Xbox 360) ISO into a playable
> Windows executable. This document covers repository structure, build-vs-
> download strategy, large-dependency handling, legal posture, update
> mechanism, and cross-game extensibility.

---

## Table of Contents

1. [Guiding Principles](#1-guiding-principles)
2. [What Exactly Are We Distributing?](#2-what-exactly-are-we-distributing)
3. [Repository Structure](#3-repository-structure)
4. [.gitignore](#4-gitignore)
5. [Build vs Download (User Tiers)](#5-build-vs-download-user-tiers)
6. [Size Budget & Large-Dependency Handling](#6-size-budget--large-dependency-handling)
7. [Prebuilt vs Source Dependencies](#7-prebuilt-vs-source-dependencies)
8. [Legal Considerations](#8-legal-considerations)
9. [Update Mechanism](#9-update-mechanism)
10. [Cross-Game Extensibility (Game Profiles)](#10-cross-game-extensibility-game-profiles)
11. [Packaging Artifacts (Release ZIPs)](#11-packaging-artifacts-release-zips)
12. [First-Run Flow](#12-first-run-flow)
13. [CI / Release Pipeline](#13-ci--release-pipeline)
14. [Risks & Open Questions](#14-risks--open-questions)
15. [Recommendation Summary](#15-recommendation-summary)

---

## 1. Guiding Principles

1. **No game-derived code, ever — the real crux, not the ISO.** The repo,
   releases, and app never contain the ISO, the XEX, extracted assets, the
   118 MB game exe, **or the 92 generated `.cpp` files / codegen cache**.
   This is the load-bearing legal constraint, and it is *not* primarily about
   the ISO (which is obviously excluded) — it is about the fact that
   `rexglue codegen` performs a **mechanical transpilation of the game's
   43,676 copyrighted PowerPC functions into x86-64 C++**. The 92
   `spiderman3_recomp.NN.cpp` shards (377 MB) are a literal lift of
   Treyarch/Activision's code section (`0x82280000`–`0x82BECE1C`), just
   retargeted to a different ISA; `spiderman3_init.cpp` (1.6 MB) embeds the
   XEX's data sections (RTTI, string tables, vtables, static data). Both
   are **derivative works of copyrighted game code**, on the same legal
   footing as a ROM hack or a decompilation. The compiled 118 MB exe is the
   same derivative, in binary form. We ship *recipes and patches* — our
   original code that hooks into generated code at link time — never the
   generated code itself, never the exe. See §8.1 for the design fork this
   forces (codegen-on-demand, not shipped source) and §8.2 for why our
   patches are legally distinct from the generated code they interpose on.
2. **Our patches are original interposition, not game code — and that
   distinction is what makes them redistributable.** Every fix we ship is
   one of: (a) a cvar name/value string set in `OnPreSetup`, (b) a
   `REX_HOOK_RAW` link-time symbol override we *wrote* (e.g.
   `xmp_bypass.cpp`'s 3 hooks), or (c) a source edit to the *RexGlue SDK
   runtime* (`rexruntime.dll`), which is third-party SDK code, not game
   code. None of these are the game's code — they are our code that
   *interposes* on the generated code at link time, the same legal posture
   as a mod loader's hook DLL or an emulator's HLE implementation. This is
   the critical asymmetry: **the generated `.cpp` shards are
   non-redistributable derivatives; our hooks/cvars/runtime-patches that
   modify behavior *at the recompilation boundary* are our original work.**
   We ship the latter, never the former. The design fork in §8.1 follows
   directly from this.
3. **Portable-first.** The working "standalone folder" model
   (`C:\tmp\Spider-Man 3\` with `spiderman3.exe` + `rexruntime.dll` +
   `TracyClient.dll` + `spiderman3.toml` + `game\` junction + `user_data\`)
   is the canonical *deployment* shape for the per-user output. No
   installer, no registry, no `%APPDATA%`. Drop the folder anywhere,
   double-click, play. (Note: the folder is *produced locally by the user*,
   not shipped — see Principles 1–2 and §8.1. We ship the app that builds
   it, not the folder itself.)
4. **The patch database is the product.** Cvars, hooks, runtime source
   fixes, and game profiles are small text/code — our original work. They
   are the thing that changes between releases and the thing users want
   updates for. The build machinery and the SDK are relatively static
   dependencies. Design so the *patch database can be updated without
   re-releasing the app* (see §9).
5. **Build tools are the user's responsibility; the SDK is not.** CMake,
   LLVM clang-cl, MSVC, Ninja, and the Windows SDK are large, licensed,
   Microsoft/LLVM-distributed tools. We do not and cannot redistribute
   them. The RexGlue SDK, by contrast, is a single self-contained
   dependency with its own license that we *can* point users at and
   (optionally) fetch automatically. The user must also run the full
   recompilation+build themselves (Principle 1) — there is no legal way
   to precompute the game exe for them.

---

## 2. What Exactly Are We Distributing?

A clarifying distinction that drives the whole design — **there are two
distinct artifacts, never conflate them:**

### Artifact A — The Recompiler App (what we ship on GitHub)

The orchestration tool that a user runs once to turn their ISO into a playable
folder. It contains:

- **The app executable** (`spiderman3-recompiler.exe` or similar — the app
  display/download name is an owner decision per §10.4; the repo name is
  `xbox360-recompiler` but the flagship app may keep the Spider-Man 3 name) — a small
  GUI/CLI tool that drives the pipeline: locate ISO → extract → codegen →
  apply patches → build → deploy. This is *new code we write*, small (a few
  MB at most).
- **The patch database** — declarative descriptions of every fix:
  - Cvar sets (the ~17 cvars from `OnPreSetup` + the TOML tunables).
  - Hook definitions (the 3 `REX_HOOK_RAW` hooks in `xmp_bypass.cpp`).
  - Runtime source patches (the 3 SDK-runtime fixes + the `kQueueFrames` and
    EDRAM-barrier edits) as structured diff/patch records.
  - Game profile metadata (entrypoint hints, manifest fields, paths).
- **Game profile(s)** — TOML/JSON describing how to recompile a given title
  (Spider-Man 3 first; extensible to others — see §10).
- **Build scripts** — `build.bat` / `rebuild_runtime_lto.bat`-equivalents,
  parameterized by the active game profile.
- **Documentation** — the `DOCS/` tree (build guide, troubleshooting, cvar
  reference, lessons learned).
- **Function identification database (funcid)** — per-game, versioned,
  shareable. Address→inferred-name mappings + confidence + evidence for
  the game's 43,676 functions (18,871 named, 43.21% coverage for
  Spider-Man 3). Pure analysis metadata — the same legal category as a
  Ghidra/IDA symbol export — **not** game-derived code (§8.2). Any user
  with the same disc revision gets the same names; it is the community's
  accumulated RE work and ships as a release asset keyed to the profile's
  XEX hash (§11). Referenced from `profile.toml` and auto-fetched after
  codegen so the rename map applies (§9.5, §12).
- **NOT included (and why):**
  - The SDK (fetched on first run — 203 MB, its own license; §7).
  - The game ISO/XEX and extracted assets (user-provided copyrighted data).
  - **The 92 generated `.cpp` shards + `spiderman3_init.cpp`** — these are
    the mechanical transpilation of the game's 43,676 PPC functions; a
    non-redistributable derivative of copyrighted game code (§8.1). Produced
    locally by `rexglue codegen` from the user's XEX.
  - **The built 118 MB `spiderman3.exe`** — the same derivative in binary
    form; produced locally by the user's build.
  - The funcid database is **not** excluded — it is shareable analysis
    metadata (address→name mappings + evidence), not game-derived code,
    and is treated as a versioned per-game release asset. See §8.2, §9.5,
    §11.

   **The repo contains zero bytes of game-derived code.** That is the
   inviolable rule and the single most important legal property of this
   design.

### Artifact B — The Playable Game Folder (what the app produces, per-user)

This is the *output* the user gets after running the app against their ISO —
produced locally, never shipped by us:

```
<user-chosen>\Spider-Man 3\
├── spiderman3.exe              # 118 MB — built from the user's XEX
├── rexruntime.dll              # 12.5 MB — from SDK (patched custom build)
├── TracyClient.dll             # 247 KB — from SDK
├── spiderman3.toml             # generated from patch DB + profile
├── game\  → junction to extracted ISO contents
└── user_data\
    └── cache\shaders\          # grows as user plays; NOT shipped
```

Artifact B is never on GitHub. It is produced locally on the user's machine
from their ISO. The app's job is to assemble it correctly.

This separation is the single most important architectural decision in this
document. It keeps the repo tiny, keeps us legally clean, and makes updates
to the patch DB independent of the (large, slow) build pipeline.

---

## 3. Repository Structure

Proposed GitHub repo: **`xbox360-recompiler`** — the safe default that
avoids both Marvel/Sony trademark exposure and the SDK's BSD-3 clause-3
naming constraint. See §10.4 for the full naming decision and alternatives
(e.g. `spiderman3-recompiler` if the owner accepts higher takedown
exposure, or `rexglue-recompiler` only with Tom Clay's written
permission). The tree below uses `xbox360-recompiler/` as the concrete
example; the structure is name-agnostic.

```
xbox360-recompiler/
├── README.md                      # What this is, the legal stance, quickstart
├── LICENSE                        # Project license (see §8 — recommendation: MIT or Apache-2.0)
├── LICENSE.thirdparty             # Attribution for bundled/used third-party code
├── NOTICE.md                      # Trademark/copyright disclaimers
├── CHANGELOG.md                   # Human-readable release notes
├── .gitignore                     # See §4
├── .gitattributes                 # LF/CRLF normalization, binary marking
├── SECURITY.md                    # (optional) vulnerability reporting
├── CONTRIBUTING.md                # (optional) how to add a game profile / patch
│
├── app/                           # The recompiler app (Artifact A's engine)
│   ├── src/                       # App source — the orchestrator GUI/CLI
│   │   ├── main.cpp
│   │   ├── pipeline/              # extract → codegen → patch → build → deploy stages
│   │   ├── sdk_manager/           # locate/download/verify the RexGlue SDK
│   │   ├── toolchain_check/       # detect CMake/LLVM/MSVC/Ninja, report missing
│   │   └── profile/               # game-profile loader + patch applier
│   ├── CMakeLists.txt             # Builds the app itself (small)
│   └── resources/                 # icons, manifest (Windows), installer assets
│
├── patches/                       # THE PATCH DATABASE — the heart of the project
│   ├── schema.md                  # Documents the patch-record format
│   ├── games/
│   │   └── spiderman3/
│   │       ├── profile.toml       # game profile: name, title_id, sdk_version, entrypoints, manifest fields
│   │       ├── cvars.toml         # the OnPreSetup cvar set (all ~17)
│   │       ├── hooks/
│   │       │   └── xmp_bypass.cpp # the 3 REX_HOOK_RAW hooks (verbatim, as a patch asset)
│   │       ├── runtime_patches/
│   │       │   ├── xam_enum.io_pending.patch       # structured patch record
│   │       │   ├── xenumerator.writeitems.patch
│   │       │   ├── xam_ui.headless_notify.patch
│   │       │   ├── command_processor.kqueueframes.patch
│   │       │   └── render_target_cache.edram_barrier.patch
│   │       ├── source_overrides/
│   │       │   ├── spiderman3_app.h.in   # app class template (cvars+paths injected from profile)
│   │       │   ├── main.cpp.in
│   │       │   └── roundevenf.cpp         # CRT shim (verbatim)
│   │       ├── funcid/                # CURATED function-ID DB (shareable analysis metadata; §8.2)
│   │       │   ├── funcid_06_master.json     # 17.8 MB — master symbol DB (18,871 names)
│   │       │   ├── funcid_06_rename_map.csv  # 1.2 MB — sub_XXXXXXXX → semantic name
│   │       │   └── funcid_06_coverage.md     # coverage report (43.21%)
│   │       └── toml/
│   │           └── spiderman3.toml.in     # runtime TOML template
│   └── shared/                    # patches that apply to ALL games (generalizable fixes)
│       ├── rov_default.toml       # "set render_target_path_d3d12=rov as baseline"
│       └── fps_unlock_pattern.md  # the vblank-rate pattern, documented
│
├── scripts/
│   ├── extract_iso.ps1            # XDVDFS extraction wrapper (calls extract-xiso or SDK)
│   ├── build_game.bat             # parameterized build.bat (profile-driven)
│   ├── build_runtime_lto.bat      # custom rexruntime.dll build (power-user path)
│   ├── deploy_portable.ps1        # assembles the Artifact B folder + junctions
│   └── verify_toolchain.ps1       # checks CMake/LLVM/MSVC/Ninja presence + versions
│
├── docs/                          # the existing DOCS tree, cleaned
│   ├── BUILD_GUIDE.md
│   ├── TROUBLESHOOTING.md
│   ├── CVAR_REFERENCE.md
│   ├── RUNTIME_FIXES.md
│   ├── LESSONS_LEARNED.md
│   ├── FUNCTION_IDENTIFICATION.md
│   └── FILE_INVENTORY.md
│
├── .github/
│   ├── workflows/
│   │   ├── ci.yml                 # lint patch DB, build app, validate profiles
│   │   └── release.yml            # package release ZIPs on tag (see §13)
│   └── ISSUE_TEMPLATE/
│
└── thirdparty/                    # ONLY vendored code we legally redistribute
    └── extract-xiso/              # if we bundle an extractor (check its license)
```

### Key structural decisions

- **`patches/` is a first-class directory, not buried in `src/`.** It is the
  thing that changes most often and the thing contributors add to. Keeping it
  declarative and separate from the app engine means a cvar tweak is a
  one-line PR that doesn't touch build infrastructure.
- **`patches/games/<id>/`** is the per-game container. Everything
  Spider-Man-3-specific lives there. Adding a second game is "add a sibling
  directory" — see §10.
- **`patches/shared/`** captures the generalizable fixes (ROV-as-baseline,
  vblank-rate FPS unlock, aniso override) that the research identified as
  applicable to "any Xenos title." These are patterns, not forced values — a
  game profile opts in.
- **The app source (`app/src/`) is small and generic.** It reads the patch DB
  and a game profile, then drives the SDK tools. It contains no game-specific
  logic — that all lives in `patches/`.
- **No `generated/`, no `out/`, no `extracted/`, no `*.iso`, no `*.xex`.**
  These are build artifacts and game data; they are gitignored and produced
  locally.

---

## 4. .gitignore

```gitignore
# === Game data & user-provided inputs (NEVER commit) ===
*.iso
*.xex
*.xepack
*.xessb
extracted/
game/
data_dump/
*.bin

# === Build outputs (reproducible locally) ===
out/
build/
[Cc]ache/
*.obj
*.pdb
*.ilk
*.exp
*.dll.bak

# === Generated recompilation code (produced by rexglue codegen) ===
# These are ~377 MB and per-user-ISO; never commit.
generated/
spiderman3_recomp.*.cpp
spiderman3_init.*
spiderman3_register.cpp
sources.cmake
*.bak

# === The built game exe & shipped DLLs (per-user output) ===
spiderman3.exe
rexruntime*.dll
TracyClient*.dll
spiderman3.toml

# === SDK (downloaded on first run, not vendored) ===
sdk/
rexglue-sdk/

# === User runtime state ===
user_data/
logs/
*.log
*.xpso
*.xtr          # Tracy traces

# === Local funcid analysis output (per-user-ISO, regenerated locally) ===
# NOTE: The *curated* funcid DB IS shareable and ships under
# patches/games/<id>/funcid/ (tracked) or as a release asset (§8.2, §11).
# This only ignores the local workspace-root funcid/ output from a user's
# own RE passes, which is per-user and large (30 MB).
funcid/
funcid_*.json
funcid_*.csv
rename_manifest.txt

# === Editor / OS cruft ===
.vs/
.vscode/
.idea/
*.user
*.suo
.DS_Store
Thumbs.db
__pycache__/

# === Release staging ===
dist/
release/
```

### What is deliberately NOT gitignored

- `app/src/**` — our source.
- `patches/**` — the patch database (the point of the repo).
- `scripts/**` — build/deploy scripts.
- `docs/**` — documentation.
- `thirdparty/extract-xiso/**` — only if we vendor it (license-checked).
- `app/resources/` — icons, manifest.

---

## 5. Build vs Download (User Tiers)

Not all users want the same thing. Propose **three tiers**, all supported
from the same repo:

### Tier 1 — Prebuilt app, guided flow (the default, ~90% of users)

1. Download the latest `spiderman3-recompiler.zip` from GitHub Releases
   (~10–20 MB).
2. Unzip anywhere, run `spiderman3-recompiler.exe`.
3. The app's setup wizard: locate ISO → it fetches the pinned prebuilt
   RexGlue SDK v0.8.0 (~200 MB, one-time, cached) → checks for
   CMake/LLVM/MSVC/Ninja (directs to installers if missing) → runs the
   pipeline → produces the playable folder (Artifact B).
4. User never touches git, never compiles the app itself.

**What the user must still install themselves:** Visual Studio 2022 Build
Tools (for MSVC + Windows SDK + `link.exe`), LLVM/clang-cl, CMake, Ninja.
We cannot redistribute these. The app detects them and links to the official
installers. This is unavoidable — the game build is a C++23 clang-cl link.

### Tier 2 — Build the app from source (contributors, ~9%)

1. `git clone` the repo.
2. Build the app with CMake + their own compiler (the app is a small, normal
   C++ project; doesn't even need clang-cl — MSVC or gcc is fine for the
   *app*).
3. Run the built app, same flow as Tier 1 from there.

### Tier 3 — Manual / power-user (the current workflow, ~1%)

1. `git clone` the repo.
2. Manually run the pipeline scripts: `extract_iso.ps1` →
   `rexglue init/codegen` → apply patches by hand → `build_game.bat` →
   `deploy_portable.ps1`.
3. Optionally build a custom `rexruntime.dll` from the SDK *source* tree
   with `build_runtime_lto.bat` (needs the SDK source + 16 git submodules,
   ~2.5 GB+ — the power-user path for ThinLTO / custom runtime fixes).

**Recommendation: ship Tier 1 as the primary path, support Tier 2/3 via the
repo.** The prebuilt app removes the steepest part of the learning curve
(the 7-step manual pipeline) while keeping the build-from-source path
available for anyone who wants to audit or extend it.

---

## 6. Size Budget & Large-Dependency Handling

### Measured sizes (from this workspace, 2026-07-07)

| Artifact | Size | Where it lives | In repo? |
|---|---|---|---|
| **Game ISO** | 7.3 GB | user-provided | ❌ never |
| **Extracted game data** | 3.67 GB | local `extracted/` | ❌ never |
| **Game exe (output)** | 118 MB | local Artifact B | ❌ never |
| **Codegen output** (`generated/`) | 377 MB | local, per-ISO | ❌ never |
| **Funcid database** (curated) | 30 MB | `patches/games/<id>/funcid/` (tracked) or release asset | ✅ **yes** (shareable metadata, §8.2) |
| **Funcid database** (local RE output) | 30 MB | workspace-root `funcid/` (per-user) | ❌ never (gitignored) |
| **SDK prebuilt total** (bin+lib+include+cmake+share+licenses) | 203 MB | fetched on first run | ❌ fetched |
| └ SDK `lib/` (static libs, link-time only) | 132 MB | subset of SDK | ❌ fetched |
| └ SDK `bin/` (rexglue.exe + runtime DLLs) | 54 MB | subset of SDK | ❌ fetched |
| └ SDK `include/` (headers) | 17 MB | subset of SDK | ❌ fetched |
| **SDK source tree** (no submodules) | 2.5 GB | power-user fetch | ❌ optional |
| └ + 16 git submodules | large (FFmpeg, SDL3, Tracy, etc.) | power-user | ❌ optional |
| **Standalone runtime DLL** (`rexruntime.dll`) | 12.5 MB | in Artifact B | produced |
| **TracyClient.dll** | 247 KB | in Artifact B | from SDK |
| **spiderman3/src/ (all hand-written fixes)** | ~14 KB | in patch DB | ✅ **yes** |
| **Patch DB (estimated, all games)** | < 1 MB | `patches/` | ✅ **yes** |
| **App source (estimated)** | ~1–5 MB | `app/src/` | ✅ **yes** |
| **Docs** | ~1 MB | `docs/` | ✅ **yes** |

### Conclusions

- **The repo itself is tiny** (~5–10 MB): app source + patch DB + docs +
  scripts. Well under GitHub's 1 GB limit and 100 MB/file limit. No LFS
  needed.
- **The big things (ISO, extracted, codegen, exe, funcid) are all per-user
  and never enter the repo.** This falls out naturally from Principle 1.
- **The SDK (203 MB prebuilt) is fetched on first run, not vendored.** It's
  too big to comfortably vendor and has its own license (see §8). The app
  downloads the pinned v0.8.0 prebuilt SDK to a cache dir (e.g.
  `%LOCALAPPDATA%\xbox360-recompiler\sdk\0.8.0\`) and verifies a hash. Users
  can also point the app at an existing SDK install (`REXSDK` env var or the
  CMake package registry — the SDK already registers itself there per
  `rexglue_install.cmake`).
- **The SDK source tree (2.5 GB + submodules) is the power-user path only.**
  Needed solely for building a custom `rexruntime.dll` with the runtime
  source patches. Default users use the prebuilt SDK's `rexruntime.dll`
  *plus* a downloadable **pre-patched custom `rexruntime.dll`** we ship in
  Releases (12.5 MB — small enough to attach to a GitHub release). See §7.
- **Git LFS is not needed and must not be introduced.** Everything we'd
  consider "large" is either fetched at runtime or attached to a GitHub
  Release (which has its own 2 GB/per-file limit, ample for a 12.5 MB DLL,
  a 30 MB funcid DB, or a 200 MB SDK zip). **LFS is the wrong tool here
  regardless:** GitHub's free-tier LFS storage (1 GB) and bandwidth
  (1 GB/month) get blown fast by clone traffic — a single clone of a
  200 MB SDK LFS object by a few hundred users exhausts the monthly
  bandwidth, and LFS overages are billed per-GB. Release assets have no
  such per-clone bandwidth accounting and are the correct channel for
  large binaries that are fetched on demand, not cloned with the repo.
  Keep the git repo source + docs + small-data only; push all binaries
  (SDK zip, runtime DLL, funcid DB) to Release assets.

---

## 7. Prebuilt vs Source Dependencies

### RexGlue SDK — prebuilt, fetched on first run

**Decision: use the prebuilt SDK v0.8.0 as the default; fetch and cache it.**

Rationale:
- The prebuilt SDK is 203 MB, self-contained, and already what the working
  build uses (`CMAKE_PREFIX_PATH` → the SDK root).
- Building the SDK from source requires the 2.5 GB source tree + 16 git
  submodules + the same clang-cl/MSVC toolchain + the 3 build-fix patches
  (xxHash CMake path, libmspack symlinks, CMake 4.x module scan). That's a
  power-user path, not a default.
- The SDK has its own license (`licenses/` in the SDK root). Fetching it from
  its official distribution point preserves the upstream license/notice
  chain. Re-hosting it in our Releases is permitted — the SDK is BSD-3
  licensed (§8.5, verified 2026-07-07), which allows redistribution with
  notice retention.

**Fetch strategy:**
1. App checks for a local SDK at `%LOCALAPPDATA%\xbox360-recompiler\sdk\0.8.0\`
   or at a user-specified path.
2. If missing, app downloads `rexglue-sdk-0.8.0-win64.zip` from either:
   - The official RexGlue release URL (preferred), or
   - Our GitHub Releases as a fallback mirror (if license permits).
3. Verify SHA-256 against a pinned hash in the app.
4. Extract to the cache dir, set `CMAKE_PREFIX_PATH` to it for the game
   build.

**Custom `rexruntime.dll` — prebuilt patched DLL shipped in Releases**

The save-system and performance fixes require a *custom* `rexruntime.dll`
built from the SDK source tree with these patches:
- `xeXamEnumerate` returns `X_ERROR_IO_PENDING` (async path)
- `WriteItems` returns `X_ERROR_SUCCESS` for 0 items
- `xeXamDispatchHeadless` XN_SYS_UI callbacks
- `kQueueFrames = 2`
- EDRAM barrier skip
- ThinLTO build

**Decision: ship the pre-patched `rexruntime.dll` (12.5 MB) as a Release
attachment. Default users download it as part of the first-run fetch. Power
users can build it themselves with `build_runtime_lto.bat` against the SDK
source.**

This is the key size optimization: the 2.5 GB SDK source tree is needed
*only* to produce a 12.5 MB DLL. By shipping that DLL, we make the custom
runtime available to everyone without the 2.5 GB download.

### LLVM / MSVC / CMake / Ninja — NOT bundled, user-installed

**Decision: do not bundle; detect and direct.**

- These are large, licensed, actively-updated toolchains with their own
  installers. Bundling is impractical (LLVM alone is ~2 GB; VS Build Tools
  several GB) and likely violates their distribution terms.
- The app's `toolchain_check` module runs `where cmake`, `where clang-cl`,
  detects VS 2022 via `vswhere.exe`, and reports exactly what's missing with
  install links:
  - CMake: https://cmake.org/download/
  - LLVM: https://github.com/llvm/llvm-project/releases
  - VS 2022 Build Tools: https://visualstudio.microsoft.com/downloads/
  - Ninja: https://github.com/ninja-build/ninja/releases
- We pin **verified versions** (CMake 4.2.1, clang-cl 22.1.8, MSVC 14.44,
  Ninja 1.13.2) in the app config and warn on mismatch, but accept newer.

### extract-xiso — vendor if license permits, else document

ISO extraction needs an XDVDFS-aware tool (`extract-xiso`, wxPirs, or the
SDK's built-in if it has one). `extract-xiso` is BSD-licensed and small
(~hundreds of KB). **Decision: vendor it under `thirdparty/extract-xiso/`
with its LICENSE file, if the license permits.** Otherwise, document it as a
prerequisite and let the app locate an existing install.

---

## 8. Legal Considerations

This is the section most likely to get scrutinized. The posture must be
defensible and clearly communicated.

### 8.1 The generated-code crux — and the design fork it forces

The real legal exposure in this project is **not** the ISO (obviously
excluded) — it is the 92 generated `.cpp` files, a literal mechanical lift
of the game's 43,676 copyrighted PPC functions. `rexglue codegen`
transpiles Treyarch/Activision's code section (`0x82280000`–`0x82BECE1C`)
into x86-64 C++ verbatim; `spiderman3_init.cpp` (1.6 MB) packages the XEX's
data sections (RTTI, string tables, vtables, static data). The compiled
118 MB exe is the same derivative in binary form. This is on the same
legal footing as a ROM hack or a decompilation — it is **not** our
original work, and we cannot redistribute it without a license from the
rights holder. This single fact drives build-vs-download, repo size, the
"easy to clone and run" goal, and the first-run UX. It must be resolved as
an explicit design fork, not inherited silently.

#### Fork A — ship the generated source in-repo

Ship the 92 `spiderman3_recomp.NN.cpp` shards + `spiderman3_init.cpp`
(377 MB) committed to the repo (via Git LFS or a Release asset).

- **Pros:** enables true clone-and-build — a contributor clones the repo,
  runs `cmake --build`, and gets an exe without owning the game or running
  codegen. Matches the posture of the existing workspace README
  ("ship recompiled code, not assets") and the ecosystem norm referenced in
  RESEARCH §6.1.
- **Cons:** **the strongest legal gray area** — we'd be distributing a
  derivative of copyrighted game code at scale. Even with a "you must own
  the game" disclaimer, shipping the literal transpilation is materially
  different from shipping patches that hook into it. Adds 377 MB of repo
  bulk (LFS or Release-asset bandwidth). GitHub may flag it. Precedent in
  the recompilation ecosystem (N64 recomp projects: SM64, OoT, MM PC ports)
  is to **not** ship the recomp output — they ship only wrapper/patch code
  and require users to supply their own ROM and run recompilation.

#### Fork B — codegen-on-demand from the user's own XEX *(RECOMMENDED)*

Ship only our original code (app, patches, profiles, funcid metadata).
The user runs `rexglue codegen` against their own `default.xex` to
produce the 92 shards locally; our patch DB + funcid rename map apply
post-codegen.

- **Pros:** **legally clean** — the repo contains zero bytes of
  game-derived code. Keeps the repo tiny (~5–10 MB). Matches the N64 recomp
  ecosystem norm and the project's stated "users provide their own ISO"
  constraint, extended by the same logic to the recomp output. No LFS, no
  takedown exposure for the generated source.
- **Cons:** users need the game + SDK + toolchain before the repo builds;
  the first-run is "install 10 GB toolchain + full build" (~20 min), not
  "clone and run." The funcid rename map must apply post-codegen (the app
  runs `funcid_06_rename.py` against the freshly-generated shards — see
  §12, step 6). Clone-and-build is only possible for contributors who also
  own the game.

#### Decision: Fork B

We adopt **Fork B (codegen-on-demand)**. Rationale:
1. The legal posture must be defensible, and shipping 377 MB of
   transpiled copyrighted code is the single largest exposure in the
   project — it dwarfs the ISO question (which is settled) and the
   patch-interposition question (which is clean, §8.2).
2. It matches the ecosystem norm (N64 recomp projects) and the project's
   own stated constraint.
3. It keeps the repo small and LFS-free (§6, §11 corollary).
4. The first-run cost is honest: anyone recompiling a copyrighted game
   *should* own the game and run the recompilation. We mitigate the UX
   friction (guided wizard, cached SDK fetch, prebuilt patched runtime
   DLL, toolchain prerequisites checker) but we do not eliminate it by
   shipping derived code.

The README must state this explicitly: "This tool does not provide or
distribute any game data or recompiled game code. You must own a
legitimate copy of Spider-Man 3 (Xbox 360), provide the ISO yourself, and
run the recompilation on your own machine. The tool applies compatibility
patches and orchestration; it does not ship the game's code in any form
(source or binary)."

### 8.2 Patches are interposition, not modification of game assets

Every fix in this project is one of:
- **A cvar** set in the recompilation runtime (a host-side config knob, not a
  change to game code or assets).
- **A `REX_HOOK_RAW` link-time symbol override** — a strong symbol that
  replaces a weak generated alias *in the recompiled C++*, not in the
  original XEX. The hook runs in the recompilation's host-side wrapper, not
  in the game's binary. This is our original code (§1 Principle 2), distinct
  from the generated shards it overrides.
- **A source edit to the RexGlue SDK runtime** (`rexruntime.dll`) — a
  third-party library we build ourselves, not the game.

None of these modify the game's copyrighted assets, code, or binary. They
are compatibility shims at the recompilation boundary — legally analogous to
a mod loader's hook DLL or an emulator's HLE implementation. This should be
stated in `NOTICE.md`.

**Funcid is analysis metadata, not game-derived code (distinct from
§8.1).** The function identification database (`funcid_*.json`, 30 MB)
contains **no game code bytes** — only `{addr, name, confidence, source,
evidence}` records mapping guest addresses to inferred names, with
evidence pointers (which vtable slot, which string literal, which import).
This is pure analysis metadata, the same legal category as a Ghidra or IDA
symbol export — **not** the literal PPC→C++ lift that the 92 generated
`.cpp` shards are. Unlike generated code, it is reusable: any user with the
same disc revision gets the same 18,871 names. It is the community's
accumulated RE work (43.21% coverage) and RESEARCH §11.5 calls for exactly
this kind of "symbol sidecar file mapping guest addresses→names, contributed
by decomp/RE efforts." Therefore funcid **is** redistributable and ships as
a versioned per-game release asset keyed to the profile's XEX hash
(§9.5, §11), referenced from `profile.toml`, and auto-fetched after codegen
so the rename map applies (§12). Each game profile ships its own funcid DB
that grows as RE progresses (§10). This must be stated in `NOTICE.md`
alongside the patch-interposition note.

### 8.3 Trademark disclaimers

- "Spider-Man" and all related characters are trademarks of Marvel/Sony. We
  do not claim any affiliation. The name is used only to identify the
  compatible game.
- The repo name and app name should be generic ("Xbox 360
  Recompiler" with "Spider-Man 3" as a supported *profile*) to reduce
  trademark exposure — see §10 and §14. **Naming constraint from the SDK
  license (BSD-3 clause 3):** neither the name of the SDK copyright holder
  (Tom Clay) nor the names of contributors may be used to endorse/promote
  products derived from the SDK without specific prior written permission.
  This means `rexglue-recompiler` — which uses the SDK author's project
  name — is **not** the safe default; prefer `xbox360-recompiler` (or any
  non-SDK-author name) unless Tom Clay's written permission is obtained.
  See §10.4 for the full repo-name decision.
- `NOTICE.md` must include the standard "not affiliated with, endorsed by,
  or sponsored by Marvel, Sony, Microsoft, Tom Clay, or the RexGlue
  project" disclaimer.

### 8.4 License for the app itself

**Recommendation: MIT or Apache-2.0.**

- **MIT** is the most permissive, simplest, and most common for small
  compatibility tools. It maximizes the chance of community uptake and
  downstream forks for other games.
- **Apache-2.0** adds an explicit patent grant and is slightly more
  "corporate-friendly," at the cost of a longer notice file. Reasonable if
  the owner expects contributions from affiliated developers.
- **GPL** is not recommended — it would copyleft the patch DB and discourage
  adoption by anyone wary of GPL entanglement, and the project has no
  commercial-monetization concern that GPL protects against.

The patch DB (`patches/`), app source, scripts, and docs would all be under
the chosen license. The `LICENSE.thirdparty` file lists the licenses of
things we vendor or depend on (extract-xiso BSD, RexGlue SDK's own license,
SDL3 zlib, FFmpeg LGPL/GPL, etc.).

### 8.5 RexGlue SDK license handling *(verified 2026-07-07)*

The SDK is licensed under a **3-clause BSD ("New BSD") license**. Source:
`rexglue-sdk-0.8.0 Source code/LICENSE` (top-level). Copyright (c) 2026
Tom Clay, "Portions derived from the Xenia project" (Copyright (c) 2022
Ben Vanik and Xenia contributors). Clauses: (1) retain copyright notice in
source redistributions; (2) reproduce notice + disclaimer in binary
distributions; (3) neither the name of the copyright holder nor
contributors may be used to endorse/promote products derived from this
software without specific prior written permission.

Concretely, this resolves both open questions:

**(a) Mirror the prebuilt SDK zip in Releases — explicitly permitted.**
BSD-3 allows source + binary redistribution with notice retention. The
conditional in earlier drafts ("if license permits") is satisfied. Firm
decision: mirror `rexglue-sdk-0.8.0-win64.zip` (203 MB) in GitHub
Releases for reliability, with the upstream URL as primary. The only
obligation is shipping the SDK's LICENSE + notice file inside the mirror
zip and crediting Tom Clay + the Xenia project in `LICENSE.thirdparty` and
the README.

**(b) Pre-patched `rexruntime.dll` shipping — permitted as a derivative.**
BSD-3 allows modification + derivative binary redistribution under the
same notice-retention duty. Our custom `rexruntime.dll` (the 3 save-system
fixes + `kQueueFrames` + EDRAM barrier + ThinLTO build) is a derivative of
the SDK and ships as a Release asset (12.5 MB) with the SDK's LICENSE +
notice reproduced. This closes the earlier "derivative binary legality"
risk.

**(c) Clause 3 is a naming constraint — see §8.3 and §10.4.** BSD-3 clause
3 restricts using "the name of the copyright holder / contributors" (i.e.
Tom Clay, and arguably "RexGlue" as his project's name) to endorse/promote
derived products without written permission. This affects the repo-name
decision (§10.4): `rexglue-recompiler` uses the SDK author's project name
and may require written permission; `xbox360-recompiler` is the safe
default.

Either way, `LICENSE.thirdparty` must reproduce the SDK's BSD-3 license and
notice (Tom Clay + Xenia attribution), and the README must credit the
RexGlue project.

### 8.6 DMCA / takedown posture

- Have a `SECURITY.md` / contact for takedown requests.
- The legal footing (no game data, patches are interposition) is the same
  that has shielded emulator and decompilation projects for two decades.
- Be prepared for the repo to be flagged anyway; have a mirror plan (a
  secondary Git host, e.g. Codeberg or a self-hosted Gitea) documented in
  `CONTRIBUTING.md` so a takedown doesn't kill the project.

---

## 9. Update Mechanism

The patch DB is the part that changes most. Design so it updates *without*
re-releasing the app binary.

### 9.1 Versioning

- **App version:** semver `MAJOR.MINOR.PATCH` (e.g. `1.2.0`). Bumped when the
  app engine, pipeline, or SDK-fetch logic changes. Released as a new
  `spiderman3-recompiler.zip` on GitHub Releases.
- **Patch DB version:** a separate monotonic integer or date-based stamp
  (e.g. `2026.07.07` or `db-v42`), recorded in `patches/VERSION` or derived
  from the latest commit on `patches/`. This is what users want updated for
  new cvar fixes / hooks.
- **Game profile version:** per-game, in `patches/games/<id>/profile.toml`
  (`profile_version = 3`). Bumped when the profile's entrypoints, manifest
  fields, or required patches change.
- **SDK pin:** in the app config (`sdk_version = "0.8.0"` + SHA-256). Bumped
  only when a new SDK version is validated.

### 9.2 Patch DB updates without app re-release

**Primary mechanism: the app fetches the `patches/` directory from the
repo's latest release tag (or `main`) on startup, if newer than the bundled
copy.**

- The app ships with a bundled `patches/` snapshot (so it works offline on
  first run).
- On startup (or via a "Check for updates" action), it fetches
  `https://raw.githubusercontent.com/<owner>/<repo>/main/patches/VERSION`
  (or the GitHub API for the latest release asset), compares to the bundled
  version, and if newer, downloads a `patches-db.zip` Release asset
  containing just `patches/`.
- This decouples "new cvar fix" from "new app build." A contributor ships a
  cvar tweak as a PR to `patches/`; once merged and a `patches-db-vN` release
  is cut, every installed app picks it up automatically.
- The patch DB is small (< 1 MB), so this fetch is fast and cheap.

### 9.3 App self-update

- The app checks its own version against the latest GitHub Release on
  startup. If newer, it offers to download and replace itself (or just
  notifies and links to the Release).
- No silent auto-update — users should consent to an app binary change. The
  patch DB update (§9.2) *can* be automatic, since it's low-risk text/code
  that's already validated by CI.

### 9.4 Update failure / rollback

- The app keeps the previous `patches/` snapshot in
  `%LOCALAPPDATA%\xbox360-recompiler\patches\db-vN-1\` and rolls back if the
  new one fails to load (schema validation, parse error).
- The game profile is pinned in the user's project folder at build time, so
  a re-build uses the profile version that was active when they first ran
  the pipeline, not a silently-updated one — unless they explicitly
  "rebuild with latest patches."

### 9.5 What gets updated when

| Change | What updates | User action |
|---|---|---|
| New cvar (TOML-tier: vsync, async_shader, resolution_scale, present_effect) | `patches/games/.../toml/*.toml.in` | auto-fetched patch DB; **instant** — save & apply, no rebuild |
| New cvar (source-baked: the OnPreSetup forced set) | `patches/games/.../cvars.toml` | auto-fetched patch DB; rebuild required (~3 min) |
| New hook / hook fix | `patches/games/.../hooks/*.cpp` | auto-fetched patch DB; rebuild required |
| New runtime source patch | `patches/games/.../runtime_patches/*` + re-hosted `rexruntime.dll` | patch DB auto + DLL re-download prompt |
| **New function identifications (funcid DB growth)** | `funcid-spiderman3-db-vN.zip` release asset | auto-fetched after codegen; rename map re-applied (rebuild required to see new names in the exe) |
| New game profile (entrypoints) | `patches/games/<id>/profile.toml` | auto-fetched, rebuild required |
| New game supported | new `patches/games/<id>/` dir + profile + funcid DB | auto-fetched, appears in game list |
| App engine / pipeline fix | new app release ZIP | app self-update prompt |
| New SDK version validated | app config bump + new SDK fetch hash | app update + SDK re-download |

---

## 10. Cross-Game Extensibility (Game Profiles)

The research (`LESSONS_LEARNED.md` §5, `RUNTIME_FIXES.md` §1 "Generalizable?"
column) already identified which fixes generalize and which are
Spider-Man-3-specific. The architecture should make that explicit.

### 10.1 Game profile as the unit of extensibility

A **game profile** (`patches/games/<id>/profile.toml`) is a self-contained
description of how to recompile one title:

```toml
# patches/games/spiderman3/profile.toml
[game]
id = "spiderman3"                    # internal id, dir name
title = "Spider-Man 3"
platform = "xbox360"
title_id = "54510836"                # Xbox 360 title ID (for ISO identification)
region = "USA, Europe"
disc_type = "XGD2"
profile_version = 3

[sdk]
version = "0.8.0"
runtime_dll = "rexruntime-patched-0.8.0.dll"   # from Releases
requires_custom_runtime = true                 # needs the 3+2 runtime patches

[recomp]
project_name = "spiderman3"
manifest_fields = { game_root = "../extracted", out_directory_path = "generated/default" }

[entrypoint.functions]              # codegen entrypoint hints
"0x82967D40" = "sub_82967D40"
"0x8297481C" = "sub_8297481C"
# ... 9 total

[iso]
# How to identify & extract this game's ISO
expected_xdvdfs = true
game_partition_marker = "default.xex"
ignore_partitions = ["$SystemUpdate"]

[patches]
cvars = "cvars.toml"                # the OnPreSetup set
hooks = ["hooks/xmp_bypass.cpp"]
runtime_patches = [
  "runtime_patches/xam_enum.io_pending.patch",
  "runtime_patches/xenumerator.writeitems.patch",
  "runtime_patches/xam_ui.headless_notify.patch",
  "runtime_patches/command_processor.kqueueframes.patch",
  "runtime_patches/render_target_cache.edram_barrier.patch",
]
source_overrides = [
  "source_overrides/spiderman3_app.h.in",
  "source_overrides/main.cpp.in",
  "source_overrides/roundevenf.cpp",
]
funcid = "funcid/"                  # curated function-ID DB (§8.2); auto-fetched + rename applied post-codegen
toml_template = "toml/spiderman3.toml.in"

[shared_patches]                    # generalizable fixes this game opts into
rov_default = true                  # render_target_path_d3d12 = "rov" baseline
vblank_fps_unlock = "120.0"         # the vblank-rate pattern
```

Adding a second game = adding `patches/games/<newgame>/` with its own
`profile.toml`, cvars, hooks, overrides, and (as RE progresses) its own
curated `funcid/` database. No app code changes required.

### 10.2 The shared patch library

`patches/shared/` holds the patterns the research flagged as generalizable:

| Pattern | Source | Generalizable? |
|---|---|---|
| ROV as default render path | `render_target_path_d3d12 = "rov"` | Yes — "try ROV first for any Xenos blackscreen" (LESSONS 2.2) |
| Vblank-rate FPS unlock | `video_mode_refresh_rate` scaling | Yes — "any game that waits for N vblanks" (LESSONS 1.1, 4.3) |
| Anisotropic override | `anisotropic_override = "5"` | Yes — trivial sampler override (check engine clamp, LESSONS 3.3) |
| FXAA post-process | `swap_post_effect = "fxaa"` | Yes — CP-level, game-agnostic |
| Device selector + XN_SYS_UI lifecycle hook | `XamShowDeviceSelectorUI_Wrapper` | Yes — every game using the selector (RUNTIME_FIXES §5) |
| Device state bypass | `XamContentGetDeviceState_Wrapper` | Yes |
| XMP video bypass | `XMPGetStatus_Wrapper` | Any XMP-using title |
| Runtime enum fixes | `xeXamEnumerate` IO_PENDING + `WriteItems` SUCCESS | Yes — all async enumeration |
| Portable path override pattern | `OnConfigurePaths` exe-relative | Yes — the shape, not the hardcoded fallback |

A new game profile declares which shared patches it opts into
(`[shared_patches]` in `profile.toml`), plus its game-specific ones. This
avoids duplicating the ROV/vblank/aniso boilerplate in every profile while
keeping game-specific symptom fixes (the 4-cvar city combo) local to
Spider-Man 3.

### 10.3 Plugin architecture (future, not day-one)

A full plugin system (loadable game packs, a profile marketplace, signed
patches) is over-engineering for the first release. The directory-based
profile system above *is* the extensibility mechanism — it's just
file-system-driven rather than dynamically loaded. A plugin architecture can
be layered on later (e.g. the app scans `patches/games/*/profile.toml` and
lists all supported games; "installing a new game profile" = drop a folder
in `patches/games/` and the app picks it up). This is sufficient and keeps
the surface area small.

### 10.4 Repo naming — `xbox360-recompiler` *(recommended)* vs alternatives

**Recommendation: `xbox360-recompiler`** (or any non-SDK-author name),
with Spider-Man 3 as the flagship/first game profile.

Candidate names, ranked by legal safety:

| Candidate | Trademark exposure | BSD-3 clause 3 risk | Verdict |
|---|---|---|---|
| `xbox360-recompiler` | Low (generic platform name; "Xbox" is Microsoft's but used descriptively) | None (doesn't use Tom Clay's name) | **Safe default** |
| `spiderman3-recompiler` | Higher (uses Marvel/Sony's character name) | None | Possible but more takedown exposure (§8.3) |
| `rexglue-recompiler` | Low | **High** — uses the SDK author's project name; BSD-3 clause 3 requires written permission from Tom Clay to endorse/promote a derived product | **Not safe without written permission** |

Rationale for the generic default:
- The architecture is explicitly cross-game (the SDK is a general Xbox 360
  recompiler; the fixes split into generalizable vs. game-specific).
- A generic name reduces both Marvel/Sony trademark exposure (§8.3) *and*
  the SDK's BSD-3 clause-3 naming constraint (§8.5), and invites
  contributions for other titles.
- The app's game-picker UI lists profiles from `patches/games/*/`; Spider-Man
  3 is just the first entry.
- "RexGlue" is Tom Clay's project name; using it in our repo name to
  promote a derived product without his written permission likely violates
  clause 3 of his BSD-3 license. If the owner wants `rexglue-recompiler`,
  obtain written permission from Tom Clay first and retain it on file.
- If the owner strongly prefers a Spider-Man-3-specific project, the
  directory structure still supports adding games later, but the repo name
  signals intent and affects community framing and takedown exposure.

This is a naming/positioning decision for the owner; the architecture
supports either. The safe default is `xbox360-recompiler`.


---

## 11. Packaging Artifacts (Release ZIPs)

Each GitHub Release (tagged `app-vX.Y.Z`) ships:

| Asset | Size | Contents | Who uses it |
|---|---|---|---|
| `spiderman3-recompiler-windows-x64.zip` | ~10–20 MB | The prebuilt app exe + bundled `patches/` snapshot + docs | Tier 1 users |
| `patches-db-vN.zip` | < 1 MB | Just the latest `patches/` tree (includes funcid if in-repo) | Auto-fetched by installed apps (§9.2) |
| `rexruntime-patched-0.8.0.dll.zip` | ~12.5 MB | The pre-patched custom runtime DLL (BSD-3 derivative, §8.5) | Fetched on first run / runtime-patch update |
| `rexglue-sdk-0.8.0-win64.zip` | ~200 MB | The prebuilt SDK mirror (BSD-3 permits redistribution, §8.5) | Fetched on first run if upstream unavailable |
| `funcid-spiderman3-db-vN.zip` | ~20 MB | The curated funcid DB (master JSON + rename map + coverage) keyed to the Spider-Man 3 XEX hash | Auto-fetched after codegen (§9.5, §12); grows as RE progresses |
| `Source code (zip / tar.gz)` | auto by GitHub | The repo snapshot (includes `patches/games/spiderman3/funcid/` if in-repo) | Tier 2/3 users |

The app's first-run fetch logic pulls `patches-db`, `rexruntime-patched`,
`rexglue-sdk` (if not already cached), and `funcid-spiderman3-db` (after
codegen, keyed to the user's XEX hash) from these Release assets, with the
upstream SDK URL as primary for the SDK.

No installer (NSIS/MSI) as a primary artifact. An optional
`spiderman3-recompiler-setup.exe` (NSIS) can be offered later for users who
want Start Menu shortcuts, but the portable zip is canonical. This matches
Principle 3 (portable-first) and is coordinated with UXDesign.

---

## 12. First-Run Flow

(Aligned with the UXDesign agent's setup wizard.)

```
┌─ Welcome / legal notice ("you must own the game; we don't provide it")
│
├─ 1. Locate ISO
│     └─ file picker → app verifies XDVDFS + title_id match against profile
│        └─ if title_id unknown: offer to try the closest profile or abort
│
├─ 2. SDK setup
│     ├─ detect existing SDK (CMake package registry / REXSDK / cache)
│     ├─ if missing: download pinned v0.8.0 prebuilt (~200 MB) + verify hash
│     └─ download pre-patched rexruntime.dll (~12.5 MB)
│
├─ 3. Build-toolchain check
│     ├─ CMake ≥ 4.x?  LLVM/clang-cl 22.x?  VS 2022?  Ninja?
│     └─ if any missing: show install links, pause, re-check on resume
│
├─ 4. Extract ISO  (XDVDFS → extracted/)
│     └─ verify default.xex exists
│
├─ 5. Codegen  (rexglue init + codegen → generated/)
│     └─ ~43,676 functions → 92 cpp files
│
├─ 6. Apply patches  (from patches/games/spiderman3/)
│     ├─ inject cvars into spiderman3_app.h from cvars.toml
│     ├─ copy hooks/xmp_bypass.cpp into src/
│     ├─ copy source_overrides (main.cpp.in, roundevenf.cpp, spiderman3_app.h.in)
│     ├─ fetch funcid-spiderman3-db-vN.zip (~20 MB, keyed to XEX hash) → apply
│     │   rename map (funcid_06_rename.py) to the 92 generated shards
│     └─ (runtime patches already baked into the fetched rexruntime.dll)
│
├─ 7. Build  (build_game.bat → spiderman3.exe, 118 MB)
│     └─ progress UI, log streaming
│
└─ 8. Deploy  (deploy_portable.ps1 → assemble Artifact B folder)
      ├─ copy spiderman3.exe + rexruntime.dll + TracyClient.dll + spiderman3.toml
      ├─ create game\ junction → extracted/
      ├─ create user_data\ + user_data\cache\shaders\
      └─ "Play" button → launches the game
```

Subsequent runs (the app is also a launcher / patch-DB updater): check for
patch DB updates, offer "rebuild with latest patches" if the profile
changed.

---

## 13. CI / Release Pipeline

`.github/workflows/ci.yml` (on every push/PR):
- Lint the patch DB: validate every `profile.toml` parses, every referenced
  patch file exists, every cvar name is in the known-cvar list (from
  `CVAR_REFERENCE.md` / a `patches/schema.toml`).
- Build the app (`app/`) on Windows + Linux (the app itself is portable C++).
- Run any app unit tests.

`.github/workflows/release.yml` (on tag `app-vX.Y.Z`):
- Build the app (Release, Windows x64).
- Package `spiderman3-recompiler-windows-x64.zip`.
- Package `patches-db-vN.zip` from the `patches/` tree at that tag.
- Attach the pre-built `rexruntime-patched-0.8.0.dll` (built once, stored in
  the repo's Release assets or built in CI from the SDK source — the latter
  needs the SDK source as a CI dependency, which is heavy; simpler to build
  it locally on a verified machine and attach it).
- Generate release notes from `CHANGELOG.md`.

The custom `rexruntime.dll` build in CI is the awkward piece — it needs the
2.5 GB SDK source + submodules + the 3 build-fix patches + ThinLTO. CI for
that is possible but slow and storage-heavy. **Recommendation: build the
patched runtime DLL locally on a trusted machine, attach it to Releases
manually, and pin its SHA-256 in the app config.** The runtime changes
rarely (only when a new runtime source patch is added), so this is not a
maintenance burden.

---

## 14. Risks & Open Questions

1. **RexGlue SDK license — redistribution rights.** ✅ **RESOLVED
   (2026-07-07).** The SDK is 3-clause BSD (Copyright (c) 2026 Tom Clay,
   Xenia-derived). BSD-3 permits source + binary redistribution with
   notice retention. Firm decision: mirror the prebuilt SDK zip
   (`rexglue-sdk-0.8.0-win64.zip`, 203 MB) in GitHub Releases with the
   SDK's LICENSE + notice inside. See §8.5 for the full analysis and the
   clause-3 naming constraint.

2. **Pre-patched `rexruntime.dll` distribution legality.** ✅ **RESOLVED.**
   BSD-3 permits modification + derivative binary redistribution under the
   same notice-retention duty. Our custom `rexruntime.dll` ships as a
   Release asset (12.5 MB) with the SDK's LICENSE + notice reproduced. See
   §8.5(b).

3. **GitHub takedown risk.** Even with clean legal footing, game-title
   projects get flagged. Mitigation: generic repo name (§10.4), clear
   disclaimers (§8.3), a mirror plan. `[OWNER DECISION]` on naming.

4. **First-run download reliability.** Depending on a 200 MB SDK fetch on
   first run is a UX risk (slow networks, GitHub rate limits on Release
   assets for unauthenticated users). Mitigations: mirror the SDK on a CDN
   or the upstream URL as primary; show clear progress; allow the user to
   supply their own SDK path.

5. **Toolchain detection accuracy.** Detecting VS 2022 / clang-cl / Windows
   SDK versions reliably across install layouts is fiddly. `vswhere.exe` is
   the canonical VS detector; `where clang-cl` + `--version` parse for LLVM;
   CMake's `--version`. This is implementable but needs careful testing on
   clean machines.

6. **Patch DB schema stability.** The `profile.toml` / `cvars.toml` / patch
   record formats will evolve. Version the schema (`patches/schema.toml` →
   `schema_version = 1`) and have the app reject/rollback incompatible DBs.

7. **Custom runtime DLL vs. SDK version drift.** If the user fetches a newer
   SDK than the one our patched DLL was built against, the DLL ABI may
   mismatch. Pin the SDK version in the app config and only fetch the
   matching patched DLL. Bumping the SDK pin = rebuilding + re-hosting the
   patched DLL.

8. **Cross-game testing surface.** Claiming "works for other Xbox 360 games"
   requires actually validating other titles. Day-one should ship only
   Spider-Man 3 as a supported profile; other games are community
   contributions with their own profiles. Don't over-claim.

---

## 15. Recommendation Summary

| Decision | Recommendation |
|---|---|
| Repo name | `xbox360-recompiler` (safe default — avoids both Marvel trademark and BSD-3 clause-3 naming constraint); `rexglue-recompiler` only with Tom Clay's written permission. `[OWNER DECISION]` |
| License | MIT (or Apache-2.0 if patent grant wanted) |
| Primary distribution | Portable ZIP of the prebuilt app (Tier 1) |
| Installer | Not day-one; optional NSIS later |
| SDK | Prebuilt v0.8.0, fetched on first run, hash-verified, cached; user can override path. BSD-3 — mirroring in Releases permitted (§8.5) |
| Custom `rexruntime.dll` | Pre-patched DLL shipped as Release asset (12.5 MB); BSD-3 derivative — permitted (§8.5); power-users can build from SDK source |
| LLVM/MSVC/CMake/Ninja | Not bundled; detected + install links provided |
| Patch DB | Lives in `patches/`, auto-fetched from latest Release, independent of app version |
| Funcid DB | Curated, shareable analysis metadata (not game code); per-game under `patches/games/<id>/funcid/` or release asset; auto-fetched after codegen (§8.2, §9.5, §11) |
| Generated game code | **Never** shipped — codegen-on-demand from user's XEX (Fork B, §8.1); repo contains zero bytes of game-derived code |
| Update model | Patch DB auto-updates; funcid DB auto-updates; app binary update on user prompt |
| Cross-game | Directory-based game profiles under `patches/games/<id>/`; shared generalizable patches in `patches/shared/`; full plugin system deferred |
| Game data | Never in repo/Releases; user provides ISO; output produced locally |
| Git LFS | **Not used** — Release assets for all binaries (SDK, runtime DLL, funcid DB); LFS free-tier bandwidth blown by clone traffic |
| CI | Lint patch DB + build app; release workflow packages ZIPs |

---

*Authored by the DistributionPackaging plan agent, 2026-07-07. Grounded in
the workspace DOCS/ tree (BUILD_GUIDE, CVAR_REFERENCE, RUNTIME_FIXES,
LESSONS_LEARNED, FILE_INVENTORY, TROUBLESHOOTING), the spiderman3 source
(spiderman3_app.h, xmp_bypass.cpp, build.bat), the SDK source CMakeLists +
install rules, and measured sizes of all artifacts.*
