// core/types.h — core enums and structs shared across the pipeline.
// Enums and trivial structs only; factory helpers are inline.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace recomp {

enum class StageStatus {
    Success,
    Failed,
    Skipped
};

struct StageResult {
    StageStatus status = StageStatus::Failed;
    std::string message;
    int exit_code = 0;

    // NOTE: `ok` has NO default arg on purpose. With a default (`= ""`) the
    // call `res.ok()` (zero args) would be ambiguous between this static
    // factory and the `bool ok() const` instance method. Requiring the msg
    // argument makes `res.ok()` resolve to the instance method and
    // `StageResult::ok("msg")` resolve to the factory.
    static StageResult ok(std::string msg) {
        return {StageStatus::Success, std::move(msg), 0};
    }
    static StageResult fail(std::string msg, int code = 1) {
        return {StageStatus::Failed, std::move(msg), code};
    }
    static StageResult skip(std::string msg = "") {
        return {StageStatus::Skipped, std::move(msg), 0};
    }

    bool ok() const { return status == StageStatus::Success; }
    bool failed() const { return status == StageStatus::Failed; }
    bool skipped() const { return status == StageStatus::Skipped; }
};

struct CheckResult {
    bool ok = false;
    std::string message;
    std::vector<std::string> missing;
    std::vector<std::string> warnings;
};

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Critical
};

enum class GraphicsBackend {
    D3D12,
    Vulkan
};

struct ProgressInfo {
    std::string stage_id;
    float fraction = 0.0f;
    std::string line;
};

// Note: ProgressCallback is defined in core/istage.h (the authoritative IStage
// interface). Core does not redefine it here to avoid an ODR violation.

} // namespace recomp
