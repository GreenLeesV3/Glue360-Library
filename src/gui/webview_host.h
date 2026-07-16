// gui/webview_host.h — WebView2-based GUI shell (Glue360 Deck).
//
// Hosts the React single-page UI (embedded as an RCDATA resource) inside a
// WebView2 window and bridges it to the pipeline via postMessage JSON-RPC.
// Falls back gracefully: returns false when the WebView2 runtime is missing
// so main() can drop back to the legacy Win32 dialog wizard.
#pragma once

namespace recomp::gui {

/// Runs the WebView2 GUI to completion (window closed).
/// Returns false if the WebView2 runtime/loader is unavailable — the caller
/// should fall back to the legacy launcher.
bool RunWebViewGui();

} // namespace recomp::gui
