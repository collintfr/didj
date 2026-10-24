#include "emu/dev/basic.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

#include "emu/linux_abi.h"
#include "emu/memory.h"

namespace didj::emu {

using namespace lx;

namespace {

class CharDevice : public OpenFile {
public:
    explicit CharDevice(uint32_t rdev) : rdev_(rdev) {}
    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.mode = kS_IFCHR | 0666;
        st.rdev = rdev_;
        return 0;
    }

private:
    uint32_t rdev_;
};

class NullDevice : public CharDevice {
public:
    using CharDevice::CharDevice;
    int64_t read(void*, size_t) override { return 0; }
    int64_t write(const void*, size_t len) override { return int64_t(len); }
};

class ZeroDevice : public CharDevice {
public:
    using CharDevice::CharDevice;
    int64_t read(void* buf, size_t len) override {
        std::memset(buf, 0, len);
        return int64_t(len);
    }
    int64_t write(const void*, size_t len) override { return int64_t(len); }
    int mmap(uint64_t, uint32_t len, uint8_t** host, std::shared_ptr<void>* owner) override {
        auto block = std::shared_ptr<uint8_t>(new uint8_t[len](), std::default_delete<uint8_t[]>());
        *host = block.get();
        *owner = block;
        return 0;
    }
};

class RandomDevice : public CharDevice {
public:
    using CharDevice::CharDevice;
    int64_t read(void* buf, size_t len) override {
        auto* p = static_cast<uint8_t*>(buf);
        for (size_t i = 0; i < len; ++i) p[i] = uint8_t(rng_());
        return int64_t(len);
    }
    int64_t write(const void*, size_t len) override { return int64_t(len); }

private:
    std::mt19937 rng_{std::random_device{}()};
};

// The Didj's serial console, connected to the host's stdin/stdout/stderr.
class Console : public CharDevice {
public:
    explicit Console(int stream) : CharDevice(make_dev(5, 1)), stream_(stream) {}
    int64_t read(void* buf, size_t len) override {
        // Line-at-a-time like a cooked tty.
        auto* p = static_cast<char*>(buf);
        size_t n = 0;
        while (n < len) {
            int c = std::fgetc(stdin);
            if (c == EOF) break;
            p[n++] = char(c);
            if (c == '\n') break;
        }
        return int64_t(n);
    }
    int64_t write(const void* buf, size_t len) override {
        std::FILE* out = stream_ == 2 ? stderr : stdout;
        std::fwrite(buf, 1, len, out);
        std::fflush(out);
        return int64_t(len);
    }
    int ioctl(uint32_t cmd, uint32_t arg, GuestMemory& mem) override {
        switch (cmd) {
            case 0x5401: {  // TCGETS: a cooked, echoing 115200 8N1 terminal
                uint8_t t[36] = {};
                uint32_t iflag = 0x500 /*ICRNL|IXON*/, oflag = 0x5 /*OPOST|ONLCR*/, cflag = 0x10b2,
                         lflag = 0x8a3b /*ISIG|ICANON|ECHO...*/;
                std::memcpy(t, &iflag, 4);
                std::memcpy(t + 4, &oflag, 4);
                std::memcpy(t + 8, &cflag, 4);
                std::memcpy(t + 12, &lflag, 4);
                return mem.write(arg, t, sizeof t) ? 0 : -kEFAULT;
            }
            case 0x5402: case 0x5403: case 0x5404: return 0;  // TCSETS*
            case 0x540f: return mem.write_u32(arg, 100) ? 0 : -kEFAULT;  // TIOCGPGRP
            case 0x5410: return 0;                                       // TIOCSPGRP
            case 0x5413: {  // TIOCGWINSZ
                uint16_t ws[4] = {24, 80, 0, 0};
                return mem.write(arg, ws, sizeof ws) ? 0 : -kEFAULT;
            }
            default: return -kENOTTY;
        }
    }

private:
    int stream_;
};

class TextFile : public OpenFile {
public:
    explicit TextFile(std::string text, uint64_t ino = 0) : text_(std::move(text)), ino_(ino) {}
    int64_t read(void* buf, size_t len) override {
        int64_t n = pread(buf, len, pos_);
        pos_ += uint64_t(n);
        return n;
    }
    int64_t pread(void* buf, size_t len, uint64_t off) override {
        if (off >= text_.size()) return 0;
        size_t n = std::min(len, size_t(text_.size() - off));
        std::memcpy(buf, text_.data() + off, n);
        return int64_t(n);
    }
    int64_t seek(int64_t off, int whence) override {
        int64_t base = whence == kSEEK_SET ? 0 : whence == kSEEK_CUR ? int64_t(pos_) : int64_t(text_.size());
        if (base + off < 0) return -kEINVAL;
        pos_ = uint64_t(base + off);
        return int64_t(pos_);
    }
    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.mode = kS_IFREG | 0444;
        if (ino_) {
            // A file with an identity: the dynamic linker tells libraries apart by dev/ino.
            st.dev = 0xd1d, st.ino = ino_;
            st.size = int64_t(text_.size());
            st.blocks = (text_.size() + 511) / 512;
        } else {
            st.size = 0;  // like procfs
        }
        return 0;
    }

private:
    std::string text_;
    uint64_t ino_;
    uint64_t pos_ = 0;
};

