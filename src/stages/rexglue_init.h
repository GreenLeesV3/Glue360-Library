// rexglue_init.h — Stage 2: Initialize the rexglue recompilation project.
//
// Runs `rexglue init` to create the CMake project skeleton + manifest TOML
// from the extracted default.xex. Uses the CORRECTED CLI flags from
// docs/03_codegen_automation.md §2.1 (the BUILD_GUIDE's --name/--xex are
// stale; the real CLI verified via `rexglue init --help` is below).

#pragma once

#include "core/istage.h"

namespace recomp {

class RexglueInitStage : public IStage {
 public:
  std::string id() const override { return "rexglue_init"; }
  std::string name() const override { return "RexGlue Project Init"; }

  CheckResult check_prereqs(const PipelineContext& ctx) const override;
  StageResult run(PipelineContext& ctx, ProgressCallback progress) override;
  bool is_complete(const PipelineContext& ctx) const override;
};

}  // namespace recomp
