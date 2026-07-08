// runtime_builder.h — Stage 5: Build custom rexruntime.dll from SDK source.
//
// Applies runtime patches (xam_enum.cpp, xenumerator.cpp, xam_ui.cpp,
// render_target_cache.cpp, command_processor.h) then runs cmake configure +
// build. SKIPPABLE if the profile declares no runtime patches (use prebuilt
// DLL from the SDK bin/).

#pragma once

#include "core/istage.h"

namespace recomp {

class RuntimeBuilderStage : public IStage {
 public:
  std::string id() const override { return "build_runtime"; }
  std::string name() const override { return "Build Custom Runtime"; }

  CheckResult check_prereqs(const PipelineContext& ctx) const override;
  StageResult run(PipelineContext& ctx, ProgressCallback progress) override;
  bool is_complete(const PipelineContext& ctx) const override;
};

}  // namespace recomp
