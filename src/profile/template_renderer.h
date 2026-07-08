// template_renderer.h — Minimal {{KEY}} placeholder renderer.
//
// Replaces {{KEY}} (and {{ KEY }} with optional inner whitespace) tokens in a
// template file with values from a map. No external dependency (inja is
// overkill for the single-placeholder deploy TOML); this is pure string
// replacement. Used by the deploy stage to render spiderman3.toml.template
// with {{GAME_DATA_ROOT}} -> the extracted game data path.
//
// Design note: the full game-project patches (spiderman3_app.h, etc.) use
// inja templating via the SDK-vendored inja; this lightweight renderer is
// only for the simple deploy TOML template that has one placeholder.

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

namespace recomp::profile {

namespace fs = std::filesystem;

// Render `input` by replacing every {{KEY}} (or {{ KEY }}) with the
// corresponding value from `vars`. Unknown keys are left untouched (so a
// typo surfaces as a literal {{...}} in the output rather than silent
// truncation). Returns the rendered string. Overloads accept either
// std::map or std::unordered_map so callers don't have to convert.
[[nodiscard]] std::string render_string(
    std::string_view input,
    const std::map<std::string, std::string>& vars);
[[nodiscard]] std::string render_string(
    std::string_view input,
    const std::unordered_map<std::string, std::string>& vars);

// Render the template file `template_path` and write the result to
// `output_path`. Creates parent directories for the output. Returns true on
// success, false on I/O error (no throw — caller logs).
bool render_template(const fs::path& template_path,
                     const fs::path& output_path,
                     const std::map<std::string, std::string>& vars);
bool render_template(const fs::path& template_path,
                     const fs::path& output_path,
                     const std::unordered_map<std::string, std::string>& vars);

// Render `template_text` and write the result to `output_path`. Convenience
// overload for in-memory templates. Returns true on success.
bool render_template_text(std::string_view template_text,
                          const fs::path& output_path,
                          const std::map<std::string, std::string>& vars);
bool render_template_text(std::string_view template_text,
                          const fs::path& output_path,
                          const std::unordered_map<std::string, std::string>& vars);

}  // namespace recomp::profile
