// Read-only JFFS2 reader for the Didj root filesystem dump (erootfs.jffs2).
//
// The whole image is scanned once: the newest directory entry per
// (parent, name) wins, and file data is rebuilt by applying an inode's data
// nodes in version order. Supports none/zero/rtime/zlib compression, which
// covers the Didj's 2.6.20 kernel configuration. Not thread-safe.
#pragma once

#include <map>
#include <unordered_map>

#include "fs/filesystem.h"

namespace didj::fs {

class Jffs2Image : public FileSystem {
public:
    static std::unique_ptr<Jffs2Image> open(const std::string& path, std::string* err);
    static std::unique_ptr<Jffs2Image> from_bytes(std::vector<uint8_t> image, std::string* err);

    std::string describe() const override;
    Entry root() override;
    std::vector<Entry> list(const Entry& dir) override;
    size_t read(const Entry& file, uint64_t offset, void* buf, size_t len) override;
    std::string readlink(const Entry& link) override;
    using FileSystem::list;

    // Nodes skipped because a CRC did not match, or data that failed to decompress.
    size_t bad_nodes() const { return bad_nodes_; }

private:
    struct DataNode {
        uint32_t version, offset, csize, dsize, isize, image_offset;
        uint8_t compr;
    };
    struct Inode {
        uint32_t version = 0;  // of the newest node, which carries the metadata
        uint32_t mode = 0, uid = 0, gid = 0, mtime = 0, isize = 0;
        std::vector<DataNode> nodes;
    };
    struct Dirent {
        uint32_t version, ino;
        uint8_t type;
    };

    Jffs2Image() = default;
    bool scan(std::string* err);
    Entry make_entry(const std::string& name, uint32_t ino, uint8_t dirent_type);
    const std::vector<uint8_t>* contents(uint32_t ino);

    std::vector<uint8_t> image_;
    std::unordered_map<uint32_t, Inode> inodes_;
    std::map<std::pair<uint32_t, std::string>, Dirent> dirents_;  // (parent, name)
    std::unordered_map<uint32_t, std::vector<uint8_t>> cache_;
    size_t bad_nodes_ = 0;
};

}  // namespace didj::fs
