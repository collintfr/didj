#include "emu/vfs.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <system_error>

#include "emu/linux_abi.h"

namespace didj::emu {

namespace stdfs = std::filesystem;
using namespace lx;

namespace {

constexpr int kMaxSymlinks = 40;

std::deque<std::string> split(const std::string& path) {
    std::deque<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t j = i;
        while (j < path.size() && path[j] != '/') ++j;
        if (j > i) parts.push_back(path.substr(i, j - i));
        i = j;
    }
    return parts;
}

std::string join_components(const std::vector<std::string>& comps) {
    std::string s;
    for (const auto& c : comps) s += "/" + c;
    return s.empty() ? "/" : s;
}

uint8_t dirent_type(fs::NodeType t) {
    switch (t) {
        case fs::NodeType::Dir: return kDT_DIR;
        case fs::NodeType::Symlink: return kDT_LNK;
        case fs::NodeType::CharDevice: return kDT_CHR;
        case fs::NodeType::BlockDevice: return kDT_BLK;
        case fs::NodeType::Fifo: return kDT_FIFO;
        case fs::NodeType::Socket: return kDT_SOCK;
        case fs::NodeType::File: return kDT_REG;
    }
    return kDT_UNKNOWN;
}

uint64_t path_ino(const std::string& s) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h & 0x7fffffff;
}

fs::NodeType host_type(const stdfs::file_status& st) {
    switch (st.type()) {
        case stdfs::file_type::directory: return fs::NodeType::Dir;
        case stdfs::file_type::symlink: return fs::NodeType::Symlink;
        default: return fs::NodeType::File;
    }
}

// --- open file implementations ---

class ImageFile : public OpenFile {
public:
    ImageFile(std::shared_ptr<fs::FileSystem> fs, fs::Entry e, StatInfo st)
        : fs_(std::move(fs)), entry_(std::move(e)), st_(st) {}
    int64_t read(void* buf, size_t len) override {
        int64_t n = pread(buf, len, pos_);
        if (n > 0) pos_ += uint64_t(n);
        return n;
    }
    int64_t pread(void* buf, size_t len, uint64_t off) override { return int64_t(fs_->read(entry_, off, buf, len)); }
    int64_t write(const void*, size_t) override { return -kEBADF; }
    int64_t seek(int64_t off, int whence) override {
        int64_t base = whence == kSEEK_SET ? 0 : whence == kSEEK_CUR ? int64_t(pos_) : int64_t(entry_.size);
        if (whence < 0 || whence > 2 || base + off < 0) return -kEINVAL;
        pos_ = uint64_t(base + off);
        return int64_t(pos_);
    }
    int fstat(StatInfo& st) override {
        st = st_;
        return 0;
    }

private:
    std::shared_ptr<fs::FileSystem> fs_;
    fs::Entry entry_;
    StatInfo st_;
    uint64_t pos_ = 0;
};

class HostFile : public OpenFile {
public:
    HostFile(std::FILE* f, stdfs::path p, uint64_t ino) : f_(f), host_(std::move(p)), ino_(ino) {}
    ~HostFile() override { std::fclose(f_); }

