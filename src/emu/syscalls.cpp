#include "emu/syscalls.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>

#include "emu/linux_abi.h"
#include "emu/loader.h"

namespace didj::emu {

using namespace lx;

namespace {

// ARM EABI syscall numbers (2.6.20).
enum : uint32_t {
    NR_restart_syscall = 0, NR_exit = 1, NR_fork = 2, NR_read = 3, NR_write = 4, NR_open = 5, NR_close = 6,
    NR_creat = 8, NR_link = 9, NR_unlink = 10, NR_execve = 11, NR_chdir = 12, NR_mknod = 14, NR_chmod = 15,
    NR_lseek = 19, NR_getpid = 20, NR_mount = 21, NR_setuid = 23, NR_getuid = 24, NR_pause = 29,
    NR_access = 33, NR_nice = 34, NR_sync = 36, NR_kill = 37, NR_rename = 38, NR_mkdir = 39, NR_rmdir = 40,
    NR_dup = 41, NR_pipe = 42, NR_times = 43, NR_brk = 45, NR_setgid = 46, NR_getgid = 47, NR_geteuid = 49,
    NR_getegid = 50, NR_umount2 = 52, NR_ioctl = 54, NR_fcntl = 55, NR_setpgid = 57, NR_umask = 60,
    NR_dup2 = 63, NR_getppid = 64, NR_getpgrp = 65, NR_setsid = 66, NR_sethostname = 74, NR_setrlimit = 75,
    NR_getrusage = 77, NR_gettimeofday = 78, NR_settimeofday = 79, NR_getgroups = 80, NR_symlink = 83,
    NR_readlink = 85, NR_reboot = 88, NR_munmap = 91, NR_truncate = 92, NR_ftruncate = 93, NR_fchmod = 94,
    NR_fchown = 95, NR_getpriority = 96, NR_setpriority = 97, NR_statfs = 99, NR_fstatfs = 100,
    NR_syslog = 103, NR_setitimer = 104, NR_getitimer = 105, NR_stat = 106, NR_lstat = 107, NR_fstat = 108,
    NR_wait4 = 114, NR_sysinfo = 116, NR_fsync = 118, NR_sigreturn = 119, NR_clone = 120, NR_uname = 122,
    NR_mprotect = 125, NR_getpgid = 132, NR_fchdir = 133, NR_personality = 136, NR__llseek = 140,
    NR_getdents = 141, NR__newselect = 142, NR_flock = 143, NR_msync = 144, NR_readv = 145, NR_writev = 146,
    NR_getsid = 147, NR_fdatasync = 148, NR_mlock = 150, NR_munlock = 151, NR_mlockall = 152,
    NR_munlockall = 153, NR_sched_setparam = 154, NR_sched_getparam = 155, NR_sched_setscheduler = 156,
    NR_sched_getscheduler = 157, NR_sched_yield = 158, NR_sched_get_priority_max = 159,
    NR_sched_get_priority_min = 160, NR_nanosleep = 162, NR_mremap = 163, NR_poll = 168, NR_prctl = 172,
    NR_rt_sigreturn = 173, NR_rt_sigaction = 174, NR_rt_sigprocmask = 175, NR_rt_sigpending = 176,
    NR_rt_sigtimedwait = 177, NR_rt_sigsuspend = 179, NR_pread64 = 180, NR_pwrite64 = 181, NR_chown = 182,
    NR_getcwd = 183, NR_sigaltstack = 186, NR_vfork = 190, NR_ugetrlimit = 191, NR_mmap2 = 192,
    NR_truncate64 = 193, NR_ftruncate64 = 194, NR_stat64 = 195, NR_lstat64 = 196, NR_fstat64 = 197,
    NR_lchown32 = 198, NR_getuid32 = 199, NR_getgid32 = 200, NR_geteuid32 = 201, NR_getegid32 = 202,
    NR_getgroups32 = 205, NR_fchown32 = 207, NR_getresuid32 = 209, NR_getresgid32 = 211, NR_chown32 = 212,
    NR_setuid32 = 213, NR_setgid32 = 214, NR_getdents64 = 217, NR_madvise = 220, NR_fcntl64 = 221,
    NR_gettid = 224, NR_tkill = 238, NR_futex = 240, NR_sched_setaffinity = 241, NR_sched_getaffinity = 242,
    NR_exit_group = 248, NR_set_tid_address = 256, NR_clock_settime = 262, NR_clock_gettime = 263,
    NR_clock_getres = 264, NR_clock_nanosleep = 265, NR_statfs64 = 266, NR_fstatfs64 = 267, NR_tgkill = 268,
    NR_utimes = 269, NR_fadvise64_64 = 270, NR_socket = 281, NR_set_robust_list = 338,
    NR_ARM_cacheflush = 0xf0002, NR_ARM_set_tls = 0xf0005,
};

// clone flags
constexpr uint32_t kCLONE_VM = 0x100, kCLONE_SETTLS = 0x80000, kCLONE_PARENT_SETTID = 0x100000,
                   kCLONE_CHILD_CLEARTID = 0x200000, kCLONE_CHILD_SETTID = 0x1000000;

// mmap
constexpr uint32_t kMAP_SHARED = 1, kMAP_FIXED = 0x10, kMAP_ANONYMOUS = 0x20;

const std::map<uint32_t, const char*>& syscall_names() {
    static const std::map<uint32_t, const char*> names = {
        {0, "restart_syscall"}, {1, "exit"}, {2, "fork"}, {3, "read"}, {4, "write"}, {5, "open"}, {6, "close"},
        {8, "creat"}, {9, "link"}, {10, "unlink"}, {11, "execve"}, {12, "chdir"}, {14, "mknod"}, {15, "chmod"},
        {19, "lseek"}, {20, "getpid"}, {21, "mount"}, {23, "setuid"}, {24, "getuid"}, {29, "pause"},
        {33, "access"}, {36, "sync"}, {37, "kill"}, {38, "rename"}, {39, "mkdir"}, {40, "rmdir"}, {41, "dup"},
        {42, "pipe"}, {43, "times"}, {45, "brk"}, {46, "setgid"}, {47, "getgid"}, {49, "geteuid"},
        {50, "getegid"}, {52, "umount2"}, {54, "ioctl"}, {55, "fcntl"}, {57, "setpgid"}, {60, "umask"},
        {63, "dup2"}, {64, "getppid"}, {65, "getpgrp"}, {66, "setsid"}, {75, "setrlimit"}, {77, "getrusage"},
        {78, "gettimeofday"}, {79, "settimeofday"}, {80, "getgroups"}, {83, "symlink"}, {85, "readlink"},
        {88, "reboot"}, {91, "munmap"}, {92, "truncate"}, {93, "ftruncate"}, {94, "fchmod"}, {95, "fchown"},
        {96, "getpriority"}, {97, "setpriority"}, {99, "statfs"}, {100, "fstatfs"}, {103, "syslog"},
        {104, "setitimer"}, {105, "getitimer"}, {106, "stat"}, {107, "lstat"}, {108, "fstat"}, {114, "wait4"},
        {116, "sysinfo"}, {118, "fsync"}, {119, "sigreturn"}, {120, "clone"}, {122, "uname"},
        {125, "mprotect"}, {132, "getpgid"}, {133, "fchdir"}, {136, "personality"}, {140, "_llseek"},
        {141, "getdents"}, {142, "_newselect"}, {143, "flock"}, {144, "msync"}, {145, "readv"},
        {146, "writev"}, {147, "getsid"}, {148, "fdatasync"}, {150, "mlock"}, {151, "munlock"},
        {152, "mlockall"}, {153, "munlockall"}, {154, "sched_setparam"}, {155, "sched_getparam"},
        {156, "sched_setscheduler"}, {157, "sched_getscheduler"}, {158, "sched_yield"},
        {159, "sched_get_priority_max"}, {160, "sched_get_priority_min"}, {162, "nanosleep"},
        {163, "mremap"}, {168, "poll"}, {172, "prctl"}, {173, "rt_sigreturn"}, {174, "rt_sigaction"},
        {175, "rt_sigprocmask"}, {176, "rt_sigpending"}, {177, "rt_sigtimedwait"}, {179, "rt_sigsuspend"},
        {180, "pread64"}, {181, "pwrite64"}, {182, "chown"}, {183, "getcwd"}, {186, "sigaltstack"},
        {190, "vfork"}, {191, "ugetrlimit"}, {192, "mmap2"}, {193, "truncate64"}, {194, "ftruncate64"},
        {195, "stat64"}, {196, "lstat64"}, {197, "fstat64"}, {198, "lchown32"}, {199, "getuid32"},
        {200, "getgid32"}, {201, "geteuid32"}, {202, "getegid32"}, {205, "getgroups32"}, {207, "fchown32"},
        {209, "getresuid32"}, {211, "getresgid32"}, {212, "chown32"}, {213, "setuid32"}, {214, "setgid32"},
        {217, "getdents64"}, {220, "madvise"}, {221, "fcntl64"}, {224, "gettid"}, {238, "tkill"},
        {240, "futex"}, {241, "sched_setaffinity"}, {242, "sched_getaffinity"}, {248, "exit_group"},
        {256, "set_tid_address"}, {257, "timer_create"}, {258, "timer_settime"}, {259, "timer_gettime"},
        {260, "timer_getoverrun"}, {261, "timer_delete"}, {262, "clock_settime"}, {263, "clock_gettime"},
        {264, "clock_getres"}, {265, "clock_nanosleep"}, {266, "statfs64"}, {267, "fstatfs64"},
        {268, "tgkill"}, {269, "utimes"}, {270, "fadvise64_64"}, {274, "mq_open"}, {275, "mq_unlink"},
        {276, "mq_timedsend"}, {277, "mq_timedreceive"}, {278, "mq_notify"}, {279, "mq_getsetattr"},
        {281, "socket"}, {282, "bind"}, {283, "connect"}, {284, "listen"}, {285, "accept"},
        {289, "send"}, {291, "recv"}, {338, "set_robust_list"},
        {0xf0002, "ARM_cacheflush"}, {0xf0005, "ARM_set_tls"},
    };
    return names;
}

void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

// A pipe: a byte queue shared by its read and write ends.
struct PipeBuffer {
    std::deque<uint8_t> data;
    int readers = 1, writers = 1;
    static constexpr size_t kCapacity = 65536;
};

class PipeEnd : public OpenFile {
public:
    PipeEnd(std::shared_ptr<PipeBuffer> b, bool write_end) : b_(std::move(b)), write_(write_end) {}
    ~PipeEnd() override { (write_ ? b_->writers : b_->readers)--; }
    int64_t read(void* buf, size_t len) override {
        if (write_) return -kEBADF;
        if (b_->data.empty()) return b_->writers ? (flags & kO_NONBLOCK ? -kEAGAIN : kWouldBlock) : 0;
        size_t n = std::min(len, b_->data.size());
        std::copy(b_->data.begin(), b_->data.begin() + std::ptrdiff_t(n), static_cast<uint8_t*>(buf));
        b_->data.erase(b_->data.begin(), b_->data.begin() + std::ptrdiff_t(n));
        return int64_t(n);
    }
    int64_t write(const void* buf, size_t len) override {
        if (!write_) return -kEBADF;
        if (!b_->readers) return -kEPIPE;
        size_t room = PipeBuffer::kCapacity - b_->data.size();
        if (!room) return flags & kO_NONBLOCK ? -kEAGAIN : kWouldBlock;
        size_t n = std::min(len, room);
        const auto* s = static_cast<const uint8_t*>(buf);
        b_->data.insert(b_->data.end(), s, s + n);
        return int64_t(n);
    }
    int16_t poll_events() override {
        if (write_) return b_->data.size() < PipeBuffer::kCapacity ? kPOLLOUT : 0;
        int16_t ev = b_->data.empty() ? 0 : kPOLLIN;
        if (!b_->writers) ev |= kPOLLHUP;
        return ev;
    }
    int fstat(StatInfo& st) override {
        st = StatInfo();
        st.mode = kS_IFIFO | 0600;
        return 0;
    }

private:
    std::shared_ptr<PipeBuffer> b_;
    bool write_;
};

}  // namespace

const char* Syscalls::name(uint32_t nr) {
    auto it = syscall_names().find(nr);
    return it == syscall_names().end() ? nullptr : it->second;
}

void Syscalls::handle() {
    Cpu& cpu = p_.cpu();
    Args a;
    for (int i = 0; i < 7; ++i) a[size_t(i)] = cpu.reg(i);
    const uint32_t nr = cpu.reg(7);
    Thread& t = p_.current();

    int64_t r = dispatch(nr, a);

    if (p_.opts_.strace) {
        const char* n = name(nr);
        char rs[32];
        if (r == kBlocked) std::snprintf(rs, sizeof rs, "<blocked>");
        else if (r == kNoResult) std::snprintf(rs, sizeof rs, "-");
        else if (r < 0 && r > -4096) std::snprintf(rs, sizeof rs, "-%lld", (long long)-r);
        else std::snprintf(rs, sizeof rs, "0x%llx", (unsigned long long)uint32_t(r));
        std::fprintf(stderr, "[%d] %s(%x, %x, %x, %x, %x, %x) = %s\n", t.tid, n ? n : std::to_string(nr).c_str(),
                     a[0], a[1], a[2], a[3], a[4], a[5], rs);
    }

    if (r == kBlocked) {
        t.state = Thread::State::Blocked;
        cpu.request_stop();
    } else if (r == kNoResult) {
        cpu.request_stop();
    } else {
        cpu.set_reg(0, uint32_t(r));
        if (p_.exited_ || t.state != Thread::State::Runnable) cpu.request_stop();
    }
}

int64_t Syscalls::retry_later(uint32_t nr, const Args& a, std::optional<Clock::time_point> deadline,
                              int32_t timeout_result) {
    if (retrying_) return kBlocked;
    Thread& t = p_.current();
    t.deadline = deadline;
    t.timeout_result = timeout_result;
    t.interruptible = true;
    t.wake = [this, nr, a](Thread& th) {
        retrying_ = true;
        int64_t r = dispatch(nr, a);
        retrying_ = false;
        if (r == kBlocked) return false;
        th.ctx.r[0] = uint32_t(r);
        return true;
    };
    return kBlocked;
}

std::string Syscalls::path_arg(uint32_t addr, bool& ok) {
    std::string s;
    ok = addr && p_.mem().read_string(addr, s, 4096);
    return s;
}

int64_t Syscalls::clock_ns(int clock) {
    using namespace std::chrono;
    if (clock == 0 /*REALTIME*/) return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    // MONOTONIC and the CPU-time clocks all count from the emulated boot.
    return duration_cast<nanoseconds>(p_.uptime()).count();
}

bool Syscalls::read_timespec(uint32_t addr, int64_t& ns) {
    int32_t ts[2];
    if (!p_.mem().read(addr, ts, sizeof ts)) return false;
    if (ts[1] < 0 || ts[1] >= 1000000000) return false;
    ns = int64_t(ts[0]) * 1000000000 + ts[1];
    return true;
}

void Syscalls::write_timespec(uint32_t addr, int64_t ns) {
    int32_t ts[2] = {int32_t(ns / 1000000000), int32_t(ns % 1000000000)};
    p_.mem().write(addr, ts, sizeof ts);
}

int64_t Syscalls::do_open(uint32_t path_addr, uint32_t flags, uint32_t mode) {
    bool ok;
    std::string path = path_arg(path_addr, ok);
    if (!ok) return -kEFAULT;
    std::shared_ptr<OpenFile> f;
    int r = p_.vfs().open(p_.cwd_, path, flags, mode & ~p_.umask_, f);
    if (r < 0) return r;
    return p_.alloc_fd(std::move(f), 0, false);
}

int64_t Syscalls::do_stat(const StatInfo& st, uint32_t out, bool is64) {
    if (is64) {
        // struct stat64 (EABI): 104 bytes
        uint8_t b[104] = {};
        put64(b + 0, st.dev);
        put32(b + 12, uint32_t(st.ino));
        put32(b + 16, st.mode);
        put32(b + 20, st.nlink);
        put32(b + 24, st.uid);
        put32(b + 28, st.gid);
        put64(b + 32, st.rdev);
        put64(b + 48, uint64_t(st.size));
        put32(b + 56, st.blksize);
        put64(b + 64, st.blocks);
        put32(b + 72, uint32_t(st.atime));
        put32(b + 80, uint32_t(st.mtime));
        put32(b + 88, uint32_t(st.ctime));
        put64(b + 96, st.ino);
        return p_.mem().write(out, b, sizeof b) ? 0 : -kEFAULT;
    }
    // struct stat: 64 bytes, 16-bit mode/nlink/uid/gid
    uint8_t b[64] = {};
    put32(b + 0, uint32_t(st.dev));
    put32(b + 4, uint32_t(st.ino));
    uint16_t m16[4] = {uint16_t(st.mode), uint16_t(st.nlink), uint16_t(st.uid), uint16_t(st.gid)};
    std::memcpy(b + 8, m16, 8);
    put32(b + 16, uint32_t(st.rdev));
    put32(b + 20, uint32_t(st.size));
    put32(b + 24, st.blksize);
    put32(b + 28, uint32_t(st.blocks));
    put32(b + 32, uint32_t(st.atime));
    put32(b + 40, uint32_t(st.mtime));
    put32(b + 48, uint32_t(st.ctime));
    return p_.mem().write(out, b, sizeof b) ? 0 : -kEFAULT;
}

int64_t Syscalls::do_getdents(int fd, uint32_t buf, uint32_t len, bool is64) {
    OpenFile* f = p_.file(fd);
    if (!f) return -kEBADF;
    if (!f->is_dir()) return -kENOTDIR;
    std::vector<uint8_t> out;
    DirItem item;
    while (true) {
        int64_t pos = f->seek(0, kSEEK_CUR);
        if (!f->next_dirent(item)) break;
        size_t namelen = item.name.size() + 1;
        size_t reclen = is64 ? ((19 + namelen + 7) & ~size_t(7)) : ((10 + namelen + 1 + 3) & ~size_t(3));
        if (out.size() + reclen > len) {
            f->seek(pos, kSEEK_SET);  // leave it for the next call
            if (out.empty()) return -kEINVAL;
            break;
        }
        size_t at = out.size();
        out.resize(at + reclen, 0);
        uint8_t* e = &out[at];
        if (is64) {
            put64(e, item.ino);
            put64(e + 8, uint64_t(pos + 1));
            uint16_t rl = uint16_t(reclen);
            std::memcpy(e + 16, &rl, 2);
            e[18] = item.type;
            std::memcpy(e + 19, item.name.c_str(), namelen);
        } else {
            put32(e, uint32_t(item.ino));
            put32(e + 4, uint32_t(pos + 1));
            uint16_t rl = uint16_t(reclen);
            std::memcpy(e + 8, &rl, 2);
            std::memcpy(e + 10, item.name.c_str(), namelen);
            e[reclen - 1] = item.type;
        }
    }
    if (!p_.mem().write(buf, out.data(), out.size())) return -kEFAULT;
    return int64_t(out.size());
}

int64_t Syscalls::do_mmap(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int fd, uint64_t off) {
    (void)prot;  // all guest memory is RWX; see GuestMemory
    if (!len) return -kEINVAL;
    GuestMemory& mem = p_.mem();
    const uint32_t size = uint32_t(page_up(len));
    if ((flags & kMAP_FIXED) && (addr % kPageSize)) return -kEINVAL;

    if (!(flags & kMAP_ANONYMOUS)) {
        OpenFile* f = p_.file(fd);
        if (!f) return -kEBADF;
        // Devices hand out host memory that stays shared with the emulator.
        uint8_t* host = nullptr;
        std::shared_ptr<void> owner;
        int r = f->mmap(off, size, &host, &owner);
        if (r == 0) {
            uint32_t at = (flags & kMAP_FIXED) ? addr : mem.find_free(size, addr);
            if (!at || !mem.map_host(at, size, host, owner, f->path)) return -kENOMEM;
            return at;
        }
        if (r != -kENODEV) return r;
        if (flags & kMAP_SHARED) {
            StatInfo st;
            f->fstat(st);
            if ((st.mode & kS_IFMT) != kS_IFREG) return -kENODEV;
        }
        // Regular files: copy the contents (kMAP_SHARED writes are not written back).
        uint32_t at = (flags & kMAP_FIXED) ? (mem.map_fixed(addr, size, f->path) ? addr : 0)
                                          : mem.map_anywhere(size, addr, f->path);
        if (!at) return -kENOMEM;
        std::vector<uint8_t> buf(len);
        int64_t n = f->pread(buf.data(), buf.size(), off);
        if (n > 0) mem.write(at, buf.data(), size_t(n));
        return at;
    }
    uint32_t at = (flags & kMAP_FIXED) ? (mem.map_fixed(addr, size, "[anon]") ? addr : 0)
                                      : mem.map_anywhere(size, addr, "[anon]");
    return at ? int64_t(at) : -kENOMEM;
}

int64_t Syscalls::do_clone(const Args& a) {
    const uint32_t flags = a[0], newsp = a[1], parent_tid = a[2], tls = a[3], child_tid = a[4];
    if (!(flags & kCLONE_VM)) {
        std::fprintf(stderr, "[didj] fork/clone without kCLONE_VM is not supported\n");
        return -kENOSYS;
    }
    auto t = std::make_unique<Thread>();
    t->tid = p_.next_tid_++;
    p_.cpu().save(t->ctx);  // PC is already past the svc
    t->ctx.r[0] = 0;
    if (newsp) t->ctx.r[kRegSP] = newsp;
    Thread& parent = p_.current();
    t->tls = (flags & kCLONE_SETTLS) ? tls : parent.tls;
    t->sigmask = parent.sigmask;
    if (flags & kCLONE_CHILD_CLEARTID) t->clear_child_tid = child_tid;
    if (flags & kCLONE_PARENT_SETTID) p_.mem().write_u32(parent_tid, uint32_t(t->tid));
    if (flags & kCLONE_CHILD_SETTID) p_.mem().write_u32(child_tid, uint32_t(t->tid));
    int tid = t->tid;
    p_.threads_.push_back(std::move(t));
    return tid;
}

int64_t Syscalls::do_futex(const Args& a) {
    const uint32_t uaddr = a[0], val = a[2], timeout = a[3], uaddr2 = a[4], val3 = a[5];
    const int op = int(a[1] & 127);  // ignore FUTEX_PRIVATE_FLAG
    Thread& t = p_.current();
    switch (op) {
        case 0: {  // FUTEX_WAIT
            uint32_t cur;
            if (!p_.mem().read_u32(uaddr, cur)) return -kEFAULT;
            if (cur != val) return -kEAGAIN;
            t.deadline.reset();
            if (timeout) {
                int64_t ns;
                if (!read_timespec(timeout, ns)) return -kEINVAL;
                t.deadline = Clock::now() + std::chrono::nanoseconds(ns);
            }
            t.timeout_result = -kETIMEDOUT;
            t.interruptible = true;
            t.futex_addr = uaddr;
            t.futex_woken = false;
            t.wake = [](Thread& th) {
                if (!th.futex_woken) return false;
                th.futex_woken = false;
                th.futex_addr = 0;
                th.ctx.r[0] = 0;
                return true;
            };
            return kBlocked;
        }
        case 1:  // FUTEX_WAKE
            return p_.futex_wake(uaddr, int(val));
        case 3:  // FUTEX_REQUEUE
            return p_.futex_wake(uaddr, int(val), uaddr2, int(timeout));
        case 4: {  // FUTEX_CMP_REQUEUE
            uint32_t cur;
            if (!p_.mem().read_u32(uaddr, cur)) return -kEFAULT;
            if (cur != val3) return -kEAGAIN;
            return p_.futex_wake(uaddr, int(val), uaddr2, int(timeout));
        }
        case 5: {  // FUTEX_WAKE_OP
            uint32_t old;
            if (!p_.mem().read_u32(uaddr2, old)) return -kEFAULT;
            int fop = int((val3 >> 28) & 15), cmp = int((val3 >> 24) & 15);
            int32_t oparg = int32_t(val3 << 8) >> 20, cmparg = int32_t(val3 << 20) >> 20;
            if (fop & 8) oparg = 1 << (oparg & 31);
            uint32_t nv = old;
            switch (fop & 7) {
                case 0: nv = uint32_t(oparg); break;
                case 1: nv = old + uint32_t(oparg); break;
                case 2: nv = old | uint32_t(oparg); break;
                case 3: nv = old & ~uint32_t(oparg); break;
                case 4: nv = old ^ uint32_t(oparg); break;
                default: return -kENOSYS;
            }
            p_.mem().write_u32(uaddr2, nv);
            int woken = p_.futex_wake(uaddr, int(val));
            int32_t o = int32_t(old);
            bool hit = cmp == 0 ? o == cmparg : cmp == 1 ? o != cmparg : cmp == 2 ? o < cmparg
                     : cmp == 3 ? o <= cmparg : cmp == 4 ? o > cmparg : cmp == 5 ? o >= cmparg : false;
            if (hit) woken += p_.futex_wake(uaddr2, int(timeout));
            return woken;
        }
        default:
            std::fprintf(stderr, "[didj] futex op %d not supported\n", op);
            return -kENOSYS;
    }
}

int64_t Syscalls::do_poll(uint32_t nr, const Args& a) {
    const uint32_t fds = a[0], nfds = a[1];
    const int32_t timeout_ms = int32_t(a[2]);
    if (nfds > 1024) return -kEINVAL;
    std::vector<uint8_t> buf(size_t(nfds) * 8);
    if (nfds && !p_.mem().read(fds, buf.data(), buf.size())) return -kEFAULT;
    int ready = 0;
    for (uint32_t i = 0; i < nfds; ++i) {
        int32_t fd;
        int16_t events, revents = 0;
        std::memcpy(&fd, &buf[i * 8], 4);
        std::memcpy(&events, &buf[i * 8 + 4], 2);
        if (fd >= 0) {
            OpenFile* f = p_.file(fd);
            revents = f ? int16_t(f->poll_events() & (events | kPOLLERR | kPOLLHUP)) : int16_t(kPOLLNVAL);
        }
        std::memcpy(&buf[i * 8 + 6], &revents, 2);
        if (revents) ++ready;
    }
    if (ready || timeout_ms == 0) {
        if (nfds) p_.mem().write(fds, buf.data(), buf.size());
        return ready;
    }
    std::optional<Clock::time_point> deadline;
    if (!retrying_ && timeout_ms > 0) deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    return retry_later(nr, a, deadline, 0);
}

int64_t Syscalls::do_select(uint32_t nr, const Args& a) {
    const int nfds = int(a[0]);
    const uint32_t rp = a[1], wp = a[2], ep = a[3], tvp = a[4];
    if (nfds < 0 || nfds > 1024) return -kEINVAL;
    const size_t words = size_t(nfds + 31) / 32;
    std::vector<uint32_t> rin(words), win(words), rout(words), wout(words);
    if (rp && words && !p_.mem().read(rp, rin.data(), words * 4)) return -kEFAULT;
    if (wp && words && !p_.mem().read(wp, win.data(), words * 4)) return -kEFAULT;
    int ready = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        uint32_t bit = 1u << (fd % 32);
        size_t w = size_t(fd) / 32;
        if (!((rin[w] | win[w]) & bit)) continue;
        OpenFile* f = p_.file(fd);
        if (!f) return -kEBADF;
        int16_t ev = f->poll_events();
        if ((rin[w] & bit) && (ev & (kPOLLIN | kPOLLHUP))) rout[w] |= bit, ++ready;
        if ((win[w] & bit) && (ev & kPOLLOUT)) wout[w] |= bit, ++ready;
    }
    int64_t timeout_ns = -1;
    if (tvp) {
        int32_t tv[2];
        if (!p_.mem().read(tvp, tv, sizeof tv)) return -kEFAULT;
        timeout_ns = int64_t(tv[0]) * 1000000000 + int64_t(tv[1]) * 1000;
    }
    if (ready || timeout_ns == 0) {
        if (rp && words) p_.mem().write(rp, rout.data(), words * 4);
        if (wp && words) p_.mem().write(wp, wout.data(), words * 4);
        if (ep && words) {
            std::vector<uint32_t> zero(words, 0);
            p_.mem().write(ep, zero.data(), words * 4);
        }
        return ready;
    }
    std::optional<Clock::time_point> deadline;
    if (!retrying_ && timeout_ns > 0) deadline = Clock::now() + std::chrono::nanoseconds(timeout_ns);
    return retry_later(nr, a, deadline, 0);
}

