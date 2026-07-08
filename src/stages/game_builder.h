// game_builder.h — Stage 6: Build spiderman3.exe.
//
// Runs vcvarsall + cmake configure (Ninja + clang-cl) + ninja build against
// the prebuilt SDK (CMAKE_PREFIX_PATH = sdk_path). The game links the prebuilt
// rexruntime.lib; the custom DLL is swapped in at deploy.

#pragma once

#include "core/istage.h"

namespace recomp {

class GameBuilderStage : public IStage {
 public:
  std::string id() const override { return "build_game"; }
  std::string name() const override { return "Build Game Executable"; }

  CheckResult check_prereqs(const PipelineContext& ctx) const override;
  StageResult run(PipelineContext& ctx, ProgressCallback progress) override;
  bool is_complete(const PipelineContext& ctx) const override;
};

}  // namespace recomp
