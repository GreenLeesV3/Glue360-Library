// core/process_runner.h — cross-platform child process execution with
// stdout/stderr capture and line streaming.
//
// On Windows this uses CreateProcessW with anonymous pipes. When `env` is
// empty the child inherits the parent's environment; when non-empty it
// becomes the child's full environment block.
//
// ProgressCallback is defined in core/istage.h (the authoritative IStage
// interface); include it so core does not redefine the type (ODR).
#pragma once

#include "core/istage.h"   // recomp::ProgressCallback

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace recomp {

namespace fs = std::filesystem;

struct ProcessOutput {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

class ProcessRunner {
public:
    static ProcessOutput run(const fs::path& exe,
                             const std::vector<std::string>& args,
                             const fs::path& cwd = {},
                             const std::map<std::string, std::string>& env = {},
                             const ProgressCallback& on_line = {});

    static int run_status(const fs::path& exe,
                          const std::vector<std::string>& args,
                          const fs::path& cwd = {},
                          const std::map<std::string, std::string>& env = {});
};

} // namespace recomp
