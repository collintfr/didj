// Read-only filesystem interface shared by the image readers (FAT, JFFS2).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace didj::fs {

enum class NodeType { File, Dir, Symlink, CharDevice, BlockDevice, Fifo, Socket };

struct Entry {
    std::string name;
    std::string alt_name;  // FAT 8.3 alias, matched by lookup too; empty elsewhere
    NodeType type = NodeType::File;
    uint64_t size = 0;
    uint32_t mode = 0;     // full st_mode, including the S_IFMT type bits
    uint32_t uid = 0, gid = 0;
    uint32_t mtime = 0;
    uint32_t rdev = 0;     // device number for device nodes (Linux new_encode_dev)
    uint64_t id = 0;       // filesystem-specific (FAT first cluster, JFFS2 inode)

    bool is_dir() const { return type == NodeType::Dir; }
};

class FileSystem {
public:
    virtual ~FileSystem() = default;

    virtual std::string describe() const = 0;  // e.g. "FAT16 volume 'Didj'"
    virtual Entry root() = 0;
    virtual std::vector<Entry> list(const Entry& dir) = 0;  // without "." and ".."
    // Reads up to len bytes of a regular file; returns the number read.
    virtual size_t read(const Entry& file, uint64_t offset, void* buf, size_t len) = 0;
    virtual std::string readlink(const Entry& link) { (void)link; return {}; }
    virtual bool case_insensitive() const { return false; }

    // Resolves a '/'-separated path from the root, following symlinks inside
    // this filesystem ("." and ".." are handled; ".." at the root stays there).
    std::optional<Entry> lookup(std::string_view path, bool follow_last = true);
    std::optional<std::vector<Entry>> list(std::string_view path);
    std::optional<std::vector<uint8_t>> read_file(std::string_view path);
    std::vector<uint8_t> read_all(const Entry& file);
};

// Opens a FAT or JFFS2 image, detected from its contents.
std::unique_ptr<FileSystem> open_filesystem(const std::string& path, std::string* err);

}  // namespace didj::fs
