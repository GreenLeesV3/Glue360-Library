// stfs_reader.cpp — Built-in STFS package reader implementation
//
// Reads Xbox 360 STFS (Secure Transacted File System) content packages.
// Supports CON, LIVE, and PIRS container formats.
//
// Format overview:
//   - Header: 0x400 bytes (certificate, metadata)
//   - Metadata: at offset 0x344, contains volume descriptor
//   - Volume descriptor: at metadata_offset + 0x35, contains file table info
//   - File table: chain of 0x1000-byte blocks, 64 entries per block
//   - Data blocks: 0x1000 bytes each, chained via hash tables
//   - Hash tables: interspersed with data blocks (every 170 blocks)
//
// Based on the STFS format reverse-engineering by Xbox 360 modding community.
// Inspired by Skate3Recomp's StfsPackageReader.

#include "iso/stfs_reader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace recomp::stfs {

namespace {

uint16_t Be16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint16_t Le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t U24Le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}

}  // namespace

bool StfsReader::LooksLikeStfs(const std::vector<uint8_t>& data) {
  if (data.size() < 4) return false;
  const std::string_view magic(reinterpret_cast<const char*>(data.data()), 4);
  return magic == "CON " || magic == "LIVE" || magic == "PIRS";
}

bool StfsReader::Open(std::vector<uint8_t> data, std::string& error) {
  data_ = std::move(data);
  if (data_.size() < 0x400 || !LooksLikeStfs(data_)) {
    error = "The file is not an Xbox 360 content package (CON/LIVE/PIRS).";
    return false;
  }
  header_size_ = Be32(At(0x340, 4));
  metadata_offset_ = 0x344;
  volume_descriptor_offset_ = metadata_offset_ + 0x35;
  const uint32_t volume_type = Be32(At(metadata_offset_ + 0x65, 4));
  if (volume_type != 0) {
    error = "The content package is not an STFS volume.";
    return false;
  }
  const uint8_t flags = *At(volume_descriptor_offset_ + 2, 1);
  blocks_per_hash_table_ = (flags & 1) ? 1 : 2;
  return true;
}

bool StfsReader::ListEntries(std::vector<StfsEntry>& entries, std::string& error) {
  entries.clear();
  const uint8_t* descriptor = At(volume_descriptor_offset_, 8);
  if (!descriptor) {
    error = "The content package header is truncated.";
    return false;
  }
  uint16_t table_block_count = Le16(descriptor + 3);
  uint32_t table_block = U24Le(descriptor + 5);

  for (uint32_t table = 0; table < table_block_count; ++table) {
    const uint64_t table_offset = BlockToOffset(table_block);
    for (uint32_t index = 0; index < 0x40; ++index) {
      const uint8_t* entry = At(table_offset + index * 0x40, 0x40);
      if (!entry) {
        error = "The content package file table is truncated.";
        return false;
      }
      if (entry[0] == 0) break;

      const uint8_t flags = entry[40];
      const uint32_t name_length = flags & 0x3F;
      if (name_length == 0 || name_length > 40) {
        error = "The content package contains an invalid file name.";
        return false;
      }

      StfsEntry parsed;
      std::string name(reinterpret_cast<const char*>(entry), name_length);
      parsed.is_dir = (flags & 0x80) != 0;
      parsed.start_block = U24Le(entry + 47);
      const uint16_t parent_index = Be16(entry + 50);
      parsed.length = Be32(entry + 52);

      std::string parent_path;
      if (parent_index != 0xFFFF) {
        if (parent_index >= entries.size()) {
          error = "The content package directory tree is invalid.";
          return false;
        }
        parent_path = entries[parent_index].path;
      }
      parsed.path = parent_path.empty() ? name : parent_path + "/" + name;
      entries.push_back(std::move(parsed));
    }

    const uint32_t next = NextBlock(table_block);
    if (next == kEndOfChain) break;
    table_block = next;
  }
  return true;
}

