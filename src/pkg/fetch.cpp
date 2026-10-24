#include "pkg/fetch.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>

#include "util/md5.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace didj::pkg {

namespace {

namespace fs = std::filesystem;

bool cancelled(const FetchOptions& opt) { return opt.cancel && *opt.cancel; }

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// Quotes one argument the way CommandLineToArgvW splits it back.
std::wstring quote_arg(const std::wstring& a) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
        } else {
            if (c == L'"') out.append(slashes + 1, L'\\');
            slashes = 0;
        }
        out += c;
    }
    out.append(slashes, L'\\');
    return out + L"\"";
}
#endif

// Runs curl with args and waits for it, killing it if opt.cancel is set.
// Returns true if it exits with status 0.
bool run_curl(const std::vector<std::string>& args, const FetchOptions& opt) {
#ifdef _WIN32
    std::wstring cmd = L"curl";
    for (const auto& a : args) cmd += L" " + quote_arg(widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // Without a console of our own (the windowed front end), keep curl from
    // opening one.
    DWORD flags = opt.show_progress ? 0 : CREATE_NO_WINDOW;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        if (cancelled(opt)) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0 && !cancelled(opt);
#else
    std::vector<char*> argv;
    std::string curl = "curl";
    argv.push_back(curl.data());
    std::vector<std::string> copy = args;
    for (auto& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execvp("curl", argv.data());
        _exit(127);
    }
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) return false;
        if (cancelled(opt)) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

}  // namespace

std::string dump_url(const DumpFile& d) {
    std::string url = "https://archive.org/download/";
    url += d.item;
    url += '/';
    for (const char* p = d.name; *p; ++p) {
        if (*p == ' ') url += "%20";
        else url += *p;
    }
    return url;
}

bool select_dumps(const std::vector<std::string>& words, bool verify_only, std::vector<const DumpFile*>& out,
                  std::string* unknown) {
    auto add = [&](const DumpFile& d) {
        if (std::find(out.begin(), out.end(), &d) == out.end()) out.push_back(&d);
    };
    for (const auto& w : words) {
        bool matched = false;
        for (const auto& d : kDumps) {
            // "os" includes JetPack Heroes, the game every Didj came with.
            const std::string n = d.name;
            bool sel = w == "all" || (w == "os" && !d.cartridge()) || w == n ||
                       (d.kind == DumpKind::Package && w + ".lfp" == n);
            if (sel) add(d), matched = true;
        }
        if (!matched) {
            if (unknown) *unknown = w;
            return false;
        }
    }
    if (!verify_only)
        for (const auto& d : kDumps)
            if (d.kind == DumpKind::System) add(d);
    std::sort(out.begin(), out.end());  // table order: system first
    return true;
}

bool fetch_dump(const DumpFile& d, const fs::path& dir, const FetchOptions& opt, std::string* err) {
    auto log = [&](const std::string& s) {
        if (opt.log) opt.log(s);
    };
    fs::path target = dir / fs::u8path(d.name);
    std::error_code ec;
    if (fs::exists(target, ec)) {
        std::string sum = util::md5_file(target.u8string());
        if (sum == d.md5) {
            log(std::string("ok        ") + d.name);
            return true;
        }
        log(std::string("mismatch  ") + d.name + " (md5 " + (sum.empty() ? "unreadable" : sum) + ")");
        if (opt.verify_only) return false;
    } else if (opt.verify_only) {
        log(std::string("missing   ") + d.name);
        return false;
    }

    fs::path part = target;
    part += ".part";
    log(std::string("download  ") + d.name);
    std::vector<std::string> args = {"-fL", "--retry", "3", "-C", "-", "-o", part.u8string(), dump_url(d)};
    if (!opt.show_progress) args.insert(args.begin(), "-sS");
    if (!run_curl(args, opt)) {
        if (!cancelled(opt)) *err = std::string("download of ") + d.name + " failed (rerun to resume)";
        return false;
    }
    std::string sum = util::md5_file(part.u8string());
    if (sum != d.md5) {
        *err = std::string(d.name) + " has md5 " + sum + ", expected " + d.md5 + "; discarding it";
        fs::remove(part, ec);
        return false;
    }
    fs::remove(target, ec);
    fs::rename(part, target, ec);
    if (ec) {
        *err = "cannot rename " + part.u8string() + ": " + ec.message();
        return false;
    }
    log(std::string("ok        ") + d.name);
    return true;
}

}  // namespace didj::pkg
