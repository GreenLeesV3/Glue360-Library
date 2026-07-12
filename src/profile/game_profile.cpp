// game_profile.cpp — TOML loader + validator for GameProfile.
//
// Reads <profile_dir>/profile.toml using tomlplusplus (vendored in the RexGlue
// SDK at include/toml++/toml.hpp). The schema mirrors docs/01_architecture.md
// §6.2 and the spiderman3/profile.toml shipped under profiles/.
//
// Schema (profile.toml):
//   [profile]
//   id = "spiderman3"
//   name = "Spider-Man 3 (Xbox 360)"
//   title_id = "415607E2"
//   sdk_version = "0.8.0"
//   xex_entrypoint = "default.xex"   # optional, default "default.xex"
//   project_name = "spiderman3"      # optional, defaults to id
//
//   [build]
//   requires_sdk_source = true       # optional, default false
//
//   [cvars]
//   render_target_path_d3d12 = "rov"
//   ...                              # 22 cvars for spiderman3
//
//   [[sources]]
//   from = "src/main.cpp"
//   to   = "src/main.cpp"
//   optional = false                 # optional, default false
//
//   [[runtime_patches]]
//   id       = "xam_enum"
//   flag     = "REX_XAM_ENUM_IO_PENDING"
//   target   = "src/kernel/xam/xam_enum.cpp"
//   category = "game-runtime"
//   required = true
//
//   runtime_flags = [...]            # array of strings under [build] or root
//
//   [deploy]
//   toml_template = "spiderman3.toml.template"  # relative to profile_dir
//   copy_dlls = ["rexruntime.dll", "TracyClient.dll"]
//   create_game_junction = true
//   create_user_data = true

#include "game_profile.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace recomp::profile {

