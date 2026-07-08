// rexglue_codegen.h — Stage 3: Run rexglue codegen to emit recompiled C++.
//
// Runs `rexglue codegen <manifest>` to generate the PPC->C++ recompilation
// shards. Verifies the shard count by parsing generated/default/sources.cmake.

#pragma once

#include "core/istage.h"

namespace recomp {

class RexglueCodegenStage : public IStage {
 public:
  std::string id() const override { return "rexglue_codegen"; }
  std::string name() const override { return "RexGlue Codegen"; }

  CheckResult check_prereqs(const PipelineContext& ctx) const override;
  StageResult run(PipelineContext& ctx, ProgressCallback progress) override;
  bool is_complete(const PipelineContext& ctx) const override;
};

}  // namespace recomp
