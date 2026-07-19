// gui/webview_host.cpp — WebView2 GUI shell (Glue360 Deck).
//
// Architecture:
//   - Win32 window hosting a WebView2 controller (dark titlebar, 1280x800).
//   - The React UI ships as an RCDATA resource (IDR_GUI_HTML), extracted to
//     %LOCALAPPDATA%\Glue360\ui\index.html and navigated via file:///.
//   - UI ↔ host bridge: window.chrome.webview.postMessage JSON-RPC.
//       UI → host:  { rpc, cmd, args }
//       host → UI:  { rpc, ok, data?, error? }              (responses)
//                   { event: "job"|"game", ... }            (pushed events)
//   - The pipeline runs on a worker thread; events are marshalled to the UI
//     thread via WM_APP_POST_JSON (heap std::string ownership transfer).
//   - Settings are host-authoritative: %LOCALAPPDATA%\Glue360\settings.json.
#include "gui/webview_host.h"

#ifdef _WIN32

#include "gui/gui_pipeline.h"
#include "core/json_mini.h"
#include "deps/dependency_checker.h"
#include "profile/game_profile.h"

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>
#include <WebView2.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace recomp::gui {

namespace fs = std::filesystem;
namespace json = recomp::json;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT WM_APP_POST_JSON = WM_APP + 1;
constexpr int  IDR_GUI_HTML     = 1001;

#ifndef RECOMP_APP_VERSION
#define RECOMP_APP_VERSION "0.0.0"
#endif

// ---------------------------------------------------------------- utf helpers

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------- app paths

fs::path AppDataDir() {
    PWSTR raw = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        base = raw;
        CoTaskMemFree(raw);
    } else {
        base = fs::temp_directory_path();
    }
    return base / "Glue360";
}

fs::path ResolveAppDir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    fs::path exe_dir = fs::path(path).parent_path();
    if (fs::exists(exe_dir / "profiles")) return exe_dir;
    if (fs::exists(exe_dir.parent_path() / "profiles")) return exe_dir.parent_path();
    return exe_dir;
}

// Extract the embedded UI to app-data (NavigateToString caps at ~2MB and
// file:/// keeps DevTools/source URLs sane).
fs::path ExtractUiHtml() {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(IDR_GUI_HTML), RT_RCDATA);
    if (!res) return {};
    HGLOBAL blob = LoadResource(mod, res);
    if (!blob) return {};
    const char* data = static_cast<const char*>(LockResource(blob));
    DWORD size = SizeofResource(mod, res);
    if (!data || size == 0) return {};

    fs::path ui_dir = AppDataDir() / "ui";
    std::error_code ec;
    fs::create_directories(ui_dir, ec);
    fs::path html = ui_dir / "index.html";
    std::ofstream out(html, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    out.write(data, size);
    return html;
}

// ---------------------------------------------------------------- settings

fs::path SettingsPath() { return AppDataDir() / "settings.json"; }

json::Value LoadSettings() {
    std::ifstream in(SettingsPath());
    if (!in) return json::Value(json::Object{});
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    try {
        json::Value v = json::parse(text);
        return v.is_object() ? v : json::Value(json::Object{});
    } catch (...) {
        return json::Value(json::Object{});
    }
}

void SaveSettings(const json::Object& obj) {
    std::error_code ec;
    fs::create_directories(AppDataDir(), ec);
    std::ofstream out(SettingsPath(), std::ios::trunc);
    out << json::dump(json::Value(obj));
}

// ---------------------------------------------------------------- dialogs

std::string PickFile(HWND owner, const wchar_t* title,
                     const COMDLG_FILTERSPEC* filters, UINT filter_count,
                     bool folders) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dlg))))
        return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM;
    if (folders) opts |= FOS_PICKFOLDERS;
    dlg->SetOptions(opts);
    dlg->SetTitle(title);
    if (filters && filter_count) dlg->SetFileTypes(filter_count, filters);
    if (FAILED(dlg->Show(owner))) return {};  // cancelled
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return {};
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return {};
    std::wstring w(raw);
    CoTaskMemFree(raw);
    return WideToUtf8(w);
}

// ---------------------------------------------------------------- host state

struct RunningJob {
    std::string id;
    std::thread worker;
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
};

struct RunningGame {
    std::string library_id;
    HANDLE process = nullptr;
    DWORD process_id = 0;
    HANDLE watcher_stop = nullptr;
    std::thread watcher;
};