    int64_t read(void* buf, size_t len) override {
        int64_t n = pread(buf, len, pos_);
        if (n > 0) pos_ += uint64_t(n);
        return n;
    }
    int64_t write(const void* buf, size_t len) override {
        if (flags & kO_APPEND) pos_ = size();
        int64_t n = pwrite(buf, len, pos_);
        if (n > 0) pos_ += uint64_t(n);
        return n;
    }
    int64_t pread(void* buf, size_t len, uint64_t off) override {
        if (!seek_to(off)) return -kEIO;
        return int64_t(std::fread(buf, 1, len, f_));
    }
    int64_t pwrite(const void* buf, size_t len, uint64_t off) override {
        if ((flags & kO_ACCMODE) == kO_RDONLY) return -kEBADF;
        uint64_t sz = size();
        if (off > sz) {  // extend with zeros up to the write position
            if (!seek_to(sz)) return -kEIO;
            std::vector<char> zeros(size_t(off - sz), 0);
            std::fwrite(zeros.data(), 1, zeros.size(), f_);
        }
        if (!seek_to(off)) return -kEIO;
        size_t n = std::fwrite(buf, 1, len, f_);
        std::fflush(f_);
        return n == len ? int64_t(n) : -kENOSPC;
    }
    int64_t seek(int64_t off, int whence) override {
        int64_t base = whence == kSEEK_SET ? 0 : whence == kSEEK_CUR ? int64_t(pos_) : int64_t(size());
        if (whence < 0 || whence > 2 || base + off < 0) return -kEINVAL;
        pos_ = uint64_t(base + off);
        return int64_t(pos_);
    }
    int truncate(uint64_t len) override {
        if ((flags & kO_ACCMODE) == kO_RDONLY) return -kEINVAL;
        std::fflush(f_);
        std::error_code ec;
        stdfs::resize_file(host_, len, ec);
        return ec ? -kEIO : 0;
    }
    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.ino = ino_;
        st.mode = kS_IFREG | 0644;
        st.size = int64_t(size());
        st.blocks = uint64_t(st.size + 511) / 512;
        return 0;
    }

private:
    bool seek_to(uint64_t off) {
#ifdef _WIN32
        return _fseeki64(f_, int64_t(off), SEEK_SET) == 0;
#else
        return fseeko(f_, off_t(off), SEEK_SET) == 0;
#endif
    }
    uint64_t size() {
        std::fflush(f_);
        std::error_code ec;
        auto s = stdfs::file_size(host_, ec);
        return ec ? 0 : uint64_t(s);
    }

    std::FILE* f_;
    stdfs::path host_;
    uint64_t ino_;
    uint64_t pos_ = 0;
};

class DirFile : public OpenFile {
public:
    DirFile(std::vector<DirItem> items, StatInfo st) : items_(std::move(items)), st_(st) {}
    bool is_dir() const override { return true; }
    bool next_dirent(DirItem& out) override {
        if (pos_ >= items_.size()) return false;
        out = items_[pos_++];
        return true;
    }
    void rewind_dir() override { pos_ = 0; }
    int64_t read(void*, size_t) override { return -kEISDIR; }
    int64_t write(const void*, size_t) override { return -kEBADF; }
    int64_t seek(int64_t off, int whence) override {
        // Positions are entry indexes, as getdents' d_off reports them.
        int64_t base = whence == kSEEK_SET ? 0 : whence == kSEEK_CUR ? int64_t(pos_) : -1;
        if (base < 0 || base + off < 0) return -kEINVAL;
        pos_ = size_t(base + off);
        return int64_t(pos_);
    }
    int fstat(StatInfo& st) override {
        st = st_;
        return 0;
    }

private:
    std::vector<DirItem> items_;
    StatInfo st_;
    size_t pos_ = 0;
};

}  // namespace

// --- OpenFile defaults ---

int64_t OpenFile::read(void*, size_t) { return -kEINVAL; }
int64_t OpenFile::write(const void*, size_t) { return -kEINVAL; }
int64_t OpenFile::pread(void*, size_t, uint64_t) { return -kESPIPE; }
int64_t OpenFile::pwrite(const void*, size_t, uint64_t) { return -kESPIPE; }
int64_t OpenFile::seek(int64_t, int) { return -kESPIPE; }
int OpenFile::truncate(uint64_t) { return -kEINVAL; }
int OpenFile::ioctl(uint32_t, uint32_t, GuestMemory&) { return -kENOTTY; }
int OpenFile::mmap(uint64_t, uint32_t, uint8_t**, std::shared_ptr<void>*) { return -kENODEV; }

// --- Vfs ---

std::string join_guest_path(const std::string& cwd, const std::string& path) {
    std::vector<std::string> comps;
    for (auto& p : split(!path.empty() && path[0] == '/' ? path : cwd + "/" + path)) {
        if (p == ".") continue;
        if (p == "..") {
            if (!comps.empty()) comps.pop_back();
            continue;
        }
        comps.push_back(p);
    }
    return join_components(comps);
}

