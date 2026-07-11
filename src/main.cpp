// main.cpp — xbox360-recompiler entry point.
//
// CLI-first (headless) driver for the Xbox 360 recompilation pipeline.
// Parses arguments, sets up the PipelineContext, initializes logging
// and the state store, registers the 7 pipeline stages, and runs the
// Orchestrator. With no arguments, prints help and exits.
//
// Usage:
//   xbox360-recompiler --iso <path> --output <dir> [--sdk <path>]
//                      [--sdk-source <path>] [--profile <name>]
//                      [--clean] [--resume] [--check-deps]
#include "core/logger.h"
#include "core/orchestrator.h"
#include "core/pipeline_context.h"
#include "core/state_store.h"
#include "core/process_runner.h"
#include "core/types.h"
#include "deps/dependency_checker.h"
#include "profile/game_profile.h"
#include "gui/gui_launcher.h"

#include <spdlog/spdlog.h>

#ifdef _WIN32
#  include <windows.h>   // GetModuleFileNameA, MAX_PATH (app_dir resolution)
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Stage headers (StageModules owns src/stages/*).
#include "stages/iso_extractor.h"
#include "stages/rexglue_init.h"
#include "stages/rexglue_codegen.h"
#include "stages/patch_applier.h"
#include "stages/runtime_builder.h"
#include "stages/game_builder.h"
#include "stages/deployer.h"

namespace {

namespace fs = std::filesystem;

void print_help() {
    std::cout <<
        "xbox360-recompiler — Xbox 360 game recompilation pipeline\n"
        "\n"
        "USAGE:\n"
        "  xbox360-recompiler --iso <path> --output <dir> [OPTIONS]\n"
        "  xbox360-recompiler --check-deps [--sdk <path>] [--sdk-source <path>]\n"
        "  xbox360-recompiler --help\n"
        "  xbox360-recompiler (no args)   Launch GUI wizard on Windows.\n"
        "\n"
        "REQUIRED (for a full run):\n"
        "  --iso <path>          Path to the Xbox 360 ISO (.iso).\n"
        "  --output <dir>        Output/workspace directory. All artifacts are\n"
        "                        written under here (extracted/, project/, builds,\n"
        "                        deploy/) plus a .recomp/ state+logs subfolder.\n"
        "\n"
        "OPTIONS:\n"
        "  --sdk <path>          Path to the prebuilt RexGlue360 SDK root\n"
        "                        (the 'RexGlue360Recomp' dir with bin/, include/,\n"
        "                        lib/). If omitted, the dep checker probes for it.\n"
        "  --sdk-source <path>   Path to the RexGlue SDK *source* tree (for the\n"
        "                        custom runtime build). Optional; if omitted the\n"
        "                        runtime build stage is skipped (prebuilt DLL).\n"
        "  --profile <name>      Game profile name (default: spiderman3).\n"
        "                        Available profiles: spiderman3, jurassic_hunted, spiderman_wos.\n"
        "  --backend <d3d12|vulkan>  Graphics backend (default: d3d12). If omitted,\n"
        "                           you will be prompted to choose.\n"
        "  --clean               Wipe state and stage outputs before running.\n"
        "  --resume              Resume from the last completed stage (skip\n"
        "                        stages already marked complete in state.json).\n"
        "  --check-deps          Run only the dependency checker and print a\n"
        "                        report; do not run the pipeline.\n"
        "  --help, -h            Show this help text and exit.\n"
        "\n"
        "PIPELINE STAGES (run in order):\n"
        "  1. iso_extract        Extract Xbox 360 ISO (built-in reader + extract-xiso fallback).\n"
        "  2. rexglue_init       rexglue init --name <profile> --xex default.xex.\n"
        "  3. rexglue_codegen    rexglue codegen <manifest>.toml (PPC to C++ codegen).\n"
        "  4. apply_patches      Copy profile source files + apply runtime patches.\n"
        "  5. build_runtime      Build custom rexruntime.dll (skipped if no SDK src).\n"
        "  6. build_game         CMake + Ninja + clang-cl game build -> <name>.exe.\n"
        "  7. deploy             Assemble portable standalone folder.\n"
        "\n"
        "EXAMPLES:\n"
        "  xbox360-recompiler --iso \"Spider-Man 3 (USA, Europe).iso\" \\\n"
        "      --output games\\spiderman3 --sdk C:\\tmp\\Workspace 1\\RexGlue360Recomp\n"
        "\n"
        "  xbox360-recompiler --check-deps --sdk C:\\tmp\\Workspace 1\\RexGlue360Recomp\n"
        "  xbox360-recompiler --resume --output games\\spiderman3\n";
}

struct Args {
    fs::path iso;
    fs::path output;
    fs::path sdk;
    fs::path sdk_source;
    std::string profile = "spiderman3";
    std::string backend;  // "d3d12" or "vulkan", empty = prompt
    bool clean = false;
    bool resume = false;
    bool check_deps = false;
    bool help = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << what << " requires a value\n\n";
                print_help();
                std::exit(2);
            }
            return std::string(argv[++i]);
        };
        if (arg == "--help" || arg == "-h")          a.help = true;
        else if (arg == "--iso")                     a.iso = next("--iso");
        else if (arg == "--output")                  a.output = next("--output");
        else if (arg == "--sdk")                     a.sdk = next("--sdk");
        else if (arg == "--sdk-source")              a.sdk_source = next("--sdk-source");
        else if (arg == "--profile")                 a.profile = next("--profile");
        else if (arg == "--backend")              a.backend = next("--backend");
        else if (arg == "--clean")                   a.clean = true;
        else if (arg == "--resume")                  a.resume = true;
        else if (arg == "--check-deps")              a.check_deps = true;
        else {
            std::cerr << "error: unknown argument '" << arg << "'\n\n";
            print_help();
            std::exit(2);
        }
    }
    return a;
}

int do_check_deps(const Args& a) {
    // Resolve app_dir (same logic as run_pipeline) so bundled tools/SDK are
    // discovered even for --check-deps.
    fs::path app_dir;
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        fs::path exe_dir = fs::path(path).parent_path();
        if (fs::exists(exe_dir / "profiles")) {
            app_dir = exe_dir;
        } else if (fs::exists(exe_dir.parent_path() / "profiles")) {
            app_dir = exe_dir.parent_path();
        } else {
            app_dir = exe_dir;
        }
    }
    recomp::deps::DepCheckOptions opts;
    if (!a.sdk.empty())              opts.sdk_root = a.sdk;
    if (!a.sdk_source.empty())       opts.sdk_source_dir = a.sdk_source;
    opts.app_dir = app_dir;
    recomp::deps::DepCheckReport rep = recomp::deps::check_dependencies(opts);
    std::cout << recomp::deps::format_report_text(rep);
    std::cout << "\n";
    if (!rep.blocking_ok) {
        std::cout << "RESULT: BLOCKING dependency failures — fix the above before "
                     "running the pipeline.\n";
        return 1;
    }
    std::cout << "RESULT: All BLOCKING dependencies present. "
                 << (rep.soft_ok ? "All SOFT deps present too."
                                 : "Some SOFT deps missing (degraded path).")
              << "\n";
    return 0;
}

void register_stages(recomp::Orchestrator& orch) {
    orch.register_stage(std::make_shared<recomp::IsoExtractorStage>());
    orch.register_stage(std::make_shared<recomp::RexglueInitStage>());
    orch.register_stage(std::make_shared<recomp::RexglueCodegenStage>());
    orch.register_stage(std::make_shared<recomp::PatchApplierStage>());
    orch.register_stage(std::make_shared<recomp::RuntimeBuilderStage>());
    orch.register_stage(std::make_shared<recomp::GameBuilderStage>());
    orch.register_stage(std::make_shared<recomp::DeployerStage>());
}

int run_pipeline(const Args& a, bool gui_mode = false) {
    using recomp::PipelineContext;
    using recomp::StateStore;
    using recomp::Orchestrator;
    using recomp::LogLevel;
    using recomp::Logger;

    if (a.iso.empty()) {
        std::cerr << "error: --iso is required for a full run\n\n";
        print_help();
        return 2;
    }
    if (a.output.empty()) {
        std::cerr << "error: --output is required for a full run\n\n";
        print_help();
        return 2;
    }

    // Resolve graphics backend
    recomp::GraphicsBackend gfx_backend = recomp::GraphicsBackend::D3D12;
    if (!a.backend.empty()) {
        if (a.backend == "vulkan" || a.backend == "vk") {
            gfx_backend = recomp::GraphicsBackend::Vulkan;
        } else if (a.backend == "d3d12" || a.backend == "dx12") {
            gfx_backend = recomp::GraphicsBackend::D3D12;
        } else {
            std::cerr << "error: --backend must be 'd3d12' or 'vulkan'\n";
            return 2;
        }
    } else if (!a.check_deps && !gui_mode) {
        // Interactive prompt
        std::cout << "\nSelect graphics backend:\n"
                  << "  [1] D3D12  (recommended, best performance on AMD/NVIDIA Windows)\n"
                  << "  [2] Vulkan (cross-platform, experimental)\n"
                  << "Enter choice [1]: ";
        std::string choice;
        std::getline(std::cin, choice);
        if (choice == "2" || choice == "vulkan" || choice == "vk") {
            gfx_backend = recomp::GraphicsBackend::Vulkan;
            std::cout << "Selected: Vulkan\n\n";
        } else {
            std::cout << "Selected: D3D12\n\n";
        }
    }

    PipelineContext ctx;
    ctx.iso_path        = a.iso;
    ctx.output_dir      = a.output;
    ctx.sdk_path        = a.sdk;
    ctx.sdk_source_path = a.sdk_source;
    ctx.profile_name    = a.profile;
    ctx.graphics_backend = gfx_backend;
    ctx.clean           = a.clean;
    ctx.resume          = a.resume;

    // Resolve app_dir = directory of the executable (profiles/ live here).
    // If running from build/, check parent dir for profiles/ too.
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        fs::path exe_dir = fs::path(path).parent_path();
        // Check exe_dir first, then parent (handles build/ subdir layout)
        if (fs::exists(exe_dir / "profiles")) {
            ctx.app_dir = exe_dir;
        } else if (fs::exists(exe_dir.parent_path() / "profiles")) {
            ctx.app_dir = exe_dir.parent_path();
        } else {
            ctx.app_dir = exe_dir;  // fallback
        }
    }

    // Initialize logging under <output>/.recomp/logs.
    fs::create_directories(ctx.recomp_dir());
    Logger::init(ctx.log_dir(), LogLevel::Info);

    Logger::info("xbox360-recompiler starting");
    Logger::info("ISO:      " + ctx.iso_path.string());
    Logger::info("Output:   " + ctx.output_dir.string());
    Logger::info("Profile:  " + ctx.profile_name);

    // --- Dependency check (mandatory before any stage runs) ---
    recomp::deps::DepCheckOptions opts;
    if (!ctx.sdk_path.empty())        opts.sdk_root = ctx.sdk_path;
    if (!ctx.sdk_source_path.empty()) opts.sdk_source_dir = ctx.sdk_source_path;
    opts.app_dir = ctx.app_dir;
    recomp::deps::DepCheckReport dep_rep = recomp::deps::check_dependencies(opts);
    std::cout << recomp::deps::format_report_text(dep_rep) << "\n";
    ctx.toolchain = dep_rep.toolchain;
    ctx.sdk_path  = dep_rep.toolchain.sdk_root;
    if (!dep_rep.blocking_ok) {
        Logger::error("Blocking dependency failures — cannot proceed.");
        return 1;
    }

    // --- Load the game profile ---
    try {
        ctx.profile = recomp::profile::load_profile(ctx.app_dir, ctx.profile_name);
        Logger::info("Loaded profile: " + ctx.profile.name +
                     " (title " + ctx.profile.title_id + ")");
    } catch (const std::exception& e) {
        Logger::error(std::string("Failed to load profile '") + ctx.profile_name +
                      "': " + e.what());
        return 1;
    }

    // --- Capture MSVC build env (vcvarsall x64) into ctx.build_env ---
    // Done lazily here so stages can pass ctx.build_env to ProcessRunner.
    // (If vcvarsall is unavailable, build_env stays empty and MSVC-dependent
    // stages will fail their prereq checks with a clear message.)
    if (fs::exists(ctx.toolchain.vcvarsall_bat)) {
        // ProcessRunner's quote_if_needed C-style-escapes inner quotes (\"),
        // which cmd.exe doesn't understand. Running vcvarsall + set through a
        // temp batch file sidesteps the quoting problem entirely.
        fs::path tmp_bat = ctx.recomp_dir() / "_capture_env.bat";
        std::string vcvars = ctx.toolchain.vcvarsall_bat.string();
        std::ofstream bf(tmp_bat);
        bf << "@echo off\n"
           << "call \"" << vcvars << "\" x64 >nul\n"
           << "set\n";
        bf.close();
        recomp::ProcessOutput pe = recomp::ProcessRunner::run(
            "cmd.exe",
            {"/c", tmp_bat.string()},
            {},
            {},
            nullptr);
        std::string env = pe.stdout_text;
        std::size_t pos = 0;
        while (pos < env.size()) {
            std::size_t nl = env.find('\n', pos);
            std::string line = (nl == std::string::npos)
                ? env.substr(pos) : env.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto eq = line.find('=');
            if (eq != std::string::npos && eq > 0)
                ctx.build_env[line.substr(0, eq)] = line.substr(eq + 1);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }

        // Strip MinGW/WinLibs paths from PATH — their ld.exe shadows MSVC's
        // link.exe when CMake's vs_link_exe wrapper searches PATH, breaking
        // the try-compile. Ninja itself is unaffected (invoked by full path).
        // Windows env vars are case-insensitive but std::map is not, so check
        // all case variants of PATH.
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
        Logger::info("Captured MSVC build env (" +
                     std::to_string(ctx.build_env.size()) + " vars)");
    } else {
        Logger::warn("vcvarsall.bat not found; MSVC-dependent stages will fail.");
    }

    // --- Set up state store + orchestrator ---
    StateStore state(ctx.recomp_dir());
    Orchestrator orch(ctx, state);

    // Wire progress to stdout + logger.
    orch.set_progress([](const std::string& stage_id, float frac,
                         const std::string& line) {
        int pct = static_cast<int>(frac * 100.0f);
        std::cout << "[" << stage_id << " " << pct << "%] " << line << "\n";
    });
    orch.set_logger([](LogLevel lv, const std::string& msg) {
        Logger::log(lv, msg);
    });

    register_stages(orch);

    bool ok = orch.run();
    std::string deploy_path = ctx.deploy_dir.string();
    std::string log_path = (ctx.log_dir() / "recomp.log").string();

    if (ok) {
        Logger::info("Pipeline succeeded.");
        std::cout << "\nPipeline succeeded. Output: " << deploy_path << "\n";
    } else {
        Logger::error("Pipeline failed.");
        std::cout << "\nPipeline failed. See " << log_path << " for details.\n";
    }

    if (gui_mode) {
        recomp::gui::ShowResult(ok, deploy_path,
                                ok ? "" : "See " + log_path + " for details.");
    }

    Logger::shutdown();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);
    if (a.help) {
        print_help();
        return 0;
    }

    // No args — try the GUI launcher, fall back to help
    if (argc <= 1) {
#ifdef _WIN32
        auto gui_result = recomp::gui::ShowLauncherWizard();
        if (gui_result.ok) {
            a.iso = gui_result.iso_path;
            a.output = gui_result.output_dir;
            a.profile = gui_result.profile_name;
            if (!gui_result.sdk_path.empty()) {
                a.sdk = gui_result.sdk_path;
            }
            return run_pipeline(a, true);
        }
#endif
        print_help();
        return 0;
    }

    if (a.check_deps) {
        return do_check_deps(a);
    }

    return run_pipeline(a);
}