int64_t Syscalls::do_ioctl(int fd, uint32_t cmd, uint32_t arg) {
    OpenFile* f = p_.file(fd);
    if (!f) return -kEBADF;
    return f->ioctl(cmd, arg, p_.mem());
}

int64_t Syscalls::do_fcntl(int fd, uint32_t cmd, uint32_t arg) {
    auto it = p_.fds_.find(fd);
    if (it == p_.fds_.end()) return -kEBADF;
    switch (int(cmd)) {
        case kF_DUPFD: return p_.alloc_fd(it->second.file, int(arg));
        case kF_GETFD: return it->second.cloexec ? kFD_CLOEXEC : 0;
        case kF_SETFD: it->second.cloexec = arg & kFD_CLOEXEC; return 0;
        case kF_GETFL: return it->second.file->flags;
        case kF_SETFL: {
            const uint32_t settable = kO_APPEND | kO_NONBLOCK;
            auto& fl = it->second.file->flags;
            fl = (fl & ~settable) | (arg & settable);
            return 0;
        }
        case kF_GETLK: case kF_GETLK64: {
            // No other process can hold a lock: report F_UNLCK (2).
            uint16_t type = 2;
            p_.mem().write(arg, &type, 2);
            return 0;
        }
        case kF_SETLK: case kF_SETLKW: case kF_SETLK64: case kF_SETLKW64: return 0;
        default: return -kEINVAL;
    }
}

