# Glue360 Library — Xbox 360 Recompiler

A Windows desktop application that **automates the recompilation of Xbox 360
games into native PC executables** using the [RexGlue360](https://github.com/) SDK.

The app takes a user-supplied ISO, runs the full RexGlue360 recompilation
pipeline, applies game-specific patches (save system, performance, cvars),
builds a custom runtime and the recompiled game, and deploys a portable,
playable standalone folder — no manual `cmake`/`ninja`/`clang-cl` wrangling
required.

> **v1.1 status:** CLI-first (headless mode). Two game profiles are supported:
> Spider-Man 3 (D3D12 + Vulkan) and Jurassic: The Hunted (D3D12). The app can
> be bundled as a fully standalone distribution with the RexGlue SDK, CMake,
> Ninja, and extract-xiso all bundled — requiring only Visual Studio 2022 on
> the target machine.

---

## Features

- **7-stage automated pipeline** — `iso_extract` → `rexglue_init` →
  `rexglue_codegen` → `apply_patches` → `build_runtime` → `build_game` →
  `deploy`. Each stage is a self-contained module behind a common `IStage`
  interface (`check_prereqs` / `run` / `is_complete`).
- **Resumable & retryable** — every stage checkpoints to `state.json`. A failed
  run can be retried from the last successful stage with `--resume`; completed
  stages are skipped automatically.
- **Game profiles are data, not code** — cvars, source patches, runtime overlays,
  function boundary overrides, switch table hints, and the deploy TOML template
  live in `profiles/<game>/`. Adding a game = adding a profile directory; no
  recompilation of the app.
- **Standalone distribution** — `create_standalone.bat` assembles a portable
  folder with the app, RexGlue SDK, CMake, Ninja, and extract-xiso all bundled.
  The only external requirement is Visual Studio 2022 with the "Desktop
  development with C++" workload.
- **Dependency check with auto-discovery** — `--check-deps` detects CMake,
  LLVM/clang-cl, MSVC, Ninja, the Windows SDK, and the RexGlue SDK. Bundled
  tools at `<app>/tools/` and a bundled SDK at `<app>/sdk/` are auto-discovered
  before falling back to PATH.
- **No hardcoded paths** — the app runs from any directory. All outputs live
  under a workspace root you choose (`--output`).
- **Graphics backend selection** — choose `--backend d3d12` (default,
  recommended on Windows) or `--backend vulkan` (Vulkan 1.2+, experimental)
  at recompile time.
- **Build optimizations** — LTO for Release builds, optional static MSVC
  runtime (when building SDK from source), ccache auto-detection, and AVX
  CPU targeting (`x86-64-v3`) for better PPC SIMD matching.
- **Manifest injection** — function boundary overrides (`[functions]` with
  `end`/`parent`), setjmp/longjmp addresses, switch table hints, and invalid
  instruction skips are injected into the codegen manifest from the game
  profile. Uses the `includes` mechanism for clean separation (inspired by
  Skate3Recomp).
- **CMakePresets** — formalized build configurations with hidden base presets
  and Debug/RelWithDebInfo/Release variants (inspired by UnleashedRecomp).

---

## Requirements

| Component | Minimum | Bundled? | Notes |
|-----------|---------|----------|-------|
| **OS** | Windows 10/11 x64 | — | The pipeline targets clang-cl + MSVC. |
| **Visual Studio 2022** | v143 toolset (14.40+) | No | "Desktop development with C++" workload. Provides MSVC + Windows SDK. |
| **LLVM / clang-cl** | LLVM 16+ | No | Must be installed separately. |
| **CMake** | 3.25+ (4.x recommended) | Yes | Bundled at `tools/cmake/` in standalone dist. |
| **Ninja** | 1.11+ | Yes | Bundled at `tools/ninja.exe` in standalone dist. |
| **RexGlue360 SDK** | 0.8.0 | Yes | Bundled at `sdk/` in standalone dist. |
| **extract-xiso** | any recent build | Yes | Bundled at `tools/extract-xiso.exe`. |
| **GPU (runtime)** | D3D12 feature level 11_0+ *or* Vulkan 1.2+ | — | For running the recompiled game. |

A complete dependency report is available at any time with:

```bat
xbox360-recompiler --check-deps
```

---

## Quick start

### Option A: Standalone distribution (recommended)

1. Download the standalone release zip and extract it.
2. Install Visual Studio 2022 with "Desktop development with C++" and LLVM/clang-cl.
3. Verify: `xbox360-recompiler.exe --check-deps`
4. Run a recompilation:

```bat
xbox360-recompiler.exe ^
    --iso   "D:\Games\Spider-Man 3.iso" ^
    --output "D:\Recomp\Spider-Man 3" ^
    --profile spiderman3 ^
    --backend d3d12
```

### Option B: Build from source

```bat
:: 1. Get the source
git clone https://github.com/GreenLeesV3/Glue360-Library.git xbox360-recompiler
cd xbox360-recompiler

:: 2. Build the app itself
build.bat

:: 3. Create a standalone distribution (bundles SDK + tools)
create_standalone.bat

:: 4. Run a recompilation
build\xbox360-recompiler.exe ^
    --iso   "D:\Games\Spider-Man 3.iso" ^
    --output "D:\Recomp\Spider-Man 3" ^
    --sdk    "C:\Tools\RexGlue360Recomp" ^
    --profile spiderman3 ^
    --backend d3d12
```

When the pipeline finishes you get a portable folder at `--output\standalone\`
containing the recompiled `.exe`, `rexruntime.dll`, the rendered `<game>.toml`,
a `game/` junction to the original game files, and a seeded `user_data/`
directory. Run the `.exe` directly — no installer.

---

## CLI usage

```
xbox360-recompiler — Xbox 360 game recompilation pipeline

USAGE:
  xbox360-recompiler --iso <path> --output <dir> [OPTIONS]
  xbox360-recompiler --check-deps [--sdk <path>] [--sdk-source <path>]
  xbox360-recompiler --help

REQUIRED (for a full run):
  --iso <path>          Path to the Xbox 360 ISO (.iso).
  --output <dir>        Output/workspace directory. All artifacts are
                        written under here (extracted/, project/, builds,
                        deploy/) plus a .recomp/ state+logs subfolder.

OPTIONS:
  --sdk <path>          Path to the prebuilt RexGlue360 SDK root
                        (the 'RexGlue360Recomp' dir with bin/, include/,
                        lib/). If omitted, the dep checker probes for it
                        (env REXGLUE_SDK, <app>/sdk/).
  --sdk-source <path>   Path to the RexGlue SDK *source* tree (for the
                        custom runtime build). Optional; if omitted the
                        runtime build stage is skipped (prebuilt DLL).
  --profile <name>      Game profile name (default: spiderman3).
                        Available profiles: spiderman3, jurassic_hunted.
  --backend <d3d12|vulkan>  Graphics backend (default: d3d12). If omitted,
                           you will be prompted to choose.
  --clean               Wipe state and stage outputs before running.
  --resume              Resume from the last completed stage (skip
                        stages already marked complete in state.json).
  --check-deps          Run only the dependency checker and print a
                        report; do not run the pipeline.
  --help, -h            Show this help text and exit.
```

---

## Supported games

| Game | Profile id | Status | Backend |
|------|-----------|--------|---------|
| **Spider-Man 3** (Title ID `415607E2`) | `spiderman3` | Fully supported — save system hooks, cvar patches, custom runtime build, FSR upscaling. | D3D12 + Vulkan |
| **Jurassic: The Hunted** (Title ID `41560870`) | `jurassic_hunted` | Fully supported — 60 FPS unlock, RTV render path, texture cache tuning, 171 function boundary hints. | D3D12 |

The profile system is designed to be **extensible**: a new Xbox 360 title is
added by creating a `profiles/<id>/` directory with its TOML manifest, source
patch templates, and (optional) runtime overlay headers. No app code changes
are required. Community-contributed profiles are welcome.

### Profile format

Each profile directory contains:

- **`profile.toml`** — declarative game description with:
  - `[profile]` metadata (id, name, title_id, sdk_version)
  - Top-level `setjmp_address` / `longjmp_address` (for Xenon CRT _setjmp fix)
  - `[functions]` table — function boundary overrides with `end`/`parent`/`name`
    (inspired by Skate3Recomp; injected via `includes` mechanism)
  - `[[switch_tables]]` — jump table overrides for misdetected switch tables
  - `[[invalid_instructions]]` — data values to skip in the instruction stream
  - `[build]` — runtime patch flags, `requires_sdk_source`
  - `[build_options]` — static MSVC runtime, LTO, CPU target
  - `[cvars]` — runtime cvar overrides
  - `[deploy]` — DLL list, game junction, user_data creation
- **`src/`** — hand-written game source (main.cpp, app.h)
- **`<game>.toml.template`** — runtime config template with `{{GAME_DATA_ROOT}}`

---

## Supported backends

| Backend | `--backend` id | Status | Requirements |
|---------|---------------|--------|--------------|
| **Direct3D 12** | `d3d12` | Default, recommended on Windows. | D3D12-compatible GPU (feature level 11_0+). |
| **Vulkan** | `vulkan` | Experimental. | Vulkan-capable GPU with Vulkan 1.2+ drivers and loader. |

The backend is selected at recompile time and compiled into the custom
`rexruntime.dll`. Both backends carry the full cvar suite; backend-specific
cvars for the inactive backend are silently ignored.

---

## Standalone distribution

`create_standalone.bat` assembles a fully self-contained distribution:

```
standalone/                    ~126 MB
├── xbox360-recompiler.exe     the app
├── profiles/                  game profiles (spiderman3 + jurassic_hunted)
├── patches/                   runtime patches
├── third_party/inja/          vendored inja header
├── sdk/                       RexGlue prebuilt SDK (release libs only)
│   ├── bin/                   rexglue.exe, rexruntime.dll, TracyClient.dll
│   ├── cmake/                 SDL3 CMake config packages
│   ├── include/               rex/, SDL3/, spdlog/, toml++/, fmt/, etc.
│   └── lib/                   release .lib files only (debug variants trimmed)
├── tools/
│   ├── ninja.exe              bundled Ninja
│   ├── extract-xiso.exe       bundled ISO extractor
│   └── cmake/                 bundled CMake (bin/ + share/)
└── README.txt                 quick-start guide
```

The only external requirement is **Visual Studio 2022** with the "Desktop
development with C++" workload (provides MSVC + Windows SDK) and **LLVM/clang-cl**.

---

## Architecture overview

```
ISO ─▶ iso_extract ─▶ rexglue_init ─▶ rexglue_codegen ─▶ apply_patches
     │                                       │              │
     │              manifest injection ◀──────┘              │
     │              (functions, setjmp,                       │
     │               switch_tables, invalid_instr)            │
     │                                                        │
                              build_runtime ◀────────────────┘
                                  │
                                  ▼
                             build_game ─▶ deploy ─▶ playable folder
```

The app is a CLI orchestrator driving seven sequential stages, each behind a
swappable `IStage` module. Game-specific knowledge is externalized into
**game profiles** (TOML + patch files) so the core stays game-agnostic.

Full design documents live in [`docs/`](docs/) — see
[`docs/README.md`](docs/README.md) for an index.

---

## Project layout

```
xbox360-recompiler/
├── build.bat                # builds the app (vcvarsall + clang-cl + ninja)
├── create_standalone.bat    # assembles standalone distribution
├── CMakeLists.txt           # app build definition
├── CMakePresets.json         # build presets (Debug/RelWithDebInfo/Release)
├── README.md                # this file
├── HOW_TO_USE.txt           # user guide
├── LICENSE                  # MIT
├── docs/                    # design documents (01–07)
├── src/
│   ├── core/                # orchestrator, PipelineContext, StateStore
│   ├── stages/              # one .cpp per pipeline stage (IStage impls)
│   ├── profile/             # GameProfile loader/validator, template renderer
│   └── deps/                # DependencyChecker
├── profiles/
│   ├── spiderman3/          # Spider-Man 3 game profile
│   └── jurassic_hunted/     # Jurassic: The Hunted game profile
├── patches/                 # bundled patches: games/<title-id>/ + shared/
└── tools/                   # bundled extract-xiso.exe
```

---

## Building from source

```bat
:: Prerequisites: VS 2022, LLVM, CMake, Ninja all installed
build.bat           :: Release build (default)
build.bat debug     :: Debug build
build.bat clean     :: Clean + rebuild
```

Or use CMake presets:

```bat
cmake --preset x64-Clang-Release -DCMAKE_PREFIX_PATH="C:\path\to\RexGlue360Recomp"
cmake --build out/build/x64-Clang-Release
```

---

## Legal disclaimer

**You must own a legitimate copy of any game you recompile with this tool.**

This application does **not** distribute, host, or fetch any copyrighted game
ISO, XEX, or asset data. It operates only on ISO files you provide. Recompiling
a game you do not own, or redistributing the resulting executable or game files,
may infringe the rights of the game's publisher and is **your** responsibility,
not this project's.

---

## License

MIT — see [`LICENSE`](LICENSE).

---

## Acknowledgments

- **RexGlue360 SDK** — the PPC→x86 recompilation framework this tool wraps.
- **Skate3Recomp** — inspired the `[functions]` format with `end`/`parent`
  boundaries and the `includes` mechanism for manifest injection.
- **UnleashedRecomp** — inspired CMakePresets patterns, LTO-for-Release-only,
  and AVX CPU targeting.
- **XenonRecomp** — inspired invalid instruction skip support and switch table
  auto-detection techniques.
