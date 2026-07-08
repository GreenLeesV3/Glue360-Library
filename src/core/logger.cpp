// core/logger.cpp — spdlog-backed logger (header-only spdlog from the SDK).
#include "core/logger.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <stdexcept>
#include <vector>

namespace recomp {

namespace fs = std::filesystem;

namespace {
bool g_init = false;
LogLevel g_level = LogLevel::Info;

spdlog::level::level_enum to_spdlog(LogLevel l) {
    switch (l) {
        case LogLevel::Trace:    return spdlog::level::trace;
        case LogLevel::Debug:    return spdlog::level::debug;
        case LogLevel::Info:     return spdlog::level::info;
        case LogLevel::Warn:     return spdlog::level::warn;
        case LogLevel::Error:    return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
    }
    return spdlog::level::info;
}
} // namespace

void Logger::init(const fs::path& log_dir, LogLevel level) {
    if (g_init) return;
    g_level = level;
    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        if (!log_dir.empty()) {
            std::error_code ec;
            fs::create_directories(log_dir, ec);
            fs::path logfile = log_dir / "recomp.log";
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                logfile.string(), true /* truncate */));
        }
        auto logger = std::make_shared<spdlog::logger>(
            "recomp", sinks.begin(), sinks.end());
        logger->set_level(to_spdlog(level));
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);
        g_init = true;
    } catch (const std::exception&) {
        // spdlog init failure is non-fatal; Logger::log becomes a no-op.
        g_init = false;
    }
}

void Logger::shutdown() {
    if (!g_init) return;
    spdlog::shutdown();
    g_init = false;
}

void Logger::set_level(LogLevel level) {
    g_level = level;
    if (g_init) spdlog::set_level(to_spdlog(level));
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (!g_init) return;
    spdlog::log(to_spdlog(level), msg);
}

void Logger::trace(const std::string& m)    { log(LogLevel::Trace, m); }
void Logger::debug(const std::string& m)    { log(LogLevel::Debug, m); }
void Logger::info(const std::string& m)     { log(LogLevel::Info, m); }
void Logger::warn(const std::string& m)     { log(LogLevel::Warn, m); }
void Logger::error(const std::string& m)    { log(LogLevel::Error, m); }
void Logger::critical(const std::string& m) { log(LogLevel::Critical, m); }

} // namespace recomp