void Vfs::mount(const std::string& guest_path, std::shared_ptr<fs::FileSystem> image, const stdfs::path& overlay) {
    auto m = std::make_unique<Mount>();
    m->prefix = join_guest_path("/", guest_path);
    m->image = std::move(image);
    m->overlay = overlay;
    m->dev = uint32_t(mounts_.size() + 1);
    mounts_.push_back(std::move(m));
}

void Vfs::mount_host(const std::string& guest_path, const stdfs::path& dir) {
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    mount(guest_path, nullptr, dir);
}

void Vfs::add_device(uint32_t major, uint32_t minor, DeviceFactory f) { devices_[make_dev(major, minor)] = std::move(f); }

void Vfs::add_synthetic(const std::string& path, DeviceFactory f) { synthetic_[path] = std::move(f); }

Vfs::Mount* Vfs::mount_for(const std::string& canonical, std::string& rel) {
    Mount* best = nullptr;
    for (auto& m : mounts_) {
        const std::string& p = m->prefix;
        bool match = p == "/" || canonical == p || (canonical.size() > p.size() && canonical.compare(0, p.size(), p) == 0 &&
                                                    canonical[p.size()] == '/');
        if (match && (!best || p.size() > best->prefix.size())) best = m.get();
    }
    if (best) {
        size_t skip = best->prefix == "/" ? 1 : best->prefix.size() + 1;
        rel = canonical.size() > skip ? canonical.substr(skip) : std::string();
    }
    return best;
}

std::optional<fs::Entry> Vfs::image_entry(Mount& m, const std::string& rel) {
    auto it = m.cache.find(rel);
    if (it != m.cache.end()) return it->second;
    auto e = m.image->lookup(rel, /*follow_last=*/false);
    m.cache.emplace(rel, e);
    return e;
}

Vfs::Node Vfs::node_at(const std::string& canonical) {
    Node n;
    if (synthetic_.count(canonical)) {
        n.kind = Node::Synthetic;
        n.rel = canonical;
        return n;
    }
    n.mount = mount_for(canonical, n.rel);
    if (!n.mount) return n;
    Mount& m = *n.mount;
    if (!m.overlay.empty()) {
        stdfs::path host = n.rel.empty() ? m.overlay : m.overlay / stdfs::u8path(n.rel);
        std::error_code ec;
        auto st = stdfs::symlink_status(host, ec);
        bool exists = !ec && st.type() != stdfs::file_type::not_found;
        // A directory in both layers is reported as the image's, and merged when listed.
        if (exists && !(st.type() == stdfs::file_type::directory && m.image && image_entry(m, n.rel))) {
            n.kind = Node::Host;
            n.host = host;
            n.type = host_type(st);
            return n;
        }
    }
    if (!m.image || m.whiteouts.count(n.rel)) return n;
    if (auto e = image_entry(m, n.rel)) {
        n.kind = Node::Image;
        n.entry = *e;
        n.type = e->type;
    }
    return n;
}

int Vfs::resolve(const std::string& cwd, const std::string& path, bool follow_last, std::string& out) {
    if (path.empty()) return -kENOENT;
    std::deque<std::string> todo = split(path[0] == '/' ? path : cwd + "/" + path);
    std::vector<std::string> comps;
    int links = 0;
    while (!todo.empty()) {
        std::string part = std::move(todo.front());
        todo.pop_front();
        if (part == ".") continue;
        if (part == "..") {
            if (!comps.empty()) comps.pop_back();
            continue;
        }
        comps.push_back(part);
        std::string cand = join_components(comps);
        bool last = todo.empty();
        Node n = node_at(cand);
        if (n.kind == Node::Missing) {
            if (last) break;  // the caller may be creating it
            return -kENOENT;
        }
        if (n.type == fs::NodeType::Symlink && (!last || follow_last)) {
            if (++links > kMaxSymlinks) return -kELOOP;
            std::string target;
            if (n.kind != Node::Image || (target = n.mount->image->readlink(n.entry)).empty()) return -kENOENT;
            comps.pop_back();
            if (target[0] == '/') comps.clear();
            auto parts = split(target);
            todo.insert(todo.begin(), parts.begin(), parts.end());
            continue;
        }
        if (!last && n.type != fs::NodeType::Dir) return -kENOTDIR;
    }
    out = join_components(comps);
    return 0;
}

