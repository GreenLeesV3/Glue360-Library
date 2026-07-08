// core/logger.h — process-wide logger backed by spdlog (header-only).
//
// Logs to console (color stdout) and a rotating file under <output>/.recomp/logs.
// Initialize once from main() before running the pipeline; shut down at exit.
#pragma once

#include "core/types.h"

#include <filesystem>
#include <string>

namespace recomp {

namespace fs = std::filesystem;

class Logger {
public:
    static void init(const fs::path& log_dir, LogLevel level = LogLevel::Info);
    static void shutdown();
    static void set_level(LogLevel level);

    static void log(LogLevel level, const std::string& msg);
    static void trace(const std::string& msg);
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);
    static void critical(const std::string& msg);
};

} // namespace recomp
