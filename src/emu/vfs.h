// The guest's view of the filesystem: read-only dump images mounted at guest
// paths, each optionally layered under a host directory that receives writes
// (copy-up on first write), plus device nodes backed by emulated drivers.
//
// Paths are resolved here, not by the images, because symlinks may cross
// mounts (/etc/mtab -> /proc/mounts).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "fs/filesystem.h"

namespace didj::emu {

class GuestMemory;

struct StatInfo {
    uint64_t dev = 0, ino = 0;
    uint32_t mode = 0, nlink = 1, uid = 0, gid = 0;
    uint64_t rdev = 0;
    int64_t size = 0;
    uint32_t blksize = 4096;
    uint64_t blocks = 0;
    int64_t atime = 0, mtime = 0, ctime = 0;
};

struct DirItem {
    uint64_t ino;
    uint8_t type;  // DT_*
    std::string name;
};

// An open file description, shared by duplicated descriptors.
// Methods return a byte count or a negative Linux errno; kWouldBlock asks the
// caller to block the thread and retry (unless O_NONBLOCK).
class OpenFile {
public:
    static constexpr int64_t kWouldBlock = -100000;

    virtual ~OpenFile() = default;
    virtual int64_t read(void* buf, size_t len);
    virtual int64_t write(const void* buf, size_t len);
    virtual int64_t pread(void* buf, size_t len, uint64_t offset);
    virtual int64_t pwrite(const void* buf, size_t len, uint64_t offset);
    virtual int64_t seek(int64_t offset, int whence);
    virtual int fstat(StatInfo& st) = 0;
    virtual int truncate(uint64_t len);
    virtual int ioctl(uint32_t cmd, uint32_t arg, GuestMemory& mem);
    // Readiness for poll/select (POLLIN/POLLOUT bits).
    virtual int16_t poll_events() { return 0x1 | 0x4; }
    // Directories: entries not yet returned (advances the position).
    virtual bool is_dir() const { return false; }
    virtual bool next_dirent(DirItem& out) { (void)out; return false; }
    virtual void rewind_dir() {}
    // Device memory for mmap: a host block that stays alive through owner.
    virtual int mmap(uint64_t offset, uint32_t len, uint8_t** host, std::shared_ptr<void>* owner);

    std::string path;   // guest path it was opened by
    uint32_t flags = 0; // O_* status flags (access mode, O_APPEND, O_NONBLOCK)
};

using DeviceFactory = std::function<std::shared_ptr<OpenFile>(uint32_t flags)>;

class Vfs {
public:
    // Mounts an image at a guest path ("/", "/Didj", "/Cart"). If overlay is
    // non-empty, writes go to that host directory; otherwise the mount is read-only.
    void mount(const std::string& guest_path, std::shared_ptr<fs::FileSystem> image,
               const std::filesystem::path& overlay);
    // Mounts a host directory directly (writable).
    void mount_host(const std::string& guest_path, const std::filesystem::path& dir);

    // Character devices by device number, and synthetic files by path.
    void add_device(uint32_t major, uint32_t minor, DeviceFactory f);
    void add_synthetic(const std::string& path, DeviceFactory f);

    // Canonical absolute path (symlinks resolved) for a guest path.
    int resolve(const std::string& cwd, const std::string& path, bool follow_last, std::string& out);

    int open(const std::string& cwd, const std::string& path, uint32_t flags, uint32_t mode,
             std::shared_ptr<OpenFile>& out);
    int stat(const std::string& cwd, const std::string& path, bool follow, StatInfo& st);
    int readlink(const std::string& cwd, const std::string& path, std::string& target);
    int access(const std::string& cwd, const std::string& path, int mode);
    int mkdir(const std::string& cwd, const std::string& path);
    int unlink(const std::string& cwd, const std::string& path);
    int rmdir(const std::string& cwd, const std::string& path);
    int rename(const std::string& cwd, const std::string& from, const std::string& to);
    int truncate(const std::string& cwd, const std::string& path, uint64_t len);

    // Reads a whole regular file (used by the ELF loader).
    int read_file(const std::string& cwd, const std::string& path, std::vector<uint8_t>& out);

private:
    struct Mount {
        std::string prefix;  // "/" or "/Didj"
        std::shared_ptr<fs::FileSystem> image;
        std::filesystem::path overlay;
        uint32_t dev;
        std::unordered_map<std::string, std::optional<fs::Entry>> cache;  // rel path -> entry
        std::set<std::string> whiteouts;  // image files deleted through the overlay
    };
    // What a canonical path refers to.
    struct Node {
        enum Kind { Missing, Image, Host, Synthetic } kind = Missing;
        Mount* mount = nullptr;
        std::string rel;                // path relative to the mount, no leading '/'
        fs::Entry entry;                // Image
        std::filesystem::path host;     // Host
        fs::NodeType type = fs::NodeType::File;
    };

    Mount* mount_for(const std::string& canonical, std::string& rel);
    Node node_at(const std::string& canonical);
    std::optional<fs::Entry> image_entry(Mount& m, const std::string& rel);
    int stat_node(const Node& n, StatInfo& st);
    int copy_up(Node& n);
    std::vector<DirItem> list_dir(const std::string& canonical, const Node& n);

    std::vector<std::unique_ptr<Mount>> mounts_;
    std::map<uint32_t, DeviceFactory> devices_;  // key: make_dev(major, minor)
    std::map<std::string, DeviceFactory> synthetic_;
};

// Joins a possibly relative guest path onto cwd and normalizes "." and "..",
// without resolving symlinks.
std::string join_guest_path(const std::string& cwd, const std::string& path);

}  // namespace didj::emu
