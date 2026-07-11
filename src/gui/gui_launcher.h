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

// Show a progress dialog during a long-running operation
// Uses a Win32 progress bar dialog
class ProgressDialog {
 public:
  // Create and show the progress dialog
  // title: window title, message: status text
  ProgressDialog(const std::string& title, const std::string& message);

  // Update progress (0.0 to 1.0) and optional status message
  void Update(float fraction, const std::string& message);

  // Check if the user clicked Cancel
  bool WasCancelled() const;

  // Close the dialog
  void Close();

 private:
#ifdef _WIN32
  void* hwnd_ = nullptr;  // HWND
  bool cancelled_ = false;
#endif
};

}  // namespace recomp::gui
