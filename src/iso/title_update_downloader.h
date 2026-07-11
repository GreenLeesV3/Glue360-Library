// title_update_downloader.h — Title update downloader for Xbox 360 games
//
// Downloads title update packages (TU) from xboxunity.net using the title ID.
// Uses WinHTTP on Windows for the download. Extracts .xexp files from STFS
// packages using the built-in StfsReader.
//
// Inspired by Skate3Recomp's title update system (skate3_title_update_installer.cpp).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace recomp::tu {

// Progress callback: (downloaded_bytes, total_bytes)
using ProgressCallback = std::function<void(uint64_t downloaded, uint64_t total)>;

// Title update payload descriptor
struct TitleUpdatePayload {
  std::string container_path;  // Path inside the STFS package (e.g. "default.xexp")
  std::string staged_path;     // Path relative to game root
  uint64_t size = 0;           // Expected file size (0 = unknown)
  std::string sha256;          // Expected SHA-256 hash (empty = skip verification)
};

// Download a title update package from a URL
// Uses WinHTTP on Windows. Saves to the destination path.
// Returns false on error.
bool DownloadPackage(const std::string& url,
                     const std::filesystem::path& destination,
                     ProgressCallback progress,
                     std::string& error);

// Extract and stage title update payloads from a downloaded STFS package
// Extracts files listed in payloads and places them in game_root
// Returns false on error.
bool StagePayloads(const std::filesystem::path& package_path,
                   const std::filesystem::path& game_root,
                   const std::vector<TitleUpdatePayload>& payloads,
                   std::string& error);

// Check if a title update is already installed
// Returns true if all payload files exist in game_root
bool IsTitleUpdateInstalled(const std::filesystem::path& game_root,
                            const std::vector<TitleUpdatePayload>& payloads);

// Full workflow: download + extract + stage
// 1. Download the package from the URL
// 2. Extract the payloads from the STFS container
// 3. Stage them in the game root
// Returns false on error.
bool InstallTitleUpdate(const std::string& url,
                        const std::filesystem::path& game_root,
                        const std::vector<TitleUpdatePayload>& payloads,
                        ProgressCallback download_progress,
                        std::string& error);

}  // namespace recomp::tu
