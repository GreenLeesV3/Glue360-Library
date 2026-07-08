// core/process_runner.cpp — Windows child process execution via CreateProcessW.
//
// Implementation notes:
//   - stdout and stderr are merged into a single pipe (both child handles point
//     to the same write end) to avoid the sequential-drain deadlock: a child
//     that writes >64KB to stderr while we are still reading stdout would block
//     forever on a full stderr pipe buffer. Merging loses stdout/stderr
//     separation in the captured text, which is acceptable for a build driver.
//   - `dest` accumulates ALL bytes read (complete lines + partial tail), so
//     callers that parse captured output (compiler errors, ninja [N/92]) see
//     the full text. Complete lines are additionally forwarded to `on_line` as
//     they arrive for live progress display.
#include "core/process_runner.h"

#include <windows.h>

#include <string>
#include <vector>

namespace recomp {

namespace {

std::string quote_if_needed(const std::string& s) {
    if (s.empty() || s.find_first_of(" \t\"") != std::string::npos) {
        std::string out = "\"";
        for (char c : s) {
            if (c == '"') out += "\\\"";
            else out += c;
        }
        out += "\"";
        return out;
    }
    return s;
}

std::string build_command_line(const fs::path& exe,
                               const std::vector<std::string>& args) {
    std::string cmd = quote_if_needed(exe.string());
    for (const auto& a : args) {
        cmd += ' ';
        cmd += quote_if_needed(a);
    }
    return cmd;
}

// Build a UTF-16 double-NUL-terminated environment block from `env`. If `env`
// is empty, returns an empty block (caller passes NULL to inherit parent env).
std::wstring build_env_block(const std::map<std::string, std::string>& env) {
    std::wstring block;
    for (const auto& [k, v] : env) {
        std::string entry = k + "=" + v;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, entry.c_str(),
                                       static_cast<int>(entry.size()), nullptr, 0);
        std::wstring wentry(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, entry.c_str(),
                            static_cast<int>(entry.size()), wentry.data(), wlen);
        block += wentry;
        block += L'\0';
    }
    if (!block.empty()) block += L'\0'; // final terminator
    return block;
}

// Read all bytes from `pipe` into `dest` (complete lines + partial tail),
// forwarding each complete line to `on_line` as it arrives. The final partial
// line (no trailing newline) is kept in `dest` but NOT forwarded here.
void drain_pipe(HANDLE pipe, std::string& dest, const ProgressCallback& on_line) {
    char buf[4096];
    DWORD n = 0;
    // `tail` holds bytes after the last '\n' that haven't been line-forwarded.
    std::string tail;
    while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n > 0) {
        // Capture ALL bytes first so dest always has the full text.
        dest.append(buf, n);
        tail.append(buf, n);
        std::size_t pos = 0;
        while (true) {
            std::size_t nl = tail.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = tail.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (on_line) on_line(0.0f, line);
            pos = nl + 1;
        }
        if (pos > 0) tail.erase(0, pos);
    }
}

} // namespace

ProcessOutput ProcessRunner::run(const fs::path& exe,
                                 const std::vector<std::string>& args,
                                 const fs::path& cwd,
                                 const std::map<std::string, std::string>& env,
                                 const ProgressCallback& on_line) {
    ProcessOutput out;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // One pipe, shared by stdout + stderr to avoid the sequential-drain
    // deadlock (a child blocking on a full stderr pipe while we read stdout).
    HANDLE child_r = nullptr, child_w = nullptr;
    CreatePipe(&child_r, &child_w, &sa, 0);
    SetHandleInformation(child_r, HANDLE_FLAG_INHERIT, 0);

    std::wstring env_block = build_env_block(env);
    void* env_ptr = env.empty() ? nullptr
                                : static_cast<void*>(env_block.data());

    std::string cmdline = build_command_line(exe, args);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(),
                                   static_cast<int>(cmdline.size()), nullptr, 0);
    std::wstring wcmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmdline.c_str(),
                        static_cast<int>(cmdline.size()), wcmd.data(), wlen);

    std::wstring wcwd;
    if (!cwd.empty()) {
        std::string cs = cwd.string();
        int cwlen = MultiByteToWideChar(CP_UTF8, 0, cs.c_str(),
                                        static_cast<int>(cs.size()), nullptr, 0);
        wcwd.resize(cwlen);
        MultiByteToWideChar(CP_UTF8, 0, cs.c_str(),
                            static_cast<int>(cs.size()), wcwd.data(), cwlen);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = child_w;        // merged
    si.hStdError  = child_w;        // merged
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        nullptr,
        wcmd.data(),
        nullptr, nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        env_ptr,
        wcwd.empty() ? nullptr : wcwd.c_str(),
        &si, &pi);

    // Close our copy of the write end so the child's EOF propagates to ReadFile.
    CloseHandle(child_w);

    if (!ok) {
        CloseHandle(child_r);
        out.exit_code = static_cast<int>(GetLastError());
        out.stderr_text = "CreateProcessW failed (error " +
                          std::to_string(out.exit_code) + ") for: " + exe.string();
        return out;
    }

    // Drain the merged stream into stdout_text (single pipe, no deadlock).
    // stderr_text is left empty; callers wanting all output read stdout_text.
    drain_pipe(child_r, out.stdout_text, on_line);

    // Forward any trailing partial line (no newline) left in the buffer.
    if (on_line && !out.stdout_text.empty() &&
        out.stdout_text.back() != '\n' && out.stdout_text.back() != '\r') {
        on_line(0.0f, out.stdout_text);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    out.exit_code = static_cast<int>(code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(child_r);
    return out;
}

int ProcessRunner::run_status(const fs::path& exe,
                              const std::vector<std::string>& args,
                              const fs::path& cwd,
                              const std::map<std::string, std::string>& env) {
    return run(exe, args, cwd, env, nullptr).exit_code;
}

} // namespace recomp
