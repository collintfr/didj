// didjtool: inspect Didj dumps without mounting them.
//
//   didjtool info    IMAGE                 volume type, label, cartridge meta.inf
//   didjtool ls      IMAGE [PATH] [-r]     list a directory (recursively with -r)
//   didjtool cat     IMAGE PATH            write a file to stdout
//   didjtool extract IMAGE [PATH] OUTDIR   copy a file or directory tree to the host
//   didjtool elf     FILE                  NEEDED, SONAME, imports/exports of an ELF
//   didjtool coverage FILE...              list imports no other FILE provides
//
// FILE is a host path or IMAGE:/path/in/image; a directory inside an image
// (IMAGE:/Base/Brio/) expands to every ELF file below it.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "elf/dynamic.h"
#include "fs/fat.h"
#include "pkg/metainf.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using didj::fs::DirEntry;
using didj::fs::FatImage;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: didjtool info IMAGE\n"
                 "       didjtool ls IMAGE [PATH] [-r]\n"
                 "       didjtool cat IMAGE PATH\n"
                 "       didjtool extract IMAGE [PATH] OUTDIR\n"
                 "       didjtool elf FILE\n"
                 "       didjtool coverage FILE...\n"
                 "FILE is a host path or IMAGE:/path (a directory expands to its ELF files)\n");
    return 2;
}

std::unique_ptr<FatImage> open_image(const std::string& path) {
    std::string err;
    auto img = FatImage::open(path, &err);
    if (!img) std::fprintf(stderr, "didjtool: %s: %s\n", path.c_str(), err.c_str());
    return img;
}

std::string join(const std::string& dir, const std::string& name) {
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

void list_tree(FatImage& img, const DirEntry& dir, const std::string& prefix, bool recursive) {
    for (const auto& e : img.list(dir)) {
        std::string p = join(prefix, e.name);
        if (e.is_dir) std::printf("%10s  %s/\n", "<dir>", p.c_str());
        else std::printf("%10u  %s\n", e.size, p.c_str());
        if (recursive && e.is_dir) list_tree(img, e, p, true);
    }
}

bool extract_entry(FatImage& img, const DirEntry& e, const std::filesystem::path& out) {
    if (e.is_dir) {
        std::filesystem::create_directories(out);
        bool ok = true;
        for (const auto& child : img.list(e)) ok &= extract_entry(img, child, out / child.name);
        return ok;
    }
    std::vector<uint8_t> buf(e.size);
    if (img.read(e, 0, buf.data(), buf.size()) != buf.size()) {
        std::fprintf(stderr, "didjtool: short read on %s\n", out.string().c_str());
        return false;
    }
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
    return bool(f);
}

int cmd_info(const std::string& path) {
    auto img = open_image(path);
    if (!img) return 1;
    std::printf("type:     %s\nlabel:    %s\ncluster:  %u bytes x %u\n", didj::fs::fat_type_name(img->type()),
                img->label().c_str(), img->cluster_size(), img->cluster_count());
    // A cartridge holds one top-level directory per title, each with a meta.inf.
    for (const auto& e : img->list(*img->stat("/"))) {
        if (!e.is_dir) continue;
        auto data = img->read_file("/" + e.name + "/meta.inf");
        if (!data) continue;
        auto meta = didj::pkg::MetaInf::parse(std::string(data->begin(), data->end()));
        std::printf("\n[%s]\n", e.name.c_str());
        for (const auto& [k, v] : meta.entries()) std::printf("  %-14s %s\n", k.c_str(), v.c_str());
    }
    return 0;
}

struct NamedElf {
    std::string name;
    didj::elf::DynamicInfo info;
};

// Loads FILE (host path or IMAGE:/path) into out; directories expand to all ELF files.
bool load_elfs(const std::string& spec, std::vector<NamedElf>& out) {
    auto add = [&](const std::string& name, const std::vector<uint8_t>& data) {
        std::string err;
        auto info = didj::elf::parse_dynamic(data, &err);
        if (!info) {
            std::fprintf(stderr, "didjtool: %s: %s\n", name.c_str(), err.c_str());
            return false;
        }
        out.push_back({name, std::move(*info)});
        return true;
    };

    size_t colon = spec.find(":/");
    if (colon == std::string::npos) {
        std::ifstream f(spec, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "didjtool: cannot open %s\n", spec.c_str());
            return false;
        }
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return add(spec, data);
    }

    auto img = open_image(spec.substr(0, colon));
    if (!img) return false;
    std::string inner = spec.substr(colon + 1);
    auto e = img->stat(inner);
    if (!e) {
        std::fprintf(stderr, "didjtool: %s: not found\n", spec.c_str());
        return false;
    }
    bool ok = true;
    std::vector<std::pair<std::string, DirEntry>> stack{{inner, *e}};
    while (!stack.empty()) {
        auto [p, ent] = stack.back();
        stack.pop_back();
        if (ent.is_dir) {
            for (const auto& c : img->list(ent)) stack.push_back({join(p, c.name), c});
            continue;
        }
        std::vector<uint8_t> data(ent.size);
        if (img->read(ent, 0, data.data(), data.size()) != data.size()) continue;
        if (!didj::elf::is_elf(data)) {
            if (!e->is_dir) {
                std::fprintf(stderr, "didjtool: %s: not an ELF file\n", spec.c_str());
                ok = false;
            }
            continue;
        }
        ok &= add(p, data);
    }
    return ok;
}