struct HostState {
    HWND hwnd = nullptr;
    EventRegistrationToken msg_token{};
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::unique_ptr<RunningJob> job;
    std::unique_ptr<RunningGame> game;
};

HostState* g_host = nullptr;

// Marshal a JSON string to the UI thread and into the webview.
void PostJsonToUi(HWND hwnd, const std::string& payload) {
    auto* heap = new std::string(payload);
    if (!PostMessageW(hwnd, WM_APP_POST_JSON, 0,
                      reinterpret_cast<LPARAM>(heap))) {
        delete heap;  // window gone — drop the event
    }
}

struct CloseWindowRequest {
    DWORD process_id;
    bool posted = false;
};

BOOL CALLBACK PostCloseToProcessWindow(HWND window, LPARAM param) {
    auto* request = reinterpret_cast<CloseWindowRequest*>(param);
    DWORD owner_process_id = 0;
    GetWindowThreadProcessId(window, &owner_process_id);
    if (owner_process_id == request->process_id) {
        request->posted = PostMessageW(window, WM_CLOSE, 0, 0) || request->posted;
    }
    return TRUE;
}


void StopGameWatcher(RunningGame& game) {
    if (game.watcher_stop) SetEvent(game.watcher_stop);
    if (game.watcher.joinable()) game.watcher.join();
    if (game.watcher_stop) {
        CloseHandle(game.watcher_stop);
        game.watcher_stop = nullptr;
    }
}
bool RequestGracefulGameClose(const RunningGame& game) {
    if (!game.process || game.process_id == 0) return false;
    CloseWindowRequest request{game.process_id};
    EnumWindows(PostCloseToProcessWindow, reinterpret_cast<LPARAM>(&request));
    return request.posted;
}

// ---------------------------------------------------------------- handlers

json::Value HandleListProfiles() {
    json::Array out;
    fs::path profiles_dir = ResolveAppDir() / "profiles";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(profiles_dir, ec)) {
        if (!entry.is_directory()) continue;
        if (!fs::exists(entry.path() / "profile.toml")) continue;
        try {
            recomp::profile::GameProfile p =
                recomp::profile::load_profile_from_dir(entry.path());
            json::Array flags;
            for (const auto& f : p.runtime_flags) flags.push_back(json::Value(f));
            out.push_back(json::Value(json::Object{
                {"id",            json::Value(p.id)},
                {"name",          json::Value(p.name)},
                {"titleId",       json::Value(p.title_id)},
                {"sdkVersion",    json::Value(p.sdk_version)},
                {"runtimeFlags",  json::Value(std::move(flags))},
                {"customRuntime", json::Value(!p.custom_runtime_dll.empty())},
                {"cvarCount",     json::Value((long long)p.cvars.size())},
            }));
        } catch (...) {
            // unparseable profile dir — skip, don't kill the listing
        }
    }
    return json::Value(std::move(out));
}

json::Value HandleCheckDeps(const json::Value& args) {
    recomp::deps::DepCheckOptions opts;
    std::string sdk = args.get_string("sdkPath");
    std::string sdk_src = args.get_string("sdkSourcePath");
    if (!sdk.empty())     opts.sdk_root = fs::path(sdk);
    if (!sdk_src.empty()) opts.sdk_source_dir = fs::path(sdk_src);
    opts.app_dir = ResolveAppDir();
    recomp::deps::DepCheckReport rep = recomp::deps::check_dependencies(opts);

    json::Array issues;
    for (const auto& r : rep.results) {
        if (r.severity == recomp::deps::Severity::Blocking && r.version.empty())
            issues.push_back(json::Value(r.display_name + ": " + r.remediation));
        else if (r.warning.present)
            issues.push_back(json::Value(r.display_name + ": " + r.warning.message));
    }
    return json::Value(json::Object{
        {"ok",           json::Value(rep.blocking_ok)},
        {"appVersion",   json::Value(RECOMP_APP_VERSION)},
        {"sdkRoot",      json::Value(rep.toolchain.sdk_root.string())},
        {"sdkVersion",   json::Value(rep.toolchain.sdk_version)},
        {"clangVersion", json::Value(rep.toolchain.clang_cl_version)},
        {"cmakeVersion", json::Value(rep.toolchain.cmake_version)},
        {"ninjaVersion", json::Value(rep.toolchain.ninja_version)},
        {"msvcVersion",  json::Value(rep.toolchain.msvc_toolset_version)},
        {"issues",       json::Value(std::move(issues))},
    });
}