int64_t Syscalls::do_rt_sigaction(const Args& a) {
    const int sig = int(a[0]);
    if (sig < 1 || sig > 64 || sig == 9 || sig == 19) return -kEINVAL;
    SigAction& act = p_.actions_[sig];
    if (a[2]) {
        uint32_t out[5] = {act.handler, act.flags, act.restorer, uint32_t(act.mask), uint32_t(act.mask >> 32)};
        if (!p_.mem().write(a[2], out, sizeof out)) return -kEFAULT;
    }
    if (a[1]) {
        uint32_t in[5];
        if (!p_.mem().read(a[1], in, sizeof in)) return -kEFAULT;
        act.handler = in[0];
        act.flags = in[1];
        act.restorer = in[2];
        act.mask = in[3] | (uint64_t(in[4]) << 32);
    }
    return 0;
}

int64_t Syscalls::do_rt_sigprocmask(const Args& a) {
    Thread& t = p_.current();
    if (a[2]) {
        uint32_t old[2] = {uint32_t(t.sigmask), uint32_t(t.sigmask >> 32)};
        if (!p_.mem().write(a[2], old, sizeof old)) return -kEFAULT;
    }
    if (a[1]) {
        uint32_t in[2];
        if (!p_.mem().read(a[1], in, sizeof in)) return -kEFAULT;
        uint64_t set = in[0] | (uint64_t(in[1]) << 32);
        switch (a[0]) {
            case 0: t.sigmask |= set; break;   // SIG_BLOCK
            case 1: t.sigmask &= ~set; break;  // SIG_UNBLOCK
            case 2: t.sigmask = set; break;    // SIG_SETMASK
            default: return -kEINVAL;
        }
        t.sigmask &= ~((uint64_t(1) << 8) | (uint64_t(1) << 18));  // KILL, STOP
    }
    return 0;
}

