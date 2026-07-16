// gui/gui_pipeline.cpp — pipeline runner for the WebView GUI.
//
// Mirrors main.cpp's run_pipeline stage-for-stage (dep check → profile load
// → MSVC env capture → orchestrator) but reports through a JSON event sink.
#include "gui/gui_pipeline.h"

#include "core/json_mini.h"
#include "core/logger.h"
#include "core/orchestrator.h"
#include "core/pipeline_context.h"
#include "core/process_runner.h"
#include "core/state_store.h"
#include "deps/dependency_checker.h"
#include "profile/game_profile.h"

#include "stages/iso_extractor.h"
#include "stages/rexglue_init.h"
#include "stages/rexglue_codegen.h"
#include "stages/patch_applier.h"
#include "stages/runtime_builder.h"
#include "stages/game_builder.h"
#include "stages/deployer.h"

#ifdef _WIN32
#  include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace recomp::gui {

namespace fs = std::filesystem;
namespace json = recomp::json;

namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string job_event(const std::string& id, json::Object fields) {
    fields["event"] = json::Value("job");
    fields["id"]    = json::Value(id);
    return json::dump(json::Value(std::move(fields)));
}

fs::path resolve_app_dir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    fs::path exe_dir = fs::path(path).parent_path();
    if (fs::exists(exe_dir / "profiles")) return exe_dir;
    if (fs::exists(exe_dir.parent_path() / "profiles")) return exe_dir.parent_path();
    return exe_dir;
}

// Capture the vcvarsall x64 environment into ctx.build_env (same recipe as
// main.cpp: temp batch + `set`, then strip MinGW/WinLibs from PATH so MSVC's
// link.exe is not shadowed).
void capture_build_env(recomp::PipelineContext& ctx) {
    if (!fs::exists(ctx.toolchain.vcvarsall_bat)) return;
    fs::path tmp_bat = ctx.recomp_dir() / "_capture_env.bat";
    {
        std::ofstream bf(tmp_bat);
        bf << "@echo off\n"
           << "call \"" << ctx.toolchain.vcvarsall_bat.string() << "\" x64 >nul\n"
           << "set\n";
    }
    recomp::ProcessOutput pe =
        recomp::ProcessRunner::run("cmd.exe", {"/c", tmp_bat.string()}, {}, {}, nullptr);
    std::string env = pe.stdout_text;
    std::size_t pos = 0;
    while (pos < env.size()) {
        std::size_t nl = env.find('\n', pos);
        std::string line = (nl == std::string::npos) ? env.substr(pos)
                                                     : env.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq != std::string::npos && eq > 0)
            ctx.build_env[line.substr(0, eq)] = line.substr(eq + 1);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    for (auto& [key, val] : ctx.build_env) {
        std::string lkey = key;
        std::transform(lkey.begin(), lkey.end(), lkey.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lkey != "path") continue;
        std::string filtered;
        std::size_t start = 0;
        bool changed = false;
        for (std::size_t i = 0; i <= val.size(); ++i) {
            if (i == val.size() || val[i] == ';') {
                std::string entry = val.substr(start, i - start);
                std::string lower = entry;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (lower.find("mingw") != std::string::npos ||
                    lower.find("winlibs") != std::string::npos) {
                    changed = true;
                } else {
                    if (!filtered.empty()) filtered += ';';
                    filtered += entry;
                }
                start = i + 1;
            }
        }
        if (changed) val = filtered;
    }
}

} // namespace

