// gui_launcher.cpp — Win32 GUI launcher implementation.
//
// Provides a wizard dialog for end users who run the tool without
// command-line arguments. Collects ISO path, profile, output directory,
// then runs the existing pipeline and shows the result.

#include "gui/gui_launcher.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
// shlobj.h needs full COM headers — WIN32_LEAN_AND_MEAN excludes them.
#ifdef WIN32_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <shlobj.h>
#endif

namespace recomp::gui {

#ifdef _WIN32

namespace {

namespace fs = std::filesystem;

// ── File / folder pickers ────────────────────────────────────────────────

fs::path PickIsoFile(HWND owner) {
  wchar_t filename[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = filename;
  ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
  ofn.lpstrFilter = L"Xbox 360 ISO (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
  ofn.lpstrTitle = L"Select Xbox 360 Game ISO";
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
              OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
  if (!GetOpenFileNameW(&ofn)) return {};
  return filename;
}

fs::path PickFolder(HWND owner, const wchar_t* title) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  bi.lpszTitle = title;
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (!pidl) return {};
  wchar_t path[MAX_PATH];
  if (!SHGetPathFromIDListW(pidl, path)) {
    CoTaskMemFree(pidl);
    return {};
  }
  CoTaskMemFree(pidl);
  return path;
}

// ── Helpers ──────────────────────────────────────────────────────────────

fs::path GetAppDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return fs::path(path).parent_path();
}

std::vector<std::string> FindProfiles(const fs::path& app_dir) {
  std::vector<std::string> profiles;
  auto profiles_dir = app_dir / "profiles";
  if (!fs::is_directory(profiles_dir)) return profiles;
  for (const auto& entry : fs::directory_iterator(profiles_dir)) {
    if (entry.is_directory()) {
      if (fs::exists(entry.path() / "profile.toml")) {
        profiles.push_back(entry.path().filename().string());
      }
    }
  }
  std::sort(profiles.begin(), profiles.end());
  return profiles;
}

std::wstring Widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());
}

std::string Narrow(const std::wstring& w) {
  return std::string(w.begin(), w.end());
}

// ── Wizard dialog ────────────────────────────────────────────────────────
//
// Control IDs
enum {
  IDC_ISO_LABEL   = 1000,
  IDC_ISO_EDIT    = 1001,
  IDC_ISO_BROWSE  = 1002,
  IDC_PROFILE_LABEL = 1003,
  IDC_PROFILE_COMBO = 1004,
  IDC_OUTPUT_LABEL  = 1005,
  IDC_OUTPUT_EDIT   = 1006,
  IDC_OUTPUT_BROWSE = 1007,
  IDC_START       = IDOK,
  IDC_CANCEL      = IDCANCEL,
};

struct WizardState {
  fs::path iso_path;
  fs::path output_dir;
  std::string profile_name;
  fs::path sdk_path;
  std::vector<std::string> profiles;
  bool ok = false;
};

