// deployer.h — Stage 7: Deploy to a portable standalone folder.
//
// Copies exe + DLLs + rendered TOML, creates the game/ junction to the
// extracted ISO files, and seeds user_data/. The TOML is rendered from the
// profile's deploy template (profiles/spiderman3/spiderman3.toml.template)
// with {{GAME_DATA_ROOT}} filled at deploy.

#pragma once

#include "core/istage.h"

namespace recomp {

class DeployerStage : public IStage {
 public:
  std::string id() const override { return "deploy"; }
  std::string name() const override { return "Deploy Standalone Game"; }

  CheckResult check_prereqs(const PipelineContext& ctx) const override;
  StageResult run(PipelineContext& ctx, ProgressCallback progress) override;
  bool is_complete(const PipelineContext& ctx) const override;
};

}  // namespace recomp