namespace {

void fail(const std::string& msg) {
  throw std::runtime_error("GameProfile: " + msg);
}

std::string opt_string(const toml::table& t, const std::string& key,
                       const std::string& def = "") {
  if (auto* n = t.get(key); n && n->is_string()) {
    return n->as_string()->get();
  }
  return def;
}

bool opt_bool(const toml::table& t, const std::string& key, bool def = false) {
  if (auto* n = t.get(key); n && n->is_boolean()) {
    return n->as_boolean()->get();
  }
  return def;
}

std::vector<std::string> opt_string_array(const toml::table& t,
                                          const std::string& key) {
  std::vector<std::string> out;
  if (auto* n = t.get(key); n && n->is_array()) {
    for (const auto& e : *n->as_array()) {
      if (e.is_string()) out.push_back(e.as_string()->get());
    }
  }
  return out;
}

GameProfile parse_profile(const fs::path& profile_dir,
                          const toml::table& root) {
  GameProfile p;
  p.profile_dir = profile_dir;

  // --- [profile] ---
  const toml::table* prof = nullptr;
  if (auto* n = root.get("profile"); n && n->is_table()) {
    prof = n->as_table();
  } else {
    fail("missing [profile] table");
  }
  p.id = opt_string(*prof, "id");
  if (p.id.empty()) fail("[profile].id is required");
  p.name = opt_string(*prof, "name", p.id);
  p.title_id = opt_string(*prof, "title_id");
  p.sdk_version = opt_string(*prof, "sdk_version", "0.8.0");
  p.xex_entrypoint = opt_string(*prof, "xex_entrypoint", "default.xex");
  p.project_name = opt_string(*prof, "project_name", p.id);

  // --- [build] ---
  if (auto* n = root.get("build"); n && n->is_table()) {
    const toml::table& build = *n->as_table();
    p.requires_sdk_source = opt_bool(build, "requires_sdk_source", false);
    p.runtime_flags = opt_string_array(build, "runtime_flags");
  }

  // --- [build_options] ---
  if (auto* n = root.get("build_options"); n && n->is_table()) {
    const toml::table& bo = *n->as_table();
    p.build_options.static_msvc_runtime = opt_bool(bo, "static_msvc_runtime", true);
    p.build_options.enable_lto = opt_bool(bo, "enable_lto", true);
    p.build_options.cpu_target = opt_string(bo, "cpu_target", "");
  }
  // runtime_flags may also live at root for the docs' schema variant.
  if (p.runtime_flags.empty()) {
    p.runtime_flags = opt_string_array(root, "runtime_flags");
  }

  // Parse runtime_patches (loaded for profiles that declare SDK source
  // overlays). The patch_applier stage skips them with a warning when SDK
  // source is not available, enabling "lite" mode (prebuilt runtime, no
  // save fixes) without removing the patch declarations from the profile.
  if (auto* n = root.get("runtime_patches"); n && n->is_array()) {
    for (const auto& e : *n->as_array()) {
      if (!e.is_table()) continue;
      const toml::table& pt = *e.as_table();
      RuntimePatch rp;
      rp.id = opt_string(pt, "id");
      rp.flag = opt_string(pt, "flag");
      rp.target = opt_string(pt, "target");
      rp.category = opt_string(pt, "category", "game-runtime");
      rp.required = opt_bool(pt, "required", true);
      if (rp.id.empty()) fail("[[runtime_patches]] requires an `id`");
      p.runtime_patches.push_back(std::move(rp));
    }
  }
  // --- [cvars] ---
  if (auto* n = root.get("cvars"); n && n->is_table()) {
    for (const auto& [k, v] : *n->as_table()) {
      if (v.is_string()) {
        p.cvars.emplace(std::string(k.str()), v.as_string()->get());
      } else if (v.is_integer()) {
        p.cvars.emplace(std::string(k.str()),
                        std::to_string(v.as_integer()->get()));
      } else if (v.is_floating_point()) {
        // Preserve "120.0" style for cvars that expect a float string.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g",
                      static_cast<double>(v.as_floating_point()->get()));
        p.cvars.emplace(std::string(k.str()), std::string(buf));
      } else if (v.is_boolean()) {
        p.cvars.emplace(std::string(k.str()),
                        v.as_boolean()->get() ? "true" : "false");
      }
    }
  }

  // --- [entrypoint_functions] (address hex string -> symbol name) ---
  if (auto* n = root.get("entrypoint_functions"); n && n->is_table()) {
    for (const auto& [k, v] : *n->as_table()) {
      if (v.is_string()) {
        p.entrypoint_functions.emplace(std::string(k.str()),
                                       v.as_string()->get());
      }
    }
  }

  // --- [functions] (address -> {end, parent, name}) ---
  // Richer format inspired by Skate3Recomp: explicit end addresses prevent
  // function scanner truncation bugs, parent relationships handle overlaps.
  // Codegen reads these from the manifest's [functions] table.
  if (auto* n = root.get("functions"); n && n->is_table()) {
    for (const auto& [k, v] : *n->as_table()) {
      if (!v.is_table()) continue;
      const toml::table& ft = *v.as_table();
      GameProfile::FunctionEntry fe;
      std::string end_str = opt_string(ft, "end");
      if (!end_str.empty()) {
        fe.end = std::stoull(end_str, nullptr, 16);
      } else if (auto* iv = ft.get("end"); iv && iv->is_integer()) {
        fe.end = static_cast<uint64_t>(iv->as_integer()->get());
      }
      std::string parent_str = opt_string(ft, "parent");
      if (!parent_str.empty()) {
        fe.parent = std::stoull(parent_str, nullptr, 16);
      } else if (auto* iv = ft.get("parent"); iv && iv->is_integer()) {
        fe.parent = static_cast<uint64_t>(iv->as_integer()->get());
      }
      fe.name = opt_string(ft, "name");
      p.functions.emplace(std::string(k.str()), std::move(fe));
    }
  }