void LayoutControls(HWND hwnd) {
  // Layout:
  //  Row 1: "ISO file:"   [edit............................] [Browse]
  //  Row 2: "Profile:"     [combo............................]
  //  Row 3: "Output dir:"  [edit............................] [Browse]
  //  Row 4:              [Start]  [Cancel]

  static const int kMargin = 12;
  static const int kLabelW = 70;
  static const int kBrowseW = 80;
  static const int kRowH = 16;
  static const int kEditH = 22;
  static const int kGap = 8;
  static const int kRowGap = 14;

  RECT rc;
  GetClientRect(hwnd, &rc);
  int width = rc.right - rc.left;
  int y = kMargin;

  int edit_w = width - kMargin * 2 - kLabelW - kBrowseW - kGap * 2;

  // Row 1: ISO
  HWND lbl = CreateWindowW(L"STATIC", L"ISO file:", SS_LEFT, kMargin, y + 2,
                           kLabelW, kRowH, hwnd, (HMENU)IDC_ISO_LABEL, nullptr, nullptr);
  HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              kMargin + kLabelW + kGap, y, edit_w, kEditH,
                              hwnd, (HMENU)IDC_ISO_EDIT, nullptr, nullptr);
  HWND btn = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
                           kMargin + kLabelW + kGap + edit_w + kGap, y, kBrowseW, kEditH,
                           hwnd, (HMENU)IDC_ISO_BROWSE, nullptr, nullptr);
  y += kEditH + kRowGap;

  // Row 2: Profile combo
  lbl = CreateWindowW(L"STATIC", L"Profile:", SS_LEFT, kMargin, y + 2,
                      kLabelW, kRowH, hwnd, (HMENU)IDC_PROFILE_LABEL, nullptr, nullptr);
  HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                               kMargin + kLabelW + kGap, y, edit_w + kGap + kBrowseW, 200,
                               hwnd, (HMENU)IDC_PROFILE_COMBO, nullptr, nullptr);
  y += kEditH + kRowGap;

  // Row 3: Output dir
  lbl = CreateWindowW(L"STATIC", L"Output dir:", SS_LEFT, kMargin, y + 2,
                      kLabelW, kRowH, hwnd, (HMENU)IDC_OUTPUT_LABEL, nullptr, nullptr);
  edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                         kMargin + kLabelW + kGap, y, edit_w, kEditH,
                         hwnd, (HMENU)IDC_OUTPUT_EDIT, nullptr, nullptr);
  btn = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
                      kMargin + kLabelW + kGap + edit_w + kGap, y, kBrowseW, kEditH,
                      hwnd, (HMENU)IDC_OUTPUT_BROWSE, nullptr, nullptr);
  y += kEditH + kRowGap + 4;

  // Row 4: Start / Cancel
  int btn_w = 90;
  int btn_gap = 10;
  int btn_x = width - kMargin - btn_w * 2 - btn_gap;
  btn = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      btn_x, y, btn_w, kEditH + 4, hwnd, (HMENU)IDC_START, nullptr, nullptr);
  btn = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                      btn_x + btn_w + btn_gap, y, btn_w, kEditH + 4,
                      hwnd, (HMENU)IDC_CANCEL, nullptr, nullptr);

  // Set a nicer font on all children
  HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
    SendMessageW(child, WM_SETFONT, (WPARAM)lp, MAKELPARAM(TRUE, 0));
    return TRUE;
  }, (LPARAM)hFont);
}

