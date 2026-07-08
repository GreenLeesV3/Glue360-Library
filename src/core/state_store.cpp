// core/state_store.cpp — state.json load/save + per-stage completion tracking.
#include "core/state_store.h"
#include "core/json_mini.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace recomp {

namespace fs = std::filesystem;
using json::Value;
using json::Object;
using json::Array;

StateStore::StateStore(fs::path state_dir)
    : state_dir_(std::move(state_dir)),
      state_file_(state_dir_ / "state.json") {}

bool StateStore::load(PipelineContext& ctx) {
    if (!fs::exists(state_file_)) return false;
    std::ifstream f(state_file_);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    Value root;
    try {
        root = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!root.is_object()) return false;

    // Context fields are stored as a nested object under "context".
    const Value& cobj = root.get("context");
    if (cobj.is_object()) {
        ctx.from_json(json::dump(cobj));
    }

    // Completed stages list.
    completed_.clear();
    const Value& done = root.get("completed");
    if (done.is_array()) {
        for (const auto& e : done.as_array()) {
            if (e.is_string()) completed_.push_back(e.as_string());
        }
    }
    return true;
}

bool StateStore::save(const PipelineContext& ctx) {
    std::error_code ec;
    fs::create_directories(state_dir_, ec);

    Object root;
    // Embed context as a nested object (parse the context's own JSON back so
    // it serializes as an object, not as an escaped string).
    Value ctx_val;
    try {
        ctx_val = json::parse(ctx.to_json());
    } catch (const std::exception&) {
        return false;
    }
    root["context"] = ctx_val;

    Array done;
    for (const auto& s : completed_) done.emplace_back(Value(s));
    root["completed"] = Value(done);

    std::string text = json::dump(Value(root));

    fs::path tmp = state_file_;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
    }
    fs::rename(tmp, state_file_, ec);
    if (ec) {
        // rename can fail in some setups; fall back to direct write.
        std::ofstream f(state_file_, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
    }
    return true;
}

void StateStore::mark_complete(const std::string& stage_id) {
    if (!is_complete(stage_id)) completed_.push_back(stage_id);
}

bool StateStore::is_complete(const std::string& stage_id) const {
    for (const auto& s : completed_) if (s == stage_id) return true;
    return false;
}

void StateStore::clear() {
    completed_.clear();
    std::error_code ec;
    fs::remove(state_file_, ec);
}

} // namespace recomp
