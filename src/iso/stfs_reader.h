// stfs_reader.h — Built-in STFS (CON/LIVE/PIRS) package reader
//
// Reads Xbox 360 content packages (title updates, DLC, saves) in the
// STFS (Secure Transacted File System) format. Extracts individual files
// from the package's block-chain storage.
//
// Container types:
//   CON  — Created on console (signed with console key)
//   LIVE — Marketplace package (signed with Microsoft key)
//   PIRS — Piracy-protected package (signed with Microsoft key)
//
// Based on the STFS format used by Xbox 360 title updates.
// Inspired by Skate3Recomp's StfsPackageReader (skate3_title_update_installer.cpp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace recomp::stfs {

// A file entry in the STFS package
struct StfsEntry {
  std::string path;        // Full path within the package (e.g. "default.xexp")
  bool is_dir = false;     // True if this is a directory
  uint32_t start_block = 0; // First block in the block chain
  uint32_t length = 0;     // File size in bytes
};

// Built-in STFS package reader — reads CON/LIVE/PIRS containers
class StfsReader {
 public:
  static constexpr uint32_t kBlockSize = 0x1000;  // 4096 bytes
  static constexpr uint32_t kEndOfChain = 0xFFFFFF;

  // Check if data looks like an STFS package
  static bool LooksLikeStfs(const std::vector<uint8_t>& data);

  // Open a package from raw data (read the entire file into memory first)
  bool Open(std::vector<uint8_t> data, std::string& error);

  // List all file entries in the package
  bool ListEntries(std::vector<StfsEntry>& entries, std::string& error);

  // Read a specific file from the package
  bool ReadFile(const StfsEntry& entry, std::vector<uint8_t>& out, std::string& error);

 private:
  const uint8_t* At(uint64_t offset, uint64_t size) const;
  uint64_t RoundUp(uint64_t value, uint64_t alignment) const;
  uint64_t BlockToOffset(uint32_t block_index) const;
  uint32_t HashBlockNumber(uint32_t block_index, int hash_level) const;
  uint64_t HashOffset(uint32_t block_index, int hash_level) const;
  uint32_t NextBlock(uint32_t block_index) const;

  std::vector<uint8_t> data_;
  uint32_t header_size_ = 0;
  uint32_t metadata_offset_ = 0;
  uint32_t volume_descriptor_offset_ = 0;
  uint32_t blocks_per_hash_table_ = 2;
};

// Convenience: extract a specific file from an STFS package on disk
bool ExtractFileFromPackage(const std::filesystem::path& package_path,
                            const std::string& file_name,
                            std::vector<uint8_t>& out,
                            std::string& error);

// Convenience: list all files in an STFS package on disk
bool ListPackageFiles(const std::filesystem::path& package_path,
                      std::vector<StfsEntry>& entries,
                      std::string& error);

}  // namespace recomp::stfs
