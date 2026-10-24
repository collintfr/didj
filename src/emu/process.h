// One emulated Linux process: address space, file descriptors, threads (run
// cooperatively on one CPU), signals and futexes. Syscalls are implemented in
// syscalls.cpp.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "emu/cpu.h"
#include "emu/memory.h"
#include "emu/vfs.h"

namespace didj::emu {

using Clock = std::chrono::steady_clock;

struct Thread {
    int tid = 0;
    CpuContext ctx;
    enum class State { Runnable, Blocked, Exited } state = State::Runnable;
    uint32_t tls = 0;
    uint32_t clear_child_tid = 0;
    uint64_t sigmask = 0;   // bit (sig - 1)
    uint64_t pending = 0;   // thread-directed pending signals

    // While Blocked: polled by the scheduler; returns true once the thread
    // can run again, having stored the syscall result in ctx.r[0].
    std::function<bool(Thread&)> wake;
    std::optional<Clock::time_point> deadline;  // when the wait times out
    int32_t timeout_result = 0;                  // syscall result on timeout (e.g. -ETIMEDOUT)
    bool interruptible = true;  // a deliverable signal ends the wait with -EINTR

    // Futex wait state.
    uint32_t futex_addr = 0;
    bool futex_woken = false;
};

struct SigAction {
    uint32_t handler = 0;  // 0 = SIG_DFL, 1 = SIG_IGN
    uint32_t flags = 0;
    uint32_t restorer = 0;
    uint64_t mask = 0;
};

struct ProcessOptions {
    bool strace = false;           // log every syscall to stderr
    uint32_t slice_us = 10000;     // time slice when several threads can run
};

class Process {
public:
    Process(Vfs& vfs, ProcessOptions opts);
    ~Process();

    // Loads an executable (and its interpreter) and prepares the main thread.
    bool exec(const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp,
              std::string* err);
    // Runs until the process exits; returns its exit status (128+sig if killed).
    int run();

    Cpu& cpu() { return cpu_; }
    GuestMemory& mem() { return mem_; }
    Vfs& vfs() { return vfs_; }
    Thread& current() { return *current_; }
    const std::string& cwd() const { return cwd_; }

    // Monotonic time since the emulated boot, and the guest's wall clock.
    Clock::duration uptime() const;

    // Raises a signal for the process (tid 0) or one thread.
    void raise_signal(int sig, int tid = 0);

    // Hook for device drivers and the front end: called each scheduler round.
    std::function<void()> on_idle;

private:
    friend class Syscalls;

    struct Fd {
        std::shared_ptr<OpenFile> file;
        bool cloexec = false;
    };
    int alloc_fd(std::shared_ptr<OpenFile> f, int min_fd = 0, bool cloexec = false);
    OpenFile* file(int fd);

    void on_interrupt(uint32_t intno);
    Thread* pick_next();
    void switch_to(Thread& t);
    bool deliver_signals(Thread& t);
    void setup_signal_frame(Thread& t, int sig, const SigAction& act);
    void restore_signal_frame(Thread& t, bool rt);
    void exit_thread(Thread& t, int status);
    void exit_process(int status);
    int futex_wake(uint32_t addr, int count, uint32_t requeue_to = 0, int requeue_max = 0);

    Vfs& vfs_;
    ProcessOptions opts_;
    Cpu cpu_;
    GuestMemory mem_;
    std::unique_ptr<class Syscalls> sys_;

    std::map<int, Fd> fds_;
    std::string cwd_ = "/";
    uint32_t brk_start_ = 0, brk_ = 0;
    uint32_t umask_ = 022;

    std::list<std::unique_ptr<Thread>> threads_;
    Thread* current_ = nullptr;
    int next_tid_ = 100;
    int pid_ = 100;
    SigAction actions_[65];
    uint64_t process_pending_ = 0;

    bool exited_ = false;
    int exit_status_ = 0;
    Clock::time_point boot_;
};

}  // namespace didj::emu