std::string sym_label(const didj::elf::DynSymbol& s) {
    return s.version.empty() ? s.name : s.name + "@" + s.version;
}

int cmd_elf(const std::string& spec) {
    std::vector<NamedElf> elfs;
    if (!load_elfs(spec, elfs)) return 1;
    for (const auto& [name, info] : elfs) {
        std::printf("%s\n  machine %u, flags 0x%08x, type %u, entry 0x%x\n", name.c_str(), info.machine,
                    info.flags, info.elf_type, info.entry);
        if (!info.interpreter.empty()) std::printf("  interpreter %s\n", info.interpreter.c_str());
        if (!info.soname.empty()) std::printf("  soname %s\n", info.soname.c_str());
        for (const auto& n : info.needed) std::printf("  needed %s\n", n.c_str());
        size_t imports = 0, exports = 0;
        for (const auto& s : info.symbols) (s.defined ? exports : imports)++;
        std::printf("  %zu imports, %zu exports\n", imports, exports);
        for (const auto& s : info.symbols)
            std::printf("  %s %s%s\n", s.defined ? "export" : "import", sym_label(s).c_str(), s.weak ? " (weak)" : "");
    }
    return 0;
}

int cmd_coverage(const std::vector<std::string>& specs) {
    std::vector<NamedElf> elfs;
    bool ok = true;
    for (const auto& s : specs) ok &= load_elfs(s, elfs);
    if (!ok) return 1;

    std::set<std::string> provided_plain, provided_versioned;
    std::set<std::string> sonames;
    for (const auto& e : elfs) {
        std::string base = std::filesystem::path(e.name).filename().string();
        sonames.insert(e.info.soname.empty() ? base : e.info.soname);
        sonames.insert(base);
        for (const auto& s : e.info.symbols) {
            if (!s.defined) continue;
            provided_plain.insert(s.name);
            if (!s.version.empty()) provided_versioned.insert(sym_label(s));
        }
    }

    std::map<std::string, std::set<std::string>> missing_libs;  // lib -> users
    size_t missing_total = 0;
    for (const auto& e : elfs) {
        std::vector<std::string> missing;
        for (const auto& s : e.info.symbols) {
            if (s.defined || s.weak) continue;
            bool found = s.version.empty() ? provided_plain.count(s.name) : provided_versioned.count(sym_label(s));
            if (!found) missing.push_back(sym_label(s));
        }
        for (const auto& n : e.info.needed)
            if (!sonames.count(n)) missing_libs[n].insert(e.name);
        if (missing.empty()) continue;
        missing_total += missing.size();
        std::printf("%s: %zu unresolved\n", e.name.c_str(), missing.size());
        for (const auto& m : missing) std::printf("  %s\n", m.c_str());
    }
    if (!missing_libs.empty()) {
        std::printf("missing libraries:\n");
        for (const auto& [lib, users] : missing_libs) std::printf("  %s (needed by %zu files)\n", lib.c_str(), users.size());
    }
    std::printf("%zu files, %zu unresolved symbols, %zu missing libraries\n", elfs.size(), missing_total,
                missing_libs.size());
    return missing_total || !missing_libs.empty() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    if (cmd == "info" && args.size() == 1) return cmd_info(args[0]);
    if (cmd == "elf" && args.size() == 1) return cmd_elf(args[0]);
    if (cmd == "coverage") return cmd_coverage(args);

    if (cmd == "ls") {
        bool recursive = false;
        std::vector<std::string> pos;
        for (const auto& a : args) {
            if (a == "-r") recursive = true;
            else pos.push_back(a);
        }
        if (pos.empty() || pos.size() > 2) return usage();
        auto img = open_image(pos[0]);
        if (!img) return 1;
        std::string path = pos.size() > 1 ? pos[1] : "/";
        auto e = img->stat(path);
        if (!e) {
            std::fprintf(stderr, "didjtool: %s: not found\n", path.c_str());
            return 1;
        }
        if (!e->is_dir) std::printf("%10u  %s\n", e->size, path.c_str());
        else list_tree(*img, *e, path, recursive);
        return 0;
    }
    if (cmd == "cat" && args.size() == 2) {
        auto img = open_image(args[0]);
        if (!img) return 1;
        auto data = img->read_file(args[1]);
        if (!data) {
            std::fprintf(stderr, "didjtool: %s: not found or not a file\n", args[1].c_str());
            return 1;
        }
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::fwrite(data->data(), 1, data->size(), stdout);
        return 0;
    }
    if (cmd == "extract" && (args.size() == 2 || args.size() == 3)) {
        auto img = open_image(args[0]);
        if (!img) return 1;
        std::string path = args.size() == 3 ? args[1] : "/";
        auto e = img->stat(path);
        if (!e) {
            std::fprintf(stderr, "didjtool: %s: not found\n", path.c_str());
            return 1;
        }
        std::filesystem::path out = args.back();
        if (!e->is_dir) {
            std::filesystem::create_directories(out);
            out /= e->name;
        }
        return extract_entry(*img, *e, out) ? 0 : 1;
    }
    return usage();
}