INT_PTR CALLBACK WizardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  static WizardState* state = nullptr;

  switch (msg) {
    case WM_CREATE: {
      state = reinterpret_cast<WizardState*>(
          reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);

      SetWindowTextW(hwnd, L"Glue360 Library - Xbox 360 Recompiler");

      LayoutControls(hwnd);

      // Populate profile combo
      HWND combo = GetDlgItem(hwnd, IDC_PROFILE_COMBO);
      for (const auto& profile : state->profiles) {
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(Widen(profile).c_str()));
      }
      if (!state->profiles.empty()) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
      }

      // Default output dir
      SetDlgItemTextW(hwnd, IDC_OUTPUT_EDIT, L"C:\\Games\\Recomp");
      return 0;
    }

    case WM_COMMAND: {
      switch (LOWORD(wParam)) {
        case IDC_ISO_BROWSE: {
          auto iso = PickIsoFile(hwnd);
          if (!iso.empty()) {
            SetDlgItemTextW(hwnd, IDC_ISO_EDIT, iso.wstring().c_str());
          }
          break;
        }
        case IDC_OUTPUT_BROWSE: {
          auto dir = PickFolder(hwnd, L"Select output directory");
          if (!dir.empty()) {
            SetDlgItemTextW(hwnd, IDC_OUTPUT_EDIT, dir.wstring().c_str());
          }
          break;
        }
        case IDC_START: {
          // Read ISO
          wchar_t iso_buf[MAX_PATH] = {};
          GetDlgItemTextW(hwnd, IDC_ISO_EDIT, iso_buf, MAX_PATH);
          state->iso_path = iso_buf;

          // Read output
          wchar_t out_buf[MAX_PATH] = {};
          GetDlgItemTextW(hwnd, IDC_OUTPUT_EDIT, out_buf, MAX_PATH);
          state->output_dir = out_buf;

          // Read profile
          HWND combo = GetDlgItem(hwnd, IDC_PROFILE_COMBO);
          int sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
          if (sel >= 0) {
            int len = SendMessageW(combo, CB_GETLBTEXTLEN, sel, 0);
            std::wstring wide(len, L'\0');
            SendMessageW(combo, CB_GETLBTEXT, sel,
                         reinterpret_cast<LPARAM>(wide.data()));
            state->profile_name = Narrow(wide);
          }

          // Validate
          if (state->iso_path.empty()) {
            MessageBoxW(hwnd, L"Please select an ISO file.", L"Error", MB_ICONERROR);
            return 0;
          }
          if (state->output_dir.empty()) {
            MessageBoxW(hwnd, L"Please select an output directory.", L"Error", MB_ICONERROR);
            return 0;
          }
          if (state->profile_name.empty()) {
            MessageBoxW(hwnd, L"Please select a game profile.", L"Error", MB_ICONERROR);
            return 0;
          }

          state->ok = true;
          DestroyWindow(hwnd);
          return 0;
        }
        case IDC_CANCEL: {
          state->ok = false;
          DestroyWindow(hwnd);
          return 0;
        }
      }
      break;
    }
    case WM_DESTROY: {
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

GuiResult ShowLauncherWizard() {
  WizardState state;
  state.profiles = FindProfiles(GetAppDir());

  if (state.profiles.empty()) {
    MessageBoxW(nullptr,
                L"No game profiles found in the profiles/ directory.",
                L"Error", MB_ICONERROR);
    return {};
  }

  // Register a window class for the wizard (CreateWindow + message loop,
  // not DialogBox — gives us full control over layout without .rc files).
  static const wchar_t* kClassName = L"Glue360Wizard";
  WNDCLASSW wc{};
  wc.lpfnWndProc = WizardProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  RegisterClassW(&wc);

  // Size: 480 x 220, centered
  int screen_w = GetSystemMetrics(SM_CXSCREEN);
  int screen_h = GetSystemMetrics(SM_CYSCREEN);
  int win_w = 480;
  int win_h = 220;
  int x = (screen_w - win_w) / 2;
  int y = (screen_h - win_h) / 2;

  HWND hwnd = CreateWindowExW(
      WS_EX_APPWINDOW, kClassName, L"Glue360 Wizard",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      x, y, win_w, win_h,
      nullptr, nullptr, GetModuleHandleW(nullptr), &state);

  if (!hwnd) return {};

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  // Message loop
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  GuiResult result;
  result.ok = state.ok;
  result.iso_path = state.iso_path;
  result.output_dir = state.output_dir;
  result.profile_name = state.profile_name;
  result.sdk_path = state.sdk_path.string();
  return result;
}

void ShowResult(bool success, const std::string& deploy_path,
                const std::string& error_msg) {
  if (success) {
    std::wstring msg = L"Recompilation complete!\n\nYour game is at:\n" +
                       Widen(deploy_path) +
                       L"\n\nYou can run the .exe directly from that folder.";
    MessageBoxW(nullptr, msg.c_str(), L"Done", MB_OK | MB_ICONINFORMATION);
  } else {
    std::wstring msg = L"Recompilation failed.\n\n" + Widen(error_msg) +
                       L"\n\nCheck the log file in the output directory.";
    MessageBoxW(nullptr, msg.c_str(), L"Error", MB_OK | MB_ICONERROR);
  }
}

#else  // !_WIN32

GuiResult ShowLauncherWizard() {
  return {};
}

void ShowResult(bool, const std::string&, const std::string&) {}

#endif

}  // namespace recomp::gui
