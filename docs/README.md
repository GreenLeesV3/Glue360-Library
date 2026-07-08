# Design Documents

This directory holds the internal design documents for the xbox360-recompiler
application. They are the authoritative reference for how each part of the
pipeline is built; the code in `../src/` implements them.

Documents are numbered in roughly the order you'd read them: architecture and
module contracts first, then each major subsystem, then UX/distribution, and
finally the user-facing specification.

| # | Document | What it covers |
|---|----------|----------------|
| 1 | [01_architecture.md](01_architecture.md) | **Architecture.** The hybrid GUI/CLI orchestrator, the seven-stage `IStage` module breakdown, the `PipelineContext` data contract, process model, and cross-cutting modules (DependencyChecker, StateStore, ProgressBus, ToolchainEnv). Start here. |
| 2 | [02_patch_system.md](02_patch_system.md) | **Patch system.** The two patch categories that cannot share a format — game-project patches (inja templates applied post-codegen) and SDK source patches (flag-guarded overlay headers on a persistent shared tree). Cvar TOML, save-system hooks, runtime fix overlays. |
| 3 | [03_codegen_automation.md](03_codegen_automation.md) | **Codegen automation.** Automating `rexglue init`/`codegen`: ISO extraction, project scaffolding, funcid rename re-application, hand-authored `src/` file preservation, incremental rebuild detection, and game identification. |
| 4 | [04_build_automation.md](04_build_automation.md) | **Build automation.** How the `build_runtime` and `build_game` stages wrap the toolchain (vcvarsall + clang-cl + cmake + ninja), ThinLTO runtime build, the custom `rexruntime.dll`, and incremental rebuild timing. |
| 5 | [05_ux_design.md](05_ux_design.md) | **UX design.** The end-to-end user experience from download to gameplay, including the two-tier settings model — runtime TOML cvars (instant, no rebuild) vs. the "Forced" `OnPreSetup` set (requires a 3-minute rebuild) — and the multi-step wizard flow. |
| 6 | [06_distribution.md](06_distribution.md) | **Distribution & packaging.** Repository structure, build-vs-download strategy, large-dependency handling (RexGlue SDK, LLVM), legal posture (user-owned ISO only), update mechanism, and cross-game extensibility. |
| 7 | [07_specification.md](07_specification.md) | **Application specification.** The user-facing spec — end-user and developer documentation covering the full ISO → recompiled-exe pipeline, CLI/GUI behavior, profile format, and the supported-games roadmap. |

## Status

All documents are **design** documents — they predate implementation and
describe the intended contract between modules. They were written when only
the Spider-Man 3 profile existed and do not reflect the Jurassic: The Hunted
profile, standalone distribution, build optimizations (LTO, static runtime,
ccache), or the `[functions]`/`includes` manifest injection mechanism. See
`README.md` and `HOW_TO_USE.txt` for current, up-to-date documentation.

Implementation in `../src/` follows these documents; where code and doc
disagree, the code is authoritative.

## How to navigate

- **New contributor?** Read `01` → `07` in order for the full picture.
- **Adding a game profile?** `02` (patch format) + `07` (profile spec).
- **Touching the build stages?** `04` (build automation) + `01` §2 (module list).
- **Working on the UI?** `05` (UX) + `07` (user-facing spec).
- **Packaging a release?** `06`.
