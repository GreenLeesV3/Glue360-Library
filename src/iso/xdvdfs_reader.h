// xdvdfs_reader.h — Built-in Xbox 360 XDVDFS ISO reader
//
// Replaces the extract-xiso external dependency. Reads the ISO directly,
// parses the XDVDFS directory tree, and extracts all files to a target directory.
//
// Based on the XDVDFS format used by Xbox 360 game discs (XGD2/XGD3):
//   - Magic: "MICROSOFT*XBOX*MEDIA" at offset (game_offset + 32 * sector_size)
//   - Directory entries are a binary tree with left/right subtree offsets
//   - Each entry: 14-byte header (left, right, sector, length, attributes, name_length)
//     followed by the name string
//
// Inspired by Skate3Recomp's XboxIsoReader (skate3_iso_installer.cpp).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace recomp::iso {

// A file entry in the ISO directory tree
struct IsoEntry {
  std::string path;    // Relative path within the ISO (e.g. "default.xex", "packs/data.xepack")
  uint64_t offset = 0; // Absolute byte offset in the ISO file
  uint64_t size = 0;   // File size in bytes
};

// Progress callback: (copied_bytes, total_bytes)
using ProgressCallback = std::function<void(uint64_t copied, uint64_t total)>;

// Built-in XDVDFS ISO reader — no external dependencies
class XdvdfsReader {
 public:
  // Open an ISO file and parse its directory tree
  // Returns false with an error message on failure
  bool Open(const std::filesystem::path& iso_path, std::string& error);

  // Check if a specific file exists in the ISO
  bool HasFile(const std::string& path) const;

  // Get total size of all files in the ISO
  uint64_t TotalSize() const;

  // Get all file entries
  const std::vector<IsoEntry>& Entries() const { return entries_; }

  // Extract all files to a target directory
  // Progress is reported via the optional callback
  bool ExtractAll(const std::filesystem::path& target_root,
                  ProgressCallback progress,
                  std::string& error);

 private:
  bool ReadAt(uint64_t offset, void* data, size_t size);
  bool ParseDirectory(const std::string& prefix, uint64_t game_offset,
                      uint64_t directory_offset, std::string& error);

  std::filesystem::path iso_path_;
  std::ifstream file_;
  uint64_t file_size_ = 0;
  std::vector<IsoEntry> entries_;
};

// Convenience: check if a path looks like an Xbox 360 ISO
bool IsXbox360Iso(const std::filesystem::path& path);

// Convenience: extract an ISO to a directory with progress reporting
// Returns false on error
bool ExtractIso(const std::filesystem::path& iso_path,
                const std::filesystem::path& target_dir,
                ProgressCallback progress,
                std::string& error);

}  // namespace recomp::iso
