#include "fs/filesystem.h"

#include <cctype>
#include <deque>
#include <fstream>

#include "fs/fat.h"
#include "fs/jffs2.h"

namespace didj::fs {

namespace {

constexpr int kMaxSymlinks = 40;  // same limit as Linux

std::deque<std::string> split_path(std::string_view path) {
    std::deque<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t j = i;
        while (j < path.size() && path[j] != '/') ++j;
        if (j > i) parts.emplace_back(path.substr(i, j - i));
        i = j;
    }
    return parts;
}

bool name_matches(std::string_view a, std::string_view b, bool icase) {
    if (a.size() != b.size()) return false;
    if (!icase) return a == b;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

}  // namespace

std::optional<Entry> FileSystem::lookup(std::string_view path, bool follow_last) {
    std::vector<Entry> stack{root()};
    std::deque<std::string> todo = split_path(path);
    int links = 0;
    const bool icase = case_insensitive();
    while (!todo.empty()) {
        std::string part = std::move(todo.front());
        todo.pop_front();
        if (part == ".") continue;
        if (part == "..") {
            if (stack.size() > 1) stack.pop_back();
            continue;
        }
        if (!stack.back().is_dir()) return std::nullopt;
        std::optional<Entry> child;
        for (auto& e : list(stack.back())) {
            if (name_matches(e.name, part, icase) || (!e.alt_name.empty() && name_matches(e.alt_name, part, icase))) {
                child = std::move(e);
                break;
            }
        }
        if (!child) return std::nullopt;
        if (child->type == NodeType::Symlink && (follow_last || !todo.empty())) {
            if (++links > kMaxSymlinks) return std::nullopt;
            std::string target = readlink(*child);
            if (target.empty()) return std::nullopt;
            if (target.front() == '/') stack.resize(1);
            auto parts = split_path(target);
            todo.insert(todo.begin(), parts.begin(), parts.end());
            continue;
        }
        stack.push_back(std::move(*child));
    }
    return stack.back();
}

std::optional<std::vector<Entry>> FileSystem::list(std::string_view path) {
    auto d = lookup(path);
    if (!d || !d->is_dir()) return std::nullopt;
    return list(*d);
}

std::vector<uint8_t> FileSystem::read_all(const Entry& file) {
    std::vector<uint8_t> data(size_t(file.size));
    data.resize(read(file, 0, data.data(), data.size()));
    return data;
}

std::optional<std::vector<uint8_t>> FileSystem::read_file(std::string_view path) {
    auto e = lookup(path);
    if (!e || e->type != NodeType::File) return std::nullopt;
    auto data = read_all(*e);
    if (data.size() != e->size) return std::nullopt;
    return data;
}

std::unique_ptr<FileSystem> open_filesystem(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return nullptr;
    }
    uint8_t magic[2] = {};
    f.read(reinterpret_cast<char*>(magic), 2);
    f.close();
    if (magic[0] == 0x85 && magic[1] == 0x19) return Jffs2Image::open(path, err);
    return FatImage::open(path, err);
}

}  // namespace didj::fs
