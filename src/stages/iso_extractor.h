// iso_extractor.h — Stage 1: Extract the XGD2 Xbox 360 ISO.
//
// Wraps extract-xiso (bundled or on PATH) to extract the XDVDFS game partition
// into <workspace>/extracted/. Verifies default.xex exists post-extraction.
//
// Design grounded in docs/03_codegen_automation.md §1:
//   - Xbox 360 ISOs use XDVDFS, not ISO9660; XGD2 has a zero video partition
//     then the game partition at 0x20800.
//   - extract-xiso -x <iso> -d <dir> auto-detects the game partition.
//   - Bundled in app tools/ (~200KB, BSD-3-Clause, redistributable).
//   - Failure modes: not an Xbox ISO, corrupted, XGD3 (wrong game), disk space.

#pragma once

#include "core/istage.h"

namespace recomp {

class IsoExtractorStage : public IStage {
 public:
  std::string id() const override { return "iso_extract"; }
  std::string name() const override { return "ISO Extraction"; }

  CheckResult check_prereqs(const PipelineContext& ctx) const override;
  StageResult run(PipelineContext& ctx, ProgressCallback progress) override;
  bool is_complete(const PipelineContext& ctx) const override;
};

}  // namespace recomp
