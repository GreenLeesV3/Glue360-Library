// xdvdfs_reader.cpp — Built-in Xbox 360 XDVDFS ISO reader implementation
//
// Reads XDVDFS (Xbox Disc Video File System) format used by Xbox 360 game
// discs. Replaces the extract-xiso external tool dependency.
//
// Format reference:
//   - Sector size: 2048 bytes
//   - Magic: "MICROSOFT*XBOX*MEDIA" at game_offset + 32 * sector_size
//   - Game offset: one of {0, 0xFB20, 0x20600, 0x2080000, 0xFD90000}
//     (XGD2 has game partition at 0x20800, XGD3 at 0xFD90000)
//   - Root directory: 8-byte record at magic_offset + 20
//     (sector_number: u32le, size: u32le)
//   - Directory entries: 14-byte header + name string, binary tree layout

#include "iso/xdvdfs_reader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <system_error>

namespace recomp::iso {

namespace {

constexpr uint64_t kSectorSize = 2048;
constexpr std::array<uint64_t, 5> kPossibleGameOffsets = {
    0x00000000ull, 0x0000FB20ull, 0x00020600ull, 0x02080000ull, 0x0FD90000ull};
constexpr std::string_view kXdvdfsMagic = "MICROSOFT*XBOX*MEDIA";
constexpr std::string_view kDefaultXex = "default.xex";

uint16_t ReadLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Prevent path traversal — reject absolute paths, ".", ".."
bool IsUnsafePath(std::string_view path) {
  if (path.empty() || path.starts_with('/') || path.starts_with('\\')) {
    return true;
  }
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string_view::npos) end = path.size();
    auto component = path.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") return true;
    if (end == path.size()) break;
    start = end + 1;
  }
  return false;
}

}  // namespace

bool XdvdfsReader::Open(const std::filesystem::path& iso_path, std::string& error) {
  iso_path_ = iso_path;
  file_.open(iso_path, std::ios::binary);
  if (!file_) {
    error = "Unable to open the ISO file: " + iso_path.string();
    return false;
  }

  file_.seekg(0, std::ios::end);
  file_size_ = static_cast<uint64_t>(file_.tellg());
  file_.seekg(0, std::ios::beg);

  // Find the XDVDFS magic at one of the known game offsets
  uint64_t game_offset = 0;
  bool found_magic = false;
  std::array<char, 20> magic{};
  for (uint64_t candidate : kPossibleGameOffsets) {
    const uint64_t magic_offset = candidate + 32 * kSectorSize;
    if (magic_offset + magic.size() > file_size_) continue;
    file_.seekg(static_cast<std::streamoff>(magic_offset), std::ios::beg);
    file_.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (std::string_view(magic.data(), magic.size()) == kXdvdfsMagic) {
      game_offset = candidate;
      found_magic = true;
      break;
    }
  }

  if (!found_magic) {
    error = "Not a recognized Xbox 360 game ISO (XDVDFS magic not found).";
    return false;
  }

  // Read root directory info
  std::array<uint8_t, 8> root_info{};
  const uint64_t root_info_offset = game_offset + 32 * kSectorSize + 20;
  if (!ReadAt(root_info_offset, root_info.data(), root_info.size())) {
    error = "Failed to read the ISO root directory.";
    return false;
  }

  const uint32_t root_sector = ReadLe32(root_info.data());
  const uint32_t root_size = ReadLe32(root_info.data() + 4);
  if (root_size < 13 || root_size > 32 * 1024 * 1024) {
    error = "Invalid ISO root directory size.";
    return false;
  }

  entries_.clear();
  if (!ParseDirectory("", game_offset, game_offset + uint64_t(root_sector) * kSectorSize, error)) {
    return false;
  }

  if (!HasFile(std::string(kDefaultXex))) {
    error = "The ISO does not contain default.xex.";
    return false;
  }

  return true;
}

bool XdvdfsReader::HasFile(const std::string& path) const {
  const auto wanted = ToLower(path);
  return std::any_of(entries_.begin(), entries_.end(), [&](const IsoEntry& entry) {
    return ToLower(entry.path) == wanted;
  });
}

uint64_t XdvdfsReader::TotalSize() const {
  uint64_t total = 0;
  for (const auto& entry : entries_) {
    total += entry.size;
  }
  return total;
}