json::Value HandleGetSettings() {
    json::Value s = LoadSettings();
    return json::Value(json::Object{
        {"sdkPath",          json::Value(s.get_string("sdkPath"))},
        {"sdkSourcePath",    json::Value(s.get_string("sdkSourcePath"))},
        {"outputRoot",       json::Value(s.get_string("outputRoot"))},
        {"defaultProfileId", json::Value(s.get_string("defaultProfileId"))},
        {"cleanBuild",       json::Value(s.get_bool("cleanBuild"))},
    });
}

json::Value HandleSaveSettings(const json::Value& args) {
    SaveSettings(json::Object{
        {"sdkPath",          json::Value(args.get_string("sdkPath"))},
        {"sdkSourcePath",    json::Value(args.get_string("sdkSourcePath"))},
        {"outputRoot",       json::Value(args.get_string("outputRoot"))},
        {"defaultProfileId", json::Value(args.get_string("defaultProfileId"))},
        {"cleanBuild",       json::Value(args.get_bool("cleanBuild"))},
    });
    return json::Value(true);
}

json::Value HandleStartRecompile(HostState& host, const json::Value& args,
                                 std::string& error) {
    if (host.job && !host.job->finished.load()) {
        error = "A build is already running — one job at a time.";
        return json::Value();
    }
    // Join a finished previous worker before replacing it.
    if (host.job && host.job->worker.joinable()) host.job->worker.join();

    GuiPipelineParams params;
    params.iso_path        = args.get_string("iso");
    params.output_dir      = args.get_string("output");
    params.profile_name    = args.get_string("profile");
    params.sdk_path        = args.get_string("sdk");
    params.sdk_source_path = args.get_string("sdkSource");
    params.clean           = args.get_bool("clean");
    if (params.iso_path.empty() || params.output_dir.empty() ||
        params.profile_name.empty()) {
        error = "start_recompile requires iso, output and profile.";
        return json::Value();
    }

    static std::atomic<unsigned> job_seq{0};
    params.job_id = "job-" + std::to_string(GetTickCount64()) + "-" +
                    std::to_string(job_seq.fetch_add(1));

    auto job = std::make_unique<RunningJob>();
    job->id = params.job_id;
    RunningJob* raw = job.get();
    HWND hwnd = host.hwnd;
    job->worker = std::thread([params, raw, hwnd]() {
        RunGuiPipeline(params, raw->cancel, [hwnd](const std::string& j) {
            PostJsonToUi(hwnd, j);
        });
        raw->finished.store(true);
    });
    host.job = std::move(job);

    return json::Value(json::Object{{"jobId", json::Value(params.job_id)}});
}

json::Value HandleLaunchGame(HostState& host, const json::Value& args,
                             std::string& error) {
    std::string library_id = args.get_string("libraryId");
    std::string exe_utf8 = args.get_string("exePath");
    std::string working_dir_utf8 = args.get_string("workingDir");
    fs::path exe = fs::path(Utf8ToWide(exe_utf8));
    fs::path working_dir = fs::path(Utf8ToWide(working_dir_utf8));
    if (library_id.empty() || exe.empty() || working_dir.empty()) {
        error = "launch_game requires libraryId, exePath and workingDir.";
        return json::Value();
    }
    // Reap a previously-exited game. Its detached watcher owns a separate
    // synchronization handle, so this control handle can close independently.
    if (host.game && host.game->process &&
        WaitForSingleObject(host.game->process, 0) == WAIT_OBJECT_0) {
        StopGameWatcher(*host.game);
        CloseHandle(host.game->process);
        host.game.reset();
    }
    if (host.game && host.game->process) {
        error = "A game is already running.";
        return json::Value();
    }
    if (!fs::is_directory(working_dir)) {
        error = "Game folder not found: " + working_dir_utf8;
        return json::Value();
    }
    if (!fs::exists(exe)) {
        error = "Game exe not found: " + exe_utf8;
        return json::Value();
    }

    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = working_dir.wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP, nullptr, cwd.c_str(), &si, &pi)) {
        error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return json::Value();
    }
    CloseHandle(pi.hThread);


    auto game = std::make_unique<RunningGame>();
    game->library_id = library_id;
    game->process = pi.hProcess;
    game->process_id = pi.dwProcessId;
    HWND hwnd = host.hwnd;
    HANDLE watch_process = OpenProcess(SYNCHRONIZE, FALSE, pi.dwProcessId);
    game->watcher_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (watch_process && game->watcher_stop) {
        HANDLE stop_event = game->watcher_stop;
        game->watcher = std::thread([hwnd, watch_process, stop_event, library_id]() {
            // Stop event first: teardown wins if both become signaled, so no
            // message can target a destroyed or reused HWND.
            HANDLE waits[] = {stop_event, watch_process};
            DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            CloseHandle(watch_process);
            if (result == WAIT_OBJECT_0 + 1) {
                PostJsonToUi(hwnd, json::dump(json::Value(json::Object{
                    {"event",     json::Value("game")},
                    {"running",   json::Value(false)},
                    {"libraryId", json::Value(library_id)},
                })));
            }
        });
    } else {
        if (watch_process) CloseHandle(watch_process);
        if (game->watcher_stop) {
            CloseHandle(game->watcher_stop);
            game->watcher_stop = nullptr;
        }
    }
    host.game = std::move(game);
    return json::Value(true);
}