void RunGuiPipeline(const GuiPipelineParams& params,
                    const std::atomic<bool>& cancel,
                    const GuiEventSink& sink) {
    const std::string& id = params.job_id;
    const long long t0 = now_ms();

    auto push_log = [&](const char* level, const std::string& msg) {
        sink(job_event(id, {
            {"kind",  json::Value("log")},
            {"t",     json::Value(now_ms() - t0)},
            {"level", json::Value(level)},
            {"msg",   json::Value(msg)},
        }));
    };
    auto push_status = [&](const char* status, const std::string& error,
                           const std::string& deploy_dir) {
        json::Object o{
            {"kind",   json::Value("status")},
            {"status", json::Value(status)},
        };
        if (!error.empty())      o["error"]     = json::Value(error);
        if (!deploy_dir.empty()) o["deployDir"] = json::Value(deploy_dir);
        sink(job_event(id, std::move(o)));
    };

    try {
        recomp::PipelineContext ctx;
        ctx.iso_path     = fs::path(params.iso_path);
        ctx.output_dir   = fs::path(params.output_dir);
        ctx.profile_name = params.profile_name;
        ctx.clean        = params.clean;
        ctx.resume       = !params.clean;  // reuse completed stages by default
        if (!params.sdk_path.empty())        ctx.sdk_path        = fs::path(params.sdk_path);
        if (!params.sdk_source_path.empty()) ctx.sdk_source_path = fs::path(params.sdk_source_path);
        ctx.app_dir = resolve_app_dir();

        fs::create_directories(ctx.recomp_dir());
        recomp::Logger::init(ctx.log_dir(), recomp::LogLevel::Info);
        // RAII: exceptions below must not leave the global logger initialized
        // (it would poison the next job's init).
        struct LoggerGuard {
            ~LoggerGuard() { recomp::Logger::shutdown(); }
        } logger_guard;

        push_log("ok", "pipeline session started — profile " + params.profile_name);
        push_log("info", "iso: " + params.iso_path);
        push_log("info", "output: " + params.output_dir);

        // --- Dependency check ---
        recomp::deps::DepCheckOptions opts;
        if (!ctx.sdk_path.empty())        opts.sdk_root       = ctx.sdk_path;
        if (!ctx.sdk_source_path.empty()) opts.sdk_source_dir = ctx.sdk_source_path;
        opts.app_dir = ctx.app_dir;
        recomp::deps::DepCheckReport rep = recomp::deps::check_dependencies(opts);
        ctx.toolchain = rep.toolchain;
        ctx.sdk_path  = rep.toolchain.sdk_root;
        if (!rep.blocking_ok) {
            std::string missing;
            for (const auto& r : rep.results) {
                if (r.severity == recomp::deps::Severity::Blocking && r.version.empty()) {
                    if (!missing.empty()) missing += ", ";
                    missing += r.display_name;
                }
            }
            push_log("err", "Blocking dependency failures: " + missing);
            push_status("failed", "Missing dependencies: " + missing, "");
            return;
        }
        push_log("info", "toolchain ok — SDK " + rep.toolchain.sdk_version +
                         ", clang " + rep.toolchain.clang_cl_version);

        // --- Profile ---
        ctx.profile = recomp::profile::load_profile(ctx.app_dir, ctx.profile_name);
        push_log("info", "profile: " + ctx.profile.name +
                         " (title " + ctx.profile.title_id + ")");

        // --- MSVC env ---
        capture_build_env(ctx);
        if (ctx.build_env.empty())
            push_log("warn", "vcvarsall.bat not found; build stages will fail.");
        else
            push_log("info", "captured MSVC build env (" +
                             std::to_string(ctx.build_env.size()) + " vars)");

        // --- Orchestrator ---
        recomp::StateStore state(ctx.recomp_dir());
        recomp::Orchestrator orch(ctx, state);
        orch.set_cancel_flag(&cancel);
        orch.set_progress([&](const std::string& stage_id, float frac,
                              const std::string& line) {
            sink(job_event(id, {
                {"kind",     json::Value("progress")},
                {"progress", json::Value(static_cast<double>(frac) * 100.0)},
                {"stageId",  json::Value(stage_id)},
            }));
            if (!line.empty()) push_log("info", "[" + stage_id + "] " + line);
        });
        orch.set_logger([&](recomp::LogLevel lv, const std::string& msg) {
            const char* level = lv == recomp::LogLevel::Error ? "err"
                              : lv == recomp::LogLevel::Warn  ? "warn"
                                                              : "info";
            push_log(level, msg);
        });

        orch.register_stage(std::make_shared<recomp::IsoExtractorStage>());
        orch.register_stage(std::make_shared<recomp::RexglueInitStage>());
        orch.register_stage(std::make_shared<recomp::RexglueCodegenStage>());
        orch.register_stage(std::make_shared<recomp::PatchApplierStage>());
        orch.register_stage(std::make_shared<recomp::RuntimeBuilderStage>());
        orch.register_stage(std::make_shared<recomp::GameBuilderStage>());
        orch.register_stage(std::make_shared<recomp::DeployerStage>());

        bool ok = orch.run();
        if (ok) {
            push_log("ok", "Pipeline succeeded. Output: " + ctx.deploy_dir.string());
            push_status("done", "", ctx.deploy_dir.string());
        } else if (cancel.load()) {
            push_status("cancelled", "", "");
        } else {
            std::string log_path = (ctx.log_dir() / "recomp.log").string();
            push_status("failed", "Pipeline halted — see " + log_path, "");
        }
    } catch (const std::exception& e) {
        push_log("err", std::string("fatal: ") + e.what());
        push_status("failed", e.what(), "");
    }
}

} // namespace recomp::gui