int Vfs::stat_node(const Node& n, StatInfo& st) {
    st = StatInfo();
    switch (n.kind) {
        case Node::Missing: return -kENOENT;
        case Node::Synthetic: {
            auto f = synthetic_[n.rel](kO_RDONLY);
            return f ? f->fstat(st) : -kENOENT;
        }
        case Node::Image: {
            const fs::Entry& e = n.entry;
            st.dev = n.mount->dev;
            st.ino = e.id ? e.id : path_ino(n.rel);
            st.mode = e.mode;
            st.uid = e.uid;
            st.gid = e.gid;
            st.rdev = e.rdev;
            st.size = int64_t(e.size);
            st.nlink = e.is_dir() ? 2 : 1;
            st.blocks = (e.size + 511) / 512;
            st.atime = st.mtime = st.ctime = e.mtime;
            return 0;
        }
        case Node::Host: {
            std::error_code ec;
            st.dev = n.mount->dev;
            st.ino = path_ino(n.host.u8string());
            if (n.type == fs::NodeType::Dir) {
                st.mode = kS_IFDIR | 0755;
                st.nlink = 2;
            } else {
                st.mode = kS_IFREG | 0644;
                st.size = int64_t(stdfs::file_size(n.host, ec));
                if (ec) st.size = 0;
                st.blocks = uint64_t(st.size + 511) / 512;
            }
            return 0;
        }
    }
    return -kENOENT;
}

std::vector<DirItem> Vfs::list_dir(const std::string& canonical, const Node& n) {
    std::map<std::string, DirItem> items;
    if (n.kind == Node::Image) {
        for (const auto& e : n.mount->image->list(n.entry)) {
            std::string rel = n.rel.empty() ? e.name : n.rel + "/" + e.name;
            if (n.mount->whiteouts.count(rel)) continue;
            items[e.name] = DirItem{e.id ? e.id : path_ino(rel), dirent_type(e.type), e.name};
        }
    }
    if (n.mount && !n.mount->overlay.empty()) {
        stdfs::path dir = n.rel.empty() ? n.mount->overlay : n.mount->overlay / stdfs::u8path(n.rel);
        std::error_code ec;
        for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name = it->path().filename().u8string();
            uint8_t t = it->is_directory(ec) ? kDT_DIR : kDT_REG;
            items[name] = DirItem{path_ino(canonical + "/" + name), t, name};
        }
    }
    std::vector<DirItem> out;
    out.push_back(DirItem{path_ino(canonical), kDT_DIR, "."});
    out.push_back(DirItem{path_ino(canonical + "/.."), kDT_DIR, ".."});
    for (auto& [name, item] : items) out.push_back(std::move(item));
    return out;
}

int Vfs::copy_up(Node& n) {
    if (n.kind == Node::Host) return 0;
    if (!n.mount || n.mount->overlay.empty()) return -kEROFS;
    stdfs::path host = n.mount->overlay / stdfs::u8path(n.rel);
    std::error_code ec;
    stdfs::create_directories(host.parent_path(), ec);
    if (ec) return -kEIO;
    if (n.kind == Node::Image && n.type == fs::NodeType::File) {
        std::vector<uint8_t> data = n.mount->image->read_all(n.entry);
        std::FILE* f = std::fopen(host.u8string().c_str(), "wb");
        if (!f) return -kEIO;
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
    }
    n.kind = Node::Host;
    n.host = host;
    return 0;
}

