// didj-fetch: downloads the Didj dumps from archive.org and verifies them.
//
//   didj-fetch [--dir DIR] [--verify] [all | os | CART...]
//
// "os" is the root filesystem plus the Didj OS partition; "all" (the
// default) adds every cartridge. Files already present with the right MD5
// are left alone, so running it again resumes or repairs a download. The
// default directory is the one didj-run looks in (see pkg/paths.h).
//
// Downloads go through the system curl, which ships with Windows 10 and
// later and with every Linux distribution.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "pkg/dumps.h"
#include "pkg/paths.h"
#include "util/md5.h"

namespace {

namespace fs = std::filesystem;
using didj::pkg::DumpFile;

int usage() {
    std::fprintf(stderr, "usage: didj-fetch [--dir DIR] [--verify] [all | os | CART...]\n");
    return 2;
}

std::string url_for(const DumpFile& d) {
    std::string url = "https://archive.org/download/";
    url += d.item;
    url += '/';
    for (const char* p = d.name; *p; ++p) {
        if (*p == ' ') url += "%20";
        else url += *p;
    }
    return url;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

bool run_curl(const std::string& url, const fs::path& out) {
    std::string cmd = "curl -fL --retry 3 -C - -o " + quote(out.u8string()) + " " + quote(url);
#ifdef _WIN32
    // cmd.exe strips the first and last quote of a command line that has more
    // than two; an extra pair keeps the inner quoting intact.
    cmd = "\"" + cmd + "\"";
#endif
    return std::system(cmd.c_str()) == 0;
}

// Returns true if the file ends up present and verified.
bool fetch(const DumpFile& d, const fs::path& dir, bool verify_only) {
    fs::path target = dir / fs::u8path(d.name);
    std::error_code ec;
    if (fs::exists(target, ec)) {
        std::string sum = didj::util::md5_file(target.u8string());
        if (sum == d.md5) {
            std::printf("ok        %s\n", d.name);
            return true;
        }
        std::printf("mismatch  %s (md5 %s)\n", d.name, sum.empty() ? "unreadable" : sum.c_str());
        if (verify_only) return false;
    } else if (verify_only) {
        std::printf("missing   %s\n", d.name);
        return false;
    }

    fs::path part = target;
    part += ".part";
    std::printf("download  %s\n", d.name);
    std::fflush(stdout);
    if (!run_curl(url_for(d), part)) {
        std::fprintf(stderr, "didj-fetch: download of %s failed (rerun to resume)\n", d.name);
        return false;
    }
    std::string sum = didj::util::md5_file(part.u8string());
    if (sum != d.md5) {
        std::fprintf(stderr, "didj-fetch: %s has md5 %s, expected %s; discarding it\n", d.name, sum.c_str(), d.md5);
        fs::remove(part, ec);
        return false;
    }
    fs::remove(target, ec);
    fs::rename(part, target, ec);
    if (ec) {
        std::fprintf(stderr, "didj-fetch: cannot rename %s: %s\n", part.u8string().c_str(), ec.message().c_str());
        return false;
    }
    std::printf("ok        %s\n", d.name);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path dir = didj::pkg::default_dump_dir();
    bool verify_only = false;
    std::vector<std::string> wanted;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dir = fs::u8path(argv[++i]);
        else if (a == "--verify") verify_only = true;
        else if (a == "-h" || a == "--help") return usage();
        else if (!a.empty() && a[0] == '-') return usage();
        else wanted.push_back(a);
    }
    if (dir.empty()) {
        std::fprintf(stderr, "didj-fetch: no home directory; pass --dir\n");
        return 2;
    }
    if (wanted.empty()) wanted.push_back("all");

    std::vector<const DumpFile*> files;
    auto add = [&](const DumpFile& d) {
        for (auto* f : files)
            if (f == &d) return;
        files.push_back(&d);
    };
    for (const auto& w : wanted) {
        bool matched = false;
        for (const auto& d : didj::pkg::kDumps) {
            bool sel = w == "all" || (w == "os" && !d.cartridge) || (d.cartridge && w == d.name);
            if (sel) add(d), matched = true;
        }
        if (!matched) {
            std::fprintf(stderr, "didj-fetch: unknown dump '%s'. Cartridges:", w.c_str());
            for (const auto& d : didj::pkg::kDumps)
                if (d.cartridge) std::fprintf(stderr, " %s", d.name);
            std::fprintf(stderr, "\n");
            return 2;
        }
    }
    // A cartridge is useless without the system it runs on.
    if (!verify_only)
        for (const auto& d : didj::pkg::kDumps)
            if (!d.cartridge) add(d);
    std::sort(files.begin(), files.end());  // table order: system first

    std::error_code ec;
    if (!verify_only) fs::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "didj-fetch: cannot create %s: %s\n", dir.u8string().c_str(), ec.message().c_str());
        return 1;
    }
    std::printf("dump directory: %s\n", dir.u8string().c_str());
    int failed = 0;
    for (auto* d : files)
        if (!fetch(*d, dir, verify_only)) ++failed;
    if (failed) {
        std::fprintf(stderr, "didj-fetch: %d file(s) not ready\n", failed);
        return 1;
    }
    return 0;
}
