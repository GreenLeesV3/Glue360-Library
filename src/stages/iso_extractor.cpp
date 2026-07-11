// iso_extractor.cpp — Stage 1 implementation.
//
// Runs:  extract-xiso -x "<iso>" -d "<extracted>"
// then verifies <extracted>/default.xex exists (the XEX entrypoint, ~14MB).
//
// The extraction target is <output_dir>/extracted/ (the fixed relative layout
// the rexglue manifest expects: game_root = "../extracted"). The manifest's
// relative paths only resolve if the rexglue project lives at
// <output_dir>/spiderman3/, which is where RexglueInitStage creates it.

#include "stages/iso_extractor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/pipeline_context.h"
#include "core/process_runner.h"
#include "core/types.h"
#include "iso/xdvdfs_reader.h"

namespace fs = std::filesystem;

namespace recomp {

namespace {

// XDVDFS volume descriptor root entry signature check (docs/03 §1.1).
bool looks_like_xdvdfs_descriptor(const std::vector<unsigned char>& buf) {
  if (buf.size() < 0x800) return false;
  std::uint32_t root_sector =
      (static_cast<std::uint32_t>(buf[0]) << 24) |
      (static_cast<std::uint32_t>(buf[1]) << 16) |
      (static_cast<std::uint32_t>(buf[2]) << 8) |
      static_cast<std::uint32_t>(buf[3]);
  if (root_sector == 0) return false;
  unsigned char attr = buf[4];
  return (attr & 0x80) != 0;
}

std::vector<unsigned char> read_at(const fs::path& p, std::uint64_t offset,
                                   std::size_t len) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!f) return {};
  std::vector<unsigned char> buf(len);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(len));
  buf.resize(static_cast<std::size_t>(f.gcount()));
  return buf;
}

bool is_xbox_iso(const fs::path& iso) {
  if (!fs::exists(iso)) return false;
  auto head = read_at(iso, 0, 0x10000);
  if (head.empty()) return false;
  bool head_all_zero = std::all_of(head.begin(), head.end(),
                                   [](unsigned char b) { return b == 0; });
  static const std::uint64_t kOffsets[] = {0x20800, 0x10000, 0xFD00000};
  for (std::uint64_t off : kOffsets) {
    auto desc = read_at(iso, off, 0x800);
    if (!desc.empty() && looks_like_xdvdfs_descriptor(desc)) return true;
  }
  if (!head_all_zero) {
    auto desc = read_at(iso, 0x10000, 0x800);
    if (!desc.empty() && looks_like_xdvdfs_descriptor(desc)) return true;
  }
  for (std::uint64_t off = 0x10000; off < 0x2000000; off += 0x800) {
    auto desc = read_at(iso, off, 0x800);
    if (desc.empty()) break;
    if (looks_like_xdvdfs_descriptor(desc)) return true;
  }
  return false;
}

// XGD3 detection — Spider-Man 3 is XGD2, so XGD3 is the wrong game.
bool is_xgd3(const fs::path& iso) {
  auto desc = read_at(iso, 0xFD00000, 0x800);
  return !desc.empty() && looks_like_xdvdfs_descriptor(desc);
}

// Resolve the extract-xiso binary. Probes candidates in order:
//   1. ctx.toolchain.extract_xiso_exe  (resolved by DependencyChecker)
//   2. <app_dir>/tools/extract-xiso.exe (bundled, BSD-3-Clause, ~200KB)
//   3. <sdk_root>/bin/extract-xiso.exe  (if the SDK ships it)
//   4. empty path                       (let ProcessRunner find it on PATH)
fs::path resolve_extract_xiso(const PipelineContext& ctx) {
  std::error_code ec;
  if (!ctx.toolchain.extract_xiso_exe.empty() &&
      fs::exists(ctx.toolchain.extract_xiso_exe, ec)) {
    return ctx.toolchain.extract_xiso_exe;
  }
  if (!ctx.app_dir.empty()) {
    fs::path bundled = ctx.app_dir / "tools" / "extract-xiso.exe";
    if (fs::exists(bundled, ec)) return bundled;
  }
  if (!ctx.toolchain.sdk_root.empty()) {
    fs::path sdk_bin = ctx.toolchain.sdk_root / "bin" / "extract-xiso.exe";
    if (fs::exists(sdk_bin, ec)) return sdk_bin;
  }
  return {};  // rely on PATH
}

}  // namespace