int Vfs::open(const std::string& cwd, const std::string& path, uint32_t flags, uint32_t mode,
              std::shared_ptr<OpenFile>& out) {
    (void)mode;
    std::string canon;
    int r = resolve(cwd, path, !(flags & kO_NOFOLLOW), canon);
    if (r < 0) return r;
    Node n = node_at(canon);
    const uint32_t acc = flags & kO_ACCMODE;
    const bool wants_write = acc != kO_RDONLY || (flags & kO_TRUNC);

    if (n.kind == Node::Missing) {
        if (!(flags & kO_CREAT)) return -kENOENT;
        std::string parent = canon.substr(0, canon.rfind('/'));
        Node pn = node_at(parent.empty() ? "/" : parent);
        if (pn.kind == Node::Missing) return -kENOENT;
        if (pn.type != fs::NodeType::Dir) return -kENOTDIR;
        if (!n.mount || n.mount->overlay.empty()) return -kEROFS;
        n.mount->whiteouts.erase(n.rel);
        stdfs::path host = n.mount->overlay / stdfs::u8path(n.rel);
        std::error_code ec;
        stdfs::create_directories(host.parent_path(), ec);
        std::FILE* f = std::fopen(host.u8string().c_str(), "w+b");
        if (!f) return -kEIO;
        auto hf = std::make_shared<HostFile>(f, host, path_ino(canon));
        hf->path = canon;
        hf->flags = flags;
        out = hf;
        return 0;
    }
    if ((flags & kO_CREAT) && (flags & kO_EXCL)) return -kEEXIST;
    if (n.kind == Node::Synthetic) {
        out = synthetic_[canon](flags);
        if (!out) return -kENOENT;
        out->path = canon;
        out->flags = flags;
        return 0;
    }
    if (n.type == fs::NodeType::Symlink) return -kELOOP;  // O_NOFOLLOW on a link
    if ((flags & kO_DIRECTORY) && n.type != fs::NodeType::Dir) return -kENOTDIR;

    switch (n.type) {
        case fs::NodeType::Dir: {
            if (acc != kO_RDONLY) return -kEISDIR;
            StatInfo st;
            stat_node(n, st);
            out = std::make_shared<DirFile>(list_dir(canon, n), st);
            break;
        }
        case fs::NodeType::CharDevice:
        case fs::NodeType::BlockDevice: {
            auto it = devices_.find(uint32_t(n.entry.rdev));
            if (it == devices_.end()) {
                std::fprintf(stderr, "[vfs] no driver for %s (%u, %u)\n", canon.c_str(), dev_major(n.entry.rdev),
                             dev_minor(n.entry.rdev));
                return -kENXIO;
            }
            out = it->second(flags);
            if (!out) return -kENXIO;
            break;
        }
        case fs::NodeType::File: {
            if (wants_write) {
                r = copy_up(n);
                if (r < 0) return r;
            }
            if (n.kind == Node::Image) {
                StatInfo st;
                stat_node(n, st);
                out = std::make_shared<ImageFile>(n.mount->image, n.entry, st);
            } else {
                const char* m = acc == kO_RDONLY ? "rb" : "r+b";
                std::FILE* f = std::fopen(n.host.u8string().c_str(), m);
                if (!f) return -kEACCES;
                auto hf = std::make_shared<HostFile>(f, n.host, path_ino(canon));
                hf->flags = flags;
                if (flags & kO_TRUNC) hf->truncate(0);
                out = hf;
            }
            break;
        }
        default: return -kENXIO;
    }
    out->path = canon;
    out->flags = flags;
    return 0;
}

int Vfs::stat(const std::string& cwd, const std::string& path, bool follow, StatInfo& st) {
    std::string canon;
    int r = resolve(cwd, path, follow, canon);
    if (r < 0) return r;
    return stat_node(node_at(canon), st);
}

int Vfs::readlink(const std::string& cwd, const std::string& path, std::string& target) {
    std::string canon;
    int r = resolve(cwd, path, false, canon);
    if (r < 0) return r;
    Node n = node_at(canon);
    if (n.kind == Node::Missing) return -kENOENT;
    if (n.type != fs::NodeType::Symlink || n.kind != Node::Image) return -kEINVAL;
    target = n.mount->image->readlink(n.entry);
    return 0;
}

int Vfs::access(const std::string& cwd, const std::string& path, int mode) {
    std::string canon;
    int r = resolve(cwd, path, true, canon);
    if (r < 0) return r;
    Node n = node_at(canon);
    if (n.kind == Node::Missing) return -kENOENT;
    if ((mode & kW_OK) && n.kind == Node::Image && n.mount->overlay.empty()) return -kEROFS;
    return 0;
}

