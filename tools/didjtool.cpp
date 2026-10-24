// didjtool: inspect Didj dumps without mounting them.
//
//   didjtool info    IMAGE                 volume type, label, cartridge meta.inf
//   didjtool ls      IMAGE [PATH] [-r]     list a directory (recursively with -r)
//   didjtool cat     IMAGE PATH            write a file to stdout
//   didjtool extract IMAGE [PATH] OUTDIR   copy a file or directory tree to the host
//   didjtool elf     FILE                  NEEDED, SONAME, imports/exports of an ELF
//   didjtool coverage FILE...              list imports no other FILE provides
//
// IMAGE is a FAT (Didj OS, cartridge) or JFFS2 (erootfs.jffs2) dump. FILE is a
// host path or IMAGE:/path/in/image; a directory inside an image
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
#include "fs/filesystem.h"
#include "fs/jffs2.h"
#include "pkg/metainf.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using didj::fs::Entry;
using didj::fs::FileSystem;
using didj::fs::NodeType;

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

std::unique_ptr<FileSystem> open_image(const std::string& path) {
    std::string err;
    auto img = didj::fs::open_filesystem(path, &err);
    if (!img) std::fprintf(stderr, "didjtool: %s: %s\n", path.c_str(), err.c_str());
    return img;
}

std::string join(const std::string& dir, const std::string& name) {
    return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name;
}

void print_entry(FileSystem& img, const Entry& e, const std::string& path) {
    switch (e.type) {
        case NodeType::Dir: std::printf("%10s  %s/\n", "<dir>", path.c_str()); break;
        case NodeType::Symlink: std::printf("%10s  %s -> %s\n", "<link>", path.c_str(), img.readlink(e).c_str()); break;
        case NodeType::CharDevice:
        case NodeType::BlockDevice:
            std::printf("%10s  %s (%u, %u)\n", e.type == NodeType::CharDevice ? "<chr>" : "<blk>", path.c_str(),
                        (e.rdev >> 8) & 0xfff, (e.rdev & 0xff) | ((e.rdev >> 12) & 0xfff00));
            break;
        case NodeType::Fifo: std::printf("%10s  %s\n", "<fifo>", path.c_str()); break;
        case NodeType::Socket: std::printf("%10s  %s\n", "<sock>", path.c_str()); break;
        case NodeType::File: std::printf("%10llu  %s\n", (unsigned long long)e.size, path.c_str()); break;
    }
}

void list_tree(FileSystem& img, const Entry& dir, const std::string& prefix, bool recursive) {
    for (const auto& e : img.list(dir)) {
        std::string p = join(prefix, e.name);
        print_entry(img, e, p);
        if (recursive && e.is_dir()) list_tree(img, e, p, true);
    }
}

// Copies regular files and directories; symlinks and device nodes are skipped.
bool extract_entry(FileSystem& img, const Entry& e, const std::filesystem::path& out) {
    if (e.type != NodeType::File && !e.is_dir()) {
        std::fprintf(stderr, "didjtool: skipping special file %s\n", out.string().c_str());
        return true;
    }
    if (e.is_dir()) {
        std::filesystem::create_directories(out);
        bool ok = true;
        for (const auto& child : img.list(e)) ok &= extract_entry(img, child, out / child.name);
        return ok;
    }
    std::vector<uint8_t> buf = img.read_all(e);
    if (buf.size() != e.size) {
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
    std::printf("%s\n", img->describe().c_str());
    if (auto* j = dynamic_cast<didj::fs::Jffs2Image*>(img.get())) {
        size_t files = 0;
        std::vector<std::pair<std::string, Entry>> stack{{"", img->root()}};
        while (!stack.empty()) {  // reading everything surfaces any bad data nodes
            auto [p, e] = stack.back();
            stack.pop_back();
            if (e.is_dir()) {
                for (const auto& c : img->list(e)) stack.push_back({join(p, c.name), c});
            } else if (e.type == NodeType::File) {
                ++files;
                if (img->read_all(e).size() != e.size) std::printf("short read: %s\n", p.c_str());
            }
        }
        std::printf("%zu regular files, %zu bad nodes\n", files, j->bad_nodes());
    }
    // A cartridge holds one top-level directory per title, each with a meta.inf.
    for (const auto& e : img->list(img->root())) {
        if (!e.is_dir()) continue;
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

// File names that reach a loaded ELF through a symlink (libz.so -> libz.so.1.2.3);
// the dynamic loader finds NEEDED entries by these names too.
std::set<std::string> g_link_names;

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
    auto e = img->lookup(inner);
    if (!e) {
        std::fprintf(stderr, "didjtool: %s: not found\n", spec.c_str());
        return false;
    }
    bool ok = true;
    std::vector<std::pair<std::string, Entry>> stack{{inner, *e}};
    while (!stack.empty()) {
        auto [p, ent] = stack.back();
        stack.pop_back();
        if (ent.is_dir()) {
            for (const auto& c : img->list(ent)) stack.push_back({join(p, c.name), c});
            continue;
        }
        if (ent.type == NodeType::Symlink) {
            auto target = img->lookup(p);
            if (target && target->type == NodeType::File) g_link_names.insert(ent.name);
            continue;
        }
        if (ent.type != NodeType::File) continue;
        std::vector<uint8_t> data = img->read_all(ent);
        if (data.size() != ent.size) continue;
        if (!didj::elf::is_elf(data)) {
            if (!e->is_dir()) {
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
    std::set<std::string> sonames = g_link_names;
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
        auto e = img->lookup(path, /*follow_last=*/false);
        if (!e) {
            std::fprintf(stderr, "didjtool: %s: not found\n", path.c_str());
            return 1;
        }
        if (e->type == NodeType::Symlink) e = img->lookup(path);
        if (!e) return 1;
        if (!e->is_dir()) print_entry(*img, *e, path);
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
        auto e = img->lookup(path);
        if (!e) {
            std::fprintf(stderr, "didjtool: %s: not found\n", path.c_str());
            return 1;
        }
        std::filesystem::path out = args.back();
        if (!e->is_dir()) {
            std::filesystem::create_directories(out);
            out /= e->name;
        }
        return extract_entry(*img, *e, out) ? 0 : 1;
    }
    return usage();
}