  // --- setjmp/longjmp addresses (top-level hex string keys) ---
  if (auto* v = root.get("setjmp_address"); v) {
    if (v->is_string()) {
      p.setjmp_address = std::stoull(v->as_string()->get(), nullptr, 16);
    } else if (v->is_integer()) {
      p.setjmp_address = static_cast<uint64_t>(v->as_integer()->get());
    }
  }
  if (auto* v = root.get("longjmp_address"); v) {
    if (v->is_string()) {
      p.longjmp_address = std::stoull(v->as_string()->get(), nullptr, 16);
    } else if (v->is_integer()) {
      p.longjmp_address = static_cast<uint64_t>(v->as_integer()->get());
    }
  }

  // --- [[switch_tables]] overrides ---
  if (auto* n = root.get("switch_tables"); n && n->is_array()) {
    for (const auto& e : *n->as_array()) {
      if (!e.is_table()) continue;
      const toml::table& st = *e.as_table();
      GameProfile::SwitchTable sw;
      std::string addr_str = opt_string(st, "address");
      if (!addr_str.empty()) {
        sw.address = std::stoull(addr_str, nullptr, 16);
      } else if (auto* iv = st.get("address"); iv && iv->is_integer()) {
        sw.address = static_cast<uint64_t>(iv->as_integer()->get());
      }
      if (auto* rv = st.get("register"); rv) {
        if (rv->is_integer()) {
          sw.register_index = static_cast<int>(rv->as_integer()->get());
        }
      }
      if (auto* lbls = st.get("labels"); lbls && lbls->is_array()) {
        for (const auto& lbl : *lbls->as_array()) {
          if (lbl.is_string()) {
            sw.labels.push_back(
                std::stoull(lbl.as_string()->get(), nullptr, 16));
          } else if (lbl.is_integer()) {
            sw.labels.push_back(static_cast<uint64_t>(lbl.as_integer()->get()));
          }
        }
      }
      if (sw.address != 0 && !sw.labels.empty()) {
        p.switch_tables.push_back(std::move(sw));
      }
    }
  }

  // --- [[invalid_instructions]] (data value -> skip size) ---
  // The codegen skips over these 32-bit values in the instruction stream.
  // XenonRecomp pattern — handles exception data, padding, frame handlers.
  if (auto* n = root.get("invalid_instructions"); n && n->is_array()) {
    for (const auto& e : *n->as_array()) {
      if (!e.is_table()) continue;
      const toml::table& ii = *e.as_table();
      uint64_t data = 0, size = 0;
      std::string data_str = opt_string(ii, "data");
      if (!data_str.empty()) {
        data = std::stoull(data_str, nullptr, 16);
      } else if (auto* iv = ii.get("data"); iv && iv->is_integer()) {
        data = static_cast<uint64_t>(iv->as_integer()->get());
      }
      std::string size_str = opt_string(ii, "size");
      if (!size_str.empty()) {
        size = std::stoull(size_str, nullptr, 16);
      } else if (auto* iv = ii.get("size"); iv && iv->is_integer()) {
        size = static_cast<uint64_t>(iv->as_integer()->get());
      }
      if (data != 0 && size > 0) {
        p.invalid_instructions[data] = size;
      }
    }
  }

  // --- [[sources]] ---
  if (auto* n = root.get("sources"); n && n->is_array()) {
    for (const auto& e : *n->as_array()) {
      if (!e.is_table()) continue;
      const toml::table& st = *e.as_table();
      SourceFile sf;
      sf.from = opt_string(st, "from");
      sf.to = opt_string(st, "to");
      sf.optional = opt_bool(st, "optional", false);
      if (sf.from.empty()) fail("[[sources]] requires a `from` path");
      if (sf.to.empty()) sf.to = sf.from;
      p.source_files.push_back(std::move(sf));
    }
  } else if (auto* n2 = root.get("sources"); n2 && n2->is_table()) {
    // alternate: [sources] files = [ {from,to}, ... ]
    const toml::table& st = *n2->as_table();
    if (auto* fa = st.get("files"); fa && fa->is_array()) {
      for (const auto& e : *fa->as_array()) {
        if (!e.is_table()) continue;
        const toml::table& ft = *e.as_table();
        SourceFile sf;
        sf.from = opt_string(ft, "from");
        sf.to = opt_string(ft, "to");
        sf.optional = opt_bool(ft, "optional", false);
        if (sf.from.empty()) fail("[sources].files entry requires a `from`");
        if (sf.to.empty()) sf.to = sf.from;
        p.source_files.push_back(std::move(sf));
      }
    }
  }

