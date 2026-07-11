// gui_launcher.h — Win32 GUI launcher for the recompiler tool
//
// Provides a lightweight Win32 dialog-based GUI for when the user runs
// the tool without command-line arguments. Shows:
//   1. ISO file picker dialog
//   2. Profile selection dropdown
//   3. Output directory picker
//   4. Progress bar during extraction + codegen + build
//
// This is NOT a full GUI — it's a simple wizard that collects the same
// information as the CLI args and then runs the existing pipeline.
// The tool remains CLI-first; the GUI is a fallback for end users.

#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace recomp::gui {

// Launch the GUI wizard and return the selected parameters
struct GuiResult {
  bool ok = false;                    // True if user clicked "Start"
  std::filesystem::path iso_path;     // Selected ISO file
  std::filesystem::path output_dir;   // Output directory
  std::string profile_name;           // Selected profile
  std::string sdk_path;               // SDK root (optional, auto-discovered if empty)
};

// Show the GUI wizard (Win32 dialog on Windows)
// Blocks until the user clicks Start or Cancel
GuiResult ShowLauncherWizard();

// Show a completion message box with the game exe path (or error).
// Called by main.cpp after the pipeline finishes in GUI mode.
void ShowResult(bool success, const std::string& deploy_path,
                const std::string& error_msg);


}  // namespace recomp::gui