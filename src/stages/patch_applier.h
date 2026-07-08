// patch_applier.h — Stage 4: Apply game patches to the generated project.
//
// Three patch strata (docs/01_architecture.md §6.3):
//   Stratum 1 (cvars)   — rendered into spiderman3_app.h::OnPreSetup
//   Stratum 2 (sources) — profile src/*.inja templates copied/rendered into
//                         <project>/src/ (xmp_bypass, spiderman3_app.h,
//                         roundevenf, particle_perf, main.cpp)
//   Stratum 3 (runtime) — flag-guarded overlay headers + inline #ifdef guards
//                         applied to the SDK source tree (for build_runtime)
//
// Bundled patch files live in the app's profiles/<id>/ directory and are
// copied into the generated project's src/ directory. Runtime overlay headers
// live in profiles/<id>/patches/<category>/<id>/overlay/.

#pragma once

#include "core/istage.h"

namespace recomp {

class PatchApplierStage : public IStage {
 public:
  std::string id() const override { return "apply_patches"; }
  std::string name() const override { return "Apply Game Patches"; }

  CheckResult check_prereqs(const PipelineContext& ctx) const override;
  StageResult run(PipelineContext& ctx, ProgressCallback progress) override;
  bool is_complete(const PipelineContext& ctx) const override;
};

}  // namespace recomp
