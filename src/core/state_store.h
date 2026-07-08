// core/state_store.h — checkpoint pipeline state to state.json for resume/retry.
//
// The orchestrator checkpoints after each successful stage; stage modules do
// not touch state.json directly.
#pragma once

#include "core/pipeline_context.h"

#include <filesystem>
#include <string>
#include <vector>

namespace recomp {

namespace fs = std::filesystem;

class StateStore {
public:
    explicit StateStore(fs::path state_dir);

    bool load(PipelineContext& ctx);
    bool save(const PipelineContext& ctx);

    void mark_complete(const std::string& stage_id);
    bool is_complete(const std::string& stage_id) const;
    const std::vector<std::string>& completed() const { return completed_; }
    void clear();

    const fs::path& state_file() const { return state_file_; }

private:
    fs::path state_dir_;
    fs::path state_file_;
    std::vector<std::string> completed_;
};

} // namespace recomp
