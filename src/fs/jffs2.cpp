#include "fs/jffs2.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "util/inflate.h"

namespace didj::fs {

namespace {

constexpr uint16_t kMagic = 0x1985;
constexpr uint16_t kNodeDirent = 0xE001;
constexpr uint16_t kNodeInode = 0xE002;
constexpr size_t kDirentHeader = 40;
constexpr size_t kInodeHeader = 68;
constexpr uint32_t kRootIno = 1;

enum Compr : uint8_t { kNone = 0, kZero = 1, kRtime = 2, kZlib = 6 };

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool rtime_decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_len) {
    size_t positions[256] = {};
    size_t pos = 0, outpos = 0;
    while (outpos < out_len) {
        if (pos + 2 > in_len) return false;
        uint8_t value = in[pos++];
        out[outpos++] = value;
        size_t repeat = in[pos++];
        size_t back = positions[value];
        positions[value] = outpos;
        if (outpos + repeat > out_len) return false;
        while (repeat--) out[outpos++] = out[back++];
    }
    return true;
}

NodeType type_from_mode(uint32_t mode) {
    switch (mode & 0170000) {
        case 0040000: return NodeType::Dir;
        case 0120000: return NodeType::Symlink;
        case 0020000: return NodeType::CharDevice;
        case 0060000: return NodeType::BlockDevice;
        case 0010000: return NodeType::Fifo;
        case 0140000: return NodeType::Socket;
        default: return NodeType::File;
    }
}

// Dirent d_type (DT_*) to st_mode type bits, for entries whose inode is missing.
uint32_t mode_from_dirent_type(uint8_t t) {
    switch (t) {
        case 4: return 0040000;
        case 10: return 0120000;
        case 2: return 0020000;
        case 6: return 0060000;
        case 1: return 0010000;
        case 12: return 0140000;
        default: return 0100000;
    }
}

}  // namespace

std::unique_ptr<Jffs2Image> Jffs2Image::open(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return nullptr;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return from_bytes(std::move(data), err);
}

std::unique_ptr<Jffs2Image> Jffs2Image::from_bytes(std::vector<uint8_t> image, std::string* err) {
    std::unique_ptr<Jffs2Image> img(new Jffs2Image());
    img->image_ = std::move(image);
    if (!img->scan(err)) return nullptr;
    return img;
}

bool Jffs2Image::scan(std::string* err) {
    const uint8_t* d = image_.data();
    const size_t n = image_.size();
    size_t off = 0;
    while (off + 12 <= n) {
        uint16_t magic = le16(d + off), type = le16(d + off + 2);
        uint32_t totlen = le32(d + off + 4);
        if (magic != kMagic || totlen < 12 || off + totlen > n) {
            off += 4;
            continue;
        }
        if (util::crc32_raw(0, d + off, 8) != le32(d + off + 8)) {
            ++bad_nodes_;
            off += 4;
            continue;
        }
        const uint8_t* p = d + off;
        if (type == kNodeDirent && totlen >= kDirentHeader) {
            uint8_t nsize = p[28];
            if (util::crc32_raw(0, p, 32) != le32(p + 32) || kDirentHeader + nsize > totlen) {
                ++bad_nodes_;
            } else {
                uint32_t pino = le32(p + 12), version = le32(p + 16), ino = le32(p + 20);
                std::string name(reinterpret_cast<const char*>(p + kDirentHeader), nsize);
                auto key = std::make_pair(pino, std::move(name));
                auto it = dirents_.find(key);
                if (it == dirents_.end() || it->second.version < version)
                    dirents_[key] = Dirent{version, ino, p[29]};
            }
        } else if (type == kNodeInode && totlen >= kInodeHeader) {
            uint32_t csize = le32(p + 48);
            if (util::crc32_raw(0, p, 60) != le32(p + 64) || kInodeHeader + csize > totlen) {
                ++bad_nodes_;
            } else {
                uint32_t ino = le32(p + 12), version = le32(p + 16);
                Inode& in = inodes_[ino];
                if (version >= in.version) {
                    in.version = version;
                    in.mode = le32(p + 20);
                    in.uid = le16(p + 24);
                    in.gid = le16(p + 26);
                    in.isize = le32(p + 28);
                    in.mtime = le32(p + 36);
                }
                in.nodes.push_back(DataNode{version, le32(p + 44), csize, le32(p + 52), le32(p + 28),
                                            uint32_t(off + kInodeHeader), p[56]});
            }
        }
        off += (totlen + 3) & ~size_t(3);
    }
    if (inodes_.empty() && dirents_.empty()) {
        if (err) *err = "no JFFS2 nodes found";
        return false;
    }
    for (auto& [ino, in] : inodes_)
        std::sort(in.nodes.begin(), in.nodes.end(),
                  [](const DataNode& a, const DataNode& b) { return a.version < b.version; });
    return true;
}