bool XdvdfsReader::ExtractAll(const std::filesystem::path& target_root,
                               ProgressCallback progress, std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(target_root, ec);
  if (ec) {
    error = "Unable to create the extraction directory: " + target_root.string();
    return false;
  }

  const uint64_t total = TotalSize();
  uint64_t copied = 0;
  std::vector<uint8_t> buffer(4 * 1024 * 1024);  // 4MB buffer

  for (const auto& entry : entries_) {
    if (IsUnsafePath(entry.path)) {
      error = "The ISO contains an unsafe file path: " + entry.path;
      return false;
    }

    const auto target = target_root / std::filesystem::path(entry.path);
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
      error = "Unable to create subdirectory: " + target.parent_path().string();
      return false;
    }

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "Unable to create file: " + entry.path;
      return false;
    }

    uint64_t remaining = entry.size;
    uint64_t read_offset = entry.offset;
    while (remaining > 0) {
      const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
      if (!ReadAt(read_offset, buffer.data(), chunk)) {
        error = "Failed to read from ISO: " + entry.path;
        return false;
      }
      out.write(reinterpret_cast<const char*>(buffer.data()),
                static_cast<std::streamsize>(chunk));
      if (!out) {
        error = "Failed to write file: " + entry.path;
        return false;
      }
      remaining -= chunk;
      read_offset += chunk;
      copied += chunk;
      if (progress) progress(copied, total);
    }
  }

  return true;
}

bool XdvdfsReader::ReadAt(uint64_t offset, void* data, size_t size) {
  if (offset + size > file_size_) return false;
  file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  file_.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
  return file_.good();
}

bool XdvdfsReader::ParseDirectory(const std::string& prefix, uint64_t game_offset,
                                   uint64_t directory_offset, std::string& error) {
  struct PendingNode {
    uint64_t directory_offset = 0;
    uint32_t node_offset = 0;
    std::string prefix;
  };

  std::vector<PendingNode> pending;
  pending.push_back({directory_offset, 0, prefix});
  std::array<uint8_t, 14> header{};
  size_t visited = 0;

  while (!pending.empty()) {
    auto node = std::move(pending.back());
    pending.pop_back();
    if (++visited > 500000) {
      error = "ISO directory tree is unexpectedly large.";
      return false;
    }

    const uint64_t entry_offset = node.directory_offset + node.node_offset;
    if (!ReadAt(entry_offset, header.data(), header.size())) {
      error = "Failed to read ISO directory entry.";
      return false;
    }

    const uint16_t left = ReadLe16(header.data());
    const uint16_t right = ReadLe16(header.data() + 2);
    const uint32_t sector = ReadLe32(header.data() + 4);
    const uint32_t length = ReadLe32(header.data() + 8);
    const uint8_t attributes = header[12];
    const uint8_t name_length = header[13];
    if (name_length == 0 || name_length > 240) {
      error = "Invalid ISO directory entry name length.";
      return false;
    }

    std::string name(name_length, '\0');
    if (!ReadAt(entry_offset + header.size(), name.data(), name.size())) {
      error = "Failed to read ISO directory entry name.";
      return false;
    }

    if (left) {
      pending.push_back({node.directory_offset, static_cast<uint32_t>(left) * 4u, node.prefix});
    }
    if (right) {
      pending.push_back({node.directory_offset, static_cast<uint32_t>(right) * 4u, node.prefix});
    }

    const bool is_directory = (attributes & 0x10) != 0;
    const std::string full_path = node.prefix + name;
    if (is_directory) {
      if (length != 0) {
        pending.push_back({game_offset + uint64_t(sector) * kSectorSize, 0, full_path + "/"});
      }
    } else {
      entries_.push_back({full_path, game_offset + uint64_t(sector) * kSectorSize, length});
    }
  }

  return true;
}

bool IsXbox360Iso(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;

  std::array<char, 20> magic{};
  for (uint64_t candidate : kPossibleGameOffsets) {
    const uint64_t magic_offset = candidate + 32 * kSectorSize;
    file.seekg(static_cast<std::streamoff>(magic_offset), std::ios::beg);
    file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (file && std::string_view(magic.data(), magic.size()) == kXdvdfsMagic) {
      return true;
    }
    file.clear();
  }
  return false;
}

bool ExtractIso(const std::filesystem::path& iso_path,
                const std::filesystem::path& target_dir,
                ProgressCallback progress,
                std::string& error) {
  XdvdfsReader reader;
  if (!reader.Open(iso_path, error)) {
    return false;
  }
  if (!reader.ExtractAll(target_dir, progress, error)) {
    return false;
  }
  return true;
}

}  // namespace recomp::iso