CheckResult IsoExtractorStage::check_prereqs(const PipelineContext& ctx) const {
  CheckResult result;

  if (ctx.iso_path.empty() || !fs::exists(ctx.iso_path)) {
    result.ok = false;
    result.missing.push_back("Input ISO file: " + ctx.iso_path.string());
    result.message = "Input ISO not found.";
    return result;
  }

  if (!is_xbox_iso(ctx.iso_path)) {
    result.ok = false;
    result.message =
        "This does not appear to be an Xbox 360 ISO "
        "(no XDVDFS volume descriptor found at known offsets).";
    result.missing.push_back("Valid Xbox 360 (XGD2) ISO");
    return result;
  }
  // extract-xiso is optional — the built-in XDVDFS reader is the primary method.
  fs::path xiso = resolve_extract_xiso(ctx);
  if (!xiso.empty() && !fs::exists(xiso)) {
    // Don't fail — the built-in reader can handle this
    result.message = "Note: extract-xiso not found; will use built-in XDVDFS reader.";
  }

  if (ctx.output_dir.empty()) {
    result.ok = false;
    result.missing.push_back("Workspace output directory");
    result.message = "Workspace root not configured.";
    return result;
  }

  std::error_code ec;
  auto space = fs::space(ctx.output_dir, ec);
  if (!ec && space.available < static_cast<std::uintmax_t>(8ull * 1024 * 1024 * 1024)) {
    result.ok = false;
    result.message = "Insufficient disk space (need >= 8GB free for extraction).";
    result.missing.push_back("8GB free disk space");
    return result;
  }

  result.ok = true;
  return result;
}

StageResult IsoExtractorStage::run(PipelineContext& ctx, ProgressCallback progress) {
  fs::path iso = ctx.iso_path;
  fs::path out = ctx.recomp_dir() / "extracted";

  std::error_code ec;
  if (fs::exists(out, ec)) {
    progress(0.05f, "Removing previous extraction directory...");
    fs::remove_all(out, ec);
  }
  fs::create_directories(out, ec);

  if (is_xgd3(iso)) {
    progress(0.0f, "Note: XGD3 detected — extraction may take longer for larger discs.");
  }

  // Try built-in XDVDFS reader first (no external dependency needed)
  progress(0.1f, "Extracting ISO (built-in XDVDFS reader)...");
  std::string iso_error;
  auto iso_progress = [&progress](uint64_t copied, uint64_t total) {
    if (total > 0) {
      float frac = 0.1f + 0.7f * static_cast<float>(copied) / static_cast<float>(total);
      progress(frac, "");
    }
  };

  if (recomp::iso::ExtractIso(iso, out, iso_progress, iso_error)) {
    // Verify default.xex
    fs::path xex = out / "default.xex";
    if (fs::exists(xex, ec)) {
      ctx.extracted_dir = out;
      progress(1.0f, "ISO extracted to " + out.string());
      return StageResult::ok("Extracted ISO to " + out.string());
    }
  }

  // Built-in reader failed — fall back to extract-xiso
  progress(0.1f, "Built-in reader: " + iso_error + ". Trying extract-xiso...");
  fs::path xiso = resolve_extract_xiso(ctx);
  if (xiso.empty()) {
    return StageResult::fail(
        "Built-in XDVDFS reader failed (" + iso_error +
        ") and extract-xiso is not available.");
  }

  std::vector<std::string> args = {"-x", iso.string()};

  recomp::ProgressCallback on_line =
      [&progress](float, const std::string& line) {
        progress(0.5f, line);
      };

  auto pr = recomp::ProcessRunner::run(xiso, args, out, {}, on_line);

  if (pr.exit_code != 0 && pr.exit_code != 1) {
    return StageResult::fail(
        "extract-xiso failed (exit " + std::to_string(pr.exit_code) + ").",
        pr.exit_code);
  }

  progress(0.9f, "Verifying default.xex...");

  // extract-xiso creates a subfolder named after the ISO inside the output
  // directory. Search for default.xex in both the direct output and any
  // immediate subdirectory.
  fs::path xex = out / "default.xex";
  if (!fs::exists(xex, ec)) {
    for (auto& entry : fs::directory_iterator(out, ec)) {
      if (entry.is_directory()) {
        fs::path sub_xex = entry.path() / "default.xex";
        if (fs::exists(sub_xex, ec)) {
          xex = sub_xex;
          out = entry.path();
          break;
        }
      }
    }
  }
  if (!fs::exists(xex, ec)) {
    return StageResult::fail(
        "Extraction completed but default.xex is missing in " + out.string());
  }

  ctx.extracted_dir = out;

  progress(1.0f, "ISO extracted to " + out.string());
  return StageResult::ok("Extracted ISO to " + out.string());
}

bool IsoExtractorStage::is_complete(const PipelineContext& ctx) const {
  if (ctx.extracted_dir.empty()) return false;
  std::error_code ec;
  return fs::exists(ctx.extracted_dir / "default.xex", ec);
}

}  // namespace recomp