json::Value HandleStopGame(HostState& host, std::string& error) {
    if (!host.game || !host.game->process) return json::Value(true);
    if (WaitForSingleObject(host.game->process, 0) == WAIT_OBJECT_0)
        return json::Value(true);

    if (!RequestGracefulGameClose(*host.game)) {
        error = "Could not find the game's window. It was left running to protect the shader cache.";
        return json::Value();
    }

    return json::Value(true);
}

// ---------------------------------------------------------------- uninstall

// Marker dirs that prove a path is a Glue360 workspace/deploy and not an
// arbitrary user folder. Deletion is refused without one.
bool HasRecompMarker(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p / ".recomp", ec) || fs::exists(p / "standalone", ec) ||
           p.filename() == "standalone" || p.filename() == ".recomp";
}

bool IsSafeDeleteTarget(const fs::path& p) {
    if (p.empty() || !p.is_absolute()) return false;
    if (p == p.root_path()) return false;  // drive root
    const std::string norm = p.lexically_normal().generic_string();
    if (norm.find("..") != std::string::npos) return false;
    // Require at least two components below the root (never C:\Games itself).
    if (std::distance(p.begin(), p.end()) < 3) return false;
    return HasRecompMarker(p);
}

// Move user_data (saves, profiles, shader cache) out of a tree about to be
// deleted. Covers the live location, a tier-2-preserved copy, and any prior
// "*_user_data" preservation folders so saves survive any operation sequence.
// Returns the preservation destination, or empty if nothing existed.
fs::path PreserveUserData(const fs::path& target) {
    std::error_code ec;
    std::vector<fs::path> candidates{target / "user_data",
                                     target / "standalone" / "user_data"};
    // Prior preservation outputs anywhere inside the tree (e.g. a tier-2
    // "standalone_user_data" folder now threatened by a tier-3 workspace wipe).
    for (const auto& entry : fs::directory_iterator(target, ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.size() >= 9 && name.find("user_data") != std::string::npos &&
            entry.path() != target / "user_data") {
            candidates.push_back(entry.path());
        }
    }
    fs::path last_preserved;
    for (const fs::path& candidate : candidates) {
        if (!fs::exists(candidate, ec)) continue;
        fs::path dest = target.parent_path() / (target.filename().string() + "_user_data");
        for (int n = 2; fs::exists(dest, ec) && n < 100; ++n) {
            dest = target.parent_path() /
                   (target.filename().string() + "_user_data_" + std::to_string(n));
        }
        if (fs::exists(dest, ec)) continue;  // no free slot — leave in place
        fs::rename(candidate, dest, ec);
        if (!ec) last_preserved = dest;
    }
    return last_preserved;
}

