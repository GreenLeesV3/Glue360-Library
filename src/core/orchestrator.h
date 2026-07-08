// core/orchestrator.h — runs registered stages in order, handling
// skip/resume/retry and reporting progress via callbacks.
//
// IStage is the core contract, defined in core/istage.h (the authoritative
// single definition). The orchestrator holds shared_ptrs to IStage and runs
// them through this interface.
#pragma once

#include "core/istage.h"        // recomp::IStage, recomp::ProgressCallback
#include "core/pipeline_context.h"
#include "core/state_store.h"
#include "core/types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace recomp {

class Orchestrator {
public:
    using ProgressFn = std::function<void(const std::string& stage_id,
                                          float fraction,
                                          const std::string& line)>;
    using LogFn = std::function<void(LogLevel, const std::string&)>;

    Orchestrator(PipelineContext& ctx, StateStore& state);

    void register_stage(std::shared_ptr<IStage> stage);

    void set_progress(ProgressFn fn) { progress_ = std::move(fn); }
    void set_logger(LogFn fn) { log_ = std::move(fn); }

    bool run();
    bool run_one(const std::string& stage_id);

    const std::vector<std::string>& completed() const { return completed_; }
    const std::vector<std::string>& failed() const { return failed_; }

private:
    PipelineContext& ctx_;
    StateStore& state_;
    std::vector<std::shared_ptr<IStage>> stages_;
    ProgressFn progress_;
    LogFn log_;
    std::vector<std::string> completed_;
    std::vector<std::string> failed_;

    void log(LogLevel lv, const std::string& msg) const;
    bool run_stage_(IStage& stage, float base_fraction, float stage_span);
};

} // namespace recomp
