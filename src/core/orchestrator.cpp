// core/orchestrator.cpp — run stages in order, handle skip/resume/retry.
#include "core/orchestrator.h"
#include "core/logger.h"

namespace recomp {

Orchestrator::Orchestrator(PipelineContext& ctx, StateStore& state)
    : ctx_(ctx), state_(state) {}

void Orchestrator::register_stage(std::shared_ptr<IStage> stage) {
    if (stage) stages_.push_back(std::move(stage));
}

void Orchestrator::log(LogLevel lv, const std::string& msg) const {
    if (log_) log_(lv, msg);
    Logger::log(lv, msg);
}

bool Orchestrator::run_stage_(IStage& stage, float base_fraction, float stage_span) {
    const std::string sid = stage.id();

    // Prereq check. IStage::check_prereqs takes PipelineContext& (stages/
    // istage.h); ctx_ is a non-const lvalue member, so this binds directly.
    CheckResult pre = stage.check_prereqs(ctx_);
    if (!pre.ok) {
        std::string msg = "Stage [" + sid + "] prereqs failed: " + pre.message;
        if (!pre.missing.empty()) {
            msg += " (missing: ";
            for (std::size_t i = 0; i < pre.missing.size(); ++i) {
                if (i) msg += ", ";
                msg += pre.missing[i];
            }
            msg += ")";
        }
        log(LogLevel::Error, msg);
        if (progress_) progress_(sid, base_fraction, msg);
        failed_.push_back(sid);
        return false;
    }
    for (const auto& w : pre.warnings)
        log(LogLevel::Warn, "Stage [" + sid + "] prereq warning: " + w);

    // Skip if already complete and resuming.
    if (ctx_.resume && stage.is_complete(ctx_) && state_.is_complete(sid)) {
        log(LogLevel::Info, "Stage [" + sid + "]: already complete, skipping (resume)");
        if (progress_) progress_(sid, base_fraction + stage_span,
                                 "skipped (already complete)");
        completed_.push_back(sid);
        return true;
    }

    // Run with a progress callback that maps stage-local 0..1 onto the global
    // pipeline fraction.
    float stage_base = base_fraction;
    ProgressCallback cb = [&, this](float frac, const std::string& line) {
        float f = stage_base + frac * stage_span;
        if (progress_) progress_(sid, f, line);
    };

    StageResult res = stage.run(ctx_, cb);
    if (res.ok()) {
        log(LogLevel::Info, "Stage [" + sid + "]: success — " + res.message);
        state_.mark_complete(sid);
        state_.save(ctx_);
        completed_.push_back(sid);
        if (progress_) progress_(sid, base_fraction + stage_span, "done");
        return true;
    }
    if (res.skipped()) {
        log(LogLevel::Info, "Stage [" + sid + "]: skipped — " + res.message);
        state_.mark_complete(sid);
        state_.save(ctx_);
        completed_.push_back(sid);
        if (progress_) progress_(sid, base_fraction + stage_span, "skipped");
        return true;
    }
    log(LogLevel::Error, "Stage [" + sid + "]: FAILED — " + res.message +
                         " (exit " + std::to_string(res.exit_code) + ")");
    failed_.push_back(sid);
    return false;
}

bool Orchestrator::run() {
    completed_.clear();
    failed_.clear();

    if (ctx_.clean) {
        log(LogLevel::Info, "Clean mode: wiping state and stage outputs");
        state_.clear();
    }

    if (ctx_.resume) {
        state_.load(ctx_);
        log(LogLevel::Info, "Resume mode: " +
             std::to_string(state_.completed().size()) +
             " stage(s) already complete");
    }

    const float span = stages_.empty() ? 0.0f
        : 1.0f / static_cast<float>(stages_.size());
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        float base = static_cast<float>(i) * span;
        if (!run_stage_(*stages_[i], base, span)) {
            log(LogLevel::Error, "Pipeline halted at stage: " + stages_[i]->id());
            return false;
        }
    }
    log(LogLevel::Info, "Pipeline complete: " +
         std::to_string(completed_.size()) + " stage(s) succeeded");
    return true;
}

bool Orchestrator::run_one(const std::string& stage_id) {
    for (auto& s : stages_) {
        if (s->id() == stage_id) {
            completed_.clear();
            failed_.clear();
            return run_stage_(*s, 0.0f, 1.0f);
        }
    }
    log(LogLevel::Error, "run_one: unknown stage id: " + stage_id);
    return false;
}

} // namespace recomp