json::Value HandleDeleteGameFiles(HostState& host, const json::Value& args,
                                  std::string& error) {
    const std::string library_id = args.get_string("libraryId");
    const bool preserve_user_data = args.get_bool("preserveUserData", true);

    // Never delete files out from under a running game — the locked exe would
    // fail the delete midway and leave a half-removed tree.
    if (host.game && host.game->process &&
        WaitForSingleObject(host.game->process, 0) == WAIT_TIMEOUT) {
        if (library_id.empty() || host.game->library_id == library_id) {
            error = "The game is currently running. Stop it before uninstalling.";
            return json::Value();
        }
    }

    json::Array results;
    const json::Value& paths_arg = args.get("paths");
    if (!paths_arg.is_array()) {
        error = "delete_game_files: 'paths' must be an array";
        return json::Value();
    }
    for (const auto& pv : paths_arg.as_array()) {
        const fs::path target = fs::path(pv.as_string());
        json::Object r{{"path", json::Value(target.string())}};
        if (!IsSafeDeleteTarget(target)) {
            r["ok"] = json::Value(false);
            r["error"] = json::Value(
                "refused: path is not a recognized Glue360 workspace/deploy directory");
            results.push_back(json::Value(std::move(r)));
            continue;
        }
        std::error_code ec;
        if (!fs::exists(target, ec)) {
            r["ok"] = json::Value(true);  // already gone externally
            r["missing"] = json::Value(true);
            results.push_back(json::Value(std::move(r)));
            continue;
        }
        fs::path preserved;
        if (preserve_user_data) preserved = PreserveUserData(target);
        fs::remove_all(target, ec);
        r["ok"] = json::Value(!ec);
        if (ec) r["error"] = json::Value(ec.message());
        if (!preserved.empty())
            r["preservedUserDataTo"] = json::Value(preserved.string());
        results.push_back(json::Value(std::move(r)));
    }
    return json::Value(std::move(results));
}

json::Value HandlePathExists(const json::Value& args) {
    std::error_code ec;
    return json::Value(fs::exists(fs::path(args.get_string("path")), ec));
}

