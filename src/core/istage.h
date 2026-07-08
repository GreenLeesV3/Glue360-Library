// core/istage.h — the common stage interface (core contract).
//
// This is the single, authoritative definition of recomp::IStage. All 7
// pipeline stage modules (src/stages/*) implement it; the orchestrator
// (src/core/orchestrator.*) runs them through this interface.
//
// Signature contract (must match every stage override):
//   check_prereqs(const PipelineContext& ctx) const  — READ-ONLY, const ref.
//   run(PipelineContext& ctx, ProgressCallback)       — mutates ctx.
//   is_complete(const PipelineContext& ctx) const     — READ-ONLY, const ref.
//
// Design grounded in docs/01_architecture.md §2.
#pragma once

#include "core/types.h"

#include <functional>
#include <string>

namespace recomp {

struct PipelineContext; // forward — full def in core/pipeline_context.h

// Progress callback signature: (fraction_in_0_1, human_readable_line).
// Stages invoke this during long-running work (codegen, builds, extraction)
// so the orchestrator can forward progress to the UI / NDJSON headless output.
// fraction is in [0.0, 1.0]; line is a single log line (no trailing newline
// required).
using ProgressCallback = std::function<void(float, const std::string&)>;

class IStage {
public:
    virtual ~IStage() = default;

    // Stable stage identifier, e.g. "iso_extract", "rexglue_init",
    // "rexglue_codegen", "apply_patches", "build_runtime", "build_game",
    // "deploy". Used as the state.json key and in progress messages.
    virtual std::string id() const = 0;

    // Human-readable name for logs/UI, e.g. "ISO Extraction".
    virtual std::string name() const = 0;

    // Verify this stage's prerequisites are present: required tools on PATH /
    // in ctx.toolchain, required input artifacts from prior stages, sufficient
    // disk space, etc. Returns ok=true if the stage can run; otherwise
    // ok=false with a human-readable message and the list of missing
    // prerequisites. Must be side-effect-free (no filesystem mutation, no
    // child processes beyond read-only version probes).
    virtual CheckResult check_prereqs(const PipelineContext& ctx) const = 0;

    // Execute the stage. Mutates ctx with this stage's outputs (e.g. sets
    // ctx.extracted_dir, ctx.project_dir, ctx.built_exe). Invokes progress
    // periodically for long-running stages. Returns StageResult with
    // status=Success on completion, Failed otherwise. On failure, ctx may
    // contain partial outputs; is_complete() will return false and the
    // orchestrator may retry run().
    virtual StageResult run(PipelineContext& ctx, ProgressCallback progress) = 0;

    // Idempotent completion check: return true iff this stage's outputs
    // already exist and are valid, so run() can be skipped on resume.
    // Must be cheap and side-effect-free.
    virtual bool is_complete(const PipelineContext& ctx) const = 0;
};

} // namespace recomp