int Vfs::mkdir(const std::string& cwd, const std::string& path) {
    std::string canon;
    int r = resolve(cwd, path, false, canon);
    if (r < 0) return r;
    Node n = node_at(canon);
    if (n.kind != Node::Missing) return -kEEXIST;
    std::string parent = canon.substr(0, canon.rfind('/'));
    Node pn = node_at(parent.empty() ? "/" : parent);
    if (pn.kind == Node::Missing) return -kENOENT;
    if (!n.mount || n.mount->overlay.empty()) return -kEROFS;
    n.mount->whiteouts.erase(n.rel);
    std::error_code ec;
    stdfs::create_directories(n.mount->overlay / stdfs::u8path(n.rel), ec);
    return ec ? -kEIO : 0;
}

int Vfs::unlink(const std::string& cwd, const std::string& path) {
    std::string canon;
    int r = resolve(cwd, path, false, canon);
    if (r < 0) return r;
    Node n = node_at(canon);
    if (n.kind == Node::Missing) return -kENOENT;
    if (n.type == fs::NodeType::Dir) return -kEISDIR;
    if (!n.mount || n.mount->overlay.empty()) return -kEROFS;
    if (n.kind == Node::Host) {
        std::error_code ec;
        stdfs::remove(n.host, ec);
    }
    if (n.mount->image && image_entry(*n.mount, n.rel)) n.mount->whiteouts.insert(n.rel);
    return 0;
}

int Vfs::rmdir(const std::string& cwd, const std::string& path) {
    std::string canon;
    int r = resolve(cwd, path, false, canon);
    if (r < 0) return r;
    Node n = node_at(canon);
    if (n.kind == Node::Missing) return -kENOENT;
    if (n.type != fs::NodeType::Dir) return -kENOTDIR;
    if (list_dir(canon, n).size() > 2) return -kENOTEMPTY;
    if (!n.mount || n.mount->overlay.empty()) return -kEROFS;
    std::error_code ec;
    stdfs::remove(n.mount->overlay / stdfs::u8path(n.rel), ec);
    if (n.mount->image && image_entry(*n.mount, n.rel)) n.mount->whiteouts.insert(n.rel);
    return 0;
}

int Vfs::rename(const std::string& cwd, const std::string& from, const std::string& to) {
    std::string src, dst;
    int r = resolve(cwd, from, false, src);
    if (r < 0) return r;
    r = resolve(cwd, to, false, dst);
    if (r < 0) return r;
    Node sn = node_at(src), dn = node_at(dst);
    if (sn.kind == Node::Missing) return -kENOENT;
    if (sn.type == fs::NodeType::Dir) return -kEXDEV;  // directory renames are not needed by the Didj software
    if (sn.mount != dn.mount || !dn.mount || dn.mount->overlay.empty()) return -kEXDEV;
    r = copy_up(sn);
    if (r < 0) return r;
    stdfs::path target = dn.mount->overlay / stdfs::u8path(dn.rel);
    std::error_code ec;
    stdfs::create_directories(target.parent_path(), ec);
    stdfs::rename(sn.host, target, ec);
    if (ec) return -kEIO;
    if (sn.mount->image && image_entry(*sn.mount, sn.rel)) sn.mount->whiteouts.insert(sn.rel);
    dn.mount->whiteouts.erase(dn.rel);
    return 0;
}

int Vfs::truncate(const std::string& cwd, const std::string& path, uint64_t len) {
    std::shared_ptr<OpenFile> f;
    int r = open(cwd, path, kO_WRONLY, 0, f);
    if (r < 0) return r;
    return f->truncate(len);
}

int Vfs::read_file(const std::string& cwd, const std::string& path, std::vector<uint8_t>& out) {
    std::shared_ptr<OpenFile> f;
    int r = open(cwd, path, kO_RDONLY, 0, f);
    if (r < 0) return r;
    StatInfo st;
    f->fstat(st);
    if ((st.mode & kS_IFMT) != kS_IFREG) return -kEACCES;
    out.resize(size_t(st.size));
    int64_t n = f->pread(out.data(), out.size(), 0);
    if (n != st.size) return -kEIO;
    return 0;
}

}  // namespace didj::emu