int64_t Syscalls::do_sleep(uint32_t nr, const Args& a, uint32_t req, bool absolute, int clock) {
    (void)nr;
    (void)a;
    int64_t ns;
    if (!read_timespec(req, ns)) return -kEINVAL;
    Thread& t = p_.current();
    if (absolute) ns -= clock_ns(clock);
    if (ns <= 0) return 0;
    t.deadline = Clock::now() + std::chrono::nanoseconds(ns);
    t.timeout_result = 0;
    t.interruptible = true;
    t.wake = [](Thread&) { return false; };  // only the deadline ends it
    return kBlocked;
}

int64_t Syscalls::dispatch(uint32_t nr, const Args& a) {
    GuestMemory& mem = p_.mem();
    Vfs& vfs = p_.vfs();
    Thread& t = p_.current();
    bool ok = true;

    switch (nr) {
        // --- process and threads ---
        case NR_exit:
            p_.exit_thread(t, int(a[0] & 0xff));
            return kNoResult;
        case NR_exit_group:
            p_.exit_process(int(a[0] & 0xff));
            return kNoResult;
        case NR_fork:
        case NR_vfork:
        case NR_execve:
            std::fprintf(stderr, "[didj] %s is not supported\n", name(nr));
            return -kENOSYS;
        case NR_clone: return do_clone(a);
        case NR_wait4: return -kECHILD;
        case NR_getpid: return p_.pid_;
        case NR_gettid: return t.tid;
        case NR_getppid: return 1;
        case NR_getpgrp: case NR_getpgid: case NR_getsid: case NR_setsid: return p_.pid_;
        case NR_setpgid: return 0;
        case NR_getuid: case NR_getuid32: case NR_geteuid: case NR_geteuid32:
        case NR_getgid: case NR_getgid32: case NR_getegid: case NR_getegid32: return 0;
        case NR_setuid: case NR_setuid32: case NR_setgid: case NR_setgid32: return 0;
        case NR_getresuid32: case NR_getresgid32: {
            uint32_t zero = 0;
            for (int i = 0; i < 3; ++i)
                if (a[size_t(i)]) mem.write_u32(a[size_t(i)], zero);
            return 0;
        }
        case NR_getgroups: case NR_getgroups32: return 0;
        case NR_set_tid_address:
            t.clear_child_tid = a[0];
            return t.tid;
        case NR_set_robust_list: return -kENOSYS;
        case NR_ARM_set_tls:
            t.tls = a[0];
            mem.write_u32(kKuserPage + 0xff0, a[0]);
            return 0;
        case NR_ARM_cacheflush:
            mem.invalidate_code(a[0], a[1] - a[0]);
            return 0;
        case NR_futex: return do_futex(a);
        case NR_sched_yield: return 0;
        case NR_sched_get_priority_max: return a[0] == 0 ? 0 : 99;
        case NR_sched_get_priority_min: return a[0] == 0 ? 0 : 1;
        case NR_sched_getscheduler: return 0;
        case NR_sched_setscheduler: case NR_sched_setparam: return 0;
        case NR_sched_getparam: return mem.write_u32(a[1], 0) ? 0 : -kEFAULT;
        case NR_sched_setaffinity: return 0;
        case NR_sched_getaffinity: {
            if (a[1] < 4) return -kEINVAL;
            mem.write_u32(a[2], 1);
            return 4;
        }
        case NR_getpriority: return 20;
        case NR_setpriority: case NR_nice: return 0;
        case NR_prctl: return 0;
        case NR_personality: return 0;

        // --- signals ---
        case NR_rt_sigaction: return do_rt_sigaction(a);
        case NR_rt_sigprocmask: return do_rt_sigprocmask(a);
        case NR_rt_sigpending: {
            uint64_t pend = t.pending | p_.process_pending_;
            uint32_t out[2] = {uint32_t(pend), uint32_t(pend >> 32)};
            return mem.write(a[0], out, sizeof out) ? 0 : -kEFAULT;
        }
        case NR_sigreturn:
        case NR_rt_sigreturn:
            p_.cpu().save(t.ctx);
            p_.restore_signal_frame(t, nr == NR_rt_sigreturn);
            p_.cpu().load(t.ctx);
            return kNoResult;
        case NR_rt_sigsuspend: {
            uint32_t in[2];
            if (!mem.read(a[0], in, sizeof in)) return -kEFAULT;
            // The temporary mask applies while waiting; the handler frame
            // saves the original, which sigreturn restores.
            t.sigmask = in[0] | (uint64_t(in[1]) << 32);
            t.deadline.reset();
            t.interruptible = true;
            t.wake = [](Thread&) { return false; };
            return kBlocked;
        }
        case NR_sigaltstack: return 0;
        case NR_kill:
            if (int(a[0]) != p_.pid_ && int(a[0]) != 0 && int(a[0]) != -1) return -kESRCH;
            if (a[1]) p_.raise_signal(int(a[1]));
            return 0;
        case NR_tkill:
        case NR_tgkill: {
            int tid = int(nr == NR_tgkill ? a[1] : a[0]);
            int sig = int(nr == NR_tgkill ? a[2] : a[1]);
            bool found = false;
            for (auto& th : p_.threads_) found |= th->tid == tid && th->state != Thread::State::Exited;
            if (!found) return -kESRCH;
            if (sig) p_.raise_signal(sig, tid);
            return 0;
        }

        // --- memory ---
        case NR_brk: {
            uint32_t want = a[0];
            if (want > p_.brk_ && want < kMmapBase) {
                uint32_t old_end = uint32_t(page_up(p_.brk_)), new_end = uint32_t(page_up(want));
                // Grow only into free space directly above the heap.
                if (new_end > old_end && (mem.find_free(new_end - old_end, old_end) != old_end ||
                                          !mem.map_fixed(old_end, new_end - old_end, "[heap]")))
                    return p_.brk_;
                p_.brk_ = want;
            } else if (want >= p_.brk_start_ && want < p_.brk_) {
                p_.brk_ = want;  // shrinking keeps the pages; they are reused on growth
            }
            return p_.brk_;
        }
        case NR_mmap2: return do_mmap(a[0], a[1], a[2], a[3], int(a[4]), uint64_t(a[5]) * kPageSize);
        case NR_munmap:
            if (a[0] % kPageSize) return -kEINVAL;
            mem.unmap(a[0], a[1]);
            return 0;
        case NR_mremap: {
            const uint32_t old_addr = a[0], old_len = uint32_t(page_up(a[1])), new_len = uint32_t(page_up(a[2]));
            if (old_addr % kPageSize || !new_len) return -kEINVAL;
            if (new_len <= old_len) {
                if (new_len < old_len) mem.unmap(old_addr + new_len, old_len - new_len);
                return old_addr;
            }
            if (!(a[3] & 1)) return -kENOMEM;  // MREMAP_MAYMOVE not set
            uint32_t at = mem.map_anywhere(new_len, 0, "[anon]");
            if (!at) return -kENOMEM;
            std::vector<uint8_t> tmp(old_len);
            mem.read(old_addr, tmp.data(), tmp.size());
            mem.write(at, tmp.data(), tmp.size());
            mem.unmap(old_addr, old_len);
            return at;
        }
        case NR_mprotect: case NR_msync: case NR_madvise: case NR_mlock: case NR_munlock:
        case NR_mlockall: case NR_munlockall: case NR_fadvise64_64:
            return 0;

        // --- files ---
        case NR_open: return do_open(a[0], a[1], a[2]);
        case NR_creat: return do_open(a[0], kO_CREAT | kO_WRONLY | kO_TRUNC, a[1]);
        case NR_close: return p_.fds_.erase(int(a[0])) ? 0 : -kEBADF;
        case NR_read: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            uint8_t* dst = mem.ptr(a[1], a[2]);
            std::vector<uint8_t> tmp;
            if (!dst) {
                tmp.resize(a[2]);
                dst = tmp.data();
            }
            int64_t n = f->read(dst, a[2]);
            if (n == OpenFile::kWouldBlock) return retry_later(nr, a, std::nullopt, 0);
            if (n > 0 && !tmp.empty() && !mem.write(a[1], tmp.data(), size_t(n))) return -kEFAULT;
            return n;
        }
        case NR_write: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            std::vector<uint8_t> tmp(a[2]);
            if (a[2] && !mem.read(a[1], tmp.data(), tmp.size())) return -kEFAULT;
            int64_t n = f->write(tmp.data(), tmp.size());
            if (n == OpenFile::kWouldBlock) return retry_later(nr, a, std::nullopt, 0);
            return n;
        }
        case NR_readv:
        case NR_writev: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            int64_t total = 0;
            for (uint32_t i = 0; i < a[2]; ++i) {
                uint32_t iov[2];
                if (!mem.read(a[1] + i * 8, iov, 8)) return -kEFAULT;
                std::vector<uint8_t> tmp(iov[1]);
                int64_t n;
                if (nr == NR_writev) {
                    if (!mem.read(iov[0], tmp.data(), tmp.size())) return -kEFAULT;
                    n = f->write(tmp.data(), tmp.size());
                } else {
                    n = f->read(tmp.data(), tmp.size());
                    if (n > 0) mem.write(iov[0], tmp.data(), size_t(n));
                }
                if (n == OpenFile::kWouldBlock) {
                    if (total) break;
                    return retry_later(nr, a, std::nullopt, 0);
                }
                if (n < 0) return total ? total : n;
                total += n;
                if (uint32_t(n) < iov[1]) break;
            }
            return total;
        }
        case NR_pread64:
        case NR_pwrite64: {
            // EABI passes the 64-bit offset in an even register pair: r4/r5.
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            uint64_t off = a[4] | (uint64_t(a[5]) << 32);
            std::vector<uint8_t> tmp(a[2]);
            if (nr == NR_pwrite64) {
                if (!mem.read(a[1], tmp.data(), tmp.size())) return -kEFAULT;
                return f->pwrite(tmp.data(), tmp.size(), off);
            }
            int64_t n = f->pread(tmp.data(), tmp.size(), off);
            if (n > 0 && !mem.write(a[1], tmp.data(), size_t(n))) return -kEFAULT;
            return n;
        }
        case NR_lseek: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            int64_t r = f->seek(int32_t(a[1]), int(a[2]));
            if (r > 0x7fffffff) return -kEINVAL;  // EOVERFLOW in spirit
            return r;
        }
        case NR__llseek: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            int64_t off = int64_t((uint64_t(a[1]) << 32) | a[2]);
            int64_t r = f->seek(off, int(a[4]));
            if (r < 0) return r;
            uint64_t res = uint64_t(r);
            return mem.write(a[3], &res, 8) ? 0 : -kEFAULT;
        }
        case NR_dup: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            return p_.alloc_fd(p_.fds_[int(a[0])].file);
        }
        case NR_dup2: {
            if (!p_.file(int(a[0]))) return -kEBADF;
            if (a[0] == a[1]) return a[1];
            p_.fds_[int(a[1])] = Process::Fd{p_.fds_[int(a[0])].file, false};
            return a[1];
        }
        case NR_pipe: {
            auto buf = std::make_shared<PipeBuffer>();
            auto r = std::make_shared<PipeEnd>(buf, false);
            auto w = std::make_shared<PipeEnd>(buf, true);
            r->flags = kO_RDONLY;
            w->flags = kO_WRONLY;
            int fds[2] = {p_.alloc_fd(r), 0};
            fds[1] = p_.alloc_fd(w);
            return mem.write(a[0], fds, sizeof fds) ? 0 : -kEFAULT;
        }
        case NR_ioctl: return do_ioctl(int(a[0]), a[1], a[2]);
        case NR_fcntl:
        case NR_fcntl64: return do_fcntl(int(a[0]), a[1], a[2]);
        case NR_flock: return 0;
        case NR_fsync: case NR_fdatasync: case NR_sync: return 0;
        case NR_getdents: return do_getdents(int(a[0]), a[1], a[2], false);
        case NR_getdents64: return do_getdents(int(a[0]), a[1], a[2], true);
        case NR_poll: return do_poll(nr, a);
        case NR__newselect: return do_select(nr, a);
        case NR_stat: case NR_lstat: case NR_stat64: case NR_lstat64: {
            std::string path = path_arg(a[0], ok);
            if (!ok) return -kEFAULT;
            StatInfo st;
            int r = vfs.stat(p_.cwd_, path, nr == NR_stat || nr == NR_stat64, st);
            if (r < 0) return r;
            return do_stat(st, a[1], nr == NR_stat64 || nr == NR_lstat64);
        }
        case NR_fstat: case NR_fstat64: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            StatInfo st;
            int r = f->fstat(st);
            if (r < 0) return r;
            return do_stat(st, a[1], nr == NR_fstat64);
        }
        case NR_statfs: case NR_fstatfs: case NR_statfs64: case NR_fstatfs64: {
            // A generic 32 MB filesystem with 4K blocks; struct statfs64 is 84 bytes.
            bool is64 = nr == NR_statfs64 || nr == NR_fstatfs64;
            uint32_t out = is64 ? a[2] : a[1];
            uint8_t b[84] = {};
            put32(b, 0x4d44);  // MSDOS_SUPER_MAGIC
            put32(b + 4, 4096);
            if (is64) {
                put64(b + 8, 8192);
                put64(b + 16, 4096);
                put64(b + 24, 4096);
                put64(b + 32, 65536);
                put64(b + 40, 32768);
                put32(b + 56, 255);
                put32(b + 60, 4096);
            } else {
                put32(b + 8, 8192);
                put32(b + 12, 4096);
                put32(b + 16, 4096);
                put32(b + 20, 65536);
                put32(b + 24, 32768);
                put32(b + 36, 255);
                put32(b + 40, 4096);
            }
            return mem.write(out, b, is64 ? std::min<uint32_t>(a[1], 84) : 64) ? 0 : -kEFAULT;
        }
        case NR_access: {
            std::string path = path_arg(a[0], ok);
            return ok ? vfs.access(p_.cwd_, path, int(a[1])) : -kEFAULT;
        }
        case NR_readlink: {
            std::string path = path_arg(a[0], ok), target;
            if (!ok) return -kEFAULT;
            int r = vfs.readlink(p_.cwd_, path, target);
            if (r < 0) return r;
            size_t n = std::min<size_t>(target.size(), a[2]);
            return mem.write(a[1], target.data(), n) ? int64_t(n) : -kEFAULT;
        }
        case NR_mkdir: {
            std::string path = path_arg(a[0], ok);
            return ok ? vfs.mkdir(p_.cwd_, path) : -kEFAULT;
        }
        case NR_rmdir: {
            std::string path = path_arg(a[0], ok);
            return ok ? vfs.rmdir(p_.cwd_, path) : -kEFAULT;
        }
        case NR_unlink: {
            std::string path = path_arg(a[0], ok);
            return ok ? vfs.unlink(p_.cwd_, path) : -kEFAULT;
        }
        case NR_rename: {
            std::string from = path_arg(a[0], ok), to;
            if (ok) to = path_arg(a[1], ok);
            return ok ? vfs.rename(p_.cwd_, from, to) : -kEFAULT;
        }
        case NR_truncate:
        case NR_truncate64: {
            std::string path = path_arg(a[0], ok);
            if (!ok) return -kEFAULT;
            uint64_t len = nr == NR_truncate ? a[1] : (a[2] | (uint64_t(a[3]) << 32));
            return vfs.truncate(p_.cwd_, path, len);
        }
        case NR_ftruncate:
        case NR_ftruncate64: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            uint64_t len = nr == NR_ftruncate ? a[1] : (a[2] | (uint64_t(a[3]) << 32));
            return f->truncate(len);
        }
        case NR_chdir: {
            std::string path = path_arg(a[0], ok), canon;
            if (!ok) return -kEFAULT;
            int r = vfs.resolve(p_.cwd_, path, true, canon);
            if (r < 0) return r;
            StatInfo st;
            r = vfs.stat("/", canon, true, st);
            if (r < 0) return r;
            if ((st.mode & kS_IFMT) != kS_IFDIR) return -kENOTDIR;
            p_.cwd_ = canon;
            return 0;
        }
        case NR_fchdir: {
            OpenFile* f = p_.file(int(a[0]));
            if (!f) return -kEBADF;
            if (!f->is_dir()) return -kENOTDIR;
            p_.cwd_ = f->path;
            return 0;
        }
        case NR_getcwd: {
            const std::string& c = p_.cwd_;
            if (a[1] < c.size() + 1) return -kERANGE;
            return mem.write(a[0], c.c_str(), c.size() + 1) ? int64_t(c.size() + 1) : -kEFAULT;
        }
        case NR_umask: {
            uint32_t old = p_.umask_;
            p_.umask_ = a[0] & 0777;
            return old;
        }
        case NR_chmod: case NR_fchmod: case NR_chown: case NR_fchown: case NR_chown32: case NR_fchown32:
        case NR_lchown32: case NR_utimes:
            return 0;
        case NR_link: case NR_symlink: case NR_mknod: return -kEPERM;
        case NR_mount: case NR_umount2: return -kEPERM;

        // --- time ---
        case NR_gettimeofday: {
            int64_t ns = clock_ns(0);
            if (a[0]) {
                int32_t tv[2] = {int32_t(ns / 1000000000), int32_t((ns / 1000) % 1000000)};
                if (!mem.write(a[0], tv, sizeof tv)) return -kEFAULT;
            }
            if (a[1]) {
                int32_t tz[2] = {0, 0};
                mem.write(a[1], tz, sizeof tz);
            }
            return 0;
        }
        case NR_settimeofday: case NR_clock_settime: return 0;
        case NR_clock_gettime:
            if (a[0] > 7) return -kEINVAL;
            write_timespec(a[1], clock_ns(int(a[0])));
            return 0;
        case NR_clock_getres:
            if (a[1]) write_timespec(a[1], 1000000);  // 1 ms, like the Didj's HZ=1000 kernel
            return 0;
        case NR_nanosleep: return do_sleep(nr, a, a[0], false, 1);
        case NR_clock_nanosleep: return do_sleep(nr, a, a[2], a[1] & 1, int(a[0]));
        case NR_times: {
            uint32_t ticks = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(p_.uptime()).count() / 10);
            if (a[0]) {
                uint32_t tms[4] = {ticks, 0, 0, 0};
                mem.write(a[0], tms, sizeof tms);
            }
            return ticks;
        }
        case NR_getrusage: {
            std::vector<uint8_t> zero(72, 0);
            return mem.write(a[1], zero.data(), zero.size()) ? 0 : -kEFAULT;
        }
        case NR_getitimer: case NR_setitimer: {
            if (nr == NR_setitimer && a[2]) {
                std::vector<uint8_t> zero(16, 0);
                mem.write(a[2], zero.data(), zero.size());
            }
            if (nr == NR_getitimer) {
                std::vector<uint8_t> zero(16, 0);
                mem.write(a[1], zero.data(), zero.size());
            }
            return 0;
        }

        // --- system information ---
        case NR_uname: {
            char u[6][65] = {};
            std::strcpy(u[0], "Linux");
            std::strcpy(u[1], "didj");
            std::strcpy(u[2], "2.6.20.1-leapfrog");
            std::strcpy(u[3], "#1 PREEMPT");
            std::strcpy(u[4], "armv5tejl");
            std::strcpy(u[5], "(none)");
            return mem.write(a[0], u, sizeof u) ? 0 : -kEFAULT;
        }
        case NR_sysinfo: {
            uint8_t b[64] = {};
            put32(b, uint32_t(std::chrono::duration_cast<std::chrono::seconds>(p_.uptime()).count()));
            put32(b + 16, 32u << 20);  // totalram
            put32(b + 20, 16u << 20);  // freeram
            uint16_t procs = uint16_t(p_.threads_.size());
            std::memcpy(b + 40, &procs, 2);
            put32(b + 52, 1);          // mem_unit
            return mem.write(a[0], b, sizeof b) ? 0 : -kEFAULT;
        }
        case NR_ugetrlimit: {
            uint32_t lim[2] = {0xffffffffu, 0xffffffffu};
            if (a[0] == 3) lim[0] = lim[1] = kStackSize;  // RLIMIT_STACK
            if (a[0] == 7) lim[0] = lim[1] = 1024;        // RLIMIT_NOFILE
            return mem.write(a[1], lim, sizeof lim) ? 0 : -kEFAULT;
        }
        case NR_setrlimit: return 0;
        case NR_syslog: return 0;
        case NR_reboot: p_.exit_process(0); return kNoResult;
        case NR_socket: return -kEAFNOSUPPORT;
        case NR_restart_syscall: return -kEINTR;

        default:
            std::fprintf(stderr, "[didj] unimplemented syscall %s (%u)\n", name(nr) ? name(nr) : "?", nr);
            return -kENOSYS;
    }
}

}  // namespace didj::emu