class StubDevice : public CharDevice {
public:
    StubDevice(std::string path, uint32_t rdev) : CharDevice(rdev), name_(std::move(path)) {}
    int64_t read(void*, size_t) override { return 0; }
    int64_t write(const void*, size_t len) override { return int64_t(len); }
    int ioctl(uint32_t cmd, uint32_t arg, GuestMemory& mem) override {
        uint32_t word = 0;
        bool readable = mem.read_u32(arg, word);
        std::fprintf(stderr, "[stub] %s ioctl 0x%08x (dir %u, '%c' nr %u, size %u) arg 0x%08x%s%08x\n", name_.c_str(),
                     cmd, cmd >> 30, int((cmd >> 8) & 0xff) >= 32 ? int((cmd >> 8) & 0xff) : '?', cmd & 0xff,
                     (cmd >> 16) & 0x3fff, arg, readable ? " -> " : " ", readable ? word : 0);
        return 0;
    }
    int mmap(uint64_t offset, uint32_t len, uint8_t** host, std::shared_ptr<void>* owner) override {
        std::fprintf(stderr, "[stub] %s mmap offset 0x%llx len 0x%x\n", name_.c_str(), (unsigned long long)offset, len);
        auto block = std::shared_ptr<uint8_t>(new uint8_t[len](), std::default_delete<uint8_t[]>());
        *host = block.get();
        *owner = block;
        return 0;
    }

private:
    std::string name_;
};

template <typename T>
DeviceFactory device(uint32_t major, uint32_t minor) {
    return [=](uint32_t) { return std::make_shared<T>(make_dev(major, minor)); };
}

}  // namespace

std::shared_ptr<OpenFile> make_console(int host_stream) {
    auto c = std::make_shared<Console>(host_stream);
    c->path = "/dev/console";
    c->flags = host_stream == 0 ? kO_RDONLY : kO_WRONLY;
    return c;
}

std::shared_ptr<OpenFile> make_stub_device(const std::string& path, uint32_t rdev, uint32_t flags) {
    auto d = std::make_shared<StubDevice>(path, rdev);
    d->flags = flags;
    return d;
}

std::shared_ptr<OpenFile> make_text_file(std::string text) { return std::make_shared<TextFile>(std::move(text)); }

std::shared_ptr<OpenFile> make_memory_file(std::string contents, uint64_t ino) {
    return std::make_shared<TextFile>(std::move(contents), ino);
}

void register_basic_devices(Vfs& vfs) {
    vfs.add_device(1, 3, device<NullDevice>(1, 3));
    vfs.add_device(1, 5, device<ZeroDevice>(1, 5));
    vfs.add_device(1, 8, device<RandomDevice>(1, 8));
    vfs.add_device(1, 9, device<RandomDevice>(1, 9));
    auto console = [](uint32_t flags) {
        auto c = make_console((flags & kO_ACCMODE) == kO_RDONLY ? 0 : 1);
        return c;
    };
    vfs.add_device(5, 0, console);   // /dev/tty
    vfs.add_device(5, 1, console);   // /dev/console
    vfs.add_device(4, 64, console);  // /dev/ttyS0

    vfs.add_synthetic("/proc/cpuinfo", [](uint32_t) {
        return make_text_file(
            "Processor\t: ARM926EJ-S rev 5 (v5l)\n"
            "BogoMIPS\t: 196.19\n"
            "Features\t: swp half thumb fastmult edsp java \n"
            "CPU implementer\t: 0x41\nCPU architecture: 5TEJ\nCPU variant\t: 0x0\nCPU part\t: 0x926\n"
            "CPU revision\t: 5\n\nHardware\t: LeapFrog LF1000\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n");
    });
    vfs.add_synthetic("/proc/meminfo", [](uint32_t) {
        return make_text_file("MemTotal:        30080 kB\nMemFree:         16384 kB\nBuffers:             0 kB\n"
                              "Cached:           4096 kB\n");
    });
    vfs.add_synthetic("/proc/mounts", [](uint32_t) {
        return make_text_file("rootfs / rootfs rw 0 0\n/dev/mtdblock2 / jffs2 ro 0 0\n"
                              "/dev/mtdblock6 /Didj vfat rw 0 0\n/dev/cart /Cart vfat ro 0 0\n"
                              "tmpfs /tmp tmpfs rw 0 0\n");
    });
}

}  // namespace didj::emu