// Router: parse {rpc, cmd, args}, produce {rpc, ok, data|error}.
std::string HandleWebMessage(HostState& host, const std::string& msg_json) {
    long long rpc = 0;
    std::string cmd;
    json::Value args;
    try {
        json::Value msg = json::parse(msg_json);
        rpc = msg.get_int("rpc");
        cmd = msg.get_string("cmd");
        args = msg.is_object() ? msg.get("args") : json::Value();
    } catch (const std::exception& e) {
        return json::dump(json::Value(json::Object{
            {"rpc",   json::Value(rpc)},
            {"ok",    json::Value(false)},
            {"error", json::Value(std::string("bad message: ") + e.what())},
        }));
    }

    json::Value data;
    std::string error;
    try {
        if (cmd == "list_profiles") {
            data = HandleListProfiles();
        } else if (cmd == "check_deps") {
            data = HandleCheckDeps(args);
        } else if (cmd == "get_settings") {
            data = HandleGetSettings();
        } else if (cmd == "save_settings") {
            data = HandleSaveSettings(args);
        } else if (cmd == "pick_iso") {
            const COMDLG_FILTERSPEC filters[] = {
                {L"Xbox 360 disc image (*.iso)", L"*.iso"},
                {L"All files (*.*)", L"*.*"},
            };
            data = json::Value(PickFile(host.hwnd, L"Select Xbox 360 ISO",
                                        filters, 2, false));
        } else if (cmd == "pick_exe") {
            const COMDLG_FILTERSPEC filters[] = {
                {L"Windows executable (*.exe)", L"*.exe"},
                {L"All files (*.*)", L"*.*"},
            };
            data = json::Value(PickFile(host.hwnd, L"Select compiled game executable",
                                        filters, 2, false));
        } else if (cmd == "pick_dir") {
            std::wstring title = Utf8ToWide(args.get_string("title", "Select folder"));
            data = json::Value(PickFile(host.hwnd, title.c_str(), nullptr, 0, true));
        } else if (cmd == "open_folder") {
            std::wstring p = Utf8ToWide(args.get_string("path"));
            if (!p.empty())
                ShellExecuteW(nullptr, L"explore", p.c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
            data = json::Value(true);
        } else if (cmd == "start_recompile") {
            data = HandleStartRecompile(host, args, error);
        } else if (cmd == "cancel_recompile") {
            if (host.job) host.job->cancel.store(true);
            data = json::Value(true);
        } else if (cmd == "launch_game") {
            data = HandleLaunchGame(host, args, error);
        } else if (cmd == "stop_game") {
            data = HandleStopGame(host, error);
        } else if (cmd == "path_exists") {
            data = HandlePathExists(args);
        } else if (cmd == "delete_game_files") {
            data = HandleDeleteGameFiles(host, args, error);
        } else {
            error = "unknown command: " + cmd;
        }
    } catch (const std::exception& e) {
        error = e.what();
    }

    json::Object resp{
        {"rpc", json::Value(rpc)},
        {"ok",  json::Value(error.empty())},
    };
    if (error.empty()) resp["data"] = data;
    else               resp["error"] = json::Value(error);
    return json::dump(json::Value(std::move(resp)));
}

// ---------------------------------------------------------------- window

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_POST_JSON: {
        auto* payload = reinterpret_cast<std::string*>(lp);
        if (g_host && g_host->webview)
            g_host->webview->PostWebMessageAsJson(Utf8ToWide(*payload).c_str());
        delete payload;
        return 0;
    }
    case WM_SIZE:
        if (g_host && g_host->controller) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            g_host->controller->put_Bounds(rc);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

bool RunWebViewGui() {
    // UI must exist before WebView2 init.
    fs::path html = ExtractUiHtml();
    if (html.empty()) return false;

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Glue360Window";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hbrBackground = CreateSolidBrush(RGB(7, 9, 7));
    RegisterClassW(&wc);

    HostState host;
    g_host = &host;

    HWND hwnd = CreateWindowExW(
        0, L"Glue360Window", L"Glue360 — Xbox 360 Recompile Station",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1360, 860,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        g_host = nullptr;
        CoUninitialize();
        return false;
    }
    host.hwnd = hwnd;

    // Dark titlebar (no-op on older Windows).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark,
                          sizeof(dark));

    ShowWindow(hwnd, SW_SHOW);

    // --- WebView2 environment ---
    fs::path user_data = AppDataDir() / "WebView2";
    std::error_code ec;
    fs::create_directories(user_data, ec);

    bool env_failed = false;
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data.wstring().c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [&host, hwnd, &html, &env_failed](HRESULT result,
                                              ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    env_failed = true;
                    PostQuitMessage(1);
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [&host, hwnd, &html, &env_failed](
                            HRESULT result2,
                            ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result2) || !controller) {
                                env_failed = true;
                                PostQuitMessage(1);
                                return S_OK;
                            }
                            host.controller = controller;
                            controller->get_CoreWebView2(&host.webview);

                            ComPtr<ICoreWebView2Settings> settings;
                            host.webview->get_Settings(&settings);
                            settings->put_IsWebMessageEnabled(TRUE);
                            settings->put_AreDefaultContextMenusEnabled(FALSE);
                            settings->put_IsZoomControlEnabled(FALSE);
                            settings->put_AreDevToolsEnabled(TRUE);

                            HRESULT reg_hr = host.webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [&host](ICoreWebView2*,
                                            ICoreWebView2WebMessageReceivedEventArgs* e)
                                        -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (FAILED(e->get_WebMessageAsJson(&raw)) || !raw)
                                            return S_OK;
                                        std::string msg = WideToUtf8(raw);
                                        CoTaskMemFree(raw);
                                        std::string resp = HandleWebMessage(host, msg);
                                        host.webview->PostWebMessageAsJson(
                                            Utf8ToWide(resp).c_str());
                                        return S_OK;
                                    })
                                    .Get(),
                                &host.msg_token);
                            if (FAILED(reg_hr)) {
                                env_failed = true;
                                PostQuitMessage(1);
                                return S_OK;
                            }

                            RECT rc;
                            GetClientRect(hwnd, &rc);
                            controller->put_Bounds(rc);

                            std::wstring url =
                                L"file:///" +
                                Utf8ToWide(html.generic_string());
                            host.webview->Navigate(url.c_str());
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        // WebView2 runtime not installed / loader failure.
        DestroyWindow(hwnd);
        g_host = nullptr;
        CoUninitialize();
        return false;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // --- teardown ---
    if (host.job) {
        host.job->cancel.store(true);
        if (host.job->worker.joinable()) host.job->worker.join();
    }
    if (host.game && host.game->process) {
        if (WaitForSingleObject(host.game->process, 0) != WAIT_OBJECT_0) {
            RequestGracefulGameClose(*host.game);
            // Never force-kill: an interrupted .xpso write corrupts the shader
            // cache. If the game ignores WM_CLOSE, closing our handle leaves
            // the independent game process running.
            WaitForSingleObject(host.game->process, 2000);
        }
        StopGameWatcher(*host.game);
        CloseHandle(host.game->process);
        host.game->process = nullptr;
    }
    if (host.webview) host.webview->remove_WebMessageReceived(host.msg_token);
    host.webview.Reset();
    host.controller.Reset();
    g_host = nullptr;
    CoUninitialize();
    return !env_failed;
}

} // namespace recomp::gui

#endif // _WIN32