bool StfsReader::ReadFile(const StfsEntry& entry, std::vector<uint8_t>& out,
                          std::string& error) {
  out.clear();
  out.reserve(entry.length);
  uint32_t block_index = entry.start_block;
  uint64_t remaining = entry.length;
  while (remaining > 0 && block_index != kEndOfChain) {
    const uint64_t chunk = std::min<uint64_t>(kBlockSize, remaining);
    const uint8_t* block = At(BlockToOffset(block_index), chunk);
    if (!block) {
      error = "The content package data is truncated.";
      return false;
    }
    out.insert(out.end(), block, block + chunk);
    remaining -= chunk;
    block_index = NextBlock(block_index);
  }
  if (remaining > 0) {
    error = "The content package block chain ended unexpectedly.";
    return false;
  }
  return true;
}

const uint8_t* StfsReader::At(uint64_t offset, uint64_t size) const {
  if (offset + size > data_.size()) return nullptr;
  return data_.data() + offset;
}

uint64_t StfsReader::RoundUp(uint64_t value, uint64_t alignment) const {
  return (value + alignment - 1) & ~(alignment - 1);
}

uint64_t StfsReader::BlockToOffset(uint32_t block_index) const {
  uint64_t block = block_index;
  uint64_t base = 170;
  for (int i = 0; i < 3; ++i) {
    block += ((block_index + base) / base) * blocks_per_hash_table_;
    if (block_index < base) break;
    base *= 170;
  }
  return RoundUp(header_size_, kBlockSize) + (block << 12);
}

uint32_t StfsReader::HashBlockNumber(uint32_t block_index, int hash_level) const {
  const uint32_t block_step0 = 170 + blocks_per_hash_table_;
  const uint32_t block_step1 = 28900 + (170 + 1) * blocks_per_hash_table_;
  if (hash_level == 0) {
    if (block_index < 170) return 0;
    uint32_t block = (block_index / 170) * block_step0;
    block += ((block_index / 28900) + 1) * blocks_per_hash_table_;
    if (block_index < 28900) return block;
    return block + blocks_per_hash_table_;
  }
  if (hash_level == 1) {
    if (block_index < 28900) return block_step0;
    const uint32_t block = (block_index / 28900) * block_step1;
    return block + blocks_per_hash_table_;
  }
  return block_step1;
}

uint64_t StfsReader::HashOffset(uint32_t block_index, int hash_level) const {
  return RoundUp(header_size_, kBlockSize) +
         (static_cast<uint64_t>(HashBlockNumber(block_index, hash_level)) << 12);
}

uint32_t StfsReader::NextBlock(uint32_t block_index) const {
  const uint64_t hash_offset = HashOffset(block_index, 0);
  const uint8_t* hash_entry = At(hash_offset + (block_index % 170) * 0x18, 0x18);
  if (!hash_entry) return kEndOfChain;
  return Be32(hash_entry + 0x14) & 0xFFFFFF;
}

// Convenience functions

bool ExtractFileFromPackage(const std::filesystem::path& package_path,
                            const std::string& file_name,
                            std::vector<uint8_t>& out,
                            std::string& error) {
  std::ifstream file(package_path, std::ios::binary);
  if (!file) {
    error = "Unable to open package: " + package_path.string();
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

  StfsReader reader;
  if (!reader.Open(std::move(data), error)) return false;

  std::vector<StfsEntry> entries;
  if (!reader.ListEntries(entries, error)) return false;

  for (const auto& entry : entries) {
    if (entry.is_dir) continue;
    // Match by filename (last path component)
    auto pos = entry.path.find_last_of('/');
    std::string name = (pos != std::string::npos) ? entry.path.substr(pos + 1) : entry.path;
    if (name == file_name) {
      return reader.ReadFile(entry, out, error);
    }
  }

  error = "File not found in package: " + file_name;
  return false;
}

bool ListPackageFiles(const std::filesystem::path& package_path,
                      std::vector<StfsEntry>& entries,
                      std::string& error) {
  std::ifstream file(package_path, std::ios::binary);
  if (!file) {
    error = "Unable to open package: " + package_path.string();
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());

  StfsReader reader;
  if (!reader.Open(std::move(data), error)) return false;
  return reader.ListEntries(entries, error);
}

}  // namespace recomp::stfs
