// core/pipeline_context.cpp — PipelineContext JSON (de)serialization + derived paths.
#include "core/pipeline_context.h"
#include "core/json_mini.h"

#include <filesystem>

namespace recomp {

namespace fs = std::filesystem;
using json::Value;
using json::Object;
using json::Array;

namespace {

Value path_val(const fs::path& p) {
    return Value(p.generic_string());
}

fs::path get_path(const Value& obj, const std::string& key) {
    std::string s = obj.get_string(key);
    if (s.empty()) return {};
    return fs::path(s);
}

Value toolchain_to_json(const deps::ToolchainInfo& t) {
    Object o;
    o["cmake_exe"]            = path_val(t.cmake_exe);
    o["cmake_version"]        = Value(t.cmake_version);
    o["ninja_exe"]            = path_val(t.ninja_exe);
    o["ninja_version"]        = Value(t.ninja_version);
    o["clang_cl_exe"]         = path_val(t.clang_cl_exe);
    o["clang_cl_version"]     = Value(t.clang_cl_version);
    o["clang_cl_target"]      = Value(t.clang_cl_target);
    o["vs_install_dir"]       = path_val(t.vs_install_dir);
    o["vcvarsall_bat"]        = path_val(t.vcvarsall_bat);
    o["msvc_toolset_version"] = Value(t.msvc_toolset_version);
    o["windows_sdk_root"]     = path_val(t.windows_sdk_root);
    o["windows_sdk_version"]  = Value(t.windows_sdk_version);
    o["sdk_root"]             = path_val(t.sdk_root);
    o["sdk_version"]          = Value(t.sdk_version);
    o["sdk_source_dir"]       = path_val(t.sdk_source_dir);
    o["sdk_source_is_git"]    = Value(t.sdk_source_is_git);
    o["git_exe"]              = path_val(t.git_exe);
    o["extract_xiso_exe"]     = path_val(t.extract_xiso_exe);
    o["blocking_ok"]          = Value(t.blocking_ok);
    return Value(o);
}

void toolchain_from_json(const Value& obj, deps::ToolchainInfo& t) {
    t.cmake_exe            = get_path(obj, "cmake_exe");
    t.cmake_version        = obj.get_string("cmake_version");
    t.ninja_exe            = get_path(obj, "ninja_exe");
    t.ninja_version        = obj.get_string("ninja_version");
    t.clang_cl_exe         = get_path(obj, "clang_cl_exe");
    t.clang_cl_version     = obj.get_string("clang_cl_version");
    t.clang_cl_target      = obj.get_string("clang_cl_target");
    t.vs_install_dir       = get_path(obj, "vs_install_dir");
    t.vcvarsall_bat        = get_path(obj, "vcvarsall_bat");
    t.msvc_toolset_version = obj.get_string("msvc_toolset_version");
    t.windows_sdk_root     = get_path(obj, "windows_sdk_root");
    t.windows_sdk_version  = obj.get_string("windows_sdk_version");
    t.sdk_root             = get_path(obj, "sdk_root");
    t.sdk_version          = obj.get_string("sdk_version");
    t.sdk_source_dir       = get_path(obj, "sdk_source_dir");
    t.sdk_source_is_git    = obj.get_bool("sdk_source_is_git");
    t.git_exe              = get_path(obj, "git_exe");
    t.extract_xiso_exe     = get_path(obj, "extract_xiso_exe");
    t.blocking_ok          = obj.get_bool("blocking_ok");
}

Value env_to_json(const std::map<std::string, std::string>& env) {
    Object o;
    for (const auto& [k, v] : env) o[k] = Value(v);
    return Value(o);
}

void env_from_json(const Value& obj, std::map<std::string, std::string>& env) {
    env.clear();
    if (!obj.is_object()) return;
    for (const auto& [k, v] : obj.as_object()) {
        if (v.is_string()) env[k] = v.as_string();
    }
}

} // namespace

std::string PipelineContext::to_json() const {
    Object root;
    root["profile_name"]          = Value(profile_name);
    root["iso_path"]              = path_val(iso_path);
    root["output_dir"]            = path_val(output_dir);
    root["app_dir"]               = path_val(app_dir);
    root["sdk_path"]              = path_val(sdk_path);
    root["sdk_source_path"]       = path_val(sdk_source_path);
    root["extracted_dir"]         = path_val(extracted_dir);
    root["project_dir"]           = path_val(project_dir);
    root["manifest_path"]         = path_val(manifest_path);
    root["generated_dir"]         = path_val(generated_dir);
    root["generated_shard_count"] = Value(static_cast<long long>(generated_shard_count));
    root["runtime_build_dir"]     = path_val(runtime_build_dir);
    root["custom_runtime_dll"]    = path_val(custom_runtime_dll);
    root["tracy_dll_path"]        = path_val(tracy_dll_path);
    root["game_build_dir"]        = path_val(game_build_dir);
    root["built_exe"]             = path_val(built_exe);
    root["deploy_dir"]            = path_val(deploy_dir);
    root["toolchain"]             = toolchain_to_json(toolchain);
    root["build_env"]             = env_to_json(build_env);
    root["graphics_backend"]      = Value("d3d12");
    return json::dump(Value(root));
}

bool PipelineContext::from_json(const std::string& text) {
    Value root;
    try {
        root = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!root.is_object()) return false;
    const Value& o = root;
    profile_name          = o.get_string("profile_name", profile_name);
    iso_path              = get_path(o, "iso_path");
    output_dir            = get_path(o, "output_dir");
    app_dir               = get_path(o, "app_dir");
    sdk_path              = get_path(o, "sdk_path");
    sdk_source_path       = get_path(o, "sdk_source_path");
    extracted_dir         = get_path(o, "extracted_dir");
    project_dir           = get_path(o, "project_dir");
    manifest_path         = get_path(o, "manifest_path");
    generated_dir         = get_path(o, "generated_dir");
    generated_shard_count = static_cast<int>(o.get_int("generated_shard_count"));
    runtime_build_dir     = get_path(o, "runtime_build_dir");
    custom_runtime_dll    = get_path(o, "custom_runtime_dll");
    tracy_dll_path        = get_path(o, "tracy_dll_path");
    game_build_dir        = get_path(o, "game_build_dir");
    built_exe             = get_path(o, "built_exe");
    std::string be = o.get_string("graphics_backend", "d3d12");
    graphics_backend = GraphicsBackend::D3D12;  // always D3D12
    deploy_dir            = get_path(o, "deploy_dir");
    toolchain_from_json(o.get("toolchain"), toolchain);
    // build_env is NOT restored from state.json — it's volatile (captured
    // fresh from vcvarsall on every run). Restoring a stale/empty env would
    // cause MSVC-dependent stages to fail on resume.
    return true;
}

fs::path PipelineContext::recomp_dir() const { return output_dir / ".recomp"; }
fs::path PipelineContext::log_dir() const   { return recomp_dir() / "logs"; }
fs::path PipelineContext::state_path() const { return recomp_dir() / "state.json"; }

} // namespace recomp