  // --- [deploy] ---
  if (auto* n = root.get("deploy"); n && n->is_table()) {
    const toml::table& dt = *n->as_table();
    std::string tpl = opt_string(dt, "toml_template");
    if (!tpl.empty()) {
      p.toml_template = profile_dir / tpl;
    }
    auto dlls = opt_string_array(dt, "copy_dlls");
    if (!dlls.empty()) p.copy_dlls = std::move(dlls);
    p.create_game_junction = opt_bool(dt, "create_game_junction", true);
    p.create_user_data = opt_bool(dt, "create_user_data", true);
  }

  // Default toml_template: <profile_dir>/<id>.toml.template
  if (p.toml_template.empty()) {
    p.toml_template = profile_dir / (p.id + ".toml.template");
  }

  return p;
}

}  // namespace

GameProfile load_profile_from_dir(const fs::path& profile_dir) {
  std::error_code ec;
  if (profile_dir.empty() || !fs::is_directory(profile_dir, ec)) {
    fail("profile directory does not exist: " + profile_dir.string());
  }
  fs::path toml_path = profile_dir / "profile.toml";
  if (!fs::exists(toml_path, ec)) {
    fail("profile.toml not found in " + profile_dir.string());
  }
  // toml++'s parse_file returns toml::parse_result. When exceptions are
  // enabled (the default for this SDK build) parse_result is an alias for
  // toml::table and parse failures throw toml::parse_error; when exceptions
  // are disabled parse_result is a discriminated union with operator bool()
  // and .error(). Handle both portable: try/catch for the throwing mode,
  // and an explicit succeeded() check for the no-exceptions mode.
  toml::table root;
  try {
    auto pr = toml::parse_file(toml_path.string());
#if TOML_EXCEPTIONS
    root = std::move(pr);
#else
    if (!pr.succeeded()) {
      fail("failed to parse " + toml_path.string() + ": " +
           std::string(pr.error().description()));
    }
    root = std::move(pr).table();
#endif
  } catch (const toml::parse_error& e) {
    fail("failed to parse " + toml_path.string() + ": " +
         std::string(e.description()));
  } catch (const std::exception& e) {
    fail("failed to parse " + toml_path.string() + ": " + e.what());
  }
  return parse_profile(profile_dir, root);
}

GameProfile load_profile(const fs::path& app_dir,
                         const std::string& profile_id) {
  if (profile_id.empty()) fail("profile_id is empty");
  fs::path dir = app_dir / "profiles" / profile_id;
  return load_profile_from_dir(dir);
}

std::vector<std::string> find_source_orphans(
    const GameProfile& profile,
    const std::vector<std::string>& cmakelists_sources) {
  std::vector<std::string> orphans;
  for (const auto& sf : profile.source_files) {
    if (sf.optional) continue;
    // Only .cpp files are compiled; headers are not "orphans" if absent.
    if (sf.to.size() < 4 ||
        sf.to.compare(sf.to.size() - 4, 4, ".cpp") != 0) {
      continue;
    }
    if (std::find(cmakelists_sources.begin(), cmakelists_sources.end(), sf.to) ==
        cmakelists_sources.end()) {
      orphans.push_back(sf.to);
    }
  }
  return orphans;
}

}  // namespace recomp::profile
