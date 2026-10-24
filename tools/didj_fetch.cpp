// didj-fetch: downloads the Didj dumps from archive.org and verifies them.
//
//   didj-fetch [--dir DIR] [--verify] [all | os | CART...]
//
// "os" is the root filesystem, the Didj OS partition and JetPack Heroes
// (POW, the game built into the Didj); "all" (the default) adds every
// cartridge. Files already present with the right MD5
// are left alone, so running it again resumes or repairs a download. The
// default directory is the one didj-run looks in (see pkg/paths.h).
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "pkg/dumps.h"
#include "pkg/fetch.h"
#include "pkg/paths.h"

namespace {

namespace fs = std::filesystem;

int usage() {
    std::fprintf(stderr, "usage: didj-fetch [--dir DIR] [--verify] [all | os | CART...]\n");
    return 2;
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

    std::vector<const didj::pkg::DumpFile*> files;
    std::string unknown;
    if (!didj::pkg::select_dumps(wanted, verify_only, files, &unknown)) {
        std::fprintf(stderr, "didj-fetch: unknown dump '%s'. Cartridges:", unknown.c_str());
        for (const auto& d : didj::pkg::kDumps)
            if (d.cartridge()) std::fprintf(stderr, " %s", d.name);
        std::fprintf(stderr, "\n");
        return 2;
    }

    std::error_code ec;
    if (!verify_only) fs::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "didj-fetch: cannot create %s: %s\n", dir.u8string().c_str(), ec.message().c_str());
        return 1;
    }
    std::printf("dump directory: %s\n", dir.u8string().c_str());
    didj::pkg::FetchOptions opt;
    opt.verify_only = verify_only;
    opt.log = [](const std::string& s) {
        std::printf("%s\n", s.c_str());
        std::fflush(stdout);
    };
    int failed = 0;
    for (auto* d : files) {
        std::string err;
        if (!didj::pkg::fetch_dump(*d, dir, opt, &err)) {
            if (!err.empty()) std::fprintf(stderr, "didj-fetch: %s\n", err.c_str());
            ++failed;
        }
    }
    if (failed) {
        std::fprintf(stderr, "didj-fetch: %d file(s) not ready\n", failed);
        return 1;
    }
    return 0;
}
