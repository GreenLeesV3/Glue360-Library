// template_renderer.cpp — minimal {{KEY}} placeholder renderer.
//
// Token form: {{KEY}} or {{ KEY }} (optional inner whitespace, no newlines).
// Unknown keys are left as-is. No recursion (a value is never re-scanned) —
// this avoids accidental infinite loops and matches the deploy TOML use case
// where values are filesystem paths, not templates.

#include "template_renderer.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace recomp::profile {

namespace {

// Find the next {{ ... }} token starting at `pos`. On success returns true and
// sets `key` (trimmed) and `end` (one past the closing }}).
bool next_token(std::string_view s, size_t pos, size_t& tok_start,
                size_t& tok_end, std::string& key) {
  tok_start = s.find("{{", pos);
  if (tok_start == std::string_view::npos) return false;
  size_t close = s.find("}}", tok_start + 2);
  if (close == std::string_view::npos) return false;
  // Inner text.
  std::string_view inner = s.substr(tok_start + 2, close - (tok_start + 2));
  // Trim whitespace.
  size_t b = 0, e = inner.size();
  while (b < e && (inner[b] == ' ' || inner[b] == '\t')) ++b;
  while (e > b && (inner[e - 1] == ' ' || inner[e - 1] == '\t')) --e;
  key = std::string(inner.substr(b, e - b));
  tok_end = close + 2;
  return true;
}

}  // namespace

namespace {
// Core lookup templated on the map type so std::map and std::unordered_map
// share one implementation without slicing/copying.
template <class Map>
std::string render_string_impl(std::string_view input, const Map& vars) {
  std::string out;
  out.reserve(input.size());
  size_t cursor = 0;
  size_t tok_start = 0, tok_end = 0;
  std::string key;
  while (next_token(input, cursor, tok_start, tok_end, key)) {
    // Append literal text before the token.
    out.append(input.data() + cursor, tok_start - cursor);
    // Look up the key; leave unknown tokens verbatim.
    auto it = vars.find(key);
    if (it != vars.end()) {
      out.append(it->second);
    } else {
      out.append(input.data() + tok_start, tok_end - tok_start);
    }
    cursor = tok_end;
  }
  // Append trailing literal.
  if (cursor < input.size()) {
    out.append(input.data() + cursor, input.size() - cursor);
  }
  return out;
}

template <class Map>
bool render_template_text_impl(std::string_view template_text,
                               const fs::path& output_path, const Map& vars) {
  std::string rendered = render_string_impl(template_text, vars);
  std::error_code ec;
  fs::create_directories(output_path.parent_path(), ec);
  std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(rendered.data(), static_cast<std::streamsize>(rendered.size()));
  out.close();
  return out.good();
}
}  // namespace

std::string render_string(std::string_view input,
                          const std::map<std::string, std::string>& vars) {
  return render_string_impl(input, vars);
}
std::string render_string(
    std::string_view input,
    const std::unordered_map<std::string, std::string>& vars) {
  return render_string_impl(input, vars);
}

bool render_template(const fs::path& template_path,
                     const fs::path& output_path,
                     const std::map<std::string, std::string>& vars) {
  std::error_code ec;
  if (!fs::exists(template_path, ec)) return false;
  std::ifstream in(template_path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  return render_template_text(text, output_path, vars);
}
bool render_template(const fs::path& template_path,
                     const fs::path& output_path,
                     const std::unordered_map<std::string, std::string>& vars) {
  std::error_code ec;
  if (!fs::exists(template_path, ec)) return false;
  std::ifstream in(template_path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  return render_template_text(text, output_path, vars);
}

bool render_template_text(std::string_view template_text,
                          const fs::path& output_path,
                          const std::map<std::string, std::string>& vars) {
  return render_template_text_impl(template_text, output_path, vars);
}
bool render_template_text(
    std::string_view template_text,
    const fs::path& output_path,
    const std::unordered_map<std::string, std::string>& vars) {
  return render_template_text_impl(template_text, output_path, vars);
}

}  // namespace recomp::profile