const std::vector<uint8_t>* Jffs2Image::contents(uint32_t ino) {
    auto cached = cache_.find(ino);
    if (cached != cache_.end()) return &cached->second;
    auto it = inodes_.find(ino);
    if (it == inodes_.end()) return nullptr;
    const Inode& in = it->second;

    std::vector<uint8_t> buf;
    std::vector<uint8_t> chunk;
    const bool sized = (in.mode & 0170000) == 0100000 || (in.mode & 0170000) == 0120000;
    for (const DataNode& dn : in.nodes) {
        // Each node records the file size as of its write, which is how
        // JFFS2 expresses truncation; data-less nodes are metadata changes.
        if (!dn.dsize) {
            if (sized) buf.resize(dn.isize);
            continue;
        }
        const uint8_t* src = image_.data() + dn.image_offset;
        if (util::crc32_raw(0, src, dn.csize) != le32(image_.data() + dn.image_offset - 8)) {
            ++bad_nodes_;
            continue;
        }
        chunk.clear();
        bool ok = true;
        switch (dn.compr) {
            case kNone: chunk.assign(src, src + std::min(dn.csize, dn.dsize)); break;
            case kZero: chunk.assign(dn.dsize, 0); break;
            case kRtime:
                chunk.resize(dn.dsize);
                ok = rtime_decompress(src, dn.csize, chunk.data(), chunk.size());
                break;
            case kZlib:
                ok = util::inflate_zlib(src, dn.csize, chunk, dn.dsize) ||
                     (chunk.clear(), util::inflate_raw(src, dn.csize, chunk, dn.dsize));
                break;
            default: ok = false; break;
        }
        if (!ok || chunk.size() != dn.dsize) {
            ++bad_nodes_;
            continue;
        }
        if (buf.size() < size_t(dn.offset) + dn.dsize) buf.resize(size_t(dn.offset) + dn.dsize);
        std::memcpy(buf.data() + dn.offset, chunk.data(), chunk.size());
        if (sized) buf.resize(dn.isize);
    }
    if (sized) buf.resize(in.isize);
    return &cache_.emplace(ino, std::move(buf)).first->second;
}

std::string Jffs2Image::describe() const {
    return "JFFS2 image, " + std::to_string(inodes_.size()) + " inodes";
}

Entry Jffs2Image::make_entry(const std::string& name, uint32_t ino, uint8_t dirent_type) {
    Entry e;
    e.name = name;
    e.id = ino;
    auto it = inodes_.find(ino);
    if (it != inodes_.end()) {
        const Inode& in = it->second;
        e.mode = in.mode;
        e.uid = in.uid;
        e.gid = in.gid;
        e.mtime = in.mtime;
        e.size = in.isize;
    } else {
        e.mode = mode_from_dirent_type(dirent_type) | 0755;
    }
    e.type = type_from_mode(e.mode);
    if (e.type == NodeType::Dir) e.size = 0;
    if (e.type == NodeType::CharDevice || e.type == NodeType::BlockDevice) {
        if (const auto* c = contents(ino)) {
            if (c->size() == 2) {  // old_encode_dev: major << 8 | minor
                uint32_t v = le16(c->data());
                e.rdev = ((v >> 8) << 8) | (v & 0xff);
            } else if (c->size() >= 4) {
                e.rdev = le32(c->data());
            }
        }
        e.size = 0;
    }
    return e;
}

Entry Jffs2Image::root() { return make_entry("/", kRootIno, 4); }

std::vector<Entry> Jffs2Image::list(const Entry& dir) {
    std::vector<Entry> out;
    uint32_t pino = uint32_t(dir.id);
    for (auto it = dirents_.lower_bound({pino, std::string()}); it != dirents_.end() && it->first.first == pino; ++it) {
        if (it->second.ino == 0) continue;  // deletion record
        out.push_back(make_entry(it->first.second, it->second.ino, it->second.type));
    }
    return out;
}

size_t Jffs2Image::read(const Entry& file, uint64_t offset, void* buf, size_t len) {
    if (file.type != NodeType::File) return 0;
    const auto* c = contents(uint32_t(file.id));
    if (!c || offset >= c->size()) return 0;
    len = size_t(std::min<uint64_t>(len, c->size() - offset));
    std::memcpy(buf, c->data() + offset, len);
    return len;
}

std::string Jffs2Image::readlink(const Entry& link) {
    if (link.type != NodeType::Symlink) return {};
    const auto* c = contents(uint32_t(link.id));
    return c ? std::string(c->begin(), c->end()) : std::string();
}

}  // namespace didj::fs
